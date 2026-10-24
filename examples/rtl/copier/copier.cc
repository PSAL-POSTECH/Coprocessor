// copier.sv behind rtl_model.hpp: sf.vc.x 1 sets the source from x[rs1], sf.vc.x 2 copies the next 256 bytes from it
// to x[rs1] and returns at once, sf.vc.x 3 waits until every earlier copy is in memory.
#include "Vcopier.h"
#include "rtl_model.hpp"

namespace {

struct Copier {
  static constexpr const char *NAME = "rtl_copier";
  static constexpr uint32_t LANES = 1, TAGS = 0;
  static constexpr bool BEATS = false, SCALAR = true, MEM = true;
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr rtl::Op OPS[] = {
      {0x0600405B, FORM, "copy source", 0, false, false},
      {0x0A00405B, FORM, "copy", 1, false, false},
      {0x0E00405B, FORM, "copy wait", 2, false, false, true},
  };
  static void check(const cp::Config &) {}
};

}  // namespace

using CopierModel = rtl::Model<Vcopier, Copier>;
VCIX_CP_REGISTER(CopierModel)
