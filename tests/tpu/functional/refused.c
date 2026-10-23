// What the model cannot carry out, one per run. Usage: refused <which>
#include "common.h"

kernel k_vexp_64, k_cross_pop_32, k_cross_fold_sum_16;
kernel_with k_pop_16, k_pop_32, k_input_16, k_input_32, k_weight_8, k_weight_16, k_weight_32, k_cross_push_32, k_msa_push_16,
    k_msa_push_32, k_msa_pop_32;

// One row of four words along the split axis, a word per lane.
static struct {
  int32_t dim_size[4], dim_low[4], dim_high[4];
  uint64_t mm_stride[4], spad_stride[4];
  uint16_t element_size, vlane_stride;
  uint8_t split_axis, unused_117;
  uint16_t flags;
  uint64_t indirect_address;
  uint16_t indirect_stride, indirect_element_size;
  uint8_t indirect_dim, unused_133;
  uint16_t indirect_lanes;
  uint64_t fill, memory_base, memory_bytes;
} row = {.dim_size = {1, 1, 1, 4}, .dim_high = {1, 1, 1, 4}, .mm_stride = {4, 4, 4, 1}, .spad_stride = {4, 4, 4, 1},
         .element_size = 4, .vlane_stride = 1, .split_axis = 3};
static uint32_t memory[4];

#define DMA(funct7, a, b) __asm__ volatile(".insn r 0x2b, 3, " #funct7 ", x0, %0, %1" : : "r"(a), "r"(b) : "memory")

int main(int argc, char **argv) {
  const char *which = argc == 2 ? argv[1] : "";
  if (!strcmp(which, "doubles")) {            // a special function without a form for doubles
    k_vexp_64(spad(0, 0), spad(0, 4096), 1);
  } else if (!strcmp(which, "pop")) {         // a pop of what was never computed
    k_pop_32(0, spad(0, 0), 1, 0, 0);
  } else if (!strcmp(which, "input")) {       // an input with no weights
    k_input_32(spad(0, 0), 0, 1, 0, 0);
  } else if (!strcmp(which, "format")) {      // a byte under rs1 3, which names a bfloat16
    k_weight_8(spad(0, 0), 0, 1, 3, 0);
  } else if (!strcmp(which, "bf16-pop")) {    // a pop to bfloat16, which the model does not round to
    k_weight_16(spad(0, 0), 0, 1, 0, 0);
    k_input_16(spad(0, 0), 0, 1, 0, 0);
    k_pop_16(0, spad(0, 0), 1, 3, 0);
  } else if (!strcmp(which, "pop-format")) {  // a pop of singles under rs1 1, which names an E4M3
    k_weight_32(spad(0, 0), 0, 1, 0, 0);
    k_input_32(spad(0, 0), 0, 1, 0, 0);
    k_pop_32(0, spad(0, 0), 1, 1, 0);
  } else if (!strcmp(which, "pattern")) {     // a shuffle by a pattern that was never given
    k_cross_push_32(spad(0, 0), 0, 1, 16, 0);
    k_cross_pop_32(0, spad(0, 0), 1);
  } else if (!strcmp(which, "xlu-pop")) {     // a cross-lane pop of more than is left
    k_cross_push_32(spad(0, 0), 0, 3, 0, 0);
    k_cross_pop_32(0, spad(0, 0), 8);
  } else if (!strcmp(which, "bf16-sum")) {    // a fold the v6e has no bf16 form of: its lane sum is taken in f32
    k_cross_fold_sum_16(spad(0, 0), 0, 4);
  } else if (!strcmp(which, "msa-pop")) {     // a multi-precision pop of more than was computed
    k_msa_push_32(spad(0, 0), 0, 3, 0, 0);
    k_msa_pop_32(0, spad(0, 0), 8, 0, 0);
  } else if (!strcmp(which, "words")) {       // a push to the multi-precision array of anything but words
    k_msa_push_16(spad(0, 0), 0, 1, 0, 0);
  } else if (!strcmp(which, "overflow")) {    // a transfer to the end of a lane's scratchpad
    DMA(7, &row, &row);
    DMA(2, memory, spad(0, LANE_BYTES));
  } else {
    puts("usage: refused doubles | pop | input | format | bf16-pop | pop-format | pattern | xlu-pop | msa-pop | words | "
         "overflow");
    return 2;
  }
  puts("it ran");
  return 0;
}
