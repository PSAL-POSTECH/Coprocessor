// A TPU's special-function unit: one instruction a cycle enters, done tpu_sfu_latency_cycles later, when it writes
// vd = f(vs2) element by element in every lane. sfu_push and sfu_pop are the v6e's EUP: a push sends its registers, one
// a cycle, into a pipeline of that latency and is done; a pop takes them back in push order once they are out.
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
  using Host = cp::Host;
  using Register = std::vector<std::vector<float>>;

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
    handle(VERF, &Sfu::special_function, 0);
    handle(VTANH, &Sfu::special_function, 1);
    handle(VSIN, &Sfu::special_function, 2);
    handle(VCOS, &Sfu::special_function, 3);
    handle(VLOG, &Sfu::special_function, 4);
    handle(VATAN, &Sfu::special_function, 5);
    handle(VEXP, &Sfu::special_function, 6);
    handle(SFU_PUSH, &Sfu::push);
    handle(SFU_POP, &Sfu::pop);
  }
  void configure(const cp::Config &config) override {
    latency_ = config.uint(LATENCY_KEY, 10);
    if (latency_ == 0) throw vcix_accel::ConfigError(LATENCY_KEY, "0", "is not a pipeline depth: at least 1");
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    eup_.configure(latency_, entries, cp::Stall::WHEN_OUTPUT_FULL);
  }

  // What the [tpu] trace adds to an issue: the EUP's queues for a push or a pop, nothing for a special function.
  void trace_queues(const cp::Insn &insn) const override {
    const uint32_t form = insn.bits & FUNCTION;
    if (form == SFU_PUSH.match || form == SFU_POP.match)
      std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(eup_.input_rows()),
                  static_cast<uint32_t>(eup_.output_rows()));
  }

 private:
  // sfu_push: the source's registers into the EUP, one a cycle, each with 2^x of its elements per lane; done when sent.
  void push(cp::Instr &in) {
    const uint32_t sew = in.sew_bits();
    in.require(sew == 32, [sew] { fail("sfu_push: an element of %u bits is not supported", sew); });
    in.use(entry_);
    in.send(eup_, registers(in), [&in](const Host &host, uint64_t j) {
      const uint32_t per = host.vlen_bits() / 32, from = static_cast<uint32_t>(j) * per, to = std::min(in.vl(), from + per);
      Register out(host.lanes());
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = from; i < to; i++) out[lane].push_back(::exp2f(get<float>(host, lane, in.rs2(), i)));
      return out;
    });
  }
  // sfu_pop: as many registers as vd's group, once they are out, each to its register of vd up to vl; one of more
  // than were pushed never ends.
  void pop(cp::Instr &in) {
    const uint32_t sew = in.sew_bits();
    in.require(sew == 32, [sew] { fail("sfu_pop: an element of %u bits is not supported", sew); });
    in.use(entry_);
    in.receive(eup_, registers(in), [&in](const Host &host, uint64_t j, Register &&got) {
      const uint32_t per = host.vlen_bits() / 32, from = static_cast<uint32_t>(j) * per;
      for (uint32_t lane = 0; lane < host.lanes() && lane < got.size(); lane++)
        for (uint32_t k = 0; k < got[lane].size() && from + k < in.vl(); k++)
          put<float>(host, lane, in.rd(), from + k, got[lane][k]);
    });
    in.on_stuck([](uint64_t) { fail("sfu_pop: lane %u has no pushed result left", 0u); });
  }
  // Function arg() of the seven, on elements of 16, 32 or (vlog, vatan) 64 bits; others are never accepted.
  void special_function(cp::Instr &in) {
    const uint32_t f = in.arg(), sew = in.sew_bits();
    const char *name = in.name();
    in.require(!in.vl() || sew == 16 || sew == 32 || (sew == 64 && WIDE[f]),
               [name, sew] { fail("%s: an element of %u bits is not supported", name, sew); });
    in.use(entry_);
    in.done_after(latency_);
    in.on_done([&in](const Host &host) {
      const uint32_t f = in.arg(), vs = in.rs2(), vd = in.rd(), sew = in.sew_bits();
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < in.vl(); i++)
          if (sew == 16) put<uint16_t>(host, lane, vd, i, double_to_half(SINGLE[f](half_to_float(get<uint16_t>(host, lane, vs, i)))));
          else if (sew == 32) put<float>(host, lane, vd, i, SINGLE[f](get<float>(host, lane, vs, i)));
          else put<double>(host, lane, vd, i, WIDE[f](get<double>(host, lane, vs, i)));
    });
  }

  static inline float (*const SINGLE[])(float) = {::erff, ::tanhf, ::sinf, ::cosf, ::logf, ::atanf, ::expf};
  static inline double (*const WIDE[])(double) = {nullptr, nullptr, nullptr, nullptr, ::log, ::atan, nullptr};
  static constexpr const char *LATENCY_KEY = "tpu_sfu_latency_cycles";
  static constexpr uint64_t DEFAULT_ENTRIES = 16;
  static constexpr const char *QUEUE_KEY = "tpu_sfu_queue_registers";
  // The registers of the instruction's group: the EUP takes one a cycle.
  static uint64_t registers(cp::Instr &in) { return vcix_accel::group_size(in.insn()); }

  cp::Port entry_{*this, "entry", "instructions", 1, cp::Port::PRIMARY};
  cp::Port eup_in_{*this, "eup", "registers", 1};
  cp::Pipeline<Register> eup_{*this, eup_in_};
  cp::Cycle latency_ = 10;
};

}

#endif
