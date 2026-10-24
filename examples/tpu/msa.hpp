// The tpu model's multi-precision array: the systolic array's pipeline, one push for weights and inputs, a row a cycle
// in whatever its active width. Its values (riscv-isa-sim branch spike-multi-precision-sa, ea9ec10): the systolic array
// on words of 32 bits that hold 1 single, 2 halves or 4 floats of 8 bits, so a column has lanes x that many weights.
#ifndef TPU_MSA_HPP
#define TPU_MSA_HPP

#include <cstring>
#include <deque>
#include <vector>

#include "vcix_uarch.hpp"

#include "values.hpp"

namespace tpu {

class Msa {
  using Host = vcix_uarch::Host;
  using Route = vcix_uarch::Route;
  using Row = std::vector<float>;

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Msa(const char *name)
      : entry_{name, "input", "rows", 1, vcix_uarch::PRIMARY},
        issue_{name, "issue", "instructions", 1},
        weight_push_{name, "weight_push", "instructions", 1},
        pop_{name, "pop", "rows", 256} {}
  void configure(const vcix_uarch::Config &config) {
    const uint64_t lanes = config.uint(LANES_KEY, 128), entries = config.uint(QUEUE_KEY, 256);
    if (lanes < 1)
      throw cp::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    array_.configure(2 * lanes - 1, entries, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
    pop_.set_capacity(entries);
    lanes_ = static_cast<uint32_t>(lanes);
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_), h.port(issue_), h.port(weight_push_), h.port(pop_);
    h.pipeline(array_, entry_);
    h.reg(weights_), h.reg(format_);
  }

  // The field at 19:15: [4] weights or inputs, [3:2] how many columns compute (all shifted right by it), [1:0] what a
  // word holds. A weight push takes its own slot and writes the matrix as it issues, and changing the format or the
  // width empties it; an input push sends vl rows, element i of every lane one input row (row()).
  void push(Route &r) {
    const uint32_t field = cp::rs1(r.insn()), sew = r.insn().sew_bits, format = field & 3, shift = (field >> 2) & 3;
    const uint32_t pack = elements(format), held = elements(format_.get()), columns = lanes_;
    const bool weights = field & WEIGHT;
    r.require(sew == 32, [sew] { fail("msa push: its elements are words of 32 bits, not of %u", sew); });
    if (weights) r.require(columns >> shift != 0, [columns, shift] { fail("msa push: no column is left of %u when shifted by %u", columns, shift); });
    else r.require(pack == held, [pack, held] { fail("msa push: inputs of %u to a word, weights of %u", pack, held); });
    r.use(issue_);
    if (weights) r.use(weight_push_);
    else r.send(array_, r.vl(), [this, &r](const Host &host, uint64_t i) { return row(host, r.insn(), i); });
    if (!weights) return;
    r.at_issue([&](const Host &host) {
      const uint32_t lanes = host.lanes(), vs = cp::rs2(r.insn());
      Weights &w = weights_.write();
      if (w.columns.size() != lanes) w.columns.assign(lanes, {});
      if (format != format_.get() || shift != w.shift)
        for (std::deque<float> &column : w.columns) column.clear();
      w.shift = shift;
      for (uint32_t lane = 0; lane < lanes; lane++)
        for (uint32_t i = 0; i < r.vl(); i++)
          for (uint32_t slot = 0; slot < pack; slot++) {
            if (w.columns[lane].size() == lanes * pack) w.columns[lane].pop_front();
            w.columns[lane].push_back(element(get<uint32_t>(host, lane, vs, i), slot, format));
          }
    });
    if (format != format_.get()) r.at_issue([&] { format_.write() = format; });
  }
  // vl rows of the output queue, row i to element i of vd, rounded to nearest even to fewer bits (8 bits E5M2 when
  // the field's [1:0] says so, else E4M3); one that asks more than is computed never ends.
  void pop(Route &r) {
    const uint32_t vl = r.vl();
    r.use(issue_).receive(array_, vl, [&r](const Host &host, uint64_t i, Row &&row) {
      const uint32_t vd = cp::rd(r.insn()), at = static_cast<uint32_t>(i);
      const Narrow &byte = (cp::rs1(r.insn()) & 3) == E5M2_FORMAT ? E5M2 : E4M3;
      for (uint32_t lane = 0; lane < host.lanes(); lane++) switch (r.insn().sew_bits) {
          case 8: put<uint8_t>(host, lane, vd, at, static_cast<uint8_t>(narrow(row[lane], byte, 0))); break;
          case 16: put<uint16_t>(host, lane, vd, at, static_cast<uint16_t>(narrow(row[lane], HALF, 0))); break;
          default: put<float>(host, lane, vd, at, row[lane]); break;
        }
    }).through(pop_);
    r.when_stuck([vl](uint64_t got) {
      fail("msa pop: %u elements asked, %llu computed", vl, static_cast<unsigned long long>(got));
    });
  }

  // funct6, vm and funct3 of custom-2: sf.vc.iv 2 and sf.vc.v.i 3.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr vcix_uarch::Instruction<Msa> INSTRUCTIONS[] = {{0x2A00305B, FORM, "msa push", &Msa::push},
                                                                  {0x0C00305B, FORM, "msa pop", &Msa::pop}};

 private:
  // Per lane its column of the matrix, and the width the last weight push said (its format is unit state, format_).
  struct Weights {
    std::vector<std::deque<float>> columns;
    uint32_t shift = 0;
  };
  // Input row i of a push, as it enters: per lane, its column of the matrix times element i of every lane; a column
  // past the width computes 0.
  Row row(const Host &host, const vcix_uarch::Insn &insn, uint64_t i) const {
    const uint32_t lanes = host.lanes(), vs = cp::rs2(insn), format = cp::rs1(insn) & 3;
    const uint32_t pack = elements(format), at = static_cast<uint32_t>(i);
    const Weights &w = weights_.get();
    Row in(lanes * pack), out(lanes);
    for (uint32_t lane = 0; lane < lanes; lane++)
      for (uint32_t slot = 0; slot < pack; slot++) in[lane * pack + slot] = element(get<uint32_t>(host, lane, vs, at), slot, format);
    for (uint32_t lane = 0; lane < lanes && lane < w.columns.size(); lane++) {
      if (lane >= lanes >> w.shift) continue;
      float output = 0;
      uint32_t k = 0;
      for (float weight : w.columns[lane]) output = multiply_add(output, in[k++], weight);
      out[lane] = output;
    }
    return out;
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

  vcix_uarch::Port entry_, issue_, weight_push_, pop_;
  vcix_uarch::Pipeline<Row> array_;
  vcix_uarch::Register<Weights> weights_;
  vcix_uarch::Register<uint32_t> format_;
  uint32_t lanes_ = 128;
};

}  // namespace tpu

#endif
