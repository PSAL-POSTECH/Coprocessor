// Drives the timing face of one instance without gem5: N copies of one instruction, one at a time.
#include <dlfcn.h>

#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "coprocessor.h"

namespace {

// The whole of `text` as a number in `base`, within [lowest, highest].
template <class T>
bool parse(const char *text, int base, T lowest, T highest, T &number) {
  const char *end = text + strlen(text);
  const std::from_chars_result parsed = std::from_chars(text, end, number, base);
  return parsed.ec == std::errc() && parsed.ptr == end && number >= lowest && number <= highest;
}

int usage(const char *self, const char *what, const char *text) {
  if (what) fprintf(stderr, "%s: '%s' is not %s\n", self, text, what);
  fprintf(stderr, "usage: %s model.so insn-hex [count [lmul-log2]]\n", self);
  return 2;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argc > 5) return usage(argv[0], nullptr, nullptr);

  const char *hex = argv[2];
  if (strncmp(hex, "0x", 2) == 0) hex += 2;
  uint32_t bits = 0;
  if (!parse<uint32_t>(hex, 16, 0, UINT32_MAX, bits)) return usage(argv[0], "an instruction: 32 bits in hex", argv[2]);
  uint32_t count = 4;
  if (argc > 3 && !parse<uint32_t>(argv[3], 10, 0, UINT32_MAX, count)) return usage(argv[0], "a count", argv[3]);
  int32_t lmul_log2 = 0;
  if (argc > 4 && !parse<int32_t>(argv[4], 10, -3, 3, lmul_log2)) return usage(argv[0], "an lmul-log2, -3 to 3", argv[4]);

  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
  auto entry = reinterpret_cast<const vcix_model *(*)()>(dlsym(lib, "coprocessor_model"));
  if (!entry) {
    fprintf(stderr, "%s does not export coprocessor_model\n", argv[1]);
    return 1;
  }
  const vcix_model *m = entry();
  if (m->abi_version != COPROCESSOR_ABI_VERSION) {
    fprintf(stderr, "%s has ABI %u, the probe has %u\n", argv[1], m->abi_version, COPROCESSOR_ABI_VERSION);
    return 1;
  }
  if (!m->create || !m->destroy || !m->can_accept || !m->issue || !m->squash || !m->commit) {
    fprintf(stderr, "%s leaves create, destroy or part of the timing face NULL\n", argv[1]);
    return 1;
  }
  if (!vcix_owner(m, bits)) {
    fprintf(stderr, "%s does not own instruction %08" PRIx32 "\n", argv[1], bits);
    return 1;
  }

  vcix_config no_config = {nullptr, [](void *, const char *) -> const char * { return nullptr; }};
  char error[256] = "";
  void *self = m->create(&no_config, error, sizeof error);
  if (!self) {
    fprintf(stderr, "%s: %s: %s\n", argv[1], m->name, error);
    return 1;
  }

  const vcix_insn decoded = {bits, 8, 32, lmul_log2};
  const vcix_insn *insn = &decoded;

  vcix_cycle_t now = 0;
  const auto advance = [&](vcix_cycle_t cycles) {
    for (; cycles; cycles--) {
      now++;
      if (m->tick) m->tick(self, now);
    }
  };
  for (uint32_t i = 0; i < count; i++) {
    while (!m->can_accept(self, insn, now)) advance(1);
    const vcix_cycle_t issued = now;
    const vcix_cycle_t lat = m->issue(self, insn, i + 1, now);
    if (lat != VCIX_LATENCY_UNKNOWN) {
      advance(lat);
    } else if (!m->ready) {
      fprintf(stderr, "%s: issue returned VCIX_LATENCY_UNKNOWN and the table has no ready\n", argv[1]);
      return 1;
    } else {
      do advance(1);
      while (!m->ready(self, i + 1, now));
    }
    printf("insn %" PRIu32 ": issued at %" PRIu64 ", committed at %" PRIu64 "\n", i, issued, now);
    m->commit(self, insn, i + 1, now);
  }
  m->destroy(self);
  return 0;
}
