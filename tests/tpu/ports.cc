// The tpu units' ports, read through the C table with no simulator: cases whose counts can be worked out by hand.
// A 4 x 4 array (7 slots, queues of 8), a cross-lane unit of 3 slots, a special-function unit of depth 4, misc 2 wide.
// Usage: tpu_ports libtpu.so
#include <dlfcn.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "vcix_accel.h"

namespace {

constexpr uint32_t INPUT_PUSH = 0x2200305B, WEIGHT_PUSH = 0x2600305B, POP = 0x0800305B, VEXP = 0x2C00305B;
constexpr uint32_t XLU_PUSH = 0x2E00305B, XLU_POP = 0x0400305B, MSA_PUSH = 0x2A00305B, MSA_WEIGHT = 0x2A08305B, MSA_POP = 0x0C00305B;
constexpr uint32_t MVIN = 0x0400302B, VLANE_IDX = 0x0000305B;

const vcix_model *m;
int failed = 0;

void check(bool ok, const std::string &what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  fflush(stdout);
  failed |= !ok;
}

// One instance, driven a cycle at a time; issued instructions commit in order once their latency has passed.
class Run {
 public:
  Run() {
    vcix_config config = {nullptr, [](void *, const char *key) -> const char * {
                            static const std::map<std::string, std::string> machine = {
                                {"vpu_num_lanes", "4"},          {"tpu_systolic_queue_entries", "8"},
                                {"tpu_xlu_latency_cycles", "3"}, {"tpu_xlu_queue_entries", "8"},
                                {"tpu_sfu_latency_cycles", "4"}};
                            const auto found = machine.find(key);
                            return found == machine.end() ? nullptr : found->second.c_str();
                          }};
    char error[256] = "";
    self_ = m->create(&config, error, sizeof error);
    if (!self_) {
      fprintf(stderr, "create: %s\n", error);
      exit(1);
    }
  }
  ~Run() { m->destroy(self_); }

  void next() {
    now_++;
    m->tick(self_, now_);
    while (!flight_.empty() && flight_.front().done <= now_) {
      m->commit(self_, &flight_.front().insn, flight_.front().id, now_);
      flight_.erase(flight_.begin());
    }
  }
  bool issue(uint32_t bits, uint32_t vl) {
    const vcix_insn insn = {bits, vl, 32, 0};
    if (!m->can_accept(self_, &insn, now_)) return false;
    const vcix_cycle_t latency = m->issue(self_, &insn, ++id_, now_);
    flight_.push_back({id_, insn, now_ + latency});
    return true;
  }
  void squash_last() {
    if (flight_.empty()) {
      fprintf(stderr, "nothing in flight to squash\n");
      exit(1);
    }
    m->squash(self_, flight_.back().id, now_);
    flight_.pop_back();
  }
  // The value of a unit's statistic at its primary port `name`, or of a commit count with unit "committed".
  uint64_t stat(const char *unit, const char *name, uint32_t kind) const {
    std::vector<uint64_t> values(m->num_stats(self_));
    m->read_stats(self_, values.data());
    for (size_t i = 0; i < values.size(); i++) {
      const vcix_stat *s = m->stat(self_, i);
      if (!strcmp(s->unit, unit) && !strcmp(s->name, name) && s->kind == kind) return values[i];
    }
    fprintf(stderr, "no statistic %s.%s of kind %u\n", unit, name, kind);
    exit(1);
  }
  std::vector<uint64_t> all() const {
    std::vector<uint64_t> values(m->num_stats(self_));
    m->read_stats(self_, values.data());
    return values;
  }
  void *self() const { return self_; }

 private:
  struct Flight {
    vcix_id_t id;
    vcix_insn insn;
    vcix_cycle_t done;
  };
  void *self_;
  vcix_cycle_t now_ = 0;
  vcix_id_t id_ = 0;
  std::vector<Flight> flight_;
};

std::string n(uint64_t v) { return std::to_string(v); }

// The list: four entries per unit at its primary port, units in the order the model asks them, then the commits.
void list() {
  Run run;
  const char *expected[][2] = {{"Sfu", "entry"}, {"Misc", "issue"}, {"Systolic", "input"}, {"Xlu", "input"}, {"Msa", "input"}};
  const size_t units = sizeof expected / sizeof expected[0];
  bool ok = m->num_stats(run.self()) == 4 * units + m->num_encodings;
  for (size_t u = 0; ok && u < units; u++)
    for (uint32_t kind = 0; kind < 4; kind++) {
      const vcix_stat *s = m->stat(run.self(), 4 * u + kind);
      ok = ok && !strcmp(s->unit, expected[u][0]) && !strcmp(s->name, expected[u][1]) && s->kind == kind;
    }
  for (size_t e = 0; ok && e < m->num_encodings; e++) {
    const vcix_stat *s = m->stat(run.self(), 4 * units + e);
    ok = !strcmp(s->unit, "committed") && !strcmp(s->name, m->encodings[e].name) && s->kind == VCIX_STAT_COUNT;
  }
  check(ok, "5 units at their primary ports (Sfu.entry, Misc.issue, Systolic/Xlu/Msa.input), no other port, then one "
            "commit count per encoding");
}

// One row through a 7-slot array: it enters in the tick after its push and is held 7 cycles.
void one_row() {
  Run run;
  run.next();
  run.issue(INPUT_PUSH, 1);
  for (int c = 2; c <= 20; c++) run.next();
  const uint64_t admitted = run.stat("Systolic", "input", VCIX_STAT_ADMITTED);
  const uint64_t cycles = run.stat("Systolic", "input", VCIX_STAT_CYCLES);
  const uint64_t occupancy = run.stat("Systolic", "input", VCIX_STAT_OCCUPANCY);
  const uint64_t capacity = run.stat("Systolic", "input", VCIX_STAT_CAPACITY);
  const uint64_t committed = run.stat("committed", "systolic input push", VCIX_STAT_COUNT);
  check(admitted == 1 && capacity == 1 && cycles == 20 && occupancy == 7 && committed == 1,
        "one input row over 20 cycles: admitted " + n(admitted) + " of capacity " + n(capacity) + " x cycles " +
            n(cycles) + ", occupancy " + n(occupancy) + " (7 slots), committed " + n(committed) +
            "; expected 1, 1, 20, 7, 1");
  run.issue(POP, 1);
  for (int c = 0; c < 20; c++) run.next();
  const uint64_t popped = run.stat("committed", "systolic pop", VCIX_STAT_COUNT);
  const uint64_t still = run.stat("Systolic", "input", VCIX_STAT_ADMITTED);
  const uint64_t others = run.stat("Sfu", "entry", VCIX_STAT_ADMITTED) + run.stat("Xlu", "input", VCIX_STAT_ADMITTED) +
                          run.stat("Msa", "input", VCIX_STAT_ADMITTED) + run.stat("Misc", "issue", VCIX_STAT_ADMITTED);
  check(popped == 1 && still == 1 && others == 0,
        "its pop commits (" + n(popped) + ") and admits nothing at the input (" + n(still) +
            "), and no other unit admitted anything (" + n(others) + ")");
}

// Pushes and pops of two, one instruction a cycle: the array takes a row every cycle once it is full.
// A pop is offered only for rows that were pushed: one offered to an empty array is issued and takes nothing.
void continuous() {
  Run run;
  const int cycles = 4000;
  uint64_t short_by = 0, unpopped = 0;
  bool bounded = true;
  for (int c = 1; c <= cycles; c++) {
    run.next();
    if (unpopped && run.issue(POP, 2))
      unpopped -= 2;
    else if (run.issue(INPUT_PUSH, 2))
      unpopped += 2;
    const uint64_t admitted = run.stat("Systolic", "input", VCIX_STAT_ADMITTED);
    bounded = bounded && admitted <= uint64_t(c);
    short_by = uint64_t(c) - admitted;
  }
  const uint64_t admitted = run.stat("Systolic", "input", VCIX_STAT_ADMITTED);
  const double utilization = double(admitted) / double(cycles);
  check(bounded && utilization > 0.99,
        "pushes and pops of two for " + n(cycles) + " cycles: input admitted " + n(admitted) +
            ", never above the cycles, utilization " + std::to_string(utilization) + " (" + n(short_by) +
            " rows short of capacity)");
}

// A vexp every cycle fills the special-function unit's entry exactly; xlu holds each element 3 cycles.
void sfu_and_xlu() {
  Run run;
  bool all = true;
  for (int c = 1; c <= 1000; c++) {
    run.next();
    all = run.issue(VEXP, 4) && all;
  }
  const uint64_t admitted = run.stat("Sfu", "entry", VCIX_STAT_ADMITTED);
  const uint64_t cycles = run.stat("Sfu", "entry", VCIX_STAT_CYCLES);
  check(all && admitted == 1000 && cycles == 1000, "a vexp each of 1000 cycles: Sfu.entry admitted " + n(admitted) +
                                                       " in " + n(cycles) + " cycles, utilization 1");

  Run xlu;
  xlu.next();
  xlu.issue(XLU_PUSH, 5);
  for (int c = 2; c <= 30; c++) xlu.next();
  const uint64_t elements = xlu.stat("Xlu", "input", VCIX_STAT_ADMITTED);
  const uint64_t held = xlu.stat("Xlu", "input", VCIX_STAT_OCCUPANCY);
  check(elements == 5 && held == 15,
        "a push of 5 through 3 slots: Xlu.input admitted " + n(elements) + ", occupancy " + n(held) + " (5 x 3)");

  Run msa;
  msa.next();
  msa.issue(MSA_WEIGHT, 4);
  msa.next();
  msa.issue(MSA_PUSH, 3);
  for (int c = 3; c <= 20; c++) msa.next();
  msa.issue(MSA_POP, 3);
  msa.next();
  for (int c = 0; c < 20; c++) msa.next();
  check(msa.stat("Msa", "input", VCIX_STAT_ADMITTED) == 3 && msa.stat("committed", "msa push", VCIX_STAT_COUNT) == 2 &&
            msa.stat("committed", "msa pop", VCIX_STAT_COUNT) == 1,
        "msa: a weight push enters nothing, an input push of 3 enters 3 rows, and both pushes and the pop commit");
}

// Ten independent misc instructions, offered until refused each cycle: two a cycle for five cycles.
void misc() {
  Run run;
  int issued = 0, cycles = 0;
  bool two_a_cycle = true;
  while (issued < 10) {
    run.next();
    cycles++;
    int now = 0;
    while (issued < 10 && run.issue(issued % 2 ? MVIN : VLANE_IDX, 4)) {
      issued++;
      now++;
    }
    two_a_cycle = two_a_cycle && now == 2;
  }
  for (int c = 0; c < 5; c++) run.next();
  const uint64_t admitted = run.stat("Misc", "issue", VCIX_STAT_ADMITTED);
  const uint64_t capacity = run.stat("Misc", "issue", VCIX_STAT_CAPACITY);
  const uint64_t committed = run.stat("committed", "mvin", VCIX_STAT_COUNT) + run.stat("committed", "vlane_idx", VCIX_STAT_COUNT);
  check(two_a_cycle && cycles == 5 && admitted == 10 && capacity == 2 && committed == 10,
        "10 independent misc instructions: 2 a cycle for " + n(cycles) + " cycles, Misc.issue admitted " + n(admitted) +
            " of capacity " + n(capacity) + " (utilized cycles " + n(admitted / capacity) + "), committed " +
            n(committed) + "; expected 5, 10, 2 (5), 10");
}

// A push squashed behind a vexp still in flight leaves every count as a run that never issued it.
void squashed() {
  Run with, without;
  for (Run *run : {&with, &without}) {
    run->next();
    run->issue(INPUT_PUSH, 3);
    run->next();
    run->issue(VEXP, 1);
  }
  with.issue(INPUT_PUSH, 4);
  for (int c = 0; c < 3; c++) {
    with.next();
    without.next();
  }
  with.squash_last();
  for (int c = 0; c < 20; c++) {
    with.next();
    without.next();
  }
  check(with.all() == without.all(), "a push squashed 3 cycles after its issue, behind a vexp, leaves every statistic as if never issued");
}

// Cycles from now until the instruction is issued, 100 if it never is.
int wait_for(Run &run, uint32_t bits, uint32_t vl) {
  int waited = 0;
  do {
    run.next();
    waited++;
  } while (waited < 100 && !run.issue(bits, vl));
  return waited;
}

struct Unit {
  const char *name;
  uint32_t push, pop;
  int slots;
};
const Unit SYSTOLIC = {"systolic", INPUT_PUSH, POP, 7}, MSA = {"msa", MSA_PUSH, MSA_POP, 7}, XLU = {"xlu", XLU_PUSH, XLU_POP, 3};

// A pop of more than was pushed is issued once nothing more is on its way, and so is the pop after it.
void fewer_pushed(const Unit &unit) {
  Run run;
  run.next();
  run.issue(unit.push, 1);
  const int first = wait_for(run, unit.pop, 2);
  const int second = wait_for(run, unit.pop, 2);
  check(first == 1 + unit.slots && second == 1,
        std::string(unit.name) + ": a push of 1 is popped by 2 after " + n(first) + " cycles, and popped again after " +
            n(second) + "; expected " + n(1 + unit.slots) + " (1 + " + n(unit.slots) + " slots), 1");
}

// 100 times a push of 4 and a pop of 2, through queues of 8: what the pops leave never stops the unit for good.
void more_pushed(const Unit &unit) {
  Run run;
  int done = 0, cycles = 0, longest = 0;
  for (; done < 100; done++) {
    const int push = wait_for(run, unit.push, 4), pop = wait_for(run, unit.pop, 2);
    if (push == 100 || pop == 100) break;
    cycles += push + pop;
    longest = push + pop > longest ? push + pop : longest;
  }
  check(done == 100, std::string(unit.name) + ": a push of 4 and a pop of 2, " + n(done) + " times of 100 in " + n(cycles) +
                         " cycles, " + n(longest) + " the longest; expected all 100");
}

// A tile that comes out shallower: every pop waits for its own tile, 2 elements through 3 slots, never served from the last.
void xlu_shallower() {
  Run run;
  int as_first = 0;
  for (int tile = 0; tile < 100; tile++) {
    const int push = wait_for(run, XLU_PUSH, 4), pop = wait_for(run, XLU_POP, 2);
    as_first += push == 1 && pop == 2 + 3;
  }
  check(as_first == 100, "xlu: 100 tiles pushed by 4 and popped by 2: " + n(as_first) +
                             " pops issued 2 + 3 cycles after their own push; expected 100");
}

// Subtiles of 4 rows, pushed 4 ahead of their pops as 2 x queues of 8 / 4 rows allows: the pushes and pops of a
// matched stream, whose timing the rule for an unmatched one must not move. 48 rows at one a cycle, then 7 slots and the pop.
void interleaved(const Unit &unit, int expected) {
  Run run;
  std::vector<uint32_t> program(4, unit.push);
  for (int t = 4; t < 12; t++) {
    program.push_back(unit.pop);
    program.push_back(unit.push);
  }
  program.insert(program.end(), 4, unit.pop);
  int cycles = 0;
  bool all = true;
  for (uint32_t bits : program) {
    const int waited = wait_for(run, bits, 4);
    all = all && waited < 100;
    cycles += waited;
  }
  check(all && cycles == expected, std::string(unit.name) + ": 12 subtiles of 4 rows, pushed 4 ahead of their pops: " +
                                       n(cycles) + " cycles; expected " + n(expected));
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s libtpu.so\n", argv[0]);
    return 2;
  }
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  auto entry = lib ? reinterpret_cast<const vcix_model *(*)()>(dlsym(lib, "vcix_accel_model")) : nullptr;
  m = entry ? entry() : nullptr;
  if (!m || m->abi_version != VCIX_ACCEL_ABI_VERSION || !m->num_stats) {
    fprintf(stderr, "%s: no table of ABI %u with statistics\n", argv[1], VCIX_ACCEL_ABI_VERSION);
    return 1;
  }
  list();
  one_row();
  continuous();
  sfu_and_xlu();
  misc();
  squashed();
  for (const Unit *unit : {&SYSTOLIC, &MSA, &XLU}) {
    fewer_pushed(*unit);
    more_pushed(*unit);
  }
  xlu_shallower();
  interleaved(SYSTOLIC, 48 + 7 + 1);
  interleaved(MSA, 48 + 7 + 1);
  return failed;
}
