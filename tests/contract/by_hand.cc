// Test model: a table written by hand, tick and reset NULL. A pop's result is ready fifteen cycles after its issue: `ready` says when, or built with READY=0 issue says so and `ready` is NULL too.
#include <cinttypes>
#include <cstdio>
#include <map>

#include "coprocessor.h"

namespace {

constexpr vcix_cycle_t CYCLES = 15;

struct State {
  std::map<vcix_id_t, vcix_cycle_t> pops;
  vcix_cycle_t last_pop = 0;
};

bool is_pop(const vcix_insn *insn) { return ((insn->bits >> 25) & 1) == 0; }
bool is_use(const vcix_insn *insn) { return (insn->bits >> 28) == 2; }

vcix_cycle_t issue(void *self, const vcix_insn *insn, vcix_id_t id, vcix_cycle_t now) {
  State &state = *static_cast<State *>(self);
  if (is_pop(insn)) {
    state.pops[id] = now;
    state.last_pop = now;
    return READY ? VCIX_LATENCY_UNKNOWN : CYCLES;
  }
  if (is_use(insn)) {
    printf("[model] issue use, %" PRIu64 " cycles after the pop\n", now - state.last_pop);
    fflush(stdout);
  }
  return 1;
}

int ready(void *self, vcix_id_t id, vcix_cycle_t now) {
  const State &state = *static_cast<State *>(self);
  const auto pop = state.pops.find(id);
  return pop != state.pops.end() && now >= pop->second + CYCLES;
}

const vcix_encoding encodings[] = {{0x5B, 0x7F, "custom-2"}};

const vcix_model table = {
    COPROCESSOR_ABI_VERSION,
    "by_hand",
    encodings,
    1,
    [](const vcix_config *, char *, size_t) -> void * { return new State; },
    [](void *self) { delete static_cast<State *>(self); },
    [](void *, const vcix_host *, const vcix_insn *) {},
    [](void *, const vcix_insn *, vcix_cycle_t) -> int { return 1; },
    issue,
    [](void *self, vcix_id_t first, vcix_cycle_t) {
      State &state = *static_cast<State *>(self);
      state.pops.erase(state.pops.lower_bound(first), state.pops.end());
    },
    [](void *self, const vcix_insn *, vcix_id_t id, vcix_cycle_t) { static_cast<State *>(self)->pops.erase(id); },
    nullptr,
    READY ? ready : nullptr,
    nullptr,
};

}  // namespace

extern "C" __attribute__((visibility("default"))) const vcix_model *coprocessor_model(void) { return &table; }
