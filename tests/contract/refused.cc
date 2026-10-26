// Built two ways, each a library a caller must refuse: WHICH 1 hands over no table, 2 a table of another ABI version.
#include "vcix_cp_base.hpp"

extern "C" __attribute__((visibility("default"))) const vcix_model *coprocessor_model(void) {
  static const vcix_model other_abi = {COPROCESSOR_ABI_VERSION + 1};
  return WHICH == 1 ? nullptr : &other_abi;
}
