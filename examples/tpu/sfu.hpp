// A TPU's special-function unit around one EUP, a pipeline of tpu_sfu_latency_cycles taking one register a cycle. A special
// function sends its group's registers through it and writes vd = f(vs2) when the last leaves; sfu_push queues its
// registers for it, and sfu_pop takes them back in push order once they are out. Queued push registers enter first.
#ifndef TPU_SFU_HPP
#define TPU_SFU_HPP

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "machine.hpp"

#include "values.hpp"

namespace tpu {

class Sfu : public Component {
  using Cycle = cp::Cycle;
  using Host = cp::Host;
  using Insn = cp::Insn;
  using Register = std::vector<std::vector<float>>;
  // One register in the EUP: of a special function (j of its group), or one sfu_push queued.
  struct Row {
    Insn insn;
    uint32_t j = 0;
    bool special = false;
    Register pushed;
  };

 public:
  // sf.vc.v.iv on custom-2: opcode, funct3, funct6 and vm; FUNCTION_FIELD adds the field at 19:15. sfu_push is
  // sf.vc.vv 3 (reads vs2, writes nothing), sfu_pop sf.vc.v.x 3 (writes vd; rs1 unread).
  static constexpr uint32_t FUNCTION = 0xFE00707F, FUNCTION_FIELD = 0xFE0FF07F;
  static constexpr cp::Encoding VERF{0x2000305B, FUNCTION, "verf"};
  static constexpr cp::Encoding VTANH{0x2400305B, FUNCTION, "vtanh"};
  static constexpr cp::Encoding VSIN{0x2800305B, FUNCTION_FIELD, "vsin"};
  static constexpr cp::Encoding VCOS{0x2800B05B, FUNCTION_FIELD, "vcos"};
  static constexpr cp::Encoding VLOG{0x2801305B, FUNCTION_FIELD, "vlog"};
  static constexpr cp::Encoding VATAN{0x2801B05B, FUNCTION_FIELD, "vatan"};
  static constexpr cp::Encoding VEXP{0x2C00305B, FUNCTION, "vexp"};
  static constexpr cp::Encoding SFU_PUSH{0x2E00005B, FUNCTION, "sfu_push"};
  static constexpr cp::Encoding SFU_POP{0x0C00405B, FUNCTION, "sfu_pop"};

  // `name` is the component as its statistics report it; the model that holds the component says it.
  Sfu(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    for (const cp::Encoding *e : ALL) register_insn(*e);
  }
  void configure(const cp::Config &config) override {
    const uint64_t latency = config.uint(LATENCY_KEY, 10);
    if (latency == 0) throw cp::ConfigError(LATENCY_KEY, "0", "is not a pipeline depth: at least 1");
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    if (entries < 1 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    eup_.configure(latency, 1);
    eup_input_.configure(entries);
    eup_output_.configure(entries);
  }
  void reset() override { moved_ = 0; }
  // The EUP's finished register leaves (a special function's last completes it); a queued push register enters, else
  // the head's next register; then the head instruction.
  void cycle() override {
    if (eup_.length()) {
      const Row &out = eup_.front();
      if (out.special) {
        const Row row = eup_.pop();
        if (row.j + 1 == cp::group_size(row.insn)) {
          values([&](const Host &host) { special_function(host, row.insn); });
          complete(row.insn);
        }
      } else if (eup_output_.push(std::move(eup_.front().pushed))) {
        eup_.pop();
      }
    }
    if (eup_input_.length() && eup_.push(Row{Insn{}, 0, false, eup_input_.front()})) eup_input_.pop();
    if (insns().length() && head(insns().front())) {
      insns().pop();
      instructions_.add(1);
      moved_ = 0;
    }
  }
  // What the [tpu] trace adds to an issue: the EUP's queues for a push or a pop, nothing for a special function.
  void trace_queues(const vcix_insn &insn) const override {
    if (form(insn) == SFU_PUSH.match || form(insn) == SFU_POP.match)
      std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(eup_input_.size()),
                  static_cast<uint32_t>(eup_output_.size()));
  }

 private:
  // One cycle of the head: a special function's next register enters the EUP; a push queues, a pop takes, what fits,
  // and both complete with their last register. An element width it does not compute, or a pop with nothing left
  // anywhere to take, is a fault. True: the head leaves insns().
  bool head(const Insn &in) {
    const uint32_t registers = cp::group_size(in);
    if (!supported(in)) {
      fault(in, message("%s: an element of %u bits is not supported", ALL[index(in)]->name, in.sew_bits));
      return true;
    }
    if (form(in) == SFU_PUSH.match) {
      for (; moved_ < registers; moved_++) {
        Register row;
        values([&](const Host &host) { row = pushed(host, in, moved_); });
        if (!eup_input_.push(std::move(row))) break;
      }
      if (moved_ == registers) complete(in);
      return moved_ == registers;
    }
    if (form(in) == SFU_POP.match) {
      for (; moved_ < registers && eup_output_.length(); moved_++) {
        const Register row = eup_output_.pop();
        values([&](const Host &host) { popped(host, in, moved_, row); });
      }
      if (moved_ == registers) complete(in);
      else if (!eup_input_.size() && eup_.idle() && !eup_output_.size()) {
        fault(in, message("sfu_pop: lane %u has no pushed result left", 0u));
        return true;
      }
      return moved_ == registers;
    }
    if (!eup_.push(Row{in, moved_, true, {}})) return false;
    return ++moved_ == registers;
  }
  // sfu_push's register j of vs2's group: 2^x of its elements per lane.
  static Register pushed(const Host &host, const Insn &in, uint32_t j) {
    const uint32_t per = host.vlen_bits() / 32, from = j * per, to = std::min(in.vl, from + per);
    Register out(host.lanes());
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = from; i < to; i++) out[lane].push_back(::exp2f(get<float>(host, lane, cp::rs2(in), i)));
    return out;
  }
  // sfu_pop's register j of vd's group, up to vl.
  static void popped(const Host &host, const Insn &in, uint32_t j, const Register &got) {
    const uint32_t per = host.vlen_bits() / 32, from = j * per;
    for (uint32_t lane = 0; lane < host.lanes() && lane < got.size(); lane++)
      for (uint32_t k = 0; k < got[lane].size() && from + k < in.vl; k++)
        put<float>(host, lane, cp::rd(in), from + k, got[lane][k]);
  }
  // Function f of the seven, element by element, on elements of 16, 32 or (vlog, vatan) 64 bits.
  static void special_function(const Host &host, const Insn &in) {
    const uint32_t f = index(in), vs = cp::rs2(in), vd = cp::rd(in);
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < in.vl; i++)
        if (in.sew_bits == 16) put<uint16_t>(host, lane, vd, i, double_to_half(SINGLE[f](half_to_float(get<uint16_t>(host, lane, vs, i)))));
        else if (in.sew_bits == 32) put<float>(host, lane, vd, i, SINGLE[f](get<float>(host, lane, vs, i)));
        else put<double>(host, lane, vd, i, WIDE[f](get<double>(host, lane, vs, i)));
  }
  // The element widths each instruction computes: push and pop 32 bits; a special function 16, 32, or 64 if WIDE.
  static bool supported(const vcix_insn &in) {
    if (form(in) == SFU_PUSH.match || form(in) == SFU_POP.match) return in.sew_bits == 32;
    const uint32_t sew = in.sew_bits;
    return !in.vl || sew == 16 || sew == 32 || (sew == 64 && WIDE[index(in)]);
  }
  static uint32_t form(const vcix_insn &in) { return in.bits & FUNCTION; }
  // Its place in ALL, which for a special function is its place in SINGLE and WIDE.
  static uint32_t index(const vcix_insn &in) {
    uint32_t i = 0;
    while ((in.bits & ALL[i]->mask) != ALL[i]->match) i++;
    return i;
  }

  static inline const cp::Encoding *const ALL[] = {&VERF, &VTANH, &VSIN, &VCOS, &VLOG, &VATAN, &VEXP, &SFU_PUSH, &SFU_POP};
  static inline float (*const SINGLE[])(float) = {::erff, ::tanhf, ::sinf, ::cosf, ::logf, ::atanf, ::expf};
  static inline double (*const WIDE[])(double) = {nullptr, nullptr, nullptr, nullptr, ::log, ::atan, nullptr};
  static constexpr const char *LATENCY_KEY = "tpu_sfu_latency_cycles";
  static constexpr uint64_t DEFAULT_ENTRIES = 16;
  static constexpr const char *QUEUE_KEY = "tpu_sfu_queue_registers";

  cp::Counter instructions_{*this, "instructions", 1};
  cp::Queue<Register> eup_input_{*this, "eup_input"};
  cp::Pipeline<Row> eup_{*this, "eup"};
  cp::Queue<Register> eup_output_{*this, "eup_output"};
  uint32_t moved_ = 0;
};

}  // namespace tpu

#endif
