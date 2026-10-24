// Test model, built twice (WHICH 1 and 2): both libraries define a class of the same global name.
#include "vcix_cp_base.hpp"

class Accel : public cp::Model {
 public:
  const char *name() const override { return WHICH == 1 ? "first" : "second"; }
  std::vector<cp::Encoding> owns() const override { return {{0x5Bu | (WHICH << 26), 0xFC00007Fu, "op"}}; }

  void execute(const cp::Host &, const vcix_insn &) override {}
  bool can_accept(const vcix_insn &, cp::Cycle) const override { return true; }
  cp::Cycle issue(const vcix_insn &, cp::Id, cp::Cycle) override {
    return WHICH;
  }
};

VCIX_CP_REGISTER(Accel)
