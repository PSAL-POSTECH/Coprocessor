// On Spike: sum and max of vl elements across the lanes, vl = 1 .. 8, against the same tree in C, bit for bit, in
// every lane. Even elements of the sum are 1e8, 1, -1e8, 1, ... across the lanes, where adding lane after lane differs.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LANE_BYTES (512ul * 1024)
#define SPAD_BASE 0xD0000000ul
#define IN 0ul
#define OUT 4096ul

unsigned long k_sum(void *source, void *destination, unsigned long elements);
unsigned long k_max(void *source, void *destination, unsigned long elements);

static float *spad(unsigned lane, unsigned long offset) { return (float *)(SPAD_BASE + lane * LANE_BYTES + offset); }
static uint32_t state = 0x2545F491u;
static float random_value(void) {
  state ^= state << 13, state ^= state >> 17, state ^= state << 5;
  return (float)((int32_t)(state % 2001) - 1000) / 64.0f;
}

static float tree(const float *x, int n, int is_max) {
  float level[64];
  memcpy(level, x, sizeof(float) * n);
  while (n > 1) {
    int m = 0;
    for (int k = 0; k + 1 < n; k += 2) level[m++] = is_max ? (level[k + 1] > level[k] ? level[k + 1] : level[k]) : level[k] + level[k + 1];
    if (n % 2) level[m++] = level[n - 1];
    n = m;
  }
  return level[0];
}

int main(void) {
  static const float pattern[4] = {1.0e8f, 1.0f, -1.0e8f, 1.0f};
  int failed = 0;
  for (int is_max = 0; is_max < 2; is_max++)
    for (unsigned long vl = 1; vl <= 8; vl++) {
      float want[8], left[8];
      for (unsigned long i = 0; i < vl; i++) {
        float x[LANES];
        for (unsigned lane = 0; lane < LANES; lane++) {
          x[lane] = !is_max && i % 2 == 0 ? pattern[lane % 4] : random_value();
          spad(lane, IN)[i] = x[lane];
        }
        want[i] = tree(x, LANES, is_max);
        left[i] = x[0];
        for (unsigned lane = 1; lane < LANES; lane++) left[i] = is_max ? (x[lane] > left[i] ? x[lane] : left[i]) : left[i] + x[lane];
      }
      for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 32);
      (is_max ? k_max : k_sum)(spad(0, IN), spad(0, OUT), vl);
      int bad = 0;
      for (unsigned lane = 0; lane < LANES; lane++) bad += memcmp(spad(lane, OUT), want, 4 * vl) != 0;
      failed |= bad != 0;
      printf("%s  %s vl %lu: r[0] model %g, tree in C %g, lane after lane %g; %d of %u lanes wrong\n", bad ? "FAIL" : "PASS",
             is_max ? "max" : "sum", vl, spad(0, OUT)[0], want[0], left[0], bad, LANES);
    }
  return failed;
}
