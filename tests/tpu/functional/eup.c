// The EUP: two groups pushed, then popped in push order; each element 2^x of what was pushed. x is a whole
// number, so 2^x is exact and the check needs no tolerance. Exit 0 and "eup ok" iff every element of every lane is.
#include <math.h>

#include "common.h"

kernel k_vlmax_32, k_eup_push_32, k_eup_pop_32;

#define A 0ul
#define B 4096ul
#define OUT_A (64ul * 1024)
#define OUT_B (68ul * 1024)

int main(void) {
  const unsigned long n = k_vlmax_32(0, 0, 1u << 20);  // one group of eight registers
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < n; i++) {
      ((float *)spad(lane, A))[i] = (float)((long)(i + lane) % 61 - 30);
      ((float *)spad(lane, B))[i] = (float)((long)(3 * i + lane) % 41 - 20);
    }
  k_eup_push_32(spad(0, A), 0, n);
  k_eup_push_32(spad(0, B), 0, n);
  k_eup_pop_32(0, spad(0, OUT_A), n);
  k_eup_pop_32(0, spad(0, OUT_B), n);
  unsigned long wrong = 0;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < n; i++) {
      wrong += ((float *)spad(lane, OUT_A))[i] != ldexpf(1.0f, (int)((float *)spad(lane, A))[i]);
      wrong += ((float *)spad(lane, OUT_B))[i] != ldexpf(1.0f, (int)((float *)spad(lane, B))[i]);
    }
  if (wrong) {
    printf("eup: %lu of %lu elements wrong\n", wrong, 2 * n * LANES);
    return 1;
  }
  printf("eup ok: %lu elements a group, %u lanes\n", n, LANES);
  return 0;
}
