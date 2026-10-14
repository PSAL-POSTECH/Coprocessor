// Example: a TPU v6e as one model, libtpuv6.so: the tpu example's special-function, misc and cross-lane units, and
// the matrix unit (mxu.hpp) as its array, in place of the systolic array and the multi-precision array.
#include "vcix_accel.hpp"

#include "tpu/machine.hpp"
#include "tpu/misc.hpp"
#include "tpu/sfu.hpp"
#include "tpu/xlu.hpp"

#include "functional.hpp"
#include "mxu.hpp"

namespace {

class Tpuv6 final : public tpu::Machine<tpuv6::Functional, tpu::Sfu, tpu::Misc, tpu::Xlu, tpuv6::Mxu> {
 public:
  Tpuv6() : Machine("TPUV6_SFU", "TPUV6_MISC", "TPUV6_XLU", "TPUV6_MXU") {}
  const char *name() const override { return "tpuv6"; }
};

}  // namespace

VCIX_ACCEL_REGISTER(Tpuv6)
