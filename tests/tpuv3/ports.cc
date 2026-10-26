// The tpuv3 model's statistics, read through the C table with no simulator: one unit per component, under the names
// the model gives them, in the order a reader that indexes the list depends on; then the commits. Usage: tpuv3_ports libtpuv3.so
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "coprocessor.h"

// The units, each a component at the primary port its four entries count, in the model's order; a component's
// insns(), queues and pipelines are not listed.
static const char *const expected[][3] = {
    {"TPUV3_SFU", "entry", "instructions"},
    {"TPUV3_MISC", "issue", "instructions"},
    {"TPUV3_XLU", "input", "elements"},
    {"TPUV3_MXU", "input", "rows"},
};

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

  // Lists what the table holds, then checks it: four entries (admitted, capacity, cycles, occupancy) per unit, then
  // one commit count per encoding.
  const size_t units = sizeof expected / sizeof expected[0], listed = m->num_stats(self);
  bool ok = !strcmp(m->name, "tpuv3") && listed == 4 * units + m->num_encodings;
  for (size_t i = 0; i < listed; i += (i < 4 * units ? 4 : 1)) {
    const vcix_stat *s = m->stat(self, i);
    printf("%-28s %-16s %s\n", s->unit, s->name, s->unit_of_work);
  }
  for (size_t u = 0; ok && u < units; u++)
    for (uint32_t kind = 0; kind < 4; kind++) {
      const vcix_stat *s = m->stat(self, 4 * u + kind);
      ok = ok && !strcmp(s->unit, expected[u][0]) && !strcmp(s->name, expected[u][1]) &&
           !strcmp(s->unit_of_work, expected[u][2]) && s->kind == kind;
    }
  for (size_t e = 0; ok && e < m->num_encodings; e++) {
    const vcix_stat *s = m->stat(self, 4 * units + e);
    ok = !strcmp(s->unit, "committed") && !strcmp(s->name, m->encodings[e].name) && s->kind == VCIX_STAT_COUNT;
  }
  printf("%s  model tpuv3: %zu units, one per component at its primary port, in the model's order; then one commit "
         "count per encoding\n",
         ok ? "PASS" : "FAIL", units);
  m->destroy(self);
  return ok ? 0 : 1;
}
