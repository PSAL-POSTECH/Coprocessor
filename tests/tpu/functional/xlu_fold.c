// The cross-lane unit's fold (xlu_push_reduce): tiles of f32 pushed in one or two pieces, folded across the
// lanes by max or by sum, popped whole. Each column's answer is checked in every lane against the same fold done
// here, lane after lane. Output: one line per case, and "xlu_fold: all pass" at the end when every case did.
#include <math.h>

#include "common.h"

kernel k_cross_fold_max, k_cross_fold_sum, k_cross_pop_32;

#define IN 0ul
#define OUT 8192ul

static int failed;

static void one(const char *what, kernel *fold, int sum, unsigned long first, unsigned long second) {
  const unsigned long depth = first + second;
  float want[64];
  for (unsigned long k = 0; k < depth; k++) want[k] = 0.0f;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long k = 0; k < depth; k++) {
      const float x = (float)((int32_t)random32() % 2001) / 64.0f;
      *(float *)spad(lane, IN + 4 * k) = x;
      want[k] = lane == 0 ? x : (sum ? want[k] + x : fmaxf(want[k], x));
    }
  if (fold(spad(0, IN), 0, first) != first) puts("a push did not take every element");
  if (second && fold(spad(0, IN + 4 * first), 0, second) != second) puts("a push did not take every element");
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 4 * depth);
  if (k_cross_pop_32(0, spad(0, OUT), depth) != depth) puts("a pop did not take every element");
  int bad = 0;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long k = 0; k < depth; k++)
      if (memcmp(spad(lane, OUT + 4 * k), &want[k], 4)) bad++;
  printf("%s  %s, %lu + %lu columns: %d wrong\n", bad ? "FAIL" : "PASS", what, first, second, bad);
  failed |= bad != 0;
}

// max as the v6e's vmax.f32: a NaN in any lane is the column's answer (in the first lane or the last), and -0 is
// below +0 (a column of -0 with one +0 is +0, a column of -0 alone is -0).
static void special(void) {
  const unsigned long depth = 4;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long k = 0; k < depth; k++) {
      float x = k < 2 ? (float)((int32_t)random32() % 2001) / 64.0f : -0.0f;
      if ((k == 0 && lane == LANES - 1) || (k == 1 && lane == 0)) x = NAN;
      if (k == 2 && lane == LANES / 2) x = 0.0f;
      *(float *)spad(lane, IN + 4 * k) = x;
    }
  if (k_cross_fold_max(spad(0, IN), 0, depth) != depth) puts("a push did not take every element");
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 4 * depth);
  if (k_cross_pop_32(0, spad(0, OUT), depth) != depth) puts("a pop did not take every element");
  int bad = 0;
  for (unsigned lane = 0; lane < LANES; lane++) {
    const float *got = (const float *)spad(lane, OUT);
    bad += !isnan(got[0]) + !isnan(got[1]);
    bad += got[2] != 0.0f || signbit(got[2]);
    bad += got[3] != 0.0f || !signbit(got[3]);
  }
  printf("%s  max of NaN and signed zeros, 4 columns: %d wrong\n", bad ? "FAIL" : "PASS", bad);
  failed |= bad != 0;
}

int main(void) {
  one("max", k_cross_fold_max, 0, 8, 0);
  one("sum", k_cross_fold_sum, 1, 8, 0);
  one("max", k_cross_fold_max, 0, 5, 11);
  one("sum", k_cross_fold_sum, 1, 16, 16);
  one("sum", k_cross_fold_sum, 1, 1, 0);
  special();
  puts(failed ? "xlu_fold: some fail" : "xlu_fold: all pass");
  return failed;
}
