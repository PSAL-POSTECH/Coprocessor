// A cp component with no simulator: an instruction issued once, then a copy, an assigned copy and the original each
// tick their own blocks to the same end; its four beats pass a 1-wide memory port one a cycle.
#include <cstdio>

#include "vcix_cp.hpp"

namespace {

class Counted : public cp::Component {
 public:
  static constexpr cp::Encoding FOUR_BEATS{0x0000005B, 0x0000007F, "four beats"};

  explicit Counted(cp::Coprocessor &cp) : Component(cp, "COUNTED") { register_insn(FOUR_BEATS); }
  void reset() override { left_ = 0; }
  // The head instruction takes the issue port once, then a beat of memory a cycle; its fourth beat completes it.
  void cycle() override {
    if (!insns().length()) return;
    if (!left_) issue_.utilize(1), left_ = 4;
    memory_.utilize(1);
    if (--left_ == 0) complete(insns().pop());
  }

 private:
  cp::Port issue_{*this, "issue", "instructions", 1, cp::Port::PRIMARY};
  cp::Port memory_{*this, "memory", "beats", 1};
  uint32_t left_ = 0;
};

class Model final : public cp::Coprocessor {
 public:
  const char *name() const override { return "route_once"; }

 private:
  Counted counted_{*this};
};

vcix_config none = {nullptr, [](void *, const char *) -> const char * { return nullptr; }};

// The cycle the instruction is ready in, ticking from `from` + 1; 0 if never.
cp::Cycle done(Model &m, cp::Id id, cp::Cycle from) {
  for (cp::Cycle now = from + 1; now < 100; now++) {
    m.tick(now);
    if (m.ready(id, now)) return now;
  }
  return 0;
}

}  // namespace

int main() {
  Model original;
  original.configure(cp::Config(&none));
  const vcix_insn four = {0x5B, 1, 32, 0};
  original.issue(four, 1, 1);
  Model copy = original, assigned;
  assigned = original;
  const cp::Cycle c = done(copy, 1, 1), a = done(assigned, 1, 1), o = done(original, 1, 1);
  std::printf("four beats issued in cycle 1: ready in copy %llu, assigned %llu, original %llu\n",
              static_cast<unsigned long long>(c), static_cast<unsigned long long>(a), static_cast<unsigned long long>(o));
  const bool ok = c == 5 && a == 5 && o == 5;
  std::printf("%s  a copy, an assigned copy and the original each tick their own blocks: beats in 2..5, ready in 5\n",
              ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
