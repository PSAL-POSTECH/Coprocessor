// systolic.sv's array stops whenever its output queue is full, a slot coming out empty or not: libtpu.so's RTL keys must
// give each instruction the cycle the RTL gives it where the two stall modes part (a full queue, the array empty, rows
// behind it, then a weight push that waits for them). Usage: rtl_stall librtl_systolic.so libtpu.so
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "vcix_accel.h"

namespace {

constexpr uint32_t INPUT_PUSH = 0x2200305B, WEIGHT_PUSH = 0x2600305B, POP = 0x0800305B;

const char *key(void *, const char *k) {
  static const std::map<std::string, std::string> machine = {
      {"vpu_num_lanes", "4"},          {"tpu_systolic_queue_entries", "64"},   {"tpu_systolic_command_queue", "2"},
      {"tpu_systolic_in_rows_per_cycle", "1"}, {"tpu_systolic_pop_rows_per_cycle", "1"}, {"tpu_systolic_weight_waits", "1"}};
  const auto found = machine.find(k);
  return found == machine.end() ? nullptr : found->second.c_str();
}

// One model, an instruction issued when it can be, the cycle each becomes ready recorded and then committed.
struct Run {
  const vcix_model *m;
  void *self;
  std::vector<uint64_t> ready;
  std::vector<vcix_insn> insns;
  explicit Run(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) fprintf(stderr, "%s\n", dlerror()), exit(2);
    m = reinterpret_cast<const vcix_model *(*)()>(dlsym(h, "vcix_accel_model"))();
    vcix_config config = {nullptr, key};
    char error[256] = "";
    if (!(self = m->create(&config, error, sizeof error))) fprintf(stderr, "create: %s\n", error), exit(2);
  }
  void tick(uint64_t now) {
    m->tick(self, now);
    for (size_t id = 0; id < ready.size(); id++)
      if (!ready[id] && m->ready(self, id + 1, now)) ready[id] = now, m->commit(self, &insns[id], id + 1, now);
  }
};

}  // namespace

int main(int argc, char **argv) {
  if (argc != 3) return fprintf(stderr, "usage: rtl_stall librtl_systolic.so libtpu.so\n"), 2;
  const std::vector<std::pair<uint32_t, uint32_t>> program = {{WEIGHT_PUSH, 4}, {INPUT_PUSH, 64}, {WEIGHT_PUSH, 1},
                                                              {INPUT_PUSH, 3},  {POP, 3},         {WEIGHT_PUSH, 1},
                                                              {POP, 64}};
  const char *names[] = {"weight push 4", "input push 64", "weight push 1", "input push 3", "pop 3", "weight push 1", "pop 64"};
  Run rtl(argv[1]), mine(argv[2]);
  size_t pc = 0;
  for (uint64_t now = 1; now < 1000; now++) {
    rtl.tick(now), mine.tick(now);
    if (pc == program.size()) continue;
    const vcix_insn insn = {program[pc].first, program[pc].second, 8, 0};
    const int a = rtl.m->can_accept(rtl.self, &insn, now), b = mine.m->can_accept(mine.self, &insn, now);
    if (a != b) return printf("FAIL  %s: can_accept at cycle %llu, rtl %d libtpu %d\n", names[pc], (unsigned long long)now, a, b), 1;
    if (!a) continue;
    for (Run *r : {&rtl, &mine}) {
      r->insns.push_back(insn);
      const vcix_cycle_t latency = r->m->issue(r->self, &insn, pc + 1, now);
      r->ready.push_back(latency == VCIX_LATENCY_UNKNOWN ? 0 : now + latency);
    }
    pc++;
  }
  int failed = pc != program.size();
  for (size_t i = 0; i < program.size(); i++) {
    const bool ok = i < rtl.ready.size() && rtl.ready[i] && rtl.ready[i] == mine.ready[i];
    printf("%s  %s: ready at cycle %llu on the RTL, %llu on libtpu.so\n", ok ? "PASS" : "FAIL", names[i],
           (unsigned long long)(i < rtl.ready.size() ? rtl.ready[i] : 0), (unsigned long long)(i < mine.ready.size() ? mine.ready[i] : 0));
    failed |= !ok;
  }
  return failed;
}
