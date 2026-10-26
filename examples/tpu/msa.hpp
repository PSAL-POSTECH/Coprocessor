// The tpu model's multi-precision array: the systolic array's input queue -> array -> output queue, one push for
// weights and inputs, a row a cycle in whatever its active width. Its values (riscv-isa-sim branch spike-multi-precision-sa,
// ea9ec10): the systolic array on words of 32 bits that hold 1 single, 2 halves or 4 floats of 8 bits.
#ifndef TPU_MSA_HPP
#define TPU_MSA_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "machine.hpp"
#include "values.hpp"

namespace tpu {

class Msa : public Component {
  using Cycle = cp::Cycle;
  using Host = cp::Host;
  using Insn = cp::Insn;
  using Row = std::vector<float>;

 public:
  // funct6, vm and funct3 of custom-2: sf.vc.iv 2 and sf.vc.v.i 3.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr cp::Encoding PUSH{0x2A00305B, FORM, "msa push"};
  static constexpr cp::Encoding POP{0x0C00305B, FORM, "msa pop"};

  // `name` is the component as its statistics report it; the model that holds the component says it.
  Msa(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    register_insn(PUSH);
    register_insn(POP);
  }
  void configure(const cp::Config &config) override {
    const uint64_t lanes = config.uint(LANES_KEY, 128), entries = config.uint(QUEUE_KEY, 256);
    if (lanes < 1)
      throw cp::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    input_.configure(entries);
    array_.configure(2 * lanes - 1, 1);
    output_.configure(entries);
    pop_.set_width(entries);
    lanes_ = static_cast<uint32_t>(lanes);
  }
  void reset() override {
    weights_ = {};
    format_ = 0;
  }
  // The array hands its finished row on while the output queue has room and takes the input queue's next; then the
  // head of insns(), carried out whole (or faulted).
  void cycle() override {
    if (array_.length() && output_.push(std::move(array_.front()))) array_.pop();
    if (input_.length() && array_.push(std::move(input_.front()))) {
      rows_.add(1);
      input_.pop();
    }
    if (insns().length() && carry_out(insns().front())) insns().pop();
    rows_in_flight_.add(array_.size());
  }
  void trace_queues(const vcix_insn &) const override {
    std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(input_.size()), static_cast<uint32_t>(output_.size()));
  }

 private:
  // Per lane its column of the matrix, and the width the last weight push said (its format is format_).
  struct Weights {
    std::vector<std::deque<float>> columns;
    uint32_t shift = 0;
  };
  static bool is(const vcix_insn &insn, const cp::Encoding &e) { return (insn.bits & e.mask) == e.match; }
  // Why the instruction is never carried out, or "": a push's elements are words, a weight push leaves a column, an
  // input push packs a word as the weights do. The field at 19:15: [4] weights, [3:2] shift, [1:0] format.
  std::string refusal(const Insn &insn) const {
    if (is(insn, POP)) return "";
    const uint32_t field = cp::rs1(insn), sew = insn.sew_bits, shift = (field >> 2) & 3;
    const uint32_t pack = elements(field & 3), held = elements(format_);
    if (sew != 32) return message("msa push: its elements are words of 32 bits, not of %u", sew);
    if (field & WEIGHT)
      return lanes_ >> shift != 0 ? "" : message("msa push: no column is left of %u when shifted by %u", lanes_, shift);
    return pack == held ? "" : message("msa push: inputs of %u to a word, weights of %u", pack, held);
  }
  // The head of insns() in this cycle, if the rows or the room allow: a weight push writes the matrix
  // (changing the format or the width empties it); an input push puts vl rows in, a pop takes vl rows out. True once
  // it completed, or faulted: a wrong push, or a pop asking more rows than the idle array and its queues hold.
  bool carry_out(const Insn &insn) {
    const uint32_t vl = insn.vl, field = cp::rs1(insn), format = field & 3;
    if (const std::string why = refusal(insn); !why.empty()) return fault(insn, why), true;
    if (is(insn, POP)) {
      if (output_.size() < vl && !input_.size() && array_.idle())
        return fault(insn, message("msa pop: %u elements asked, %llu computed", vl, static_cast<unsigned long long>(output_.size()))), true;
      if (output_.length() < vl || !pop_.utilize(vl)) return false;
      for (uint32_t i = 0; i < vl; i++) {
        const Row row = output_.pop();
        values([&](const Host &host) { popped(host, insn, i, row); });
      }
    } else if (!(field & WEIGHT)) {
      if (input_.available() < vl) return false;
      for (uint32_t i = 0; i < vl; i++) {
        Row row;
        values([&](const Host &host) { row = input(host, insn, i); });
        if (!input_.push(std::move(row))) std::abort();
      }
    } else {
      values([&](const Host &host) { latch(host, insn); });
      format_ = format;
    }
    complete(insn);
    return true;
  }
  // A weight push: vl rows, element i of every lane, each slot of a word shifting into its lane's column.
  void latch(const Host &host, const Insn &insn) {
    const uint32_t lanes = host.lanes(), vs = cp::rs2(insn), field = cp::rs1(insn);
    const uint32_t format = field & 3, shift = (field >> 2) & 3, pack = elements(format);
    if (weights_.columns.size() != lanes) weights_.columns.assign(lanes, {});
    if (format != format_ || shift != weights_.shift)
      for (std::deque<float> &column : weights_.columns) column.clear();
    weights_.shift = shift;
    for (uint32_t lane = 0; lane < lanes; lane++)
      for (uint32_t i = 0; i < insn.vl; i++)
        for (uint32_t slot = 0; slot < pack; slot++) {
          if (weights_.columns[lane].size() == lanes * pack) weights_.columns[lane].pop_front();
          weights_.columns[lane].push_back(element(get<uint32_t>(host, lane, vs, i), slot, format));
        }
  }
  // Input row i of a push, as it enters: per lane, its column of the matrix times element i of every lane; a column
  // past the width computes 0.
  Row input(const Host &host, const Insn &insn, uint32_t i) const {
    const uint32_t lanes = host.lanes(), vs = cp::rs2(insn), format = cp::rs1(insn) & 3;
    const uint32_t pack = elements(format);
    Row in(lanes * pack), out(lanes);
    for (uint32_t lane = 0; lane < lanes; lane++)
      for (uint32_t slot = 0; slot < pack; slot++) in[lane * pack + slot] = element(get<uint32_t>(host, lane, vs, i), slot, format);
    for (uint32_t lane = 0; lane < lanes && lane < weights_.columns.size(); lane++) {
      if (lane >= lanes >> weights_.shift) continue;
      float output = 0;
      uint32_t k = 0;
      for (float weight : weights_.columns[lane]) output = multiply_add(output, in[k++], weight);
      out[lane] = output;
    }
    return out;
  }
  // Row i of a pop to element i of vd, rounded to nearest even to fewer bits (8 bits E5M2 when the field's [1:0] says
  // so, else E4M3).
  static void popped(const Host &host, const Insn &insn, uint32_t i, const Row &row) {
    const uint32_t vd = cp::rd(insn);
    const Narrow &byte = (cp::rs1(insn) & 3) == E5M2_FORMAT ? E5M2 : E4M3;
    for (uint32_t lane = 0; lane < host.lanes(); lane++) switch (insn.sew_bits) {
        case 8: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(narrow(row[lane], byte, 0))); break;
        case 16: put<uint16_t>(host, lane, vd, i, static_cast<uint16_t>(narrow(row[lane], HALF, 0))); break;
        default: put<float>(host, lane, vd, i, row[lane]); break;
      }
  }
  // A word's elements: 1 single (0), 2 halves (1), 4 of E4M3 (2) or of E5M2 (3); element `slot` from its low bits up.
  static uint32_t elements(uint32_t format) { return format == 0 ? 1 : format == 1 ? 2 : 4; }
  static float element(uint32_t word, uint32_t slot, uint32_t format) {
    const uint32_t part = word >> (slot * (32 / elements(format)));
    if (format == 0) {
      float value;
      std::memcpy(&value, &word, sizeof value);
      return value;
    }
    if (format == 1) return half_to_float(static_cast<uint16_t>(part));
    return fp8_to_float(static_cast<uint8_t>(part), format == E5M2_FORMAT ? E5M2 : E4M3);
  }

  static constexpr uint32_t WEIGHT = 0x10, E5M2_FORMAT = 3;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_systolic_queue_entries";

  cp::Counter rows_{*this, "rows", 1};
  cp::Counter rows_in_flight_{*this, "rows_in_flight"};
  cp::Port pop_{*this, "pop", 256};
  cp::Queue<Row> input_{*this, "input_queue"};
  cp::Pipeline<Row> array_{*this, "array"};
  cp::Queue<Row> output_{*this, "output_queue"};
  Weights weights_;
  uint32_t format_ = 0;
  uint32_t lanes_ = 128;
};

}

#endif
