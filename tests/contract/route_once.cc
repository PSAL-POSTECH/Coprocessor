// A vcix_uarch unit whose route function counts its calls, with no simulator: issued once and ticked until done, the
// function ran once; the queue head replays what issue recorded.
#include <cstdio>

#include "vcix_uarch.hpp"

namespace {

int calls = 0;

class Counted {
 public:
  explicit Counted(const char *name)
      : issue_{name, "issue", "instructions", 1, vcix_uarch::PRIMARY}, memory_{name, "memory", "beats", 1} {}
  void configure(const vcix_uarch::Config &) { commands_.configure(2); }
  template <class H>
  void hardware(H &&h) {
    h.port(issue_), h.port(memory_);
    h.queue(commands_);
  }
  void four_beats(vcix_uarch::Route &r) {
    calls++;
    r.use(issue_).enqueue(commands_).transfer(4).through(memory_);
  }
  static constexpr vcix_uarch::Instruction<Counted> INSTRUCTIONS[] = {{0x0000005B, 0x0000007F, "four beats", &Counted::four_beats}};

 private:
  vcix_uarch::Port issue_, memory_;
  vcix_uarch::CommandQueue commands_;
};

class Model final : public vcix_uarch::Machine<Counted> {
 public:
  Model() : Machine("COUNTED") {}
  const char *name() const override { return "counted"; }
};

}  // namespace

int main() {
  vcix_config none = {nullptr, [](void *, const char *) -> const char * { return nullptr; }};
  Model m;
  m.configure(vcix_uarch::Config(&none));
  const vcix_uarch::Insn insn = {0x5B, 1, 32, 0};
  const bool unknown = m.issue(insn, 1, 1) == cp::Unknown;
  vcix_uarch::Cycle done = 0;
  for (vcix_uarch::Cycle now = 2; now < 20 && !done; now++) {
    m.tick(now);
    if (m.ready(1, now)) done = now;
  }
  const bool ok = unknown && done == 6 && calls == 1;
  printf("%s  route_once: issued at cycle 1, in the queue at 2, four beats one a cycle from 3, done at %llu (6); the "
         "route function ran %d time(s) (1: at issue)\n",
         ok ? "PASS" : "FAIL", static_cast<unsigned long long>(done), calls);
  return ok ? 0 : 1;
}
