// Shared by the programs of this directory: where the scratchpad is, and how a result leaves the program.
// tests/tpuv6e/functional includes it too, for the same machine.yml.
#ifndef TPU_FUNCTIONAL_COMMON_H
#define TPU_FUNCTIONAL_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// As machine.yml says, and as the old Spike is started.
#define LANES 4u
#define LANE_BYTES (512ul * 1024)
#define SPAD_BASE 0xD0000000ul

// `offset` in the scratchpad of `lane`. A kernel is given lane 0's address and works on every lane's.
static inline void *spad(unsigned lane, unsigned long offset) {
  return (void *)(SPAD_BASE + lane * LANE_BYTES + offset);
}
// The program's standard output is its result; run.sh compares it.
static inline void emit(const void *bytes, size_t size) { fwrite(bytes, 1, size, stdout); }
static inline void emit_lanes(unsigned long offset, size_t size) {
  for (unsigned lane = 0; lane < LANES; lane++) emit(spad(lane, offset), size);
}

static uint32_t random_state = 0x2545F491u;
static inline uint32_t random32(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}
static inline uint32_t random_below(uint32_t bound) { return random32() % bound; }

typedef unsigned long kernel(void *source, void *destination, unsigned long elements);
// The same, with what else a kernel takes: a number that rides the instruction, and a pattern.
typedef unsigned long kernel_with(void *source, void *destination, unsigned long elements, unsigned long number,
                                  void *pattern);

#endif
