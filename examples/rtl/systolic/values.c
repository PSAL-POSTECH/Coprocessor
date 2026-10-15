// int8 through the systolic array, by tests/tpu/functional's kernels: fewer weight columns than lanes, a full matrix,
// a matrix that slides, pops in pieces, more elements than a register holds, and a weight push between an input push
// and its pop. Output: what each pop left in every lane, so two models' runs compare byte for byte. Inputs and weights
// are in [-3, 3], so every sum of four fits int8: the RTL keeps a sum's low byte, libtpu.so converts it from a float.
#include "common.h"

kernel_with k_input_8, k_weight_8, k_pop_8;

#define IN 0ul
#define OUT 4096ul

static void fill(unsigned long count, int scale) {
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < count; i++)
      *(int8_t *)spad(lane, IN + i) = (int8_t)((int)random_below(2 * scale + 1) - scale);
}
static void push(kernel_with *function, unsigned long count, int scale) {
  fill(count, scale);
  if (function(spad(0, IN), 0, count, 0, 0) != count) puts("a push did not take every element");
}
static void pop(unsigned long count) {
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 64);
  if (k_pop_8(0, spad(0, OUT), count, 0, 0) != count) puts("a pop did not take every element");
  emit_lanes(OUT, 64);
}

int main(void) {
  push(k_weight_8, 4, 3);
  push(k_input_8, 6, 3);
  push(k_weight_8, 2, 3);
  pop(6);
  push(k_weight_8, 2, 3);
  push(k_input_8, 3, 3);
  pop(3);
  push(k_weight_8, 2, 3);
  push(k_input_8, 5, 3);
  pop(2);
  pop(3);
  push(k_weight_8, 3, 3);
  push(k_input_8, 4, 3);
  push(k_input_8, 1, 3);
  pop(5);
  push(k_weight_8, 4, 3);
  push(k_input_8, 40, 3);
  pop(40);
  return 0;
}
