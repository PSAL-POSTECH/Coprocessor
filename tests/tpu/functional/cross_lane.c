// The cross-lane unit and vlane_idx. Usage: cross_lane lanes | cross_lane <operation 0 to 23>... Each operation at
// each element width: tiles of several depths, pushed in one or two pieces, with the patterns it reads, popped in
// pieces or whole. Output: what each pop left in every lane.
#include <stdlib.h>

#include "common.h"

kernel_with k_cross_push_8, k_cross_push_16, k_cross_push_32;
kernel k_cross_pop_8, k_cross_pop_16, k_cross_pop_32, k_cross_load, k_cross_set_pattern;
kernel_with k_lane_index_8, k_lane_index_16, k_lane_index_32, k_lane_index_64;

static kernel_with *const PUSH[3] = {k_cross_push_8, k_cross_push_16, k_cross_push_32};
static kernel *const POP[3] = {k_cross_pop_8, k_cross_pop_16, k_cross_pop_32};

#define IN 0ul
#define LANE_NUMBERS 4096ul
#define OUT 8192ul

// A lane number per element in every lane, now and then one that is not a lane.
static void lane_numbers(unsigned long count) {
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < count; i++) *(uint32_t *)spad(lane, LANE_NUMBERS + 4 * i) = random_below(LANES + 2);
}
static unsigned long pushed, left;  // elements per lane of the tile not yet taken; slots of the result not yet popped
// The shuffle before the crossing reads a pattern when the field's [4:3] is 2: the one set before the push.
static void push(unsigned width, unsigned operation, unsigned long count) {
  const unsigned bytes = 1u << width;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < count; i++) {
      const uint32_t value = random32();
      memcpy(spad(lane, IN + i * bytes), &value, bytes);
    }
  lane_numbers(count);
  if (operation >> 3 == 2 && k_cross_set_pattern(spad(0, LANE_NUMBERS), 0, count) != count)
    puts("a set did not take every lane number");
  if (PUSH[width](spad(0, IN), 0, count, operation, 0) != count) puts("a push did not take every element");
  pushed += count;
}
// At most what is left of the result: a slot an element, LANES for an exchange ([2]), whose rest the pushes after it
// drop; with nothing left, the pushes since are the next tile. The shuffle after reads a pattern when the field's
// [1:0] is 2: it is loaded, as wide as that tile is by then.
static void pop(unsigned width, unsigned operation, unsigned long count) {
  if (pushed && (operation & 4)) left = 0;
  if (!left && pushed) {
    left = operation & 4 ? LANES : pushed;
    if ((operation & 3) == 2) {
      const unsigned long columns = operation & 4 ? LANES : pushed;
      lane_numbers(columns);
      if (k_cross_load(spad(0, LANE_NUMBERS), 0, columns) != columns) puts("a load did not take every element");
    }
    pushed = 0;
  }
  if (count > left) count = left;
  left -= count;
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 64);
  if (POP[width](0, spad(0, OUT), count) != count) puts("a pop did not take every element");
  emit_lanes(OUT, 64);
}

static void lane_index(kernel_with *function, unsigned long count) {
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 256);
  function(0, spad(0, OUT), count, 32, 0);
  emit_lanes(OUT, 256);
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "lanes")) {
    kernel_with *const at[] = {k_lane_index_8, k_lane_index_16, k_lane_index_32, k_lane_index_64};
    for (unsigned width = 0; width < 4; width++) {
      lane_index(at[width], 3);
      lane_index(at[width], 32u >> width);
    }
    return 0;
  }
  if (argc < 2) {
    puts("usage: cross_lane lanes | cross_lane <operation>...");
    return 1;
  }
  for (int argument = 1; argument < argc; argument++) {
    const unsigned operation = (unsigned)strtoul(argv[argument], 0, 10);
    if (operation > 23) {
      printf("%s is not an operation of the unit\n", argv[argument]);
      return 1;
    }
    for (unsigned width = 0; width < 3; width++) {
      for (unsigned long depth = 1; depth <= 6; depth++) {
        push(width, operation, depth);
        pop(width, operation, depth);
      }
      // Two pieces, then a pop that takes less than there is, and one that takes the rest.
      push(width, operation, 3);
      push(width, operation, 2);
      pop(width, operation, 2);
      pop(width, operation, 8);
      // A move's tile waits behind what the last pop left, the rest first; an exchange's replaces it.
      push(width, operation, 4);
      pop(width, operation, 1);
      push(width, operation, 2);
      pop(width, operation, 8);
      pop(width, operation, 6);
    }
  }
  return 0;
}
