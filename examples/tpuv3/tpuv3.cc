// A TPU v3 TensorCore as one model, libtpuv3.so: the tpu example's units and its matrix unit (tpu/mxu.hpp) in the
// v3's shape. The TensorCore's second MXU is TOGSim's array split, so tpu_mxu_units stays 1.
#include "vcix_cp_base.hpp"

#include "tpu/machine.hpp"
#include "tpu/misc.hpp"
#include "tpu/mxu.hpp"
#include "tpu/sfu.hpp"
#include "tpu/xlu.hpp"

namespace {

// The v3's MXU: 128 x 128 on 128 lanes, so an operand is one register; a row a cycle, which gives 123 TFLOPS a
// chip at 940 MHz. SLOTS 1, a placeholder: the TPUv2 bundle has one push slot, for its one MXU and the transpose unit
// (Norrie et al., IEEE Micro 2021, p.4; Jouppi et al., CACM 2020, p.70); the v3's, for two MXUs, no source states.
// INT8_MACS 1: an int8 row a cycle, as a bfloat16 one; no source gives the v3 an int8 rate above its bfloat16 rate.
struct Shape {
  static constexpr uint32_t REGISTERS = 1;
  static constexpr uint64_t SLOTS = 1;
  static constexpr bool WORD_A_CYCLE = false;
  static constexpr uint32_t INT8_MACS = 1;
};

class Tpuv3 final : public tpu::Coprocessor {
 public:
  const char *name() const override { return "tpuv3"; }

 private:
  tpu::Sfu sfu_{*this, "TPUV3_SFU"};
  tpu::Misc misc_{*this, "TPUV3_MISC"};
  tpu::Xlu xlu_{*this, "TPUV3_XLU"};
  tpu::Mxu<Shape> mxu_{*this, "TPUV3_MXU"};
};

}

VCIX_CP_REGISTER(Tpuv3)
