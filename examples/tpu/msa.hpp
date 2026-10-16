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
    if (lanes < 1 || lanes > MAX_LANES)
      throw vcix_accel::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    array_.configure(2 * lanes - 1, entries, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
    pop_.set_capacity(entries);
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_), h.port(issue_), h.port(weight_push_), h.port(pop_);
    h.pipeline(array_, entry_);
    h.reg(weights_);
  }

  // The field at 19:15: [4] weights or inputs, [3:2] how many columns compute (all shifted right by it), [1:0] what a
  // word holds. A weight push takes its own slot and changing the format or the width empties the matrix; an input
  // push sends vl rows, element i of every lane one input row, and a column past the width computes 0.
  void push(Route &r) {
    const bool weights = vcix_accel::rs1(r.insn()) & WEIGHT;
    r.use(issue_);
    if (weights) r.use(weight_push_);
    else r.send(array_, r.vl());
    r.compute([&](const Host &host) {
      if (r.insn().sew_bits != 32) fail("msa push: its elements are words of 32 bits, not of %u", r.insn().sew_bits);
      const uint32_t lanes = host.lanes(), field = vcix_accel::rs1(r.insn()), vs = vcix_accel::rs2(r.insn());
      const uint32_t format = field & 3, pack = elements(format);
      if (weights) {
        const uint32_t shift = (field >> 2) & 3;
        if (lanes >> shift == 0) fail("msa push: no column is left of %u when shifted by %u", lanes, shift);
        Weights &w = weights_.write();
        if (w.columns.size() != lanes) w.columns.assign(lanes, {});
        if (format != w.format || shift != w.shift)
          for (std::deque<float> &column : w.columns) column.clear();
        w.format = format, w.shift = shift;
        for (uint32_t lane = 0; lane < lanes; lane++)
          for (uint32_t i = 0; i < r.vl(); i++)
            for (uint32_t slot = 0; slot < pack; slot++) {
              if (w.columns[lane].size() == lanes * pack) w.columns[lane].pop_front();
              w.columns[lane].push_back(element(get<uint32_t>(host, lane, vs, i), slot, format));
            }
        return;
      }
      const Weights &w = weights_.get();
      if (pack != elements(w.format)) fail("msa push: inputs of %u to a word, weights of %u", pack, elements(w.format));
      Row in(lanes * pack);
      for (uint32_t i = 0; i < r.vl(); i++) {
        for (uint32_t lane = 0; lane < lanes; lane++)
          for (uint32_t slot = 0; slot < pack; slot++) in[lane * pack + slot] = element(get<uint32_t>(host, lane, vs, i), slot, format);
        Row row(lanes);
        for (uint32_t lane = 0; lane < lanes && lane < w.columns.size(); lane++) {
          if (lane >= lanes >> w.shift) continue;
          float output = 0;
          uint32_t k = 0;
          for (float weight : w.columns[lane]) output = multiply_add(output, in[k++], weight);
          row[lane] = output;
        }
        array_.put(std::move(row));
      }
    });
  }
  // Up to vl rows of the output queue, once vl are out or none is on its way: what is there and no more. To fewer
  // bits it rounds to nearest even; 8 bits are E5M2 when the field's [1:0] says so and E4M3 otherwise.
  void pop(Route &r) {
    r.use(issue_).receive_at_most(array_, r.vl()).through(pop_);
    r.compute([&](const Host &host) {
      const uint32_t vd = vcix_accel::rd(r.insn());
      const Narrow &byte = (vcix_accel::rs1(r.insn()) & 3) == E5M2_FORMAT ? E5M2 : E4M3;
      const std::vector<Row> rows = array_.take(r.vl());
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < rows.size(); i++) switch (r.insn().sew_bits) {
            case 8: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(narrow(rows[i][lane], byte, 0))); break;
            case 16: put<uint16_t>(host, lane, vd, i, static_cast<uint16_t>(narrow(rows[i][lane], HALF, 0))); break;
            default: put<float>(host, lane, vd, i, rows[i][lane]); break;
          }
    });
  }

  // funct6, vm and funct3 of custom-2: sf.vc.iv 2 and sf.vc.v.i 3.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr vcix_uarch::Instruction<Msa> INSTRUCTIONS[] = {{0x2A00305B, FORM, "msa push", &Msa::push},
                                                                  {0x0C00305B, FORM, "msa pop", &Msa::pop}};

 private:
  // Per lane its column of the matrix, and the format and the width the last weight push said.
  struct Weights {
    std::vector<std::deque<float>> columns;
    uint32_t format = 0, shift = 0;
  };
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
  static constexpr uint64_t MAX_LANES = 1u << 20;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_systolic_queue_entries";

  vcix_uarch::Port entry_, issue_, weight_push_, pop_;
  vcix_uarch::Pipeline<Row> array_;
  vcix_uarch::Register<Weights> weights_;
};

}  // namespace tpu

#endif
