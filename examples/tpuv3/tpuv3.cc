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
struct Shape {
  static constexpr uint32_t REGISTERS = 1;
  static constexpr uint64_t SLOTS = 1;
  static constexpr bool WORD_A_CYCLE = false;
};

class Tpuv3 final : public tpu::Machine<tpu::Sfu, tpu::Misc, tpu::Xlu, tpu::Mxu<Shape>> {
 public:
  Tpuv3() : Machine("TPUV3_SFU", "TPUV3_MISC", "TPUV3_XLU", "TPUV3_MXU") {}
  const char *name() const override { return "tpuv3"; }
};

}  // namespace

VCIX_CP_REGISTER(Tpuv3)
