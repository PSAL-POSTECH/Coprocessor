// Built three ways, each a library a caller must refuse: WHICH 1 hands over no table, 2 a table of another ABI version, 3 a model whose constructor throws.
#include <stdexcept>

#include "vcix_cp_base.hpp"

#if WHICH == 3
namespace {

using namespace cp;
using Insn = vcix_insn;

class CannotBeMade : public Model {
 public:
  CannotBeMade() { throw std::runtime_error("no such unit can be built"); }
  const char *name() const override { return "cannot_be_made"; }
  std::vector<Encoding> owns() const override { return {}; }
  void execute(const Host &, const Insn &) override {}
  bool can_accept(const Insn &, Cycle) const override { return true; }
  Cycle issue(const Insn &, Id, Cycle) override { return 1; }
};

}  // namespace

VCIX_CP_REGISTER(CannotBeMade)
#else
extern "C" __attribute__((visibility("default"))) const vcix_model *vcix_accel_model(void) {
  static const vcix_model other_abi = {VCIX_ACCEL_ABI_VERSION + 1};
  return WHICH == 1 ? nullptr : &other_abi;
}
#endif
