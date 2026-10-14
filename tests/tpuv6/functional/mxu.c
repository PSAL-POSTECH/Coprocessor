// The matrix unit of libtpuv6.so: an array of 2 x LANES columns, an operand as two registers (a vmatprep, then
// the vmatpush or the vmatmul, each operand's held apart), a word of one single or two rows of 16 bits, weights
// staged a half of the depth at a time and made active by a vmatmul that latches, a result popped a half of the
// columns at a time. Checked against a formula. Usage: mxu [arrays | what the model refuses]
//   arrays: on a machine of tpu_mxu_units: 2, each array its own tile and its own held halves.
#include "common.h"

// sf.vc.iv 0, 1 and 2 and sf.vc.v.i 2: the encodings the matrix unit has. The number is the instruction's field.
// The _a1 and _a2 kernels name array 1 or 2 in the field the encodings leave free (sf.vc.iv's rd, sf.vc.v.i's rs2).
kernel_with k_mxu_matmul_32, k_mxu_push_32, k_mxu_prep_32, k_mxu_pop_32, k_mxu_push_16;
kernel_with k_mxu_matmul_32_a1, k_mxu_push_32_a1, k_mxu_prep_32_a1, k_mxu_pop_32_a1, k_mxu_matmul_32_a2;
// The array the helpers below work on: its kernels, and the weights it holds as this program models them.
static unsigned array_on;
static kernel_with *const matmuls[] = {k_mxu_matmul_32, k_mxu_matmul_32_a1};
static kernel_with *const pushes[] = {k_mxu_push_32, k_mxu_push_32_a1};
static kernel_with *const preps[] = {k_mxu_prep_32, k_mxu_prep_32_a1};
static kernel_with *const pops[] = {k_mxu_pop_32, k_mxu_pop_32_a1};
#define k_vmatmul (matmuls[array_on])
#define k_vmatpush (pushes[array_on])
#define k_vmatprep (preps[array_on])
#define k_vpop (pops[array_on])

#define IN 0ul
#define OUT 4096ul
#define WIDTH (2 * LANES)
#define SLAB 2u
enum { SINGLE = 0, HALF = 1, BF16 = 3, INPUTS = 4, UPPER = 4, LATCH = 4, PUSH1 = 8, PUSH2 = 16 };

// A float that a bfloat16 holds, of exponent 2^-30 to 2^30: every product of two is exact.
static float value(void) {
  const uint32_t bits = (random32() & 0x8000u) << 16 | (97 + random_below(61)) << 23 | (random32() & 0x7fu) << 16;
  float made;
  memcpy(&made, &bits, sizeof made);
  return made;
}
static uint32_t bits_of(float made) {
  uint32_t bits;
  memcpy(&bits, &made, sizeof bits);
  return bits;
}
// The half of a single that a half holds exactly and that is not subnormal as one.
static uint16_t half_of(uint32_t bits) {
  if ((bits & 0x7fffffff) == 0) return (uint16_t)(bits >> 16);
  const int exponent = (int)((bits >> 23) & 0xff) - 127 + 15;
  return (uint16_t)((bits >> 16 & 0x8000) | exponent << 10 | (bits >> 13 & 0x3ff));
}
// `rows` rows of the columns from `first`, a column per lane, as the words of `format`.
static void place(float (*matrix)[WIDTH], unsigned rows, unsigned first, unsigned format) {
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned row = 0; row < rows; row++) {
      const uint32_t bits = bits_of(matrix[row][first + lane]);
      if (format == SINGLE) *(uint32_t *)spad(lane, IN + 4 * row) = bits;
      else if (format == BF16) *(uint16_t *)spad(lane, IN + 2 * row) = (uint16_t)(bits >> 16);
      else *(uint16_t *)spad(lane, IN + 2 * row) = half_of(bits);
    }
}
// The upper half of an operand under a vmatprep with `prep` in its field, which the unit holds until its operand's
// next vmatpush or vmatmul.
static void prep(float (*matrix)[WIDTH], unsigned rows, unsigned format, unsigned field) {
  const unsigned long words = format == SINGLE ? rows : rows / 2;
  place(matrix, rows, LANES, format);
  if (k_vmatprep(spad(0, IN), 0, words, format | field, 0) != words) puts("a vmatprep did not take every word");
}
// The lower half of an operand with `field` in its field, which takes the upper half held for its operand.
static void bring(kernel_with *lower, float (*matrix)[WIDTH], unsigned rows, unsigned format, unsigned field) {
  const unsigned long words = format == SINGLE ? rows : rows / 2;
  place(matrix, rows, 0, format);
  if (lower(spad(0, IN), 0, words, format | field, 0) != words) puts("an operand did not take every word");
}
// An operand: its upper half under a vmatprep, then its lower half.
static void operand(kernel_with *lower, float (*matrix)[WIDTH], unsigned rows, unsigned format, unsigned upper,
                    unsigned field) {
  prep(matrix, rows, format, upper);
  bring(lower, matrix, rows, format, field);
}

// What each array holds, a weight at each depth of each column: the staged weights, and the active ones, which the
// last vmatmul that latched copied whole.
static float staged[2][WIDTH][WIDTH], active[2][WIDTH][WIDTH];
// `count` rows of weights to one half of the depth: what the half held moves down `count` depths, what passes its
// last is gone, and the rows take its top in their order. The other half is as it was.
static void stage(float (*rows)[WIDTH], unsigned count, unsigned half) {
  float (*top)[WIDTH] = staged[array_on] + (half == PUSH2 ? LANES : 0);
  memmove(top + count, top, (LANES - count) * sizeof *top);
  memcpy(top, rows, count * sizeof *rows);
}
static void push(float (*rows)[WIDTH], unsigned count, unsigned format, unsigned half) {
  stage(rows, count, half);
  operand(k_vmatpush, rows, count, format, 0, half);
}
// One half of the depth of a tile as the v6e pushes it, a slab at a time and the deepest first: the last slab
// pushed is at the top, and the half holds the tile's rows as they are.
static void push_half(float (*tile)[WIDTH], unsigned format, unsigned half) {
  for (unsigned slab = LANES / SLAB; slab-- > 0;)
    push(tile + (half == PUSH2 ? LANES : 0) + slab * SLAB, SLAB, format, half);
}
static void matmul(float (*a)[WIDTH], unsigned rows, unsigned format, unsigned latch) {
  if (latch) memcpy(active[array_on], staged[array_on], sizeof active[0]);
  operand(k_vmatmul, a, rows, format, INPUTS, latch);
}
// `rows` rows of the result, the lower half of the columns and then the upper, against a x the active weights.
static unsigned checked;
static unsigned check(float (*a)[WIDTH], unsigned from, unsigned rows) {
  unsigned wrong = 0;
  for (unsigned half = 0; half < 2; half++) {
    if (k_vpop(0, spad(0, OUT), rows, half ? UPPER : 0, 0) != rows) puts("a vpop did not take every row");
    for (unsigned lane = 0; lane < LANES; lane++)
      for (unsigned row = 0; row < rows; row++) {
        float want = 0;
        for (unsigned k = 0; k < WIDTH; k++) want += a[from + row][k] * active[array_on][k][half * LANES + lane];
        const float got = *(float *)spad(lane, OUT + 4 * row);
        if (memcmp(&got, &want, sizeof got)) {
          if (!wrong) printf("row %u column %u: got %g, want %g\n", from + row, half * LANES + lane, (double)got, (double)want);
          wrong++;
        }
      }
  }
  checked += 2 * LANES * rows;
  return wrong;
}

static int refused(const char *which) {
  static float one[2][WIDTH];
  if (!strcmp(which, "pop")) {              // a vpop of what was never computed
    k_vpop(0, spad(0, OUT), 1, UPPER, 0);
  } else if (!strcmp(which, "matmul")) {    // a vmatmul with no vmatprep before it
    k_vmatmul(spad(0, IN), 0, 1, SINGLE, 0);
  } else if (!strcmp(which, "push")) {      // a vmatpush after the vmatprep of an input
    k_vmatprep(spad(0, IN), 0, 1, SINGLE | INPUTS, 0);
    k_vmatpush(spad(0, IN), 0, 1, SINGLE | PUSH1, 0);
  } else if (!strcmp(which, "rows")) {      // two words under the vmatprep, one under the vmatpush
    k_vmatprep(spad(0, IN), 0, 2, SINGLE, 0);
    k_vmatpush(spad(0, IN), 0, 1, SINGLE | PUSH1, 0);
  } else if (!strcmp(which, "weights")) {   // a vmatmul that latches before any weight
    operand(k_vmatmul, one, 2, BF16, INPUTS, LATCH);
  } else if (!strcmp(which, "latch")) {     // weights pushed, and no vmatmul has latched them
    operand(k_vmatpush, one, 2, BF16, 0, PUSH1);
    operand(k_vmatmul, one, 2, BF16, INPUTS, 0);
  } else if (!strcmp(which, "depth")) {     // a vmatpush whose field names no half of the depth
    operand(k_vmatpush, one, 2, BF16, 0, 0);
  } else if (!strcmp(which, "push3")) {     // both bits of [4:3]: the v6e's vmatpush3
    operand(k_vmatpush, one, 2, BF16, 0, PUSH1 | PUSH2);
  } else if (!strcmp(which, "prep-weights")) {  // a vmatprep of weights while one is held, untaken
    prep(one, 2, BF16, 0);
    prep(one, 2, BF16, INPUTS);
    prep(one, 2, BF16, 0);
  } else if (!strcmp(which, "prep-inputs")) {   // a vmatprep of inputs while one is held, untaken
    prep(one, 2, BF16, INPUTS);
    operand(k_vmatpush, one, 2, BF16, 0, PUSH1);
    prep(one, 2, BF16, INPUTS);
  } else if (!strcmp(which, "format")) {    // format 2, which names nothing
    k_vmatprep(spad(0, IN), 0, 1, 2, 0);
  } else if (!strcmp(which, "words")) {     // elements of 16 bits
    k_mxu_push_16(spad(0, IN), 0, 1, SINGLE, 0);
  } else if (!strcmp(which, "pop-array")) { // a vpop of array 1 on a machine of one array
    k_mxu_pop_32_a1(0, spad(0, OUT), 1, 0, 0);
  } else if (!strcmp(which, "array")) {     // a vmatmul for array 2 on a machine of two
    k_mxu_matmul_32_a2(spad(0, IN), 0, 1, SINGLE, 0);
  }
  puts("it ran");
  return 0;
}

// On a machine of two arrays: each its own tile, pushed by turns a slab at a time, so that what one array holds
// under a vmatprep the other does not take; both inputs' upper halves held before either vmatmul; and array 1's
// results popped before array 0's.
static int arrays(void) {
  enum { M = 8 };
  static float a[M][WIDTH], b[2][WIDTH][WIDTH];
  unsigned wrong = 0;
  for (unsigned row = 0; row < M; row++)
    for (unsigned k = 0; k < WIDTH; k++) a[row][k] = value();
  for (unsigned tile = 0; tile < 2; tile++)
    for (unsigned k = 0; k < WIDTH; k++)
      for (unsigned column = 0; column < WIDTH; column++) b[tile][k][column] = value();
  for (unsigned half = PUSH1; half <= PUSH2; half += PUSH1)
    for (unsigned at = LANES / SLAB; at-- > 0;) {
      float (*slab[2])[WIDTH];
      for (array_on = 0; array_on < 2; array_on++) {
        slab[array_on] = b[array_on] + (half == PUSH2 ? LANES : 0) + at * SLAB;
        prep(slab[array_on], SLAB, BF16, 0);           // both arrays' upper halves held, then taken the other way round
      }
      for (array_on = 2; array_on-- > 0;) {
        stage(slab[array_on], SLAB, half);
        bring(k_vmatpush, slab[array_on], SLAB, BF16, half);
      }
    }
  for (array_on = 0; array_on < 2; array_on++) prep(a, M, BF16, INPUTS);
  for (array_on = 0; array_on < 2; array_on++) {
    memcpy(active[array_on], staged[array_on], sizeof active[0]);
    bring(k_vmatmul, a, M, BF16, LATCH);
  }
  for (array_on = 2; array_on-- > 0;) wrong += check(a, 0, M);
  // The next input to array 1 alone, between pushes of a new slab to array 0 that it does not latch.
  array_on = 0;
  push(b[1] + LANES - SLAB, SLAB, BF16, PUSH1);
  array_on = 1;
  matmul(a + 2, 4, BF16, 0);
  wrong += check(a, 2, 4);
  array_on = 0;
  matmul(a + 4, 2, BF16, LATCH);
  wrong += check(a, 4, 2);
  printf("mxu arrays: %u of %u elements as the formula says\n", checked - wrong, checked);
  return wrong != 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "arrays")) return arrays();
  if (argc == 2) return refused(argv[1]);
  enum { M = 16 };
  static float a[M][WIDTH], low[M][WIDTH], b[3][WIDTH][WIDTH], slab[SLAB][WIDTH];
  unsigned wrong = 0;
  for (unsigned format = SINGLE; format <= BF16; format += format == HALF ? 2 : 1) {
    for (unsigned row = 0; row < M; row++)
      for (unsigned k = 0; k < WIDTH; k++) {
        a[row][k] = format == HALF ? ((int)random_below(129) - 64) / 16.0f : value();
        low[row][k] = k < LANES ? a[row][k] : 0;
      }
    for (unsigned tile = 0; tile < 3; tile++)
      for (unsigned k = 0; k < WIDTH; k++)
        for (unsigned column = 0; column < WIDTH; column++)
          b[tile][k][column] = format == HALF ? ((int)random_below(129) - 64) / 16.0f : value();
    for (unsigned k = 0; k < SLAB; k++)
      for (unsigned column = 0; column < WIDTH; column++)
        slab[k][column] = format == HALF ? ((int)random_below(129) - 64) / 16.0f : value();
    // A tile, both halves of the depth, which the first vmatmul latches.
    push_half(b[0], format, PUSH1);
    push_half(b[0], format, PUSH2);
    matmul(a, 2, format, LATCH);
    wrong += check(a, 0, 2);
    // The next tile, a slab at a time between vmatmuls that do not latch: they compute with the tile before.
    for (unsigned half = PUSH1; half <= PUSH2; half += PUSH1)
      for (unsigned at = LANES / SLAB; at-- > 0;) {
        push(b[1] + (half == PUSH2 ? LANES : 0) + at * SLAB, SLAB, format, half);
        matmul(a + 2, 2, format, 0);
        wrong += check(a, 2, 2);
      }
    // A vmatmul that latches: the next tile is the one computed with.
    matmul(a, M, format, LATCH);
    wrong += check(a, 0, M / 2);
    wrong += check(a, M / 2, M / 2);
    // A tile LANES deep, as the v6e's K = 128: only the first half of the depth pushed, and an input whose half
    // under the vmatprep is zeros, so that the tile before's weights the second half still holds take no part.
    push_half(b[2], format, PUSH1);
    matmul(low, 4, format, LATCH);
    wrong += check(low, 0, 4);
    // The second half alone: a slab moves its rows down, the deepest are gone, and the first half is as it was.
    push(slab, SLAB, format, PUSH2);
    matmul(a, 4, format, LATCH);
    wrong += check(a, 0, 4);
    // The two orders of the v6e's bundles (bundle: op). 43 vmatprep.subr / 44 vmatprep.mubr / 45 vmatpush2: the
    // inputs' upper half waits under the weights' pushes, here every push of the second half, until 64
    // vmatmul.mubr.vlgmr takes it. 63 vmatprep.subr / 64 vmatmul.vlgmr / 65 vmatpush1: the next tile's weights wait
    // under that vmatmul, which computes with the tile before, and the vmatpush1 after it takes them.
    push_half(b[0], format, PUSH1);
    for (unsigned at = LANES / SLAB; at-- > 0;) {
      prep(b[0] + LANES + at * SLAB, SLAB, format, 0);
      if (at == LANES / SLAB - 1) prep(a + 4, 2, format, INPUTS);
      stage(b[0] + LANES + at * SLAB, SLAB, PUSH2);
      bring(k_vmatpush, b[0] + LANES + at * SLAB, SLAB, format, PUSH2);
    }
    prep(b[2] + LANES - SLAB, SLAB, format, 0);
    memcpy(active[array_on], staged[array_on], sizeof active[0]);
    bring(k_vmatmul, a + 4, 2, format, LATCH);
    stage(b[2] + LANES - SLAB, SLAB, PUSH1);
    bring(k_vmatpush, b[2] + LANES - SLAB, SLAB, format, PUSH1);
    wrong += check(a, 4, 2);
    matmul(a + 6, 2, format, LATCH);
    wrong += check(a, 6, 2);
  }
  printf("mxu: %u of %u elements as the formula says\n", checked - wrong, checked);
  return wrong != 0;
}
