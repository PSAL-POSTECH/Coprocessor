// The tpuv6e model's statistics, read through the C table with no simulator: a unit per component and per block
// (<C>.insns, its queues and pipelines), under the names it gives them, in the order a reader that indexes the list
// depends on. Usage: tpuv6e_ports libtpuv6e.so
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "vcix_accel.h"

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s libtpuv6e.so\n", argv[0]);
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

  // Four entries per unit at its one primary port: per component its insns, its primary port, then its blocks, the
  // components in the order the model holds them; then the commits.
  const char *expected[][2] = {{"TPUV6E_SFU.insns", "insns"},
                               {"TPUV6E_SFU", "entry"},
                               {"TPUV6E_SFU.eup_input", "eup_input"},
                               {"TPUV6E_SFU.eup", "eup"},
                               {"TPUV6E_SFU.eup_output", "eup_output"},
                               {"TPUV6E_MISC.insns", "insns"},
                               {"TPUV6E_MISC", "issue"},
                               {"TPUV6E_XLU.insns", "insns"},
                               {"TPUV6E_XLU", "input"},
                               {"TPUV6E_XLU.queue", "queue"},
                               {"TPUV6E_XLU.stages", "stages"},
                               {"TPUV6E_XLU.moves", "moves"},
                               {"TPUV6E_XLU.buffer", "buffer"},
                               {"TPUV6E_XLU.line", "line"},
                               {"TPUV6E_XLU.out", "out"},
                               {"TPUV6E_MXU.insns", "insns"},
                               {"TPUV6E_MXU", "input"},
                               {"TPUV6E_MXU.array0_input", "array0_input"},
                               {"TPUV6E_MXU.array0_line", "array0_line"},
                               {"TPUV6E_MXU.array0_lower", "array0_lower"},
                               {"TPUV6E_MXU.array0_upper", "array0_upper"}};
  const size_t units = sizeof expected / sizeof expected[0];
  bool ok = !strcmp(m->name, "tpuv6e") && m->num_stats(self) == 4 * units + m->num_encodings;
  for (size_t u = 0; ok && u < units; u++)
    for (uint32_t kind = 0; kind < 4; kind++) {
      const vcix_stat *s = m->stat(self, 4 * u + kind);
      ok = ok && !strcmp(s->unit, expected[u][0]) && !strcmp(s->name, expected[u][1]) && s->kind == kind;
    }
  for (size_t e = 0; ok && e < m->num_encodings; e++) {
    const vcix_stat *s = m->stat(self, 4 * units + e);
    ok = !strcmp(s->unit, "committed") && !strcmp(s->name, m->encodings[e].name) && s->kind == VCIX_STAT_COUNT;
  }
  printf("%s  model tpuv6e: 21 units, each at its one primary port: per component its insns (TPUV6E_<C>.insns), its own port "
         "(TPUV6E_SFU.entry, TPUV6E_MISC.issue, TPUV6E_XLU/TPUV6E_MXU.input), then its queues and pipelines (TPUV6E_SFU.eup_input, "
         ".eup, .eup_output; TPUV6E_XLU.queue, .stages, .moves, .buffer, .line, .out; TPUV6E_MXU.array0_input, .array0_line, "
         ".array0_lower, .array0_upper), components in the model's order; then one commit count per encoding\n",
         ok ? "PASS" : "FAIL");
  m->destroy(self);
  return ok ? 0 : 1;
}
