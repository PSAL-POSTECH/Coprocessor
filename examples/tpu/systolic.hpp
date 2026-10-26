// The tpu model's systolic array: rows enter an input queue, cross 2 x lanes - 1 stages, the PEs of one anti-diagonal
// a stage, and wait in an output queue, a row a cycle in. Default keys: the golden timing (the head instruction moves
// whole), a row meeting the weights pushed before it; tpu_systolic_command_queue and the keys after it make it systolic.sv's.
#ifndef TPU_SYSTOLIC_HPP
#define TPU_SYSTOLIC_HPP

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <vector>

#include "machine.hpp"

#include "values.hpp"

namespace tpu {

// Its encodings are funct6, vm and funct3 of custom-2: sf.vc.iv 0, sf.vc.iv 1 and sf.vc.v.i 2.
class Systolic : public Component {
  using Cycle = cp::Cycle;
  using Host = cp::Host;
  using Insn = cp::Insn;
  // A lane's last `lanes` weights, oldest first, in one block.
  struct LaneWeights {
    std::vector<float> held;
    size_t first = 0;
    size_t size() const { return held.size() - first; }
    float operator[](size_t i) const { return held[first + i]; }
    void push(float weight, size_t keep) {
      if (size() == keep) first++;
      held.push_back(weight);
      if (first >= keep) held.erase(held.begin(), held.begin() + static_cast<long>(first)), first = 0;
    }
  };
  using Weights = std::vector<LaneWeights>;
  // A row (Spike; empty on gem5): element i of every lane, each output lane's sum, with no command queue the weights
  // its PEs hold for it (those pushed before it), and the array advance it entered at.
  struct Row {
    std::vector<float> input, sums;
    std::shared_ptr<const Weights> weights;
    uint64_t entered = 0;
  };
  // Weights the PEs hold from array advance `from` on.
  struct Landed {
    uint64_t from;
    std::shared_ptr<const Weights> weights;
  };

 public:
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr cp::Encoding INPUT_PUSH{0x2200305B, FORM, "systolic input push"};
  static constexpr cp::Encoding WEIGHT_PUSH{0x2600305B, FORM, "systolic weight push"};
  static constexpr cp::Encoding POP{0x0800305B, FORM, "systolic pop"};

  // `name` is the component as its statistics report it; the model that holds the component says it.
  Systolic(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    register_insn(INPUT_PUSH);
    register_insn(WEIGHT_PUSH);
    register_insn(POP);
  }
  void configure(const cp::Config &config) override {
    const uint64_t lanes = config.uint(LANES_KEY, 128), entries = config.uint(QUEUE_KEY, 256);
    if (lanes < 1)
      throw cp::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    const uint64_t commands = config.uint(COMMANDS_KEY, 0);
    weight_waits_ = config.uint(WAITS_KEY, 0) != 0;
    if (!commands && (config.get(IN_KEY) || config.get(POP_KEY) || weight_waits_))
      throw cp::ConfigError(COMMANDS_KEY, "0", "is no queue: what moves at issue moves whole, and waits for nothing");
    whole_ = !commands;
    stages_ = static_cast<uint32_t>(2 * lanes - 1);
    commands_.configure(whole_ ? 1 : commands);
    input_.configure(entries);
    output_.configure(entries);
    array_.configure(stages_, 1);
    in_.set_width(config.uint(IN_KEY, cp::UNLIMITED));
    pop_.set_width(config.uint(POP_KEY, entries));
  }
  void reset() override {
    landed_.clear();
    advances_ = 0;
    landing_.clear();
    weighted_ = false;
    done_ = 0;
  }
  // Behind a command queue, one pending instruction at a time with a command slot free, as rtl_model.hpp's can_accept.
  bool valid(const Insn &) const override {
    return whole_ || (!insns().size() && commands_.size() < commands_.capacity());
  }
  // Weights taken last cycle reach the PEs; the head instruction works (a wrong one faults as it reaches the head),
  // the pending one enters the command queue; then systolic.sv's array: while the output queue had room as the cycle
  // began, a finished row leaves (summed) and a row enters; the array advances unless its finished head waits.
  void cycle() override {
    values([&](const Host &) { land(); });
    const bool enable = !output_.full();
    if (whole_) {
      if (insns().length() && wrong(insns().front())) insns().pop();
      else if (insns().length() && work(insns().front())) weigh(insns().pop());
    } else {
      if (commands_.length() && work(commands_.front())) commands_.pop();
      if (insns().length() && !commands_.full()) {
        const Insn in = insns().pop();
        if (!wrong(in)) enter(in);
      }
    }
    if (enable && array_.length()) {
      Row row = array_.pop();
      values([&](const Host &) { sum(row); });
      if (!output_.push(std::move(row))) std::abort();
    }
    if (enable && input_.length()) {
      input_.front().entered = advances_;
      if (array_.push(std::move(input_.front()))) {
        input_.pop();
        rows_.add(1);
      }
    }
    rows_in_flight_.add(array_.size());
    values([&](const Host &) { advances_ += array_.size() && !array_.length(); });
  }
  void trace_queues(const vcix_insn &) const override {
    std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(input_.size()), static_cast<uint32_t>(output_.size()));
  }

 private:
  // What an element is rides the push or the pop in its rs1 field, one table at every width: 0 the width's own (an
  // integer of 8 bits, a half, a single; 64 bits are zeros), 1 an E4M3, 2 an E5M2, 3 a bfloat16; else NONE.
  enum class Element { INTEGER, E4M3, E5M2, HALF, BF16, SINGLE, ZERO, NONE };
  static Element element_format(const vcix_insn &insn) {
    const uint32_t code = cp::rs1(insn), sew = insn.sew_bits;
    if (code == 0) return sew == 8 ? Element::INTEGER : sew == 16 ? Element::HALF : sew == 32 ? Element::SINGLE : Element::ZERO;
    if (sew == 8 && (code == 1 || code == 2)) return code == 1 ? Element::E4M3 : Element::E5M2;
    if (sew == 16 && code == 3) return Element::BF16;
    return Element::NONE;
  }
  static bool is(const cp::Encoding &e, const vcix_insn &insn) { return (insn.bits & e.mask) == e.match; }
  // A wrong instruction as it reaches the head, faulted with the first rule it breaks: a format its width does not
  // name, an input push before any weight, a pop to bfloat16. True: it leaves without working.
  bool wrong(const Insn &insn) {
    const Element format = element_format(insn);
    if ((is(POP, insn) || insn.vl) && format == Element::NONE)
      return fault(insn, message("systolic: format %u names no element of %u bits", cp::rs1(insn), insn.sew_bits)), true;
    if (is(INPUT_PUSH, insn) && !weighted_) return fault(insn, message("systolic input push: no weight was pushed before it")), true;
    if (is(POP, insn) && format == Element::BF16) return fault(insn, message("systolic pop: a pop to bfloat16 is not modeled")), true;
    return false;
  }
  // An instruction leaving insns(): a weight push of a beat or more lets input pushes in.
  void weigh(const Insn &insn) { weighted_ = weighted_ || (is(WEIGHT_PUSH, insn) && insn.vl); }
  // Into the command queue as systolic.sv's cmd port takes it; with vl 0 it is ready at once and never enters.
  void enter(const Insn &insn) {
    weigh(insn);
    if (!insn.vl) complete(insn);
    else if (!commands_.push(insn)) std::abort();
  }
  // One cycle of the head: as many rows or beats as its port, the queues and the array allow (all vl or none when
  // whole); true and complete() once it has moved vl. A weight push under tpu_systolic_weight_waits waits for an empty
  // array. A pop asking more rows than the unit holds anywhere (nothing can come behind it) is a fault.
  bool work(const Insn &insn) {
    const uint64_t left = insn.vl - done_;
    uint64_t n = 0;
    if (is(INPUT_PUSH, insn)) {
      n = std::min({left, in_.available(), input_.available()});
      if ((whole_ && n < left) || !in_.utilize(n)) return false;
      for (uint64_t k = 0; k < n; k++) {
        Row row;
        values([&](const Host &host) { row = pushed(host, insn, done_ + k); });
        if (!input_.push(std::move(row))) std::abort();
      }
    } else if (is(WEIGHT_PUSH, insn)) {
      if (weight_waits_ && (input_.size() || !array_.idle())) return false;
      n = std::min(left, in_.available());
      if (!in_.utilize(n)) return false;
      values([&](const Host &host) {
        for (uint64_t k = 0; k < n; k++) landing_.push_back(beat(host, insn, done_ + k));
      });
    } else {
      n = std::min({left, pop_.available(), output_.length()});
      if (n < left && !input_.size() && array_.idle() && output_.size() < left) {
        fault(insn, message("systolic pop: %u elements asked, %llu computed", insn.vl, static_cast<unsigned long long>(done_ + output_.size())));
        done_ = 0;
        return true;
      }
      if ((whole_ && n < left) || !pop_.utilize(n)) return false;
      for (uint64_t k = 0; k < n; k++) {
        const Row row = output_.pop();
        values([&](const Host &host) { popped(host, insn, done_ + k, row); });
      }
    }
    done_ += n;
    if (done_ < insn.vl) return false;
    done_ = 0;
    complete(insn);
    return true;
  }
  // Stages k = 0.. of a leaving row: PE (lane, k - lane) adds input k - lane times the weight it held at stage k, the
  // array's advance entered + k, to lane's sum; the last stage also does the anti-diagonals of a hart of more lanes.
  void sum(Row &row) const {
    const uint32_t lanes = static_cast<uint32_t>(row.sums.size());
    for (uint32_t k = 0; k < stages_; k++) {
      const Weights &weights = row.weights ? *row.weights : met(row.entered + k);
      const uint32_t last = k + 1 == stages_ && 2 * lanes > stages_ + 1 ? 2 * lanes - 2 : k;
      for (uint32_t d = k; d <= last; d++)
        for (uint32_t lane = 0; lane < lanes && lane <= d; lane++) {
          const uint32_t at = d - lane;
          if (at < weights[lane].size()) row.sums[lane] = multiply_add(row.sums[lane], row.input[at], weights[lane][at]);
        }
    }
  }
  // The weights the PEs hold at array advance n: the last landed by then.
  const Weights &met(uint64_t n) const {
    static const Weights none;
    for (auto v = landed_.rbegin(); v != landed_.rend(); ++v)
      if (v->from <= n) return *v->weights;
    return none;
  }
  // Row i of an input push: element i of every lane, and sums of 0.
  Row pushed(const Host &host, const Insn &insn, uint64_t i) const {
    const uint32_t lanes = host.lanes();
    Row row{std::vector<float>(lanes), std::vector<float>(lanes, 0.0f), whole_ && landed_.size() ? landed_.back().weights : nullptr};
    for (uint32_t lane = 0; lane < lanes; lane++) row.input[lane] = element(host, insn, lane, static_cast<uint32_t>(i));
    return row;
  }
  // Beat i of a weight push, element i of every lane; it shifts into the lanes' weights in the next cycle, after the
  // array's advance in this one, as systolic.sv's weight registers do at the clock edge.
  static std::vector<float> beat(const Host &host, const Insn &insn, uint64_t i) {
    std::vector<float> weights(host.lanes());
    for (uint32_t lane = 0; lane < host.lanes(); lane++) weights[lane] = element(host, insn, lane, static_cast<uint32_t>(i));
    return weights;
  }
  // The beats taken last cycle into a lane's last `lanes` weights, held from this cycle's advance on; weights no row
  // in the array can still meet are dropped.
  void land() {
    if (landing_.empty()) return;
    const std::shared_ptr<Weights> weights = std::make_shared<Weights>(met(advances_));
    for (const std::vector<float> &b : landing_) {
      if (weights->empty()) weights->assign(b.size(), {});
      for (size_t lane = 0; lane < b.size(); lane++) (*weights)[lane].push(b[lane], b.size());
    }
    landing_.clear();
    if (landed_.size() && landed_.back().from == advances_) landed_.pop_back();
    landed_.push_back({advances_, weights});
    while (landed_.size() > 1 && landed_[1].from + stages_ <= advances_) landed_.pop_front();
  }
  // Row i a pop takes: each lane's sum to element i of vd.
  static void popped(const Host &host, const Insn &insn, uint64_t i, const Row &row) {
    const uint64_t mode = host.csr(CSR_FRM);
    const Element format = element_format(insn);
    const uint32_t vd = cp::rd(insn), at = static_cast<uint32_t>(i);
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
    const uint32_t reg = cp::rs2(insn);
    switch (element_format(insn)) {
      case Element::INTEGER: return static_cast<float>(get<int8_t>(host, lane, reg, i));
      case Element::E4M3: return fp8_to_float(get<uint8_t>(host, lane, reg, i), E4M3);
      case Element::E5M2: return fp8_to_float(get<uint8_t>(host, lane, reg, i), E5M2);
      case Element::HALF: return half_to_float(get<uint16_t>(host, lane, reg, i));
      case Element::BF16: return bf16_to_float(get<uint16_t>(host, lane, reg, i));
      case Element::SINGLE: return get<float>(host, lane, reg, i);
      case Element::ZERO:
      case Element::NONE: return 0.0f;
    }
    return 0.0f;
  }

  static constexpr uint32_t CSR_FRM = 0x002;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_systolic_queue_entries";
  static constexpr const char *COMMANDS_KEY = "tpu_systolic_command_queue";
  static constexpr const char *IN_KEY = "tpu_systolic_in_rows_per_cycle";
  static constexpr const char *POP_KEY = "tpu_systolic_pop_rows_per_cycle";
  static constexpr const char *WAITS_KEY = "tpu_systolic_weight_waits";

  cp::Counter rows_{*this, "rows", 1};
  cp::Counter rows_in_flight_{*this, "rows_in_flight"};
  cp::Port in_{*this, "in", cp::UNLIMITED};
  cp::Port pop_{*this, "pop", 256};
  cp::Queue<Insn> commands_{*this, "commands"};
  cp::Queue<Row> input_{*this, "input_queue"};
  cp::Pipeline<Row> array_{*this, "array"};
  cp::Queue<Row> output_{*this, "output_queue"};
  std::deque<Landed> landed_;
  std::vector<std::vector<float>> landing_;
  uint32_t stages_ = 1;
  uint64_t done_ = 0, advances_ = 0;
  bool whole_ = true, weight_waits_ = false, weighted_ = false;
};

}

#endif
