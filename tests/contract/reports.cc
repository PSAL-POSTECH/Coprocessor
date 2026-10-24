// Test model: reports what it is given and every call; `latency`, `depth` and `base` of the machine description shape it.
#include <cinttypes>
#include <cstdio>

#include "vcix_cp_base.hpp"

extern "C" __attribute__((noinline)) uint32_t f16_to_f32(uint16_t) { return 0x0badc0de; }

namespace {

using namespace cp;
using Insn = vcix_insn;

constexpr const char *KEYS[] = {"plain",        "quoted",     "empty",        "tilde",  "null_word", "quoted_empty",
                                "quoted_null",  "quoted_tilde", "leading_zero", "spaced", "nested",    "listed",
                                "not_in_the_file"};

class Reports : public Model {
 public:
  const char *name() const override { return "reports"; }
  std::vector<Encoding> owns() const override {
    return {{0x0600405B, 0xFE00707F, "owned"}, {0x0E00405B, 0xFE00707F, "base"}};
  }

  void configure(const Config &config) override {
    for (const char *key : KEYS) {
      const char *value = config.get(key);
      if (value) printf("[config] %s = <%s>\n", key, value);
      else printf("[config] %s is absent\n", key);
    }
    printf("[config] f16_to_f32(0x3c00) = 0x%08x\n", f16_to_f32(0x3c00));
    fflush(stdout);
    latency_ = config.uint("latency", 1);
    depth_ = config.uint("depth", UINT64_MAX);
    base_ = config.hex("base", 0x1000);
  }

  void execute(const Host &host, const Insn &insn) override {
    report("execute", insn);
    printf("[host] frm %" PRIu64 "\n", host.csr(0x002));
    fflush(stdout);
  }
  bool can_accept(const Insn &insn, Cycle) const override {
    report("accept", insn);
    return in_flight_ < depth_;
  }
  Cycle issue(const Insn &insn, Id, Cycle) override {
    report("issue", insn);
    in_flight_++;
    return (insn.bits >> 26) == 3 ? base_ : latency_ + commits_;
  }
  void commit(const Insn &insn, Id, Cycle) override {
    in_flight_--;
    commits_++;
    if (!replaying()) report("commit", insn);
  }
  void reset() override { in_flight_ = commits_ = 0; }

 private:
  void report(const char *entry, const Insn &insn) const {
    printf("[model] %s %08" PRIx32 ", %" PRIu64 " in flight, %" PRIu64 " commits seen\n", entry, insn.bits, in_flight_,
           commits_);
    fflush(stdout);
  }
  uint64_t latency_ = 1, depth_ = UINT64_MAX, base_ = 0x1000;
  uint64_t in_flight_ = 0, commits_ = 0;
};

}  // namespace

VCIX_CP_REGISTER(Reports)
