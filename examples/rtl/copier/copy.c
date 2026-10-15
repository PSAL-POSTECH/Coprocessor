// A copy by the unit, then the destination read by ordinary loads. -DWAIT puts a copy wait between them; -DUNTOUCHED
// copies into a page of the heap the program never touched: pk maps it at its first store, the unit's, so that store
// faults inside execute. A copy taken twice would copy the source's second half, which differs from the first.
// No source byte is 0; exit 0 when the first half's 256 arrived. -DTIMING, for gem5, which moves no data: always 0.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint8_t src[512] __attribute__((aligned(8)));

int main(void) {
  for (int i = 0; i < 512; i++) src[i] = (uint8_t)(i * 7 % 255 + 1);
#ifdef UNTOUCHED
  uint8_t *dst = (uint8_t *)(((uintptr_t)sbrk(2 * 4096) + 4095) & ~(uintptr_t)4095);
#else
  static uint8_t dst[256] __attribute__((aligned(8)));
  memset(dst, 0, sizeof dst);
#endif
  __asm__ volatile("vsetivli zero, 1, e8, m1, ta, ma");
  __asm__ volatile(".insn r 0x5b, 4, 0x03, x0, %0, x0" : : "r"(src) : "memory");
  __asm__ volatile(".insn r 0x5b, 4, 0x05, x0, %0, x0" : : "r"(dst) : "memory");
#ifdef WAIT
  __asm__ volatile(".insn r 0x5b, 4, 0x07, x0, x0, x0" : : : "memory");
#endif
  int same = 0;
  for (int i = 0; i < 256; i++) same += dst[i] == src[i];
  printf("%d of 256 bytes copied\n", same);
#ifdef TIMING
  return 0;
#else
  return same == 256 ? 0 : 1;
#endif
}
