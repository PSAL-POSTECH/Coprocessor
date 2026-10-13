// The matrix unit of libtpu_mxu.so: an array of 2 x LANES columns, an operand as two registers (a vmatprep, then
// the vmatpush or the vmatmul), a word of one single or two rows of 16 bits, a result popped a half of the columns
// at a time. Checked against a formula. Usage: mxu [what the model refuses]
#include "common.h"

// sf.vc.iv 0, 1 and 2 and sf.vc.v.i 2: the encodings the matrix unit has. The number is the instruction's field.
kernel_with k_input_32, k_weight_32, k_msa_push_32, k_mxu_pop_32, k_weight_16;
#define k_vmatmul k_input_32
#define k_vmatpush k_weight_32
#define k_vmatprep k_msa_push_32
#define k_vpop k_mxu_pop_32

#define IN 0ul
#define OUT 4096ul
#define WIDTH (2 * LANES)
enum { SINGLE = 0, HALF = 1, BF16 = 3, INPUTS = 4, UPPER = 4 };

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
// An operand: its upper half under a vmatprep, then its lower half.
static void operand(kernel_with *lower, float (*matrix)[WIDTH], unsigned rows, unsigned format, unsigned inputs) {
  const unsigned long words = format == SINGLE ? rows : rows / 2;
  place(matrix, rows, LANES, format);
  if (k_vmatprep(spad(0, IN), 0, words, format | inputs, 0) != words) puts("a vmatprep did not take every word");
  place(matrix, rows, 0, format);
  if (lower(spad(0, IN), 0, words, format, 0) != words) puts("an operand did not take every word");
}
// Every row of weights pushed, in order: the array holds the last WIDTH of them, whatever their words were.
static float pushed[3 * (WIDTH + 2)][WIDTH];
static unsigned weights;
static void push_weights(float (*rows)[WIDTH], unsigned count, unsigned format) {
  memcpy(pushed[weights], rows, count * sizeof *rows);
  weights += count;
  operand(k_vmatpush, rows, count, format, 0);
}
// `rows` rows of the result, the lower half of the columns and then the upper, against a x the weights the array holds.
static unsigned check(float (*a)[WIDTH], unsigned from, unsigned rows) {
  float (*b)[WIDTH] = pushed;
  unsigned wrong = 0;
  for (unsigned half = 0; half < 2; half++) {
    if (k_vpop(0, spad(0, OUT), rows, half ? UPPER : 0, 0) != rows) puts("a vpop did not take every row");
    for (unsigned lane = 0; lane < LANES; lane++)
      for (unsigned row = 0; row < rows; row++) {
        const unsigned kept = weights < WIDTH ? weights : WIDTH;
        float want = 0;
        for (unsigned k = 0; k < kept; k++) want += a[from + row][k] * b[weights - kept + k][half * LANES + lane];
        const float got = *(float *)spad(lane, OUT + 4 * row);
        if (memcmp(&got, &want, sizeof got)) {
          if (!wrong) printf("row %u column %u: got %g, want %g\n", from + row, half * LANES + lane, (double)got, (double)want);
          wrong++;
        }
      }
  }
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
    k_vmatpush(spad(0, IN), 0, 1, SINGLE, 0);
  } else if (!strcmp(which, "rows")) {      // two words under the vmatprep, one under the vmatpush
    k_vmatprep(spad(0, IN), 0, 2, SINGLE, 0);
    k_vmatpush(spad(0, IN), 0, 1, SINGLE, 0);
  } else if (!strcmp(which, "weights")) {   // an input before any weight
    operand(k_vmatmul, one, 2, BF16, INPUTS);
  } else if (!strcmp(which, "format")) {    // format 2, which names nothing
    k_vmatprep(spad(0, IN), 0, 1, 2, 0);
  } else if (!strcmp(which, "words")) {     // elements of 16 bits
    k_weight_16(spad(0, IN), 0, 1, SINGLE, 0);
  }
  puts("it ran");
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2) return refused(argv[1]);
  enum { M = 16 };
  static float a[M][WIDTH], b[WIDTH + 2][WIDTH];
  unsigned wrong = 0;
  for (unsigned format = SINGLE; format <= BF16; format += format == HALF ? 2 : 1) {
    for (unsigned row = 0; row < M; row++)
      for (unsigned k = 0; k < WIDTH; k++) a[row][k] = format == HALF ? ((int)random_below(129) - 64) / 16.0f : value();
    for (unsigned k = 0; k < WIDTH + 2; k++)
      for (unsigned column = 0; column < WIDTH; column++)
        b[k][column] = format == HALF ? ((int)random_below(129) - 64) / 16.0f : value();
    // Two rows of weights (the first time, fewer than the array is deep), then a full array of them, then two
    // more: the oldest are gone, and what an earlier format left is weights like any other.
    push_weights(b, 2, format);
    operand(k_vmatmul, a, 2, format, INPUTS);
    wrong += check(a, 0, 2);
    push_weights(b + 2, WIDTH - 2, format);
    operand(k_vmatmul, a, M, format, INPUTS);
    wrong += check(a, 0, M / 2);
    wrong += check(a, M / 2, M / 2);
    push_weights(b + WIDTH, 2, format);
    operand(k_vmatmul, a, 4, format, INPUTS);
    wrong += check(a, 0, 4);
  }
  const unsigned all = 3 * WIDTH * (2 + M + 4);
  printf("mxu: %u of %u elements as the formula says\n", all - wrong, all);
  return wrong != 0;
}
