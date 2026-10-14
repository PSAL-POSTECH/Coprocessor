// The matrix unit of libtpuv3.so: an array of WIDTH x WIDTH, WIDTH the machine's lanes (LANES unless the build says),
// an operand one register, a word of one single or two rows of 16 bits, weights staged and latched, a result popped
// whole. Checked against a formula. Usage: mxu [what the model refuses]
#include "common.h"

#ifndef WIDTH
#define WIDTH LANES
#endif

// sf.vc.iv 0, 1 and 2 and sf.vc.v.i 2, of tests/tpuv6e/functional/kernels.S. The number is the instruction's field.
kernel_with k_mxu_matmul_32, k_mxu_push_32, k_mxu_prep_32, k_mxu_pop_32;

#define IN 0ul
#define OUT 4096ul
#define SLAB 2u
enum { SINGLE = 0, HALF = 1, BF16 = 3, UPPER = 4, LATCH = 4, PUSH1 = 8, PUSH2 = 16 };

// A float that a bfloat16 holds, of exponent 2^-30 to 2^30: every product of two is exact.
static float value(void) {
  const uint32_t bits = (random32() & 0x8000u) << 16 | (97 + random_below(61)) << 23 | (random32() & 0x7fu) << 16;
  float made;
  memcpy(&made, &bits, sizeof made);
  return made;
}
// A number of the format, every product of two exact: a bfloat16's for singles and bfloat16, sixteenths for halves.
static float number(unsigned format) { return format == HALF ? ((int)random_below(129) - 64) / 16.0f : value(); }
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
// `rows` rows of an operand, a column per lane, as the words of `format`, under the kernel with `field`.
static void operand(kernel_with *kernel, float (*matrix)[WIDTH], unsigned rows, unsigned format, unsigned field) {
  const unsigned long words = format == SINGLE ? rows : rows / 2;
  for (unsigned lane = 0; lane < WIDTH; lane++)
    for (unsigned row = 0; row < rows; row++) {
      const uint32_t bits = bits_of(matrix[row][lane]);
      if (format == SINGLE) *(uint32_t *)spad(lane, IN + 4 * row) = bits;
      else if (format == BF16) *(uint16_t *)spad(lane, IN + 2 * row) = (uint16_t)(bits >> 16);
      else *(uint16_t *)spad(lane, IN + 2 * row) = half_of(bits);
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
// A tile a slab at a time, the deepest first, so that the depth holds the tile's rows as they are.
static void push_tile(float (*tile)[WIDTH], unsigned format) {
  for (unsigned slab = WIDTH / SLAB; slab-- > 0;) push(tile + slab * SLAB, SLAB, format);
}
static void matmul(float (*a)[WIDTH], unsigned rows, unsigned format, unsigned latch) {
  if (latch) memcpy(active, staged, sizeof active);
  operand(k_mxu_matmul_32, a, rows, format, latch);
}
// `rows` rows of the result against a x the active weights.
static unsigned checked;
static unsigned check(float (*a)[WIDTH], unsigned from, unsigned rows) {
  unsigned wrong = 0;
  if (k_mxu_pop_32(0, spad(0, OUT), rows, 0, 0) != rows) puts("a vpop did not take every row");
  for (unsigned lane = 0; lane < WIDTH; lane++)
    for (unsigned row = 0; row < rows; row++) {
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
// with the one before; a vmatmul that latches it, popped in two; a slab more, which moves the rows down.
int main(int argc, char **argv) {
  if (argc == 2) return refused(argv[1]);
  enum { M = 16 };
  static float a[M][WIDTH], b[2][WIDTH][WIDTH], slab[SLAB][WIDTH];
  unsigned wrong = 0;
  for (unsigned format = SINGLE; format <= BF16; format += format == HALF ? 2 : 1) {
    for (unsigned row = 0; row < M; row++)
      for (unsigned k = 0; k < WIDTH; k++) a[row][k] = number(format);
    for (unsigned tile = 0; tile < 2; tile++)
      for (unsigned k = 0; k < WIDTH; k++)
        for (unsigned column = 0; column < WIDTH; column++) b[tile][k][column] = number(format);
    for (unsigned k = 0; k < SLAB; k++)
      for (unsigned column = 0; column < WIDTH; column++) slab[k][column] = number(format);
    push_tile(b[0], format);
    matmul(a, 2, format, LATCH);
    wrong += check(a, 0, 2);
    for (unsigned at = WIDTH / SLAB; at-- > 0;) {
      push(b[1] + at * SLAB, SLAB, format);
      matmul(a + 2, 2, format, 0);
      wrong += check(a, 2, 2);
    }
    matmul(a, M, format, LATCH);
    wrong += check(a, 0, M / 2);
    wrong += check(a, M / 2, M / 2);
    push(slab, SLAB, format);
    matmul(a + 4, 4, format, LATCH);
    wrong += check(a, 4, 4);
  }
  printf("mxu: %u of %u elements as the formula says\n", checked - wrong, checked);
  return wrong != 0;
}
