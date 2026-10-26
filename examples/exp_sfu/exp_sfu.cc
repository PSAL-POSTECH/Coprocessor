// libexp_sfu.so: an SFU that computes only vexp (sf.vc.v.iv 0x3, vd, vs2, 0) on a pipelined EUP.
#include <cmath>

#include "vcix_cp.hpp"

namespace {

class ExpSfu : public cp::Component {
 public:
  static constexpr cp::Encoding VEXP{0x2C00305B, 0xFE00707F, "vexp"};

  ExpSfu(cp::Coprocessor &cp, const char *name) : Component(cp, name) { register_insn(VEXP); }

  void configure(const cp::Config &config) override {
    const uint64_t latency = config.uint("exp_sfu_latency_cycles", 10);
    if (latency == 0) throw cp::ConfigError("exp_sfu_latency_cycles", "0", "is not a pipeline depth: at least 1");
    eup_.configure(latency, 1);  // latency L, a new row every cycle
  }
  void cycle() override {
    // 1. Drain first: a finished vexp writes vd and completes.
    if (eup_.length()) {
      const cp::Insn in = eup_.pop();
      values([&](const cp::Host &host) { exp_of(host, in); });
      complete(in);
    }
    // 2. Then fill: the head instruction enters the EUP when it takes a row; a wrong one is a fault there instead.
    if (insns().length()) {
      const cp::Insn &head = insns().front();
      const bool wrong = !supported(head);
      if (wrong ? eup_.available() : eup_.push(head)) {
        if (wrong) fault(head, "exp_sfu: vexp: only 32-bit elements in one register (LMUL <= 1) are supported");
        instructions_.add(1);
        insns().pop();
      }
    }
  }

 private:
  static bool supported(const vcix_insn &in) { return in.sew_bits == 32 && in.lmul_log2 <= 0; }
  static void exp_of(const cp::Host &host, const cp::Insn &in) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      const float *vs2 = host.vreg<float>(lane, cp::rs2(in));
      float *vd = host.vreg_mut<float>(lane, cp::rd(in));
      for (uint32_t i = 0; i < in.vl; i++) vd[i] = std::exp(vs2[i]);
    }
  }

  cp::Counter instructions_{*this, "instructions", 1};
  cp::Pipeline<cp::Insn> eup_{*this, "eup"};
};

class ExpSfuCP final : public cp::Coprocessor {
 public:
  const char *name() const override { return "exp_sfu"; }

 private:
  ExpSfu sfu_{*this, "SFU"};
};

}  // namespace

VCIX_CP_REGISTER(ExpSfuCP)
