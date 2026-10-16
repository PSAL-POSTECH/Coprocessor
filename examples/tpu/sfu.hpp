// A TPU's special-function unit: one instruction a cycle enters, done tpu_sfu_latency_cycles later; vd = f(vs2)
// element by element in every lane.
#ifndef TPU_SFU_HPP
#define TPU_SFU_HPP

#include <cmath>

#include "vcix_uarch.hpp"

#include "values.hpp"

namespace tpu {

class Sfu {
  using Host = vcix_uarch::Host;
  using Route = vcix_uarch::Route;

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Sfu(const char *name) : entry_{name, "entry", "instructions", 1, vcix_uarch::PRIMARY} {}
  void configure(const vcix_uarch::Config &config) {
    latency_ = config.uint(LATENCY_KEY, 10);
    if (latency_ == 0) throw vcix_accel::ConfigError(LATENCY_KEY, "0", "is not a pipeline depth: at least 1");
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_);
  }

  // Function arg() of the seven, on elements of 16, 32 or (vlog, vatan) 64 bits.
  void special_function(Route &r) {
    r.use(entry_).done_after(latency_);
    r.compute([&](const Host &host) {
      const uint32_t f = r.arg(), vs = vcix_accel::rs2(r.insn()), vd = vcix_accel::rd(r.insn()), sew = r.insn().sew_bits;
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < r.vl(); i++)
          if (sew == 16) put<uint16_t>(host, lane, vd, i, double_to_half(SINGLE[f](half_to_float(get<uint16_t>(host, lane, vs, i)))));
          else if (sew == 32) put<float>(host, lane, vd, i, SINGLE[f](get<float>(host, lane, vs, i)));
          else if (sew == 64 && WIDE[f]) put<double>(host, lane, vd, i, WIDE[f](get<double>(host, lane, vs, i)));
          else fail("%s: an element of %u bits is not supported", INSTRUCTIONS[f].name, sew);
    });
  }

  // sf.vc.v.iv on custom-2: opcode, funct3, funct6 and vm; FUNCTION_FIELD adds the field at 19:15.
  static constexpr uint32_t FUNCTION = 0xFE00707F, FUNCTION_FIELD = 0xFE0FF07F;
  static constexpr vcix_uarch::Instruction<Sfu> INSTRUCTIONS[] = {
      {0x2000305B, FUNCTION, "verf", &Sfu::special_function, 0},
      {0x2400305B, FUNCTION, "vtanh", &Sfu::special_function, 1},
      {0x2800305B, FUNCTION_FIELD, "vsin", &Sfu::special_function, 2},
      {0x2800B05B, FUNCTION_FIELD, "vcos", &Sfu::special_function, 3},
      {0x2801305B, FUNCTION_FIELD, "vlog", &Sfu::special_function, 4},
      {0x2801B05B, FUNCTION_FIELD, "vatan", &Sfu::special_function, 5},
      {0x2C00305B, FUNCTION, "vexp", &Sfu::special_function, 6},
  };

 private:
  static inline float (*const SINGLE[])(float) = {::erff, ::tanhf, ::sinf, ::cosf, ::logf, ::atanf, ::expf};
  static inline double (*const WIDE[])(double) = {nullptr, nullptr, nullptr, nullptr, ::log, ::atan, nullptr};
  static constexpr const char *LATENCY_KEY = "tpu_sfu_latency_cycles";

  vcix_uarch::Port entry_;
  vcix_uarch::Cycle latency_ = 10;
};

}  // namespace tpu

#endif
