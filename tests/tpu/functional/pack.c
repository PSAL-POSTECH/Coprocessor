// The bf16 pack on every lane: 4096 singles a lane, listed values (ties, NaNs, infinities, the ends of the
// range) then any bits, packed two to a word. Checked against bf16 rounding to nearest, ties to even, a NaN
// kept a NaN. Output: the count of words and of wrong ones; exit 1 on any wrong one.
#include "common.h"

kernel k_pack;

#define IN 0ul
#define OUT (128ul * 1024)
#define SINGLES 4096ul

static const uint32_t LISTED[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f808000, 0x3f818000, 0x3f808001, 0x3f80ffff,
    0xbf808000, 0xbf818000, 0x7f7fffff, 0xff7fffff, 0x7f7f8000, 0x7f800000, 0xff800000, 0x7fc00000,
    0xffc00000, 0x7f800001, 0xff800001, 0x7fa00000, 0x00000001, 0x007fffff, 0x00800000, 0x0000ffff,
};

static uint16_t bf16(uint32_t u) {
  if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu)) return (uint16_t)((u >> 16) | 0x40u);
  return (uint16_t)((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16);
}

int main(void) {
  const unsigned listed = sizeof LISTED / sizeof LISTED[0];
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < SINGLES; i++)
      *(uint32_t *)spad(lane, IN + 4 * i) = i < listed ? LISTED[(i + lane) % listed] : random32();
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, SINGLES * 2);

  // Each call packs one register group: vl words from 2 * vl singles, vs2's then vs1's.
  unsigned long words = 0;
  while (2 * words < SINGLES) {
    const unsigned long base = 2 * words;
    const unsigned long vl = k_pack(spad(0, IN + 4 * base), spad(0, OUT + 4 * words), (SINGLES - base) / 2);
    unsigned long wrong = 0;
    for (unsigned lane = 0; lane < LANES; lane++)
      for (unsigned long w = 0; w < vl; w++) {
        // Word w of the group: elements 2w and 2w + 1 of vs2 ++ vs1, the first in the low half.
        const unsigned long e = 2 * w, f = 2 * w + 1;
        const uint32_t a = *(uint32_t *)spad(lane, IN + 4 * (e < vl ? base + e : base + vl + e - vl));
        const uint32_t b = *(uint32_t *)spad(lane, IN + 4 * (f < vl ? base + f : base + vl + f - vl));
        const uint32_t want = bf16(a) | (uint32_t)bf16(b) << 16;
        if (*(uint32_t *)spad(lane, OUT + 4 * (words + w)) != want) wrong++;
      }
    if (wrong) { printf("pack: %lu wrong of %lu words at word %lu\n", wrong, vl * LANES, words); return 1; }
    words += vl;
  }
  printf("pack: %lu words a lane, 0 wrong\n", words);
  return 0;
}
