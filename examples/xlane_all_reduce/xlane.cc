// libxlane_all_reduce.so: the all-reduce sum unit (xlane_all_reduce.hpp) beside the TPU v6e's units. Hands-on 1 runs it
// on Spike and gem5; a kernel PTB compiles for this model (its loads and stores are the misc unit's DMA) issues sum.
// The all-reduce sum unit is constructed first: its encoding (sf.vc.v.iv 0) takes the place of the SFU's verf. On
// gem5 each sum prints, at its commit, its registers, the cycles it issued and was done, and the cycles between.
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <utility>

#include "vcix_cp_base.hpp"

#include "tpu/machine.hpp"
#include "tpu/misc.hpp"
#include "tpu/mxu.hpp"
#include "tpu/sfu.hpp"
#include "tpu/xlu.hpp"
#include "xlane_all_reduce/xlane_all_reduce.hpp"

namespace {

using xlane::XlaneAllReduce;

struct Shape {
  static constexpr uint32_t REGISTERS = 2;
  static constexpr uint64_t SLOTS = 2;
  static constexpr bool WORD_A_CYCLE = true;
};

bool is_xlane(const vcix_insn &insn) {
  return (insn.bits & XlaneAllReduce::SUM.mask) == XlaneAllReduce::SUM.match;
}

class Xlane final : public tpu::Coprocessor {
 public:
  const char *name() const override { return "xlane_all_reduce"; }

  cp::Cycle issue(const vcix_insn &insn, cp::Id id, cp::Cycle now) override {
    if (is_xlane(insn)) issued_.push_back({now, insn.vl});
    return tpu::Coprocessor::issue(insn, id, now);
  }
  void commit(const vcix_insn &insn, cp::Id id, cp::Cycle now) override {
    if (!is_xlane(insn)) return tpu::Coprocessor::commit(insn, id, now);
    const auto [at, vl] = issued_.front();
    issued_.pop_front();
    const cp::Cycle done = xlane_.take_done(id);
    tpu::Coprocessor::commit(insn, id, now);
    if (replaying()) return;
    std::printf("[xlane_all_reduce] v%u = sum(v%u), vl %u: issue %" PRIu64 ", done %" PRIu64 ", %" PRIu64 " cycles\n",
                cp::rd(insn), cp::rs2(insn), vl, at, done, done - at);
    std::fflush(stdout);
  }

 private:
  XlaneAllReduce xlane_{*this, "XlaneAllReduce"};
  tpu::Sfu sfu_{*this, "TPUV6E_SFU"};
  tpu::Misc misc_{*this, "TPUV6E_MISC"};
  tpu::Xlu xlu_{*this, "TPUV6E_XLU"};
  tpu::Mxu<Shape> mxu_{*this, "TPUV6E_MXU"};
  std::deque<std::pair<cp::Cycle, uint32_t>> issued_;
};

}  // namespace

VCIX_CP_REGISTER(Xlane)
