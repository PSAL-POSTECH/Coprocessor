// Checks made on the tables themselves, with no simulator. Usage: direct <directory of the model libraries>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "coprocessor.h"

namespace {

using Description = std::map<std::string, std::string>;

std::string directory;
int failed = 0;
vcix_id_t next_id = 1;
const vcix_insn owned = {0x062541db, 8, 32, 0};
const vcix_insn base = {0x0e2541db, 8, 32, 0};

void expect(bool ok, const std::string &what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  fflush(stdout);
  if (!ok) failed = 1;
}

const vcix_model *table(const std::string &library) {
  const std::string path = directory + "/lib" + library + ".so";
  void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  void *entry = lib ? dlsym(lib, "coprocessor_model") : nullptr;
  if (!entry) {
    fprintf(stderr, "%s: %s\n", path.c_str(), dlerror());
    exit(2);
  }
  return reinterpret_cast<const vcix_model *(*)()>(entry)();
}

void *create(const vcix_model *m, const Description &description, char *error, size_t error_size) {
  const vcix_config config = {const_cast<Description *>(&description), [](void *ctx, const char *key) -> const char * {
                                const Description &d = *static_cast<const Description *>(ctx);
                                const auto found = d.find(key);
                                return found == d.end() ? nullptr : found->second.c_str();
                              }};
  return m->create(&config, error, error_size);
}

vcix_cycle_t answer(const vcix_model *m, void *self, const vcix_insn &insn) {
  const vcix_id_t id = next_id++;
  const vcix_cycle_t cycles = m->issue(self, &insn, id, 0);
  m->squash(self, id, 0);
  return cycles;
}

void commit_one(const vcix_model *m, void *self) {
  const vcix_id_t id = next_id++;
  m->issue(self, &owned, id, 0);
  m->commit(self, &owned, id, 0);
}

void two_libraries(const std::string &name, const char *built) {
  const vcix_model *first = table(name + "_1"), *second = table(name + "_2");
  char error[64] = "";
  void *one = create(first, {}, error, sizeof error), *two = create(second, {}, error, sizeof error);
  expect(first != second && one && two && one != two && strcmp(first->name, "first") == 0 &&
             strcmp(second->name, "second") == 0 && first->encodings[0].match != second->encodings[0].match &&
             answer(first, one, owned) == 1 && answer(second, two, owned) == 2,
         std::string("two libraries with a class of one name, built ") + built + ", have each its own table and model");
}

void instances() {
  const vcix_model *m = table("reports");
  char error[64] = "";
  void *one = create(m, {}, error, sizeof error), *two = create(m, {}, error, sizeof error);
  expect(one && two && one != two, "one library makes two instances");
  if (!one || !two) return;
  commit_one(m, one);
  expect(answer(m, one, owned) == 2 && answer(m, two, owned) == 1, "a commit on one instance does not change the other");
  commit_one(m, two);
  commit_one(m, two);
  m->destroy(one);
  one = create(m, {}, error, sizeof error);
  expect(one && answer(m, one, owned) == 1 && answer(m, two, owned) == 3,
         "destroy then create gives a fresh instance and leaves the other as it was");
  m->reset(two);
  expect(answer(m, two, owned) == 1, "reset returns an instance to the state create left");
  m->destroy(one);
  m->destroy(two);
}

struct Number {
  const char *key, *written;
  uint64_t read;
  const char *why_not;
};
const char NOT_DECIMAL[] = "is not an unsigned decimal number";
const char NOT_HEX[] = "is not a hexadecimal number written 0x...";
const char TOO_LARGE[] = "does not fit in 64 bits";
const Number NUMBERS[] = {
    {"latency", nullptr, 1},
    {"latency", "8", 8},
    {"latency", "0", 0},
    {"latency", "abc", 0, NOT_DECIMAL},
    {"latency", "-1", 0, NOT_DECIMAL},
    {"latency", "8 cycles", 0, NOT_DECIMAL},
    {"latency", "010", 0, "has a leading zero, which reads as octal or as decimal depending on the reader"},
    {"latency", "18446744073709551616", 0, TOO_LARGE},
    {"base", nullptr, 0x1000},
    {"base", "0x80001000", 0x80001000},
    {"base", "0xffffffffffffffff", UINT64_MAX},
    {"base", "80001000", 0, NOT_HEX},
    {"base", "0x", 0, NOT_HEX},
    {"base", "0x80zz", 0, NOT_HEX},
    {"base", "0X80", 0, NOT_HEX},
    {"base", "0x10000000000000000", 0, TOO_LARGE},
};

void numbers() {
  const vcix_model *m = table("reports");
  unsigned wrong = 0;
  for (const Number &n : NUMBERS) {
    const std::string written = n.written ? n.written : "";
    char error[256] = "";
    void *self = create(m, n.written ? Description{{n.key, written}} : Description{}, error, sizeof error);
    const std::string what = std::string(n.key) + ": " + (n.written ? "'" + written + "' " : "absent ");
    const bool ok = n.why_not ? !self && error == "machine description: " + what + n.why_not
                              : self && answer(m, self, strcmp(n.key, "base") ? owned : base) == n.read;
    if (!ok) expect(false, what + (n.why_not ? n.why_not : "is read") + ", not: " + error);
    wrong += !ok;
    if (self) m->destroy(self);
  }
  expect(!wrong, "Config::uint and Config::hex read a number as written, the fallback when absent, or say why not");
  char small[16];
  memset(small, '#', sizeof small);
  void *self = create(m, {{"latency", "abc"}}, small, 8);
  expect(!self && memcmp(small, "machine\0########", sizeof small) == 0, "the reason is cut to error_size and NUL-terminated");
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <directory of the model libraries>\n", argv[0]);
    return 2;
  }
  directory = argv[1];
  two_libraries("same_name", "hidden");
  two_libraries("same_name_default", "with the compiler's default visibility");
  instances();
  numbers();
  expect(table("cannot_be_made") == nullptr, "a model whose constructor throws hands over no table");
  return failed;
}
