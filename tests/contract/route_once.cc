// A cp component with no simulator: an instruction issued once, then a copy, an assigned copy and the original each
// tick their own blocks and count their own counter to the same end; its four beats take a cycle each.
#include <cstdio>

#include "vcix_cp.hpp"

namespace {

class Counted : public cp::Component {
 public:
  static constexpr cp::Encoding FOUR_BEATS{0x0000005B, 0x0000007F, "four beats"};

  explicit Counted(cp::Coprocessor &cp) : Component(cp, "COUNTED") { register_insn(FOUR_BEATS); }
  void reset() override { left_ = 0; }
  // The head instruction counts once, then takes a beat a cycle; its fourth beat completes it.
  void cycle() override {
    if (!insns().length()) return;
    if (!left_) instructions_.add(1), left_ = 4;
    if (--left_ == 0) complete(insns().pop());
  }

 private:
  cp::Counter instructions_{*this, "instructions", 1};
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
  const cp::Cycle c = done(copy, 1, 1);
  const uint64_t counted = copy.counters()[0]->value(), untouched = original.counters()[0]->value();
  const cp::Cycle a = done(assigned, 1, 1), o = done(original, 1, 1);
  std::printf("four beats issued in cycle 1: ready in copy %llu, assigned %llu, original %llu; copy counted %llu, the "
              "original then %llu\n",
              static_cast<unsigned long long>(c), static_cast<unsigned long long>(a), static_cast<unsigned long long>(o),
              static_cast<unsigned long long>(counted), static_cast<unsigned long long>(untouched));
  const bool ok = c == 5 && a == 5 && o == 5 && counted == 1 && untouched == 0;
  std::printf("%s  a copy, an assigned copy and the original each tick their own blocks and counters: beats in 2..5, "
              "ready in 5\n",
              ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
