// A vcix_cp component whose handler counts its calls, with no simulator: issued once and ticked until done, the
// handler ran once; the queue head replays what issue recorded. A copy, and a model assigned one, as vcix_accel::Instance
// makes them, tick their own blocks: the original, left as it was, is done as late when ticked after.
#include <cstdio>

#include "vcix_cp.hpp"

namespace {

int calls = 0;

class Counted : public cp::Component {
 public:
  static constexpr cp::Encoding FOUR_BEATS{0x0000005B, 0x0000007F, "four beats"};
  explicit Counted(cp::Coprocessor &cp) : Component(cp, "COUNTED") { handle(FOUR_BEATS, &Counted::four_beats); }
  void configure(const cp::Config &) override { commands_.configure(2); }

 private:
  void four_beats(cp::Instr &in) {
    calls++;
    in.use(issue_);
    in.enqueue(commands_);
    in.transfer(4, cp::Through(memory_));
  }
  cp::Port issue_{*this, "issue", "instructions", 1, cp::Port::PRIMARY};
  cp::Port memory_{*this, "memory", "beats", 1};
  cp::CmdQueue commands_{*this};
};

class Model final : public cp::Coprocessor {
 public:
  const char *name() const override { return "counted"; }

 private:
  Counted counted_{*this};
};

// The first cycle after `from` in which instruction 1 is ready, 0 if none before 20.
cp::Cycle done(Model &m, cp::Cycle from) {
  for (cp::Cycle now = from + 1; now < 20; now++) {
    m.tick(now);
    if (m.ready(1, now)) return now;
  }
  return 0;
}

}

int main() {
  vcix_config none = {nullptr, [](void *, const char *) -> const char * { return nullptr; }};
  Model m;
  m.configure(cp::Config(&none));
  const cp::Insn insn = {0x5B, 1, 32, 0};
  const bool unknown = m.issue(insn, 1, 1) == vcix_accel::Unknown;
  Model copy = m, assigned;
  assigned = m;
  const cp::Cycle copied = done(copy, 1), reassigned = done(assigned, 1);
  const cp::Cycle original = m.ready(1, copied) ? 0 : done(m, 1);
  const bool ok = unknown && copied == 6 && calls == 1;
  printf("%s  route_once: issued at cycle 1, in the queue at 2, four beats one a cycle from 3, done at %llu (6); the "
         "handler ran %d time(s) (1: at issue)\n",
         ok ? "PASS" : "FAIL", static_cast<unsigned long long>(copied), calls);
  const bool own = reassigned == 6 && original == 6;
  printf("%s  route_once: a copy and an assigned model, done at %llu and %llu (6), leave the original's blocks as they "
         "were: ticked after, it is done at %llu (6)\n",
         own ? "PASS" : "FAIL", static_cast<unsigned long long>(copied), static_cast<unsigned long long>(reassigned),
         static_cast<unsigned long long>(original));
  return ok && own ? 0 : 1;
}
