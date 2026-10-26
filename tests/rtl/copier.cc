// libcopier.so against librtl_copier.so through the C table as gem5 drives them, with no simulator: seeded programs
// issued 1 to 3 a cycle, committed late, squashed and reset; every can_accept, latency, ready and common statistic must
// agree. Usage: copier_lockstep librtl_copier.so libcopier.so seeds cycles
#include <dlfcn.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "coprocessor.h"

namespace {

const char *nothing(void *, const char *) { return nullptr; }

const vcix_model *load(const char *path) {
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!h) fprintf(stderr, "%s\n", dlerror()), exit(2);
  return reinterpret_cast<const vcix_model *(*)()>(dlsym(h, "coprocessor_model"))();
}

// One seed: "" when the two answered alike throughout, else the first difference.
std::string run(const vcix_model *const m[2], uint64_t seed, uint64_t cycles, uint64_t &issued) {
  void *s[2];
  vcix_config config = {nullptr, nothing};
  char error[256] = "";
  for (int k = 0; k < 2; k++)
    if (!(s[k] = m[k]->create(&config, error, sizeof error))) fprintf(stderr, "create: %s\n", error), exit(2);
  std::vector<std::pair<size_t, size_t>> stats;
  for (size_t i = 0; i < m[0]->num_stats(s[0]); i++)
    for (size_t j = 0; j < m[1]->num_stats(s[1]); j++) {
      const vcix_stat *x = m[0]->stat(s[0], i), *y = m[1]->stat(s[1], j);
      if (!strcmp(x->group, y->group) && !strcmp(x->name, y->name) && x->capacity == y->capacity) stats.push_back({i, j});
    }
  std::mt19937_64 rng(seed);
  const auto draw = [&](uint64_t n) { return n ? rng() % n : 0; };
  std::vector<vcix_insn> program;
  for (int i = 0; i < 300; i++) {
    const vcix_encoding &e = m[0]->encodings[draw(m[0]->num_encodings)];
    program.push_back({e.match | (static_cast<uint32_t>(rng()) & ~e.mask), static_cast<uint32_t>(draw(24)), 32, 0});
  }
  struct Flight {
    vcix_id_t id;
    size_t pc;
    uint64_t due;
    vcix_insn insn;
  };
  std::deque<Flight> flight;
  std::vector<uint64_t> values[2] = {std::vector<uint64_t>(m[0]->num_stats(s[0])),
                                     std::vector<uint64_t>(m[1]->num_stats(s[1]))};
  size_t pc = 0;
  vcix_id_t next = 1;
  std::string differs;
  const auto same = [&](const char *what, uint64_t now, uint64_t a, uint64_t b) {
    if (a != b && differs.empty())
      differs = std::string(what) + " at cycle " + std::to_string(now) + ": " + std::to_string(a) + " on the RTL, " +
                std::to_string(b);
    return a == b;
  };
  for (uint64_t now = 1; now <= cycles && differs.empty(); now++) {
    for (int k = 0; k < 2; k++) m[k]->tick(s[k], now);
    for (Flight &f : flight)
      if (f.due == UINT64_MAX) {
        const int ready = m[0]->ready(s[0], f.id, now);
        if (same("ready", now, ready, m[1]->ready(s[1], f.id, now)) && ready) f.due = now;
      }
    while (!flight.empty() && flight.front().due <= now && draw(4)) {
      for (int k = 0; k < 2; k++) m[k]->commit(s[k], &flight.front().insn, flight.front().id, now);
      flight.pop_front();
    }
    if (!flight.empty() && draw(40) == 0) {
      const size_t at = draw(flight.size());
      for (int k = 0; k < 2; k++) m[k]->squash(s[k], flight[at].id, now);
      pc = flight[at].pc;
      flight.erase(flight.begin() + static_cast<long>(at), flight.end());
    }
    if (flight.empty() && pc == 0 && issued && draw(2) == 0)
      for (int k = 0; k < 2; k++) m[k]->reset(s[k]);
    const vcix_insn &probe = program[draw(program.size())];
    same("can_accept", now, m[0]->can_accept(s[0], &probe, now), m[1]->can_accept(s[1], &probe, now));
    for (uint64_t k = 0, width = 1 + draw(3); k < width && pc < program.size() && differs.empty(); k++) {
      const vcix_insn &insn = program[pc];
      const int accepts = m[0]->can_accept(s[0], &insn, now);
      if (!same("can_accept", now, accepts, m[1]->can_accept(s[1], &insn, now)) || !accepts) break;
      const vcix_cycle_t latency = m[0]->issue(s[0], &insn, next, now);
      same("latency", now, latency, m[1]->issue(s[1], &insn, next, now));
      flight.push_back({next++, pc++, latency == VCIX_LATENCY_UNKNOWN ? UINT64_MAX : now + latency, insn});
      issued++;
    }
    if (pc == program.size() && flight.empty()) pc = 0;
    for (int k = 0; k < 2; k++) m[k]->read_stats(s[k], values[k].data());
    for (const auto &[i, j] : stats) same(m[0]->stat(s[0], i)->name, now, values[0][i], values[1][j]);
  }
  for (int k = 0; k < 2; k++) m[k]->destroy(s[k]);
  return differs;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 5) return fprintf(stderr, "usage: copier_lockstep librtl_copier.so libcopier.so seeds cycles\n"), 2;
  const vcix_model *const m[2] = {load(argv[1]), load(argv[2])};
  if (m[0]->num_encodings != m[1]->num_encodings)
    return printf("FAIL  the two own %zu and %zu encodings\n", m[0]->num_encodings, m[1]->num_encodings), 1;
  for (size_t i = 0; i < m[0]->num_encodings; i++)
    if (m[0]->encodings[i].match != m[1]->encodings[i].match || m[0]->encodings[i].mask != m[1]->encodings[i].mask)
      return printf("FAIL  encoding %zu differs\n", i), 1;
  const uint64_t seeds = strtoull(argv[3], nullptr, 10), cycles = strtoull(argv[4], nullptr, 10);
  uint64_t issued = 0;
  for (uint64_t seed = 1; seed <= seeds; seed++) {
    const std::string differs = run(m, seed, cycles, issued);
    if (!differs.empty()) return printf("FAIL  seed %" PRIu64 ": %s\n", seed, differs.c_str()), 1;
  }
  printf("PASS  %" PRIu64 " seeds of %" PRIu64 " cycles, %" PRIu64 " instructions issued: every answer alike\n", seeds,
         cycles, issued);
  return 0;
}
