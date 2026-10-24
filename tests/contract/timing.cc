// Test model: a queue of two commands, each ten ticks and then an item; a wait, accepted when the queue is empty; a pop that waits inside for an item, its result time unknown; a use of what a pop wrote. Each issue reports the state.
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <set>

#include "vcix_cp_base.hpp"

namespace {

using namespace cp;
using Insn = vcix_insn;

constexpr size_t DEPTH = 2;
constexpr Cycle TICKS = 10;
constexpr Cycle LATENCY = 3;

enum Form { COMMAND, WAIT, POP, USE };
constexpr const char *NAMES[] = {"command", "wait", "pop", "use"};

Form form(const Insn &insn) {
  if (((insn.bits >> 25) & 1) == 0) return POP;
  if ((insn.bits >> 28) == 2) return USE;
  return (insn.bits >> 26) == 2 ? WAIT : COMMAND;
}

class Timing : public Model {
 public:
  const char *name() const override { return "timing"; }
  std::vector<Encoding> owns() const override { return {{0x5B, 0x7F, "custom-2"}}; }

  void execute(const Host &, const Insn &) override {}
  bool can_accept(const Insn &insn, Cycle) const override {
    const Form f = form(insn);
    return f == WAIT ? queue_.empty() : f != COMMAND || queue_.size() < DEPTH;
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    const Form f = form(insn);
    printf("[model] issue %s: %u issued, %u committed, %zu pops waiting, %" PRIu64 " ticks missing, %u finished", NAMES[f],
           issued_, committed_, waiting_.size(), ticks_ ? now - first_tick_ + 1 - ticks_ : 0, finished_);
    if (finished_) printf(", the last %" PRIu64 " cycles ago", now - finished_at_);
    if (fed_) printf(", the last pop ready %" PRIu64 " cycles ago", now - ready_at_);
    printf("\n");
    fflush(stdout);
    issued_++;
    if (f == COMMAND) queue_.push_back(TICKS);
    if (f != POP) return LATENCY;
    waiting_.push_back(id);
    return Unknown;
  }
  void commit(const Insn &insn, Id id, Cycle now) override {
    committed_++;
    if (form(insn) != POP) return;
    done_.erase(id);
    if (replaying()) return;
    printf("[model] commit pop, ready %" PRIu64 " cycles ago\n", now - ready_at_);
    fflush(stdout);
  }
  void tick(Cycle now) override {
    if (!ticks_++) first_tick_ = now;
    if (!queue_.empty() && --queue_.front() == 0) {
      queue_.pop_front();
      finished_++;
      items_++;
      finished_at_ = now;
    }
    if (!waiting_.empty() && items_) {
      items_--;
      fed_++;
      done_.insert(waiting_.front());
      waiting_.pop_front();
      ready_at_ = now;
    }
  }
  bool ready(Id id, Cycle) const override { return done_.count(id) != 0; }

 private:
  unsigned issued_ = 0, committed_ = 0, finished_ = 0, items_ = 0, fed_ = 0;
  uint64_t ticks_ = 0;
  Cycle first_tick_ = 0, finished_at_ = 0, ready_at_ = 0;
  std::deque<Cycle> queue_;
  std::deque<Id> waiting_;
  std::set<Id> done_;
};

}  // namespace

VCIX_CP_REGISTER(Timing)
