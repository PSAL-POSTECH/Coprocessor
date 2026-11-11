// The matrix unit of libtpuv3.so: an array of WIDTH x WIDTH, WIDTH the machine's lanes (LANES unless the build says),
// an operand one register, a word of one single, two bfloat16 rows or four int8 rows, weights staged and latched, a
// result popped whole, int8's as int32. Checked against a formula. Usage: mxu [what the model refuses]
#include "common.h"

#ifndef WIDTH
#define WIDTH LANES
#endif

// sf.vc.iv 0, 1 and 2 and sf.vc.v.i 2, of tests/tpuv6e/functional/kernels.S. The number is the instruction's field.
kernel_with k_mxu_matmul_32, k_mxu_push_32, k_mxu_prep_32, k_mxu_pop_32;

#define IN 0ul
#define OUT 4096ul
#define SLAB 2u
enum { SINGLE = 0, INT8 = 1, BF16 = 3, UPPER = 4, LATCH = 4, PUSH1 = 8, PUSH2 = 16 };

// A float that a bfloat16 holds, of exponent 2^-30 to 2^30: every product of two is exact.
static float value(void) {
  const uint32_t bits = (random32() & 0x8000u) << 16 | (97 + random_below(61)) << 23 | (random32() & 0x7fu) << 16;
  float made;
  memcpy(&made, &bits, sizeof made);
  return made;
}
// A number of the format, every product of two exact: a bfloat16's for singles and bfloat16, an int8 for int8.
static float number(unsigned format) { return format == INT8 ? (float)((int)random_below(256) - 128) : value(); }
static uint32_t bits_of(float made) {
  uint32_t bits;
  memcpy(&bits, &made, sizeof bits);
  return bits;
}
// `rows` rows of an operand, a column per lane, as the words of `format`, under the kernel with `field`.
static void operand(kernel_with *kernel, float (*matrix)[WIDTH], unsigned rows, unsigned format, unsigned field) {
  const unsigned long words = format == SINGLE ? rows : format == INT8 ? rows / 4 : rows / 2;
  for (unsigned lane = 0; lane < WIDTH; lane++)
    for (unsigned row = 0; row < rows; row++) {
      const uint32_t bits = bits_of(matrix[row][lane]);
      if (format == SINGLE) *(uint32_t *)spad(lane, IN + 4 * row) = bits;
      else if (format == INT8) *(int8_t *)spad(lane, IN + row) = (int8_t)matrix[row][lane];
      else *(uint16_t *)spad(lane, IN + 2 * row) = (uint16_t)(bits >> 16);
    }
  if (kernel(spad(0, IN), 0, words, format | field, 0) != words) puts("an operand did not take every word");
}

// The staged weights, a weight at each depth of each column, and the active ones, which a vmatmul that latches copied.
static float staged[WIDTH][WIDTH], active[WIDTH][WIDTH];
// `count` rows of weights: what the depth held moves down `count`, what passes its last is gone, the rows take its top.
static void push(float (*rows)[WIDTH], unsigned count, unsigned format) {
  memmove(staged + count, staged, (WIDTH - count) * sizeof *staged);
  memcpy(staged, rows, count * sizeof *rows);
  operand(k_mxu_push_32, rows, count, format, PUSH1);
}
// A tile `step` rows at a time, the deepest first, so that the depth holds the tile's rows as they are.
static void push_tile(float (*tile)[WIDTH], unsigned format, unsigned step) {
  for (unsigned slab = WIDTH / step; slab-- > 0;) push(tile + slab * step, step, format);
}
static void matmul(float (*a)[WIDTH], unsigned rows, unsigned format, unsigned latch) {
  if (latch) memcpy(active, staged, sizeof active);
  operand(k_mxu_matmul_32, a, rows, format, latch);
}
// `rows` rows of the result against a x the active weights: singles, or of int8 inputs int32 (a vpop of format 1).
static unsigned checked;
static unsigned check(float (*a)[WIDTH], unsigned from, unsigned rows, unsigned format) {
  unsigned wrong = 0;
  if (k_mxu_pop_32(0, spad(0, OUT), rows, format == INT8 ? INT8 : 0, 0) != rows) puts("a vpop did not take every row");
  for (unsigned lane = 0; lane < WIDTH; lane++)
    for (unsigned row = 0; row < rows; row++) {
      if (format == INT8) {
        int32_t want = 0;
        for (unsigned k = 0; k < WIDTH; k++) want += (int32_t)a[from + row][k] * (int32_t)active[k][lane];
        const int32_t got = *(int32_t *)spad(lane, OUT + 4 * row);
        if (got != want) {
          if (!wrong) printf("row %u column %u: got %d, want %d\n", from + row, lane, (int)got, (int)want);
          wrong++;
        }
        continue;
      }
      float want = 0;
      for (unsigned k = 0; k < WIDTH; k++) want += a[from + row][k] * active[k][lane];
      const float got = *(float *)spad(lane, OUT + 4 * row);
      if (memcmp(&got, &want, sizeof got)) {
        if (!wrong) printf("row %u column %u: got %g, want %g\n", from + row, lane, (double)got, (double)want);
        wrong++;
      }
    }
  checked += WIDTH * rows;
  return wrong;
}

// What an operand of one register has not: a vmatprep, a second half of the depth, an upper half of the columns.
static int refused(const char *which) {
  static float one[2][WIDTH];
  if (!strcmp(which, "prep")) {
    k_mxu_prep_32(spad(0, IN), 0, 1, SINGLE, 0);
  } else if (!strcmp(which, "push2")) {
    operand(k_mxu_push_32, one, 2, BF16, PUSH2);
  } else if (!strcmp(which, "pop-upper")) {
    operand(k_mxu_push_32, one, 2, BF16, PUSH1);
    operand(k_mxu_matmul_32, one, 2, BF16, LATCH);
    k_mxu_pop_32(0, spad(0, OUT), 1, UPPER, 0);
  }
  puts("it ran");
  return 0;
}

// In each format: a tile, which the first vmatmul latches; the next a slab at a time between vmatmuls that compute
// with the one before; a vmatmul that latches it, popped in two; a slab more, which moves the rows down. A slab and an
// input are two rows, four of int8, a word's.
int main(int argc, char **argv) {
  if (argc == 2) return refused(argv[1]);
  enum { M = 16 };
  static float a[M][WIDTH], b[2][WIDTH][WIDTH], slab[4][WIDTH];
  unsigned wrong = 0;
  for (unsigned format = SINGLE; format <= BF16; format += format == INT8 ? 2 : 1) {
    const unsigned step = format == INT8 ? 4 : SLAB;
    for (unsigned row = 0; row < M; row++)
      for (unsigned k = 0; k < WIDTH; k++) a[row][k] = number(format);
    for (unsigned tile = 0; tile < 2; tile++)
      for (unsigned k = 0; k < WIDTH; k++)
        for (unsigned column = 0; column < WIDTH; column++) b[tile][k][column] = number(format);
    for (unsigned k = 0; k < step; k++)
      for (unsigned column = 0; column < WIDTH; column++) slab[k][column] = number(format);
    push_tile(b[0], format, step);
    matmul(a, step, format, LATCH);
    wrong += check(a, 0, step, format);
    for (unsigned at = WIDTH / step; at-- > 0;) {
      push(b[1] + at * step, step, format);
      matmul(a + step, step, format, 0);
      wrong += check(a, step, step, format);
    }
    matmul(a, M, format, LATCH);
    wrong += check(a, 0, M / 2, format);
    wrong += check(a, M / 2, M / 2, format);
    push(slab, step, format);
    matmul(a + 4, 4, format, LATCH);
    wrong += check(a, 4, 4, format);
  }
  printf("mxu: %u of %u elements as the formula says\n", checked - wrong, checked);
  return wrong != 0;
}
