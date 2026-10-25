// libxlane_tpuv6e.so: the cross-lane reduction unit (xlane_reduce.hpp) beside the TPU v6e's units, so a kernel PTB
// compiles for tpuv6e (its loads and stores are the misc unit's DMA) can issue sum and max. The reduction unit is
// constructed first: its encodings (sf.vc.v.iv 0 and 1) take the place of the SFU's verf and vtanh.
#include "vcix_cp_base.hpp"

#include "tpu/machine.hpp"
#include "tpu/misc.hpp"
#include "tpu/mxu.hpp"
#include "tpu/sfu.hpp"
#include "tpu/xlu.hpp"
#include "xlane_reduce/xlane_reduce.hpp"

namespace {

struct Shape {
  static constexpr uint32_t REGISTERS = 2;
  static constexpr uint64_t SLOTS = 2;
  static constexpr bool WORD_A_CYCLE = true;
};

class XlaneTpuv6e final : public tpu::Coprocessor {
 public:
  const char *name() const override { return "xlane_tpuv6e"; }

 private:
  xlane::XlaneReduce xlane_{*this, "XLANE"};
  tpu::Sfu sfu_{*this, "TPUV6E_SFU"};
  tpu::Misc misc_{*this, "TPUV6E_MISC"};
  tpu::Xlu xlu_{*this, "TPUV6E_XLU"};
  tpu::Mxu<Shape> mxu_{*this, "TPUV6E_MXU"};
};

}

VCIX_CP_REGISTER(XlaneTpuv6e)
