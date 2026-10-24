// Example: a TPU's units as one model, libtpu.so; each call goes to the unit that owns the instruction.
#include "vcix_cp_base.hpp"

#include "machine.hpp"
#include "misc.hpp"
#include "msa.hpp"
#include "sfu.hpp"
#include "systolic.hpp"
#include "xlu.hpp"

namespace {

class Tpu final : public tpu::Machine<tpu::Sfu, tpu::Misc, tpu::Systolic, tpu::Xlu, tpu::Msa> {
 public:
  Tpu() : Machine("SFU", "MISC", "SYSTOLIC", "XLU", "MSA") {}
  const char *name() const override { return "tpu"; }
};

}  // namespace

VCIX_CP_REGISTER(Tpu)
