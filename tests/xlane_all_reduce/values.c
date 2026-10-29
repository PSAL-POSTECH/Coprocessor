// On Spike: sum of vl elements across the lanes, vl = 1 .. 8, against the same tree in C, bit for bit, in
// every lane. Even elements of the sum are 1e8, 8, -1e8, 1, ... across the lanes, where adding lane after lane differs.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LANE_BYTES (512ul * 1024)
#define SPAD_BASE 0xD0000000ul
#define IN 0ul
#define OUT 4096ul

unsigned long k_sum(void *source, void *destination, unsigned long elements);

static float *spad(unsigned lane, unsigned long offset) { return (float *)(SPAD_BASE + lane * LANE_BYTES + offset); }
static uint32_t state = 0x2545F491u;
static float random_value(void) {
  state ^= state << 13, state ^= state >> 17, state ^= state << 5;
  return (float)((int32_t)(state % 2001) - 1000) / 64.0f;
}

static float tree(const float *x, int n) {
  float level[LANES];
  memcpy(level, x, sizeof(float) * n);
  while (n > 1) {
    int m = 0;
    for (int k = 0; k + 1 < n; k += 2) level[m++] = level[k] + level[k + 1];
    if (n % 2) level[m++] = level[n - 1];
    n = m;
  }
  return level[0];
}

int main(void) {
  static const float pattern[4] = {1.0e8f, 8.0f, -1.0e8f, 1.0f};
  int failed = 0;
  for (unsigned long vl = 1; vl <= 8; vl++) {
    float want[8];
    for (unsigned long i = 0; i < vl; i++) {
      float x[LANES];
      for (unsigned lane = 0; lane < LANES; lane++) {
        x[lane] = i % 2 == 0 ? pattern[lane % 4] : random_value();
        spad(lane, IN)[i] = x[lane];
      }
      want[i] = tree(x, LANES);
    }
    for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 32);
    k_sum(spad(0, IN), spad(0, OUT), vl);
    int bad = 0;
    for (unsigned lane = 0; lane < LANES; lane++) bad += memcmp(spad(lane, OUT), want, 4 * vl) != 0;
    failed |= bad != 0;
    if (bad)
      printf("FAIL  sum vl %lu: %d of %u lanes differ from reference\n", vl, bad, LANES);
    else
      printf("PASS  sum vl %lu: r[%lu] %g, reference %g\n", vl, vl - 1, spad(0, OUT)[vl - 1], want[vl - 1]);
  }
  return failed;
}
