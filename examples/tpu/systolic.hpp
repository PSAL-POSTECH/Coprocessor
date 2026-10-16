// The tpu model's systolic array: rows enter an input queue, cross 2 x lanes - 1 stages, the PEs of one anti-diagonal
// a stage, and wait in an output queue, a row a cycle in. Default keys: the golden timing (every step at issue);
// tpu_systolic_command_queue and the three keys after it make it systolic.sv's (examples/rtl/systolic).
#ifndef TPU_SYSTOLIC_HPP
#define TPU_SYSTOLIC_HPP

#include <deque>
#include <vector>

#include "vcix_uarch.hpp"

#include "values.hpp"

namespace tpu {

class Systolic {
  using Host = vcix_uarch::Host;
  using Insn = vcix_uarch::Insn;
  using Route = vcix_uarch::Route;
  using Weights = std::vector<std::deque<float>>;
  // A row in the array: element i of every lane, and each output lane's sum so far.
  struct Row {
    std::vector<float> input, sums;
  };

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Systolic(const char *name)
      : entry_{name, "input", "rows", 1, vcix_uarch::PRIMARY},
        issue_{name, "issue", "instructions", 1},
        weight_push_{name, "weight_push", "instructions", 1},
        pop_{name, "pop", "rows", 256},
        in_{name, "in", "rows", vcix_uarch::UNLIMITED} {}
  void configure(const vcix_uarch::Config &config) {
    const uint64_t lanes = config.uint(LANES_KEY, 128), entries = config.uint(QUEUE_KEY, 256);
    if (lanes < 1 || lanes > MAX_LANES)
      throw vcix_accel::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    stages_ = static_cast<uint32_t>(2 * lanes - 1);
    array_.configure(stages_, entries, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
    commands_.configure(static_cast<uint32_t>(config.uint(COMMANDS_KEY, 0)));
    in_.set_capacity(config.uint(IN_KEY, vcix_uarch::UNLIMITED));
    pop_.set_capacity(config.uint(POP_KEY, entries));
    weight_waits_ = config.uint(WAITS_KEY, 0) != 0;
    if (!commands_.depth() && (config.get(IN_KEY) || config.get(POP_KEY) || weight_waits_))
      throw vcix_accel::ConfigError(COMMANDS_KEY, "0", "is no queue: what moves at issue moves whole, and waits for nothing");
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_), h.port(issue_), h.port(weight_push_), h.port(pop_), h.port(in_);
    h.pipeline(array_, entry_, &Systolic::stage);
    h.queue(commands_);
    h.reg(weights_);
  }

  // vl rows into the array, row i element i of every lane.
  void input_push(Route &r) {
    r.use(issue_).enqueue(commands_);
    r.send(array_, r.vl(), [&r](const Host &host, uint64_t i) { return pushed(host, r.insn(), i); }).through(in_);
    r.compute([&](const Host &) {
      if (r.vl()) element_format(r.insn());
      const Weights &weights = weights_.get();
      if (weights.empty() || weights[0].empty()) fail("systolic input push: no weight was pushed before it");
    });
  }
  // vl beats of weights into the registers, beat i element i of every lane; a lane keeps its last `lanes`. Under
  // tpu_systolic_weight_waits they move only once the array is empty.
  void weight_push(Route &r) {
    r.use(issue_).use(weight_push_).enqueue(commands_);
    if (weight_waits_) r.wait_until_empty(array_);
    r.transfer(r.vl(), [this, &r](const Host &host, uint64_t i) { latch(host, r.insn(), i); }).through(in_);
  }
  // Up to vl rows of the output queue, once vl are out or none is on its way, row i to element i of vd in every lane
  // in rs1's format, rounded by frm.
  void pop(Route &r) {
    r.use(issue_).enqueue(commands_);
    r.receive_at_most(array_, r.vl(), [&r](const Host &host, uint64_t i, Row &&row) { popped(host, r.insn(), i, row); })
        .through(pop_);
    r.compute([&](const Host &) {
      if (element_format(r.insn()) == Element::BF16) fail("systolic pop: a pop to bfloat16 is not modeled");
    });
    r.when_short([&r](const Host &, uint64_t taken) {
      fail("systolic pop: %u elements asked, %llu computed", r.vl(), static_cast<unsigned long long>(taken));
    });
  }

  // funct6, vm and funct3 of custom-2: sf.vc.iv 0, sf.vc.iv 1 and sf.vc.v.i 2.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr vcix_uarch::Instruction<Systolic> INSTRUCTIONS[] = {
      {0x2200305B, FORM, "systolic input push", &Systolic::input_push},
      {0x2600305B, FORM, "systolic weight push", &Systolic::weight_push},
      {0x0800305B, FORM, "systolic pop", &Systolic::pop},
  };

 private:
  // What an element is rides the push or the pop in its rs1 field, one table at every width: 0 the width's own (an
  // integer of 8 bits, a half, a single; 64 bits are zeros), 1 an E4M3, 2 an E5M2, 3 a bfloat16; else the run ends.
  enum class Element { INTEGER, E4M3, E5M2, HALF, BF16, SINGLE, ZERO };
  static Element element_format(const Insn &insn) {
    const uint32_t code = vcix_accel::rs1(insn), sew = insn.sew_bits;
    if (code == 0) return sew == 8 ? Element::INTEGER : sew == 16 ? Element::HALF : sew == 32 ? Element::SINGLE : Element::ZERO;
    if (sew == 8 && (code == 1 || code == 2)) return code == 1 ? Element::E4M3 : Element::E5M2;
    if (sew == 16 && code == 3) return Element::BF16;
    fail("systolic: format %u names no element of %u bits", code, sew);
  }
  // Stage k: PE (lane, k - lane) adds input k - lane times the weight it holds now to lane's sum, so a lane sums
  // k = 0.. in order; the last stage also does the anti-diagonals a hart of more lanes than the array has would need.
  void stage(Row &row, uint32_t k) {
    const Weights &weights = weights_.get();
    const uint32_t lanes = static_cast<uint32_t>(row.sums.size());
    const uint32_t last = k + 1 == stages_ && 2 * lanes > stages_ + 1 ? 2 * lanes - 2 : k;
    for (uint32_t d = k; d <= last; d++)
      for (uint32_t lane = 0; lane < lanes && lane <= d; lane++) {
        const uint32_t at = d - lane;
        if (at < weights[lane].size()) row.sums[lane] = multiply_add(row.sums[lane], row.input[at], weights[lane][at]);
      }
  }
  // Row i of an input push: element i of every lane, and sums of 0.
  static Row pushed(const Host &host, const Insn &insn, uint64_t i) {
    const uint32_t lanes = host.lanes();
    Row row{std::vector<float>(lanes), std::vector<float>(lanes, 0.0f)};
    for (uint32_t lane = 0; lane < lanes; lane++) row.input[lane] = element(host, insn, lane, static_cast<uint32_t>(i));
    return row;
  }
  // Beat i of a weight push: element i of every lane shifts into that lane's weights.
  void latch(const Host &host, const Insn &insn, uint64_t i) {
    const uint32_t lanes = host.lanes();
    Weights &weights = weights_.write();
    if (weights.empty()) weights.assign(lanes, {});
    for (uint32_t lane = 0; lane < lanes; lane++) {
      const float weight = element(host, insn, lane, static_cast<uint32_t>(i));
      if (weights[lane].size() == lanes) weights[lane].pop_front();
      weights[lane].push_back(weight);
    }
  }
  // Row i a pop takes: each lane's sum to element i of vd.
  static void popped(const Host &host, const Insn &insn, uint64_t i, const Row &row) {
    const uint64_t mode = host.csr(CSR_FRM);
    const Element format = element_format(insn);
    const uint32_t vd = vcix_accel::rd(insn), at = static_cast<uint32_t>(i);
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      const float sum = row.sums[lane];
      switch (format) {
        case Element::INTEGER: put<int8_t>(host, lane, vd, at, static_cast<int8_t>(sum)); break;
        case Element::E4M3: put<uint8_t>(host, lane, vd, at, static_cast<uint8_t>(narrow(sum, E4M3, mode))); break;
        case Element::E5M2: put<uint8_t>(host, lane, vd, at, static_cast<uint8_t>(narrow(sum, E5M2, mode))); break;
        case Element::HALF: put<uint16_t>(host, lane, vd, at, static_cast<uint16_t>(narrow(sum, HALF, mode))); break;
        default: put<float>(host, lane, vd, at, sum); break;
      }
    }
  }
  static float element(const Host &host, const Insn &insn, uint32_t lane, uint32_t i) {
    const uint32_t reg = vcix_accel::rs2(insn);
    switch (element_format(insn)) {
      case Element::INTEGER: return static_cast<float>(get<int8_t>(host, lane, reg, i));
      case Element::E4M3: return fp8_to_float(get<uint8_t>(host, lane, reg, i), E4M3);
      case Element::E5M2: return fp8_to_float(get<uint8_t>(host, lane, reg, i), E5M2);
      case Element::HALF: return half_to_float(get<uint16_t>(host, lane, reg, i));
      case Element::BF16: return bf16_to_float(get<uint16_t>(host, lane, reg, i));
      case Element::SINGLE: return get<float>(host, lane, reg, i);
      case Element::ZERO: return 0.0f;
    }
    return 0.0f;
  }

  static constexpr uint32_t CSR_FRM = 0x002;
  static constexpr uint64_t MAX_LANES = 1u << 20;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_systolic_queue_entries";
  static constexpr const char *COMMANDS_KEY = "tpu_systolic_command_queue";
  static constexpr const char *IN_KEY = "tpu_systolic_in_rows_per_cycle";
  static constexpr const char *POP_KEY = "tpu_systolic_pop_rows_per_cycle";
  static constexpr const char *WAITS_KEY = "tpu_systolic_weight_waits";

  vcix_uarch::Port entry_, issue_, weight_push_, pop_, in_;
  vcix_uarch::Pipeline<Row> array_;
  vcix_uarch::CommandQueue commands_;
  vcix_uarch::Register<Weights> weights_;
  uint32_t stages_ = 1;
  bool weight_waits_ = false;
};

}  // namespace tpu

#endif
