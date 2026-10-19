// The DMA. Usage: dma <first case> <cases>. A case is a descriptor made from its number (sizes, dimensions, strides,
// and now and then a mask, skip axes, a bound on memory, an accumulating mvout, indices); it fills the scratchpad and
// memory from its number, runs an mvin and emits every lane's scratchpad, then an mvout and emits memory.
#include <stddef.h>
#include <stdlib.h>

#include "common.h"

struct descriptor {
  int32_t dim_size[4], dim_low[4], dim_high[4];
  uint64_t mm_stride[4], spad_stride[4];
  uint16_t element_size, vlane_stride;
  uint8_t split_axis, unused_117;
  uint16_t flags;
  uint64_t indirect_address;
  uint16_t indirect_stride, indirect_element_size;
  uint8_t indirect_dim, unused_133;
  uint16_t indirect_lanes;
  uint64_t fill, memory_base, memory_bytes;
};
_Static_assert(offsetof(struct descriptor, mm_stride) == 48 && offsetof(struct descriptor, element_size) == 112 &&
                   offsetof(struct descriptor, flags) == 118 && offsetof(struct descriptor, indirect_address) == 120 &&
                   offsetof(struct descriptor, indirect_lanes) == 134 && offsetof(struct descriptor, fill) == 136 &&
                   sizeof(struct descriptor) == 160,
               "the descriptor is laid out as the DMA reads it");
enum { INDIRECT = 1, MASKED = 2, ACCUMULATE = 4, ACCUMULATE_FLOAT = 8, SKIP_AXES = 0xF0 };

// custom-1, funct3 3: funct7 says which.
#define DMA(funct7, a, b) __asm__ volatile(".insn r 0x2b, 3, " #funct7 ", x0, %0, %1" : : "r"(a), "r"(b) : "memory")
static void config(const struct descriptor *d) { DMA(7, d, d); }
static void mvin(unsigned which, void *memory, void *scratchpad) {
  if (which == 0) DMA(2, memory, scratchpad);
  if (which == 1) DMA(1, memory, scratchpad);
  if (which == 2) DMA(14, memory, scratchpad);
}
static void mvout(void *memory, void *scratchpad) { DMA(3, memory, scratchpad); }

#define WINDOW 8192ul           // of each lane's scratchpad, from 0: where a tensor lands
#define INDICES 16384ul         // of each lane's scratchpad: the indices of an indirect transfer
#define MARGIN 4096ul           // of memory on either side of the tensor, for what an index reaches
#define TENSOR 32768ul          // of memory: the most a tensor of a case spans
static unsigned char memory[MARGIN + TENSOR + MARGIN];

// Bytes that are a finite number at every element size when `finite`, any bytes otherwise.
static void fill(unsigned char *to, unsigned long bytes, unsigned element, int finite) {
  for (unsigned long at = 0; at < bytes; at += element) {
    if (finite && element == 4) {
      const float value = ((int32_t)random_below(400001) - 200000) / 64.0f;
      memcpy(to + at, &value, 4);
    } else if (finite && element == 8) {
      const double value = ((int32_t)random_below(400001) - 200000) / 64.0;
      memcpy(to + at, &value, 8);
    } else {
      const uint64_t value = (uint64_t)random32() << 32 | random32();
      memcpy(to + at, &value, element);
    }
  }
}

static void one(unsigned number) {
  random_state = 0x9E3779B9u * (number + 1) | 1;
  struct descriptor d;
  memset(&d, 0, sizeof d);
  d.element_size = 1u << random_below(4);
  d.split_axis = random_below(4);
  d.vlane_stride = 1 + random_below(2);
  for (int i = 0; i < 4; i++) d.dim_size[i] = 1 + random_below(4);
  // Along the split axis: whole strides, and one pass of at most every lane or whole passes of all of them.
  // Split along the first axis, any length: the positions past the end are not in memory.
  const unsigned strides = random_below(2) ? 1 + random_below(LANES) : LANES * (1 + random_below(3));
  d.dim_size[d.split_axis] = strides * d.vlane_stride;
  if (d.split_axis == 0 && random_below(2)) d.dim_size[0] = 1 + random_below(16);

  // In memory the tensor sits in a larger one, now and then with two axes exchanged.
  uint64_t stride = 1;
  for (int i = 3; i >= 0; i--) {
    d.mm_stride[i] = stride;
    stride *= d.dim_size[i] + random_below(2);
  }
  if (random_below(4) == 0) {
    const int a = random_below(4), b = random_below(4);
    uint64_t exchanged[4], last = 0;
    memcpy(exchanged, d.mm_stride, sizeof exchanged);
    exchanged[a] = d.mm_stride[b];
    exchanged[b] = d.mm_stride[a];
    for (int i = 0; i < 4; i++) last += (d.dim_size[i] - 1) * exchanged[i];
    if ((last + 1) * d.element_size <= TENSOR) memcpy(d.mm_stride, exchanged, sizeof exchanged);
  }
  d.spad_stride[3] = 1;
  for (int i = 2; i >= 0; i--) d.spad_stride[i] = d.spad_stride[i + 1] * d.dim_size[i + 1];

  for (int i = 0; i < 4; i++) d.dim_high[i] = d.dim_size[i];
  if (random_below(5) < 2) {
    d.flags |= MASKED;
    for (int i = 0; i < 4; i++) {
      d.dim_low[i] = random_below(1 + d.dim_size[i] / 2);
      d.dim_high[i] = d.dim_low[i] + 1 + random_below(d.dim_size[i] - d.dim_low[i]);
    }
    if (random_below(8) == 0) d.dim_low[random_below(4)] = -1;
  }
  if (random_below(3) == 0) d.flags |= random_below(16) << 4;
  if (random_below(3) == 0) d.flags |= ACCUMULATE | (random_below(2) ? ACCUMULATE_FLOAT : 0);
  d.fill = (uint64_t)random32() << 32 | random32();
  unsigned char *const tensor = memory + MARGIN;
  if (random_below(3) == 0) {
    d.memory_base = (uint64_t)tensor + d.element_size * random_below(8);
    d.memory_bytes = d.element_size * (1 + random_below(64));
  }
  if (random_below(4) == 0) {
    d.flags |= INDIRECT;
    d.indirect_address = (uint64_t)spad(0, INDICES);
    d.indirect_stride = 1 + random_below(3);
    d.indirect_element_size = 1u << random_below(4);
    d.indirect_dim = random_below(5);
    d.indirect_lanes = random_below(2) ? 0 : 1u << random_below(3);
  }
  const int finite = (d.flags & ACCUMULATE_FLOAT) != 0;

  // The indices: small and signed, at every position an index can be read from.
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long at = 0; at < 8192; at += d.indirect_element_size ? d.indirect_element_size : 8) {
      const int64_t index = (int64_t)random_below(6) - 2;
      memcpy(spad(lane, INDICES + at), &index, d.indirect_element_size ? d.indirect_element_size : 8);
    }

  config(&d);
  for (unsigned lane = 0; lane < LANES; lane++) fill(spad(lane, 0), WINDOW, d.element_size, finite);
  fill(memory, sizeof memory, d.element_size, finite);
  mvin(number % 3, tensor, spad(0, 8 * random_below(4)));
  emit_lanes(0, WINDOW);

  for (unsigned lane = 0; lane < LANES; lane++) fill(spad(lane, 0), WINDOW, d.element_size, finite);
  fill(memory, sizeof memory, d.element_size, finite);
  mvout(tensor, spad(0, 8 * random_below(4)));
  emit(memory, sizeof memory);
}

// With `keyed`, each case first names itself with dma_index_key, so its indices go under its own number.
int main(int argc, char **argv) {
  if (argc != 3 && !(argc == 4 && strcmp(argv[3], "keyed") == 0)) {
    puts("usage: dma <first case> <cases> [keyed]");
    return 1;
  }
  const unsigned first = (unsigned)strtoul(argv[1], 0, 10), cases = (unsigned)strtoul(argv[2], 0, 10);
  for (unsigned number = first; number < first + cases; number++) {
    if (argc == 4) DMA(8, (uint64_t)number, 0);
    one(number);
  }
  return 0;
}
