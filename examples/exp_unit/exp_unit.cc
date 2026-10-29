// libexp_unit.so: a unit that computes only vexp (sf.vc.v.iv 0x3, vd, vs2, 0) on a pipeline.
#include <cmath>

#include "vcix_cp.hpp"

namespace {

class ExpUnit : public cp::Component {
 public:
  static constexpr cp::Encoding VEXP{0x2C00305B, 0xFE00707F, "vexp"};

  ExpUnit(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    register_insn(VEXP);
  }

  void configure(const cp::Config &config) override {
    const uint64_t latency = config.uint("exp_unit_latency_cycles", 10);
    if (latency == 0) throw cp::ConfigError("exp_unit_latency_cycles", "0", "is not a pipeline depth: at least 1");
    pipe_.configure(latency, 1);  // latency L, a new row every cycle
  }
  void cycle() override {
    if (pipe_.length()) {  // drain: the exit first
      const cp::Insn in = pipe_.pop();
      values([&](auto &h) { exp_of(h, in); });
      complete(in);
    }
    // fill: then the entry
    if (insns().length() && pipe_.push(insns().front())) {
      instructions_.add(1);
      insns().pop();
    }
  }

 private:
  static void exp_of(const cp::Host &host, const cp::Insn &in) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      const float *vs2 = host.vreg<float>(lane, cp::rs2(in));
      float *vd = host.vreg_mut<float>(lane, cp::rd(in));
      for (uint32_t i = 0; i < in.vl; i++) vd[i] = std::exp(vs2[i]);
    }
  }

  cp::Counter instructions_{*this, "instructions", 1};
  cp::Pipeline<cp::Insn> pipe_{*this, "pipe"};
};

class ExpCoprocessor final : public cp::Coprocessor {
 public:
  const char *name() const override { return "exp_unit"; }

 private:
  ExpUnit exp_unit_{*this, "ExpUnit"};
};

}  // namespace

VCIX_CP_REGISTER(ExpCoprocessor)
