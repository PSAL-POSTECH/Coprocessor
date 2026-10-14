// The tpuv6 model's statistics, read through the C table with no simulator: the units it holds, under the names it
// gives them, in the order a reader that indexes the list depends on. Usage: tpuv6_ports libtpuv6.so
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "vcix_accel.h"

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s libtpuv6.so\n", argv[0]);
    return 2;
  }
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto entry = lib ? reinterpret_cast<const vcix_model *(*)()>(dlsym(lib, "vcix_accel_model")) : nullptr;
  const vcix_model *m = entry ? entry() : nullptr;
  if (!m || m->abi_version != VCIX_ACCEL_ABI_VERSION || !m->num_stats) {
    fprintf(stderr, "%s: no table of ABI %u with statistics\n", argv[1], VCIX_ACCEL_ABI_VERSION);
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

  // Four entries per unit at its primary port, units in the order the model holds them, then the commits.
  const char *expected[][2] = {{"TPUV6_SFU", "entry"}, {"TPUV6_MISC", "issue"}, {"TPUV6_XLU", "input"}, {"TPUV6_MXU", "input"}};
  const size_t units = sizeof expected / sizeof expected[0];
  bool ok = !strcmp(m->name, "tpuv6") && m->num_stats(self) == 4 * units + m->num_encodings;
  for (size_t u = 0; ok && u < units; u++)
    for (uint32_t kind = 0; kind < 4; kind++) {
      const vcix_stat *s = m->stat(self, 4 * u + kind);
      ok = ok && !strcmp(s->unit, expected[u][0]) && !strcmp(s->name, expected[u][1]) && s->kind == kind;
    }
  for (size_t e = 0; ok && e < m->num_encodings; e++) {
    const vcix_stat *s = m->stat(self, 4 * units + e);
    ok = !strcmp(s->unit, "committed") && !strcmp(s->name, m->encodings[e].name) && s->kind == VCIX_STAT_COUNT;
  }
  printf("%s  model tpuv6: 4 units at their primary ports (TPUV6_SFU.entry, TPUV6_MISC.issue, TPUV6_XLU/TPUV6_MXU.input), "
         "no other port, then one commit count per encoding\n",
         ok ? "PASS" : "FAIL");
  m->destroy(self);
  return ok ? 0 : 1;
}
