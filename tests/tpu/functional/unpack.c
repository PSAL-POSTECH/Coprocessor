// The bf16 unpack on every lane: 2048 words a lane (4096 bf16: listed values, then any bits), each group of vl
// words unpacked to 2 * vl singles by unpack_lo then unpack_hi. Checked against the exact widening (a bf16's
// bits are the single's upper half). Output: the count of singles and of wrong ones; exit 1 on any wrong one.
#include "common.h"

kernel k_unpack;

#define IN 0ul
#define OUT (128ul * 1024)
#define WORDS 2048ul

static const uint16_t LISTED[] = {0x0000, 0x8000, 0x3f80, 0xbf80, 0x7f7f, 0xff7f, 0x7f80, 0xff80,
                                  0x7fc0, 0xffc0, 0x7fa0, 0x0001, 0x007f, 0x0080, 0x4049, 0xc049};

int main(void) {
  const unsigned listed = sizeof LISTED / sizeof LISTED[0];
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < 2 * WORDS; i++)
      *(uint16_t *)spad(lane, IN + 2 * i) = i < listed ? LISTED[(i + lane) % listed] : (uint16_t)random32();
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, WORDS * 8);

  unsigned long words = 0;
  while (words < WORDS) {
    const unsigned long vl = k_unpack(spad(0, IN + 4 * words), spad(0, OUT + 8 * words), WORDS - words);
    unsigned long wrong = 0;
    for (unsigned lane = 0; lane < LANES; lane++)
      for (unsigned long e = 0; e < 2 * vl; e++) {
        // Element e of the group is bf16 number 2 * words + e; unpack_lo's singles then unpack_hi's.
        const uint32_t want = (uint32_t)*(uint16_t *)spad(lane, IN + 2 * (2 * words + e)) << 16;
        if (*(uint32_t *)spad(lane, OUT + 4 * (2 * words + e)) != want) wrong++;
      }
    if (wrong) { printf("unpack: %lu wrong of %lu singles at word %lu\n", wrong, 2 * vl * LANES, words); return 1; }
    words += vl;
  }
  printf("unpack: %lu singles a lane, 0 wrong\n", 2 * words);
  return 0;
}
