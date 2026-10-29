// The EUP: two groups pushed, then popped in push order; each element 2^x of what was pushed, exact for a whole x. Then a
// push of each other function (field 1..6) against the same function's sf.vc.v.iv, bit for bit. Exit 0 and "eup ok" iff
// every element of every lane is.
#include <math.h>

#include "common.h"

kernel k_vlmax_32, k_eup_push_32, k_eup_pop_32, k_eup_push_1_32, k_eup_push_2_32, k_eup_push_3_32, k_eup_push_4_32,
    k_eup_push_5_32, k_eup_push_6_32, k_verf_32, k_vtanh_32, k_vsin_32, k_vcos_32, k_vlog_32, k_vatan_32;
static kernel *const PUSH[] = {k_eup_push_1_32, k_eup_push_2_32, k_eup_push_3_32, k_eup_push_4_32, k_eup_push_5_32,
                               k_eup_push_6_32};
static kernel *const SYNC[] = {k_verf_32, k_vtanh_32, k_vsin_32, k_vcos_32, k_vlog_32, k_vatan_32};

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
  for (unsigned f = 0; f < 6; f++) {
    for (unsigned lane = 0; lane < LANES; lane++)
      for (unsigned long i = 0; i < n; i++) ((float *)spad(lane, A))[i] = 0.0371f * (float)((i * 7 + lane) % 97) + 0.01f;
    PUSH[f](spad(0, A), 0, n);
    k_eup_pop_32(0, spad(0, OUT_A), n);
    SYNC[f](spad(0, A), spad(0, OUT_B), n);
    for (unsigned lane = 0; lane < LANES; lane++)
      wrong += memcmp(spad(lane, OUT_A), spad(lane, OUT_B), n * sizeof(float)) != 0;
    if (wrong) {
      printf("eup: the push of function %u differs from its sf.vc.v.iv in %lu lanes\n", f + 1, wrong);
      return 1;
    }
  }
  printf("eup ok: %lu elements a group, %u lanes, 2^x and six functions\n", n, LANES);
  return 0;
}
