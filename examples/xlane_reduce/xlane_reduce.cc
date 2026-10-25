// libxlane_reduce.so: the cross-lane reduction unit alone (xlane_reduce.hpp). On gem5 each commit prints when its
// instruction issued and how many cycles it took.
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <utility>

#include "xlane_reduce.hpp"

namespace {

using xlane::XlaneReduce;

class XlaneReduceCP final : public cp::Coprocessor {
 public:
  const char *name() const override { return "xlane_reduce"; }

  cp::Cycle issue(const vcix_insn &insn, cp::Id id, cp::Cycle now) override {
    issued_.push_back({now, insn.vl});
    return Coprocessor::issue(insn, id, now);
  }
  void commit(const vcix_insn &insn, cp::Id id, cp::Cycle now) override {
    const auto [at, vl] = issued_.front();
    issued_.pop_front();
    const cp::Cycle done = unit_.take_done(id);
    Coprocessor::commit(insn, id, now);
    if (replaying()) return;
    const char *what = (insn.bits & XlaneReduce::MAX.mask) == XlaneReduce::MAX.match ? "max" : "sum";
    std::printf("[xlane] %s issue at cycle %" PRIu64 ", vl %u: done %" PRIu64 " cycles later\n", what, at, vl, done - at);
    std::fflush(stdout);
  }

 private:
  XlaneReduce unit_{*this, "XLANE"};
  std::deque<std::pair<cp::Cycle, uint32_t>> issued_;
};

}  // namespace

VCIX_CP_REGISTER(XlaneReduceCP)
