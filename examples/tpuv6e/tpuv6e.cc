// Example: a TPU v6e as one model, libtpuv6e.so: the tpu example's special-function, misc and cross-lane units, and
// its matrix unit (tpu/mxu.hpp) in the v6e's shape, in place of the systolic array and the multi-precision array.
#include "vcix_cp_base.hpp"

#include "tpu/machine.hpp"
#include "tpu/misc.hpp"
#include "tpu/mxu.hpp"
#include "tpu/sfu.hpp"
#include "tpu/xlu.hpp"

namespace {

// The v6e's MXU: 256 x 256 on 128 lanes, so an operand is two registers; two MXU slots a bundle, shared by its
// MXUs; a word of two 16-bit rows a cycle.
struct Shape {
  static constexpr uint32_t REGISTERS = 2;
  static constexpr uint64_t SLOTS = 2;
  static constexpr bool WORD_A_CYCLE = true;
};

class Tpuv6e final : public tpu::Coprocessor {
 public:
  const char *name() const override { return "tpuv6e"; }

 private:
  tpu::Sfu sfu_{*this, "TPUV6E_SFU"};
  tpu::Misc misc_{*this, "TPUV6E_MISC"};
  tpu::Xlu xlu_{*this, "TPUV6E_XLU"};
  tpu::Mxu<Shape> mxu_{*this, "TPUV6E_MXU"};
};

}

VCIX_CP_REGISTER(Tpuv6e)
