// The cross-lane unit's set pattern (xlu_set_pattern): a pattern of P lane numbers a lane, kept until the next set,
// which each push takes as it issues (its element i reads entry i mod P), pops in between or not. Each tile is pushed
// with the shuffle before ARBITRARY (field 16) and checked in every lane against the same shuffle done here. Output:
// one line per case, and "xlu_pattern: all pass" at the end when every case did.
#include "common.h"

kernel k_cross_pop_32, k_cross_set_pattern;
kernel_with k_cross_push_32;

#define IN 0ul
#define PATTERN 4096ul
#define OUT 8192ul
#define ARBITRARY_BEFORE 16

static int failed;
static uint32_t pattern[LANES][8], want[LANES][64], in[LANES][64];
static unsigned long width, pushed;

static void set(unsigned long w) {
  width = w;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < width; i++) {
      pattern[lane][i] = random_below(LANES);
      *(uint32_t *)spad(lane, PATTERN + 4 * i) = pattern[lane][i];
    }
  if (k_cross_set_pattern(spad(0, PATTERN), 0, width) != width) puts("a set did not take every lane number");
}

// `depth` columns more of the tile, under the pattern set now.
static void push(unsigned long depth) {
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < depth; i++)
      *(uint32_t *)spad(lane, IN + 4 * (pushed + i)) = in[lane][pushed + i] = random32();
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long i = 0; i < depth; i++) want[lane][pushed + i] = in[pattern[lane][i % width]][pushed + i];
  if (k_cross_push_32(spad(0, IN + 4 * pushed), 0, depth, ARBITRARY_BEFORE, 0) != depth)
    puts("a push did not take every element");
  pushed += depth;
}

static void pop(const char *what) {
  for (unsigned lane = 0; lane < LANES; lane++) memset(spad(lane, OUT), 0xA5, 4 * pushed);
  if (k_cross_pop_32(0, spad(0, OUT), pushed) != pushed) puts("a pop did not take every element");
  int bad = 0;
  for (unsigned lane = 0; lane < LANES; lane++)
    for (unsigned long k = 0; k < pushed; k++)
      if (*(uint32_t *)spad(lane, OUT + 4 * k) != want[lane][k]) bad++;
  printf("%s  %s, %lu columns: %d wrong\n", bad ? "FAIL" : "PASS", what, pushed, bad);
  failed |= bad != 0;
  pushed = 0;
}

static void tile(const char *what, unsigned long depth) {
  push(depth);
  pop(what);
}

int main(void) {
  set(1);
  tile("one lane number a lane, for every element of a push", 8);
  tile("the same pattern, after a pop", 5);
  set(4);
  tile("a set replaces it", 8);
  tile("element i of a push reads entry i mod 4", 16);
  set(8);
  tile("as wide as the push", 8);
  set(2);
  push(3);
  push(3);
  set(1);
  push(4);
  pop("a set between pushes of one tile changes only the pushes after it");
  if (!failed) puts("xlu_pattern: all pass");
  return failed;
}
