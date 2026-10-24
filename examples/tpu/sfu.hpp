// A TPU's special-function unit: one instruction a cycle enters, done tpu_sfu_latency_cycles later, when it writes
// vd = f(vs2) element by element in every lane. sfu_push and sfu_pop are the v6e's EUP: a push sends its registers, one
// a cycle, into a pipeline of that latency and is done; a pop takes them back in push order once they are out.
#ifndef TPU_SFU_HPP
#define TPU_SFU_HPP

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "vcix_uarch.hpp"

#include "values.hpp"

namespace tpu {

class Sfu {
  using Host = vcix_uarch::Host;
  using Route = vcix_uarch::Route;
  using Register = std::vector<std::vector<float>>;

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Sfu(const char *name)
      : entry_{name, "entry", "instructions", 1, vcix_uarch::PRIMARY}, eup_in_{name, "eup", "registers", 1} {}
  void configure(const vcix_uarch::Config &config) {
    latency_ = config.uint(LATENCY_KEY, 10);
    if (latency_ == 0) throw cp::ConfigError(LATENCY_KEY, "0", "is not a pipeline depth: at least 1");
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    if (entries < 1 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    eup_.configure(latency_, entries, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_), h.port(eup_in_);
    h.pipeline(eup_, eup_in_);
  }

  // sfu_push: the source's registers into the EUP, one a cycle, each with 2^x of its elements per lane; done when sent.
  void push(Route &r) {
    const uint32_t sew = r.insn().sew_bits;
    r.require(sew == 32, [sew] { fail("sfu_push: an element of %u bits is not supported", sew); });
    r.use(entry_).send(eup_, registers(r), [&r](const Host &host, uint64_t j) {
      const uint32_t per = host.vlen_bits() / 32, from = static_cast<uint32_t>(j) * per, to = std::min(r.vl(), from + per);
      Register out(host.lanes());
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = from; i < to; i++) out[lane].push_back(::exp2f(get<float>(host, lane, cp::rs2(r.insn()), i)));
      return out;
    });
  }
  // sfu_pop: as many registers as vd's group, once they are out, each to its register of vd up to vl; one of more
  // than were pushed never ends.
  void pop(Route &r) {
    const uint32_t sew = r.insn().sew_bits;
    r.require(sew == 32, [sew] { fail("sfu_pop: an element of %u bits is not supported", sew); });
    r.use(entry_).receive(eup_, registers(r), [&r](const Host &host, uint64_t j, Register &&in) {
      const uint32_t per = host.vlen_bits() / 32, from = static_cast<uint32_t>(j) * per;
      for (uint32_t lane = 0; lane < host.lanes() && lane < in.size(); lane++)
        for (uint32_t k = 0; k < in[lane].size() && from + k < r.vl(); k++)
          put<float>(host, lane, cp::rd(r.insn()), from + k, in[lane][k]);
    });
    r.when_stuck([](uint64_t) { fail("sfu_pop: lane %u has no pushed result left", 0u); });
  }

  // Function arg() of the seven, on elements of 16, 32 or (vlog, vatan) 64 bits; others are never accepted.
  void special_function(Route &r) {
    const uint32_t f = r.arg(), sew = r.insn().sew_bits;
    const char *name = INSTRUCTIONS[f].name;
    r.require(!r.vl() || sew == 16 || sew == 32 || (sew == 64 && WIDE[f]),
              [name, sew] { fail("%s: an element of %u bits is not supported", name, sew); });
    r.use(entry_).done_after(latency_);
    r.when_done([&r](const Host &host) {
      const uint32_t f = r.arg(), vs = cp::rs2(r.insn()), vd = cp::rd(r.insn()), sew = r.insn().sew_bits;
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < r.vl(); i++)
          if (sew == 16) put<uint16_t>(host, lane, vd, i, double_to_half(SINGLE[f](half_to_float(get<uint16_t>(host, lane, vs, i)))));
          else if (sew == 32) put<float>(host, lane, vd, i, SINGLE[f](get<float>(host, lane, vs, i)));
          else put<double>(host, lane, vd, i, WIDE[f](get<double>(host, lane, vs, i)));
    });
  }

  // What the [tpu] trace adds to an issue: the EUP's queues for a push or a pop, nothing for a special function.
  void trace_queues(const vcix_insn &insn) const {
    const uint32_t form = insn.bits & FUNCTION;
    if (form == PUSH || form == POP)
      std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(eup_.input_rows()),
                  static_cast<uint32_t>(eup_.output_rows()));
  }

  // sf.vc.v.iv on custom-2: opcode, funct3, funct6 and vm; FUNCTION_FIELD adds the field at 19:15. sfu_push is
  // sf.vc.vv 3 (reads vs2, writes nothing), sfu_pop sf.vc.v.x 3 (writes vd; rs1 unread).
  static constexpr uint32_t FUNCTION = 0xFE00707F, FUNCTION_FIELD = 0xFE0FF07F;
  static constexpr uint32_t PUSH = 0x2E00005B, POP = 0x0C00405B;
  static constexpr vcix_uarch::Instruction<Sfu> INSTRUCTIONS[] = {
      {0x2000305B, FUNCTION, "verf", &Sfu::special_function, 0},
      {0x2400305B, FUNCTION, "vtanh", &Sfu::special_function, 1},
      {0x2800305B, FUNCTION_FIELD, "vsin", &Sfu::special_function, 2},
      {0x2800B05B, FUNCTION_FIELD, "vcos", &Sfu::special_function, 3},
      {0x2801305B, FUNCTION_FIELD, "vlog", &Sfu::special_function, 4},
      {0x2801B05B, FUNCTION_FIELD, "vatan", &Sfu::special_function, 5},
      {0x2C00305B, FUNCTION, "vexp", &Sfu::special_function, 6},
      {PUSH, FUNCTION, "sfu_push", &Sfu::push},
      {POP, FUNCTION, "sfu_pop", &Sfu::pop},
  };

 private:
  static inline float (*const SINGLE[])(float) = {::erff, ::tanhf, ::sinf, ::cosf, ::logf, ::atanf, ::expf};
  static inline double (*const WIDE[])(double) = {nullptr, nullptr, nullptr, nullptr, ::log, ::atan, nullptr};
  static constexpr const char *LATENCY_KEY = "tpu_sfu_latency_cycles";
  static constexpr uint64_t DEFAULT_ENTRIES = 16;
  static constexpr const char *QUEUE_KEY = "tpu_sfu_queue_registers";
  // The registers of the instruction's group: the EUP takes one a cycle.
  static uint64_t registers(Route &r) { return cp::group_size(r.insn()); }

  vcix_uarch::Port entry_, eup_in_;
  vcix_uarch::Pipeline<Register> eup_;
  vcix_uarch::Cycle latency_ = 10;
};

}  // namespace tpu

#endif
