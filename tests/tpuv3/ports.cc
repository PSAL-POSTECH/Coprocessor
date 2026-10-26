// The tpuv3 model's statistics, read through the C table with no simulator: each component's counters under the
// names the model gives them, in the order a reader that indexes the list depends on; then the commits.
// Usage: tpuv3_ports libtpuv3.so
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "coprocessor.h"

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s libtpuv3.so\n", argv[0]);
    return 2;
  }
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto entry = lib ? reinterpret_cast<const vcix_model *(*)()>(dlsym(lib, "coprocessor_model")) : nullptr;
  const vcix_model *m = entry ? entry() : nullptr;
  if (!m || m->abi_version != COPROCESSOR_ABI_VERSION || !m->num_stats) {
    fprintf(stderr, "%s: no table of ABI %u with statistics\n", argv[1], COPROCESSOR_ABI_VERSION);
    return 1;
  }
  vcix_config config = {nullptr, [](void *, const char *key) -> const char * {
                          static const std::map<std::string, std::string> machine = {{"vpu_num_lanes", "4"}};
                          const auto found = machine.find(key);
                          return found == machine.end() ? nullptr : found->second.c_str();
                        }};
  char error[256] = "";
  void *self = m->create(&config, error, sizeof error);
  if (!self) {
    fprintf(stderr, "create: %s\n", error);
    return 1;
  }

  // The counters in the model's order, none of a component's insns(), ports, queues and pipelines; then one commit
  // count per encoding.
  const vcix_stat expected[] = {{"TPUV3_SFU", "instructions", 1},
                                {"TPUV3_MISC", "instructions", 2},
                                {"TPUV3_XLU", "elements", 1},
                                {"TPUV3_XLU", "elements_in_flight", 0},
                                {"TPUV3_MXU", "rows", 1},
                                {"TPUV3_MXU", "rows_in_flight", 0},
                                {"TPUV3_MXU", "active_cycles", 0}};
  const size_t counters = sizeof expected / sizeof expected[0], listed = m->num_stats(self);
  bool ok = !strcmp(m->name, "tpuv3") && listed == counters + m->num_encodings;
  for (size_t i = 0; i < listed; i++) {
    const vcix_stat *s = m->stat(self, i);
    printf("%-28s %-28s %llu\n", s->group, s->name, static_cast<unsigned long long>(s->capacity));
  }
  for (size_t i = 0; ok && i < counters; i++) {
    const vcix_stat *s = m->stat(self, i);
    ok = !strcmp(s->group, expected[i].group) && !strcmp(s->name, expected[i].name) && s->capacity == expected[i].capacity;
  }
  for (size_t e = 0; ok && e < m->num_encodings; e++) {
    const vcix_stat *s = m->stat(self, counters + e);
    ok = !strcmp(s->group, "committed") && !strcmp(s->name, m->encodings[e].name) && !s->capacity;
  }
  printf("%s  model tpuv3: %zu counters (TPUV3_SFU/MISC.instructions, TPUV3_XLU.elements, TPUV3_MXU.rows of capacity "
         "1, the _in_flight counts and TPUV3_MXU.active_cycles) in the model's order; then one commit count per "
         "encoding\n",
         ok ? "PASS" : "FAIL", counters);
  m->destroy(self);
  return ok ? 0 : 1;
}
