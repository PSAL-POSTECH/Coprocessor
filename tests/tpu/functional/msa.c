// The multi-precision array: words of 1 single, 2 halves or 4 floats of 8 bits, every width, pops at every
// element width. Output: what each pop left in every lane.
#include "common.h"

kernel_with k_msa_push_32, k_msa_pop_8, k_msa_pop_16, k_msa_pop_32;

#define IN 0ul
#define OUT 4096ul
enum { SINGLE = 0, HALF = 1, E4M3 = 2, E5M2 = 3, WEIGHT = 0x10 };

static uint16_t half_of(int sixteenths) {  // exact for |sixteenths| < 2048
  float value = sixteenths / 16.0f;
  uint32_t bits;
  memcpy(&bits, &value, sizeof bits);
  if ((bits & 0x7fffffff) == 0) return (uint16_t)(bits >> 16);
  const int exponent = (int)((bits >> 23) & 0xff) - 127 + 15;
  return (uint16_t)((bits >> 16 & 0x8000) | exponent << 10 | (bits >> 13 & 0x3ff));
}
static uint8_t finite_byte(void) {  // finite as either float of 8 bits
  uint8_t byte = (uint8_t)random32();
  while ((byte & 0x7c) == 0x7c || (byte & 0x7f) == 0x7f) byte = (uint8_t)random32();
  return byte;
}
// `count` words in every lane, each holding small finite numbers of `format`.
static void push(unsigned field, unsigned long count) {
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < count; i++) {
      uint32_t word;
      if ((field & 3) == SINGLE) {
        const float value = ((int)random_below(129) - 64) / 16.0f;
        memcpy(&word, &value, sizeof word);
      } else if ((field & 3) == HALF) {
        word = (uint32_t)half_of((int)random_below(129) - 64) << 16 | half_of((int)random_below(129) - 64);
      } else {
        word = (uint32_t)finite_byte() << 24 | (uint32_t)finite_byte() << 16 | (uint32_t)finite_byte() << 8 | finite_byte();
      }
      *(uint32_t *)spad(lane, IN + 4 * i) = word;
    }
  if (k_msa_push_32(spad(0, IN), 0, count, field, 0) != count) puts("a push did not take every element");
}
static void pop(kernel_with *function, unsigned long count, unsigned field) {
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 64);
  if (function(0, spad(0, OUT), count, field, 0) != count) puts("a pop did not take every element");
  emit_lanes(OUT, 64);
}

int main(void) {
  // Inputs before any weight compute zeros.
  push(SINGLE, 3);
  pop(k_msa_pop_32, 3, 0);
  for (unsigned format = SINGLE; format <= E5M2; format++) {
    // A full matrix, then two words more: the oldest weights are gone.
    push(WEIGHT | format, 4);
    push(format, 6);
    pop(k_msa_pop_32, 6, 0);
    push(WEIGHT | format, 2);
    push(format, 12);
    pop(k_msa_pop_32, 3, 0);
    pop(k_msa_pop_16, 3, 0);
    pop(k_msa_pop_8, 2, E4M3);
    pop(k_msa_pop_8, 2, E5M2);
    pop(k_msa_pop_8, 2, HALF);
    // Half of the columns compute, then a quarter; the change of width empties the matrix.
    for (unsigned shift = 1; shift <= 2; shift++) {
      push(WEIGHT | shift << 2 | format, 3);
      push(shift << 2 | format, 4);
      pop(k_msa_pop_32, 4, 0);
    }
  }
  // Inputs of the other float of 8 bits than the weights: both are four to a word.
  push(WEIGHT | E4M3, 4);
  push(E5M2, 4);
  pop(k_msa_pop_32, 4, 0);
  // One word of weights of the other kind: the matrix holds that word alone.
  push(WEIGHT | E5M2, 1);
  push(E5M2, 4);
  pop(k_msa_pop_32, 4, 0);
  return 0;
}
