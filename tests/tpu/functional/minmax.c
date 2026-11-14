// The f32 minimum and maximum on every lane, at LMUL 1 and 8: 4096 pairs a lane, listed values (NaNs of either sign
// and payload, both zeros, the infinities, the ends of the range, subnormals) against each other, then any bits.
// Checked against IEEE 754-2019 minimum and maximum: a NaN if either is one (the quiet 0x7FC00000), -0 below +0.
// Output: the count of pairs and of wrong ones; exit 1 on any wrong one.
#include "common.h"

// Each kernel's vs2 and vs1 elements, where it writes, and how many: it returns the vl it took.
typedef unsigned long pair_kernel(void *a, void *b, void *out, unsigned long elements);
pair_kernel k_extremes_m1, k_extremes_m8;

#define A 0ul
#define B (64ul * 1024)
#define OUT (128ul * 1024)
#define PAIRS 4096ul

static const uint32_t LISTED[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000,
    0x7f800001, 0xff800001, 0x7fa00000, 0x7f7fffff, 0xff7fffff, 0x00000001, 0x80000001, 0x007fffff,
    0x00800000, 0x40490fdb, 0xc0490fdb, 0x3f800001,
};

static int nan_bits(uint32_t u) { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu); }
static float as_float(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

static uint32_t want(uint32_t a, uint32_t b, int greatest) {
  if (nan_bits(a) || nan_bits(b)) return 0x7FC00000u;
  const float x = as_float(a), y = as_float(b);
  if (x == y) {
    if (x != 0.0f) return a;
    const int neg = (a >> 31) || (b >> 31), both = (a >> 31) && (b >> 31);
    return (greatest ? both : neg) ? 0x80000000u : 0u;
  }
  return (x > y) == greatest ? a : b;
}

static int check(pair_kernel *k, const char *what) {
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 2 * PAIRS * 4);
  unsigned long done = 0;
  while (done < PAIRS) {
    const unsigned long vl = k(spad(0, A + 4 * done), spad(0, B + 4 * done), spad(0, OUT + 8 * done), PAIRS - done);
    unsigned long wrong = 0;
    for (unsigned lane = 0; lane < LANES; lane++)
      for (unsigned long i = 0; i < vl; i++) {
        const uint32_t a = *(uint32_t *)spad(lane, A + 4 * (done + i)), b = *(uint32_t *)spad(lane, B + 4 * (done + i));
        if (*(uint32_t *)spad(lane, OUT + 8 * done + 4 * i) != want(a, b, 0)) wrong++;
        if (*(uint32_t *)spad(lane, OUT + 8 * done + 4 * (vl + i)) != want(a, b, 1)) wrong++;
      }
    if (wrong) { printf("%s: %lu wrong of %lu pairs at pair %lu\n", what, wrong, vl * LANES, done); return 1; }
    done += vl;
  }
  printf("%s: %lu pairs a lane, 0 wrong\n", what, done);
  return 0;
}

int main(void) {
  const unsigned listed = sizeof LISTED / sizeof LISTED[0];
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < PAIRS; i++) {
      const int both = i < listed * listed;
      *(uint32_t *)spad(lane, A + 4 * i) = both ? LISTED[(i / listed + lane) % listed] : random32();
      *(uint32_t *)spad(lane, B + 4 * i) = both ? LISTED[i % listed] : random32();
    }
  return check(k_extremes_m1, "minimum, maximum (m1)") | check(k_extremes_m8, "minimum, maximum (m8)");
}
