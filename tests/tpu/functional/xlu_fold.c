// The cross-lane unit's fold (xlu_push_reduce): tiles of f32 pushed in one or two pieces, folded across the
// lanes by max, sum or min, popped whole; argmax and argmin of f32; max and min of bf16. Each column's answer is checked in every lane against the same fold done
// here, lane after lane. Output: one line per case, and "xlu_fold: all pass" at the end when every case did.
#include <math.h>

#include "common.h"

kernel k_cross_fold_max, k_cross_fold_sum, k_cross_fold_min, k_cross_fold_argmax, k_cross_fold_argmin,
    k_cross_fold_max_16, k_cross_fold_min_16, k_cross_pop_16, k_cross_pop_32;

enum { MAX, SUM, MIN, ARGMAX, ARGMIN };

#define IN 0ul
#define OUT 8192ul

static int failed;

static void one(const char *what, kernel *fold, int kind, unsigned long first, unsigned long second) {
  const unsigned long depth = first + second;
  float best[64];
  uint32_t want[64];
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long k = 0; k < depth; k++) {
      const float x = (float)((int32_t)random32() % 2001) / 64.0f;
      *(float *)spad(lane, IN + 4 * k) = x;
      const int first_lane = lane == 0;
      const float b = best[k];
      if (first_lane || (kind == SUM) || (kind == MAX && x > b) || (kind == MIN && x < b) ||
          (kind == ARGMAX && x > b) || (kind == ARGMIN && x < b)) {
        best[k] = first_lane ? x : kind == SUM ? b + x : x;
        if (kind == ARGMAX || kind == ARGMIN) want[k] = first_lane || x != b ? lane : want[k];
      }
      if (kind != ARGMAX && kind != ARGMIN) memcpy(&want[k], &best[k], 4);
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

// max and min of bf16 (e16): each column's answer, of 16 bits, is the bf16 of the max or min of the bf16 values.
static void bf16(const char *what, kernel *fold, int kind, unsigned long depth) {
  uint16_t want[64];
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long k = 0; k < depth; k++) {
      const float f = (float)((int32_t)random32() % 2001) / 64.0f;
      uint32_t bits;
      memcpy(&bits, &f, 4);
      const uint16_t x = (uint16_t)(bits >> 16);
      *(uint16_t *)spad(lane, IN + 2 * k) = x;
      float xv, wv;
      bits = (uint32_t)x << 16, memcpy(&xv, &bits, 4);
      bits = (uint32_t)want[k] << 16, memcpy(&wv, &bits, 4);
      if (lane == 0 || (kind == MAX ? xv > wv : xv < wv)) want[k] = x;
    }
  if (fold(spad(0, IN), 0, depth) != depth) puts("a push did not take every element");
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 2 * depth);
  if (k_cross_pop_16(0, spad(0, OUT), depth) != depth) puts("a pop did not take every element");
  int bad = 0;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long k = 0; k < depth; k++)
      if (memcmp(spad(lane, OUT + 2 * k), &want[k], 2)) bad++;
  printf("%s  %s, %lu columns: %d wrong\n", bad ? "FAIL" : "PASS", what, depth, bad);
  failed |= bad != 0;
}

// argmax and argmin: the first lane holding the answer when two lanes tie, and a NaN's lane over any number.
static void arg_ties(void) {
  const unsigned long depth = 3;
  for (unsigned lane = 0; lane < LANES; lane++) {
    float *x = (float *)spad(lane, IN);
    x[0] = 7.0f;                                   // every lane ties: lane 0
    x[1] = lane == 1 || lane == 3 ? 9.0f : 1.0f;   // lanes 1 and 3 tie at the max: 1; the min at lane 0
    x[2] = lane == 2 ? NAN : (float)lane;          // a NaN in lane 2: 2 for both
  }
  const uint32_t want_max[3] = {0, 1, 2}, want_min[3] = {0, 0, 2};
  int bad = 0;
  for (int which = 0; which < 2; which++) {
    if ((which ? k_cross_fold_argmin : k_cross_fold_argmax)(spad(0, IN), 0, depth) != depth)
      puts("a push did not take every element");
    for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 4 * depth);
    if (k_cross_pop_32(0, spad(0, OUT), depth) != depth) puts("a pop did not take every element");
    for (unsigned lane = 0; lane < LANES; lane++)
      bad += memcmp(spad(lane, OUT), which ? want_min : want_max, 4 * depth) != 0;
  }
  printf("%s  argmax and argmin of ties and a NaN, 3 columns: %d wrong\n", bad ? "FAIL" : "PASS", bad);
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
  one("max", k_cross_fold_max, MAX, 8, 0);
  one("sum", k_cross_fold_sum, SUM, 8, 0);
  one("max", k_cross_fold_max, MAX, 5, 11);
  one("sum", k_cross_fold_sum, SUM, 16, 16);
  one("sum", k_cross_fold_sum, SUM, 1, 0);
  one("min", k_cross_fold_min, MIN, 8, 0);
  one("min", k_cross_fold_min, MIN, 5, 11);
  one("argmax", k_cross_fold_argmax, ARGMAX, 8, 0);
  one("argmin", k_cross_fold_argmin, ARGMIN, 16, 16);
  bf16("bf16 max", k_cross_fold_max_16, MAX, 16);
  bf16("bf16 min", k_cross_fold_min_16, MIN, 32);
  arg_ties();
  special();
  puts(failed ? "xlu_fold: some fail" : "xlu_fold: all pass");
  return failed;
}
