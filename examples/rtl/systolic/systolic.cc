// The tpu example's systolic array (examples/tpu/systolic.hpp's encodings) as systolic.sv behind rtl_model.hpp: what
// each instruction is to the unit, and the machine key the RTL was built for. SYSTOLIC_LANES is its parameter L.
#include "Vsystolic.h"
#include "rtl_model.hpp"

static_assert(SYSTOLIC_LANES >= 2, "systolic.sv elaborates for L of 2 or more");

namespace {

struct Systolic {
  static constexpr const char *NAME = "rtl_systolic";
  static constexpr uint32_t LANES = SYSTOLIC_LANES, TAGS = 256;
  static constexpr bool BEATS = true, SCALAR = false, MEM = false;
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr rtl::Op OPS[] = {
      {0x2200305B, FORM, "systolic input push", 0, true, false},
      {0x2600305B, FORM, "systolic weight push", 1, true, false},
      {0x0800305B, FORM, "systolic pop", 2, false, true},
  };
  static void check(const cp::Config &config) {
    if (config.uint("vpu_num_lanes", 128) != LANES) {
      const char *lanes = config.get("vpu_num_lanes");
      throw cp::ConfigError("vpu_num_lanes", lanes ? lanes : "(absent)",
                                    "is not the lanes this RTL was built with");
    }
  }
};

}  // namespace

using SystolicModel = rtl::Model<Vsystolic, Systolic>;
VCIX_CP_REGISTER(SystolicModel)
