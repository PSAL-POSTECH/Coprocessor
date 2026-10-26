// Test model: holds up to DEPTH instructions at once; reports at each commit the cycles since its issue.
#include <cinttypes>
#include <cstdio>
#include <deque>

#include "vcix_cp_base.hpp"

namespace {

using namespace cp;
using Insn = vcix_insn;

constexpr size_t DEPTH = 4;
constexpr Cycle LATENCY = 10;

class Pipelined : public Model {
 public:
  const char *name() const override { return "pipelined"; }
  std::vector<Encoding> owns() const override { return {{0x0600405B, 0xFE00707F, "piped"}}; }

  void execute(const Host &, const Insn &) override {}
  bool can_accept(const Insn &, Cycle) const override { return issued_.size() < DEPTH; }
  Cycle issue(const Insn &, Id, Cycle now) override {
    issued_.push_back(now);
    return LATENCY;
  }
  void commit(const Insn &, Id, Cycle now) override {
    const Cycle issued = issued_.front();
    issued_.pop_front();
    if (replaying()) return;
    printf("[model] commit, %" PRIu64 " cycles after its issue\n", now - issued);
    fflush(stdout);
  }

 private:
  std::deque<Cycle> issued_;
};

}  // namespace

VCIX_CP_REGISTER(Pipelined)
