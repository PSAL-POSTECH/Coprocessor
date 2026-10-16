// The tpu model's systolic array: rows enter an input queue, cross 2 x lanes - 1 stages and wait in an output queue,
// a row a cycle in and an instruction a cycle into the unit. The default keys are the golden timing (every step at
// issue); tpu_systolic_command_queue and the three keys after it make it systolic.sv's (examples/rtl/systolic).
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
  using Row = std::vector<float>;

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
    array_.configure(2 * lanes - 1, entries, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
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
    h.pipeline(array_, entry_);
    h.queue(commands_);
    h.reg(weights_);
  }

  // vl rows into the array; each is the weights times element i of every lane, computed as it is pushed.
  void input_push(Route &r) {
    r.use(issue_).enqueue(commands_).send(array_, r.vl()).through(in_);
    r.compute([&](const Host &host) {
      const uint32_t lanes = host.lanes();
      std::vector<Row> input(lanes, Row(r.vl()));
      for (uint32_t lane = 0; lane < lanes; lane++)
        for (uint32_t i = 0; i < r.vl(); i++) input[lane][i] = element(host, r.insn(), lane, i);
      const std::vector<std::deque<float>> &weights = weights_.get();
      if (weights.empty() || weights[0].empty()) fail("systolic input push: no weight was pushed before it");
      for (uint32_t i = 0; i < r.vl(); i++) {
        Row row(lanes);
        for (uint32_t lane = 0; lane < lanes; lane++) {
          float output = 0;
          uint32_t k = 0;
          for (float weight : weights[lane]) output = multiply_add(output, input[k++][i], weight);
          row[lane] = output;
        }
        array_.put(std::move(row));
      }
    });
  }
  // vl beats of weights into the unit's registers; a row keeps its last `lanes`. Under tpu_systolic_weight_waits they
  // move only once the array is empty.
  void weight_push(Route &r) {
    r.use(issue_).use(weight_push_).enqueue(commands_);
    if (weight_waits_) r.wait_until_empty(array_);
    r.transfer(r.vl()).through(in_);
    r.compute([&](const Host &host) {
      const uint32_t lanes = host.lanes();
      std::vector<Row> pushed(lanes, Row(r.vl()));
      for (uint32_t lane = 0; lane < lanes; lane++)
        for (uint32_t i = 0; i < r.vl(); i++) pushed[lane][i] = element(host, r.insn(), lane, i);
      std::vector<std::deque<float>> &weights = weights_.write();
      if (weights.empty()) weights.assign(lanes, {});
      for (uint32_t lane = 0; lane < lanes; lane++)
        for (float weight : pushed[lane]) {
          if (weights[lane].size() == lanes) weights[lane].pop_front();
          weights[lane].push_back(weight);
        }
    });
  }
  // Up to vl rows of the output queue, once vl are out or none is on its way; written in rs1's format, rounded by frm.
  void pop(Route &r) {
    r.use(issue_).enqueue(commands_).receive_at_most(array_, r.vl()).through(pop_);
    r.compute([&](const Host &host) {
      const uint64_t mode = host.csr(CSR_FRM);
      const Element format = element_format(r.insn());
      if (format == Element::BF16) fail("systolic pop: a pop to bfloat16 is not modeled");
      if (array_.values_out() < r.vl())
        fail("systolic pop: %u elements asked, %llu computed", r.vl(), static_cast<unsigned long long>(array_.values_out()));
      const std::vector<Row> rows = array_.take(r.vl());
      const uint32_t vd = vcix_accel::rd(r.insn());
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < rows.size(); i++) switch (format) {
            case Element::INTEGER: put<int8_t>(host, lane, vd, i, static_cast<int8_t>(rows[i][lane])); break;
            case Element::E4M3: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(narrow(rows[i][lane], E4M3, mode))); break;
            case Element::E5M2: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(narrow(rows[i][lane], E5M2, mode))); break;
            case Element::HALF: put<uint16_t>(host, lane, vd, i, static_cast<uint16_t>(narrow(rows[i][lane], HALF, mode))); break;
            default: put<float>(host, lane, vd, i, rows[i][lane]); break;
          }
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
  vcix_uarch::Register<std::vector<std::deque<float>>> weights_;
  bool weight_waits_ = false;
};

}  // namespace tpu

#endif
