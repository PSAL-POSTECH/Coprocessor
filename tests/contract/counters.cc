// Checks of the statistics a model counts in its counters, with no simulator: through the C table and through Instance.
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "vcix_cp_base.hpp"

namespace {

using namespace cp;
using Insn = vcix_insn;

int failed = 0;

void expect(bool ok, const std::string &what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  fflush(stdout);
  if (!ok) failed = 1;
}

enum Variant { GOOD, NAMED_COMMITTED, LISTED_TWICE, CLASHING_GROUPS, CLASHING_NAMES, CLASHING_ENCODINGS };
enum Form { OP, PUSH, POP };
constexpr uint32_t FORM_MASK = 0xF000007F;

uint32_t bits_of(Form f) { return 0x5B | (uint32_t(f) << 28); }
Form form_of(const Insn &insn) { return Form(insn.bits >> 28); }

// Room for `width` a cycle, the model's own: taken by take(), given back by a reset.
struct Room {
  uint64_t width;
  Cycle at = Unknown;
  uint64_t used = 0;
  uint64_t available(Cycle now) const { return now == at ? width - used : width; }
  void take(uint64_t n, Cycle now) {
    if (now != at) at = now, used = 0;
    used += n;
  }
};

// A pipe that takes two instructions and eight elements a cycle, and an array: queues of four rows around a line of
// three slots, which one row a cycle enters; a pop takes up to vl once vl are out or none is on its way. Pushed more
// than popped, the output queue fills and the line stops. Counters: what entered each, and the rows in the line.
template <Variant V>
class Toy : public Model {
 public:
  const char *name() const override { return "toy"; }
  std::vector<Encoding> owns() const override {
    return {{bits_of(OP), FORM_MASK, "op"},
            {bits_of(PUSH), FORM_MASK, "array push"},
            {bits_of(POP), FORM_MASK, V == CLASHING_ENCODINGS ? "array_push" : "pop"}};
  }
  void execute(const Host &, const Insn &) override {}
  bool can_accept(const Insn &insn, Cycle now) const override {
    if (form_of(insn) == OP) return issue_room_.available(now) >= 1 && lanes_room_.available(now) >= insn.vl;
    if (form_of(insn) == PUSH) return insn.vl <= QUEUE - input_;
    return insn.vl <= output_ || (!input_ && !in_line_);
  }
  Cycle issue(const Insn &insn, Id, Cycle now) override {
    if (form_of(insn) == OP) {
      issue_room_.take(1, now), issue_.add(1);
      lanes_room_.take(insn.vl, now), lanes_.add(insn.vl);
    }
    if (form_of(insn) == PUSH) input_ += insn.vl;
    if (form_of(insn) == POP) output_ -= insn.vl < output_ ? insn.vl : output_;
    return 1;
  }
  void tick(Cycle now) override {
    if (output_ != QUEUE) {
      if (line_[head_]) output_++, in_line_--;
      line_[head_] = input_ != 0;
      if (line_[head_]) rows_.add(1), input_--, in_line_++;
      head_ = (head_ + 1) % SLOTS;
    }
    rows_in_flight_.add(in_line_);
  }
  void reset() override {
    issue_room_ = Room{2};
    lanes_room_ = Room{8};
    input_ = output_ = in_line_ = head_ = 0;
    for (bool &slot : line_) slot = false;
  }
  std::vector<const Counter *> counters() const override {
    if (V == LISTED_TWICE) return {&issue_, &lanes_, &rows_, &rows_in_flight_, &issue_};
    return {&issue_, &lanes_, &rows_, &rows_in_flight_};
  }

 private:
  Counter issue_{V == NAMED_COMMITTED ? "committed" : V == CLASHING_GROUPS ? "pi.pe" : "pipe",
                 V == CLASHING_NAMES ? "la.nes" : "instructions", 2};
  Counter lanes_{V == CLASHING_GROUPS ? "pi_pe" : "pipe", V == CLASHING_NAMES ? "la_nes" : "elements", 8};
  static constexpr uint32_t SLOTS = 3, QUEUE = 4;
  Counter rows_{"array", "rows", 1};
  Counter rows_in_flight_{"array", "rows_in_flight"};
  Room issue_room_{2}, lanes_room_{8};
  uint32_t input_ = 0, output_ = 0, in_line_ = 0, head_ = 0;
  bool line_[SLOTS] = {};
};

const vcix_config NO_CONFIG = {nullptr, [](void *, const char *) -> const char * { return nullptr; }};

std::vector<uint64_t> read(const vcix_model *m, void *self) {
  std::vector<uint64_t> values(m->num_stats(self));
  m->read_stats(self, values.data());
  return values;
}

// Runs a seeded program through the C table: in each cycle tick, commit what is ready, sometimes squash, issue.
struct Driver {
  const vcix_model *m = export_model<Toy<GOOD>>();
  void *self;
  std::mt19937 rng;
  std::deque<std::pair<Id, Insn>> in_flight;
  Id next = 1;
  Cycle now = 0;
  bool reads;
  std::string trace;

  Driver(unsigned seed, bool reads_between) : rng(seed), reads(reads_between) {
    char error[128] = "";
    self = m->create(&NO_CONFIG, error, sizeof error);
  }
  ~Driver() { m->destroy(self); }
  void poke() {
    if (!reads) return;
    for (int i = 0; i < 3; i++) read(m, self);
    m->stat(self, 0);
    m->num_stats(self);
  }
  Insn draw(std::mt19937 &r) {
    const Form f = Form(r() % 3);
    return {bits_of(f), uint32_t(1 + r() % (f == OP ? 8 : 3)), 32, 0};
  }
  bool try_issue(const Insn &insn) {
    poke();
    const bool ok = m->can_accept(self, &insn, now);
    trace += ok ? 'a' : 'r';
    if (!ok) return false;
    const Id id = next++;
    trace += char('0' + m->issue(self, &insn, id, now));
    in_flight.push_back({id, insn});
    return true;
  }
  // One cycle; `wrong` issues a wrong path from its own generator, which `squash_wrong` takes back later. While
  // `held`, the right path neither issues nor squashes, so the wrong path stays the youngest.
  void cycle(std::mt19937 *wrong = nullptr, bool held = false) {
    now++;
    m->tick(self, now);
    poke();
    for (int k = 0; k < 2 && !in_flight.empty() && in_flight.front().first < wrong_from; k++) {
      m->commit(self, &in_flight.front().second, in_flight.front().first, now);
      in_flight.pop_front();
    }
    const bool squash = rng() % 7 == 0;
    const Insn first_draw = draw(rng), second_draw = draw(rng);
    if (held) {
      trace += '-';
    } else if (squash && !in_flight.empty()) {
      const Id first = in_flight.back().first;
      m->squash(self, first, now);
      while (!in_flight.empty() && in_flight.back().first >= first) in_flight.pop_back();
    }
    if (!held) {
      try_issue(first_draw);
      try_issue(second_draw);
    }
    if (wrong) {
      if (wrong_from == UINT64_MAX) wrong_from = next;
      for (int k = 0; k < 3; k++) {
        const Insn insn = draw(*wrong);
        if (!m->can_accept(self, &insn, now)) continue;
        const Id id = next++;
        m->issue(self, &insn, id, now);
        in_flight.push_back({id, insn});
      }
    }
  }
  void squash_wrong() {
    if (wrong_from == UINT64_MAX) return;
    m->squash(self, wrong_from, now);
    while (!in_flight.empty() && in_flight.back().first >= wrong_from) in_flight.pop_back();
    wrong_from = UINT64_MAX;
  }
  Id wrong_from = UINT64_MAX;
};

void the_list() {
  const vcix_model *m = export_model<Toy<GOOD>>();
  char error[128] = "";
  void *self = m->create(&NO_CONFIG, error, sizeof error);
  const size_t n = m->num_stats(self);
  bool ok = n == 4 + 3 && m->stat(self, n) == nullptr;
  const vcix_stat want[] = {{"pipe", "instructions", 2}, {"pipe", "elements", 8}, {"array", "rows", 1},
                            {"array", "rows_in_flight", 0}, {"committed", "op", 0}, {"committed", "array push", 0},
                            {"committed", "pop", 0}};
  for (size_t i = 0; ok && i < n; i++) {
    const vcix_stat *s = m->stat(self, i);
    ok = !strcmp(s->group, want[i].group) && !strcmp(s->name, want[i].name) && s->capacity == want[i].capacity;
  }
  expect(ok, "the list: each counter as counters() gives it, its capacity 0 for a plain count, then a commit count per "
             "encoding of capacity 0");
  m->destroy(self);

  const auto refused = [](const vcix_model *t, const char *why) {
    char e[160] = "";
    void *s = t->create(&NO_CONFIG, e, sizeof e);
    if (s) t->destroy(s);
    if (s || std::string(e) != why) printf("refused with '%s'\n", e);
    return !s && std::string(e) == why;
  };
  expect(refused(export_model<Toy<NAMED_COMMITTED>>(),
                 "a counter's group is named 'committed', the name of the commit counts") &&
             refused(export_model<Toy<LISTED_TWICE>>(),
                     "counters pipe.instructions and pipe.instructions are both vcix.pipe.instructions as statistics"),
         "create refuses a group named committed and a counter listed twice");
  expect(refused(export_model<Toy<CLASHING_GROUPS>>(), "groups pi.pe and pi_pe are both pi_pe as statistics") &&
             refused(export_model<Toy<CLASHING_NAMES>>(),
                     "counters pipe.la.nes and pipe.la_nes are both vcix.pipe.la_nes as statistics") &&
             refused(export_model<Toy<CLASHING_ENCODINGS>>(),
                     "encodings 'array push' and 'array_push' are both committed.array_push as statistics"),
         "create refuses two groups, two counters of a group or two encodings whose names differ only where "
         "stat_name() prints '_'");
}

void within_capacity() {
  Driver d(1, false);
  bool ok = true, moved = false;
  for (int c = 0; c < 3000; c++) {
    d.cycle();
    const std::vector<uint64_t> v = read(d.m, d.self);
    for (size_t i = 0; i < 3; i++) ok = ok && v[i] <= d.m->stat(d.self, i)->capacity * d.now;
    moved = moved || (v[0] && v[2] && v[3]);
  }
  expect(ok && moved, "each counter of a capacity stays within capacity * cycles at every cycle");
}

// A reset gives the model its room again within the cycle; the counters keep what they counted.
void reset_keeps_the_counts() {
  const vcix_model *m = export_model<Toy<GOOD>>();
  char error[128] = "";
  void *self = m->create(&NO_CONFIG, error, sizeof error);
  const Insn op = {bits_of(OP), 4, 32, 0};
  int accepted = 0;
  m->tick(self, 1);
  for (int round = 0; round < 5; round++) {
    for (int k = 0; k < 3; k++)
      if (m->can_accept(self, &op, 1)) m->issue(self, &op, ++accepted, 1);
    m->reset(self);
  }
  if (m->can_accept(self, &op, 2)) m->issue(self, &op, ++accepted, 2);
  const std::vector<uint64_t> v = read(m, self);
  m->destroy(self);
  expect(accepted == 11 && v[0] == 11 && v[1] == 44,
         "issue, reset and issue again in one cycle: the counters keep counting across resets (11 instructions, 44 "
         "elements)");
}

void squash_restores() {
  bool same = true;
  for (unsigned seed = 1; seed <= 40; seed++) {
    Driver spec(seed, false), plain(seed, false);
    std::mt19937 wrong(seed * 7919);
    for (int c = 0; c < 400; c++) {
      const int phase = c % 23;
      const bool held = phase >= 5 && phase <= 12;
      spec.cycle(phase >= 5 && phase < 9 ? &wrong : nullptr, held);
      plain.cycle(nullptr, held);
      if (phase == 12) spec.squash_wrong();
      if (phase < 5 || phase > 12) same = same && read(spec.m, spec.self) == read(plain.m, plain.self);
    }
    same = same && spec.trace == plain.trace;
  }
  expect(same, "a wrong path issued over four cycles and squashed four later leaves every counter as if never issued");
}

void registration_follows_copies() {
  Instance<Toy<GOOD>> instance;
  instance.configure(Config(&NO_CONFIG));
  const auto inside = [](const Toy<GOOD> &model) {
    for (const Counter *p : model.counters())
      if (reinterpret_cast<const char *>(p) < reinterpret_cast<const char *>(&model) ||
          reinterpret_cast<const char *>(p) >= reinterpret_cast<const char *>(&model + 1))
        return false;
    return true;
  };
  Toy<GOOD> copy = instance.model;
  const Insn op = {bits_of(OP), 5, 32, 0};
  copy.issue(op, 1, 3);
  instance.model = copy;
  std::vector<uint64_t> values(instance.stats().size());
  instance.read_stats(values.data());
  expect(inside(instance.model) && inside(copy) && values[0] == 1 && instance.model.counters()[1]->value() == 5,
         "after a copy-assignment the counters are the live model's, and the counts are the copy's");
}

void reads_change_nothing() {
  Driver quiet(5, false), asked(5, true);
  std::mt19937 wrong_q(11), wrong_a(11);
  for (int c = 0; c < 2000; c++) {
    const bool on = c % 31 < 3, held = c % 31 <= 6;
    quiet.cycle(on ? &wrong_q : nullptr, held);
    asked.cycle(on ? &wrong_a : nullptr, held);
    if (c % 31 == 6) {
      quiet.squash_wrong();
      asked.squash_wrong();
    }
  }
  expect(quiet.trace == asked.trace && read(quiet.m, quiet.self) == read(asked.m, asked.self),
         "reading the statistics, three times before each call, changes no answer and no count");
}

void the_dump() {
  Instance<Toy<GOOD>> instance;
  instance.configure(Config(&NO_CONFIG));
  const Insn op = {bits_of(OP), 4, 32, 0}, push = {bits_of(PUSH), 2, 32, 0};
  instance.tick(1);
  instance.issue(op, 1, 1);
  instance.issue(push, 2, 1);
  for (Cycle c = 2; c <= 4; c++) instance.tick(c);
  instance.commit(op, 1, 4);
  instance.commit(push, 2, 4);
  char text[4096] = "";
  FILE *out = fmemopen(text, sizeof text - 1, "w");
  instance.dump_stats(out);
  fclose(out);
  const char *want =
      "vcix.pipe.instructions                           1  # instructions, at most 2 a cycle\n"
      "vcix.pipe.elements                               4  # elements, at most 8 a cycle\n"
      "vcix.array.rows                                  2  # rows, at most 1 a cycle\n"
      "vcix.array.rows_in_flight                        5  # rows_in_flight\n"
      "vcix.committed.op                                1  # op\n"
      "vcix.committed.array_push                        1  # array push\n"
      "vcix.committed.pop                               0  # pop\n";
  expect(std::string(text) == want, "dump_stats prints the gem5 names: each counter, then the commits");
  if (std::string(text) != want) printf("%s", text);
}

}  // namespace

int main() {
  the_list();
  within_capacity();
  reset_keeps_the_counts();
  squash_restores();
  registration_follows_copies();
  reads_change_nothing();
  the_dump();
  return failed;
}
