// Example: a TPU's units as one model, libtpu.so; each call goes to the unit that owns the instruction.
#include "vcix_cp_base.hpp"

#include "machine.hpp"
#include "misc.hpp"
#include "msa.hpp"
#include "sfu.hpp"
#include "systolic.hpp"
#include "xlu.hpp"

namespace {

class Tpu final : public tpu::Coprocessor {
 public:
  const char *name() const override { return "tpu"; }

 private:
  tpu::Sfu sfu_{*this, "SFU"};
  tpu::Misc misc_{*this, "MISC"};
  tpu::Systolic systolic_{*this, "SYSTOLIC"};
  tpu::Xlu xlu_{*this, "XLU"};
  tpu::Msa msa_{*this, "MSA"};
};

}

VCIX_CP_REGISTER(Tpu)
