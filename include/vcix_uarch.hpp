// vcix_uarch: a unit lists its hardware blocks (ports, pipelines, a command queue, registers) and, per instruction,
// the route it takes through them. Machine reads one route as gem5's can_accept and issue, and Spike issues it the same
// way and ticks the same cycles with the values on, unless its values are all compute(). Moves are seen the next cycle.
#ifndef VCIX_UARCH_HPP
#define VCIX_UARCH_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "vcix_accel.hpp"

namespace vcix_uarch {

using vcix_accel::Config;
using vcix_accel::Cycle;
using vcix_accel::Encoding;
using vcix_accel::Host;
using vcix_accel::Id;
using vcix_accel::Insn;
using Port = vcix_accel::Port;
constexpr Port::Role PRIMARY = Port::PRIMARY;
constexpr uint64_t UNLIMITED = UINT64_MAX;

// When a pipeline holds still: as soon as an output queue is full, or only when the entry leaving does not fit.
enum class Stall { WHEN_OUTPUT_FULL, WHEN_HEAD_BLOCKED };
// When a command gives back its queue slot: once its work is done, or as the head starts it (a cycle of its own).
enum class Free { WHEN_DONE, WHEN_STARTED };

// Unit state: get() reads it, write() changes it, reset puts back what configure gave. Spike's values keep theirs in
// program order; gem5's copies share it until one writes.
template <class S>
class Register {
 public:
  void configure(S at_reset) { p_ = reset_ = std::make_shared<S>(std::move(at_reset)); }
  const S &get() const { return p_ ? *p_ : none(); }
  S &write() {
    if (!p_) p_ = std::make_shared<S>();
    else if (p_.use_count() > 1) p_ = std::make_shared<S>(*p_);
    return *p_;
  }
  void reset() { p_ = reset_; }
  // What it holds now, which a later write() leaves as it is.
  std::shared_ptr<const S> share() const { return p_ ? p_ : std::make_shared<const S>(); }

 private:
  static const S &none() {
    static const S s{};
    return s;
  }
  std::shared_ptr<S> p_, reset_;
};

class Route;
template <class... U>
class Machine;
constexpr uint32_t MAX_STEPS = 12;

// The timing of a pipeline: an input queue -> `stages` stages -> 1 or 2 output queues, each `depth` deep; an entry of
// `weight` elements enters a cycle through the entry port, and every output queue gets each one that leaves.
class PipelineTiming {
 public:
  void configure(uint64_t stages, uint64_t depth, Stall stall, uint32_t outputs = 1) {
    if (!stages || !depth || outputs < 1 || outputs > 2) {
      std::fprintf(stderr, "vcix_uarch: a pipeline of %llu stages, %llu deep, %u outputs\n",
                   static_cast<unsigned long long>(stages), static_cast<unsigned long long>(depth), outputs);
      std::abort();
    }
    depth_ = depth, stall_ = stall, outputs_ = outputs;
    line_.assign(stages, 0);
    reset_timing();
    head_ = 0;
  }
  bool drained() const { return !input_ && !add_in_ && !in_line_; }
  uint64_t input_entries() const { return input_; }
  uint64_t output_entries(uint32_t output = 0) const { return out_[output]; }
  uint32_t outputs() const { return outputs_; }
  // Forgets what is queued and in the stages; the stages keep their phase.
  void flush() { reset_timing(); }

 protected:
  friend class Route;
  template <class...>
  friend class Machine;
  struct Run {
    uint64_t items;
    uint32_t weight;
  };
  // With the values on (Spike), the value of each entry rides beside its count, in the same slot: Pipeline<T> keeps
  // them, and these move them as the counts move. A route that gives none moves T{} or drops what it takes.
  virtual void enter_value() {}
  virtual void leave_value() {}
  virtual void drop_values() {}
  virtual void send_empty(uint64_t) {}
  virtual void take_unread(uint64_t, uint32_t) {}

  void reset_timing() {
    input_ = in_line_ = add_in_ = 0;
    out_[0] = out_[1] = take_[0] = take_[1] = 0;
    runs_.clear();
    std::fill(line_.begin(), line_.end(), 0);
    drop_values();
  }
  void queue_run(uint64_t items, uint32_t weight) {
    if (!runs_.empty() && runs_.back().weight == weight) runs_.back().items += items;
    else runs_.push_back({items, weight});
  }
  // One cycle of the stages; false when they hold still. `values`: the entries' values move with them.
  bool advance(Cycle now, Port &entry, bool values) {
    const uint8_t leaving = line_[head_];
    for (uint32_t q = 0; q < outputs_; q++)
      if (stall_ == Stall::WHEN_OUTPUT_FULL ? out_[q] == depth_ : leaving > depth_ - out_[q]) return false;
    for (uint32_t q = 0; q < outputs_; q++) out_[q] += leaving;
    in_line_ -= leaving;
    if (values && leaving) leave_value();
    const uint32_t w = runs_.empty() ? 0 : runs_.front().weight;
    const uint32_t entering = w && w <= input_ && w <= entry.room(now) ? w : 0;
    if (entering) {
      entry.admit(entering, now);
      if (--runs_.front().items == 0) runs_.erase(runs_.begin());
      if (values && entering != 1) {
        std::fprintf(stderr, "vcix_uarch: a pipeline whose entries carry values takes entries of one element\n");
        std::abort();
      }
      if (values) enter_value();
    }
    line_[head_] = static_cast<uint8_t>(entering);
    input_ -= entering;
    in_line_ += entering;
    head_ = head_ + 1 == line_.size() ? 0 : head_ + 1;
    return true;
  }
  void apply() {
    input_ += add_in_;
    add_in_ = 0;
    for (uint32_t q = 0; q < 2; q++) out_[q] -= take_[q], take_[q] = 0;
  }
  uint64_t depth_ = 1, input_ = 0, in_line_ = 0, add_in_ = 0, out_[2] = {}, take_[2] = {};
  uint32_t head_ = 0, outputs_ = 1;
  Stall stall_ = Stall::WHEN_OUTPUT_FULL;
  std::vector<Run> runs_;
  std::vector<uint8_t> line_ = std::vector<uint8_t>(1, 0);
};

// A pipeline whose entries carry values of type T. A route's send, receive and the unit's stage function move them in
// the cycles (Spike); for a unit whose values are compute(), it is instead a plain queue that takes no time: put
// appends an entry for every output, take(n, output) removes up to n of that output's.
template <class T = char>
class Pipeline : public PipelineTiming {
 public:
  void put(T value) { values_.write().items.push_back(std::move(value)); }
  std::vector<T> take(uint64_t n, uint32_t output = 0) {
    Values &v = values_.write();
    const uint64_t at = v.taken[output], k = std::min<uint64_t>(n, v.items.size() - at);
    std::vector<T> got(v.items.begin() + static_cast<long>(at), v.items.begin() + static_cast<long>(at + k));
    v.taken[output] += k;
    const uint64_t gone = outputs() == 1 ? v.taken[0] : std::min(v.taken[0], v.taken[1]);
    v.items.erase(v.items.begin(), v.items.begin() + static_cast<long>(gone));
    for (uint32_t q = 0; q < outputs(); q++) v.taken[q] -= gone;
    return got;
  }
  uint64_t values_out(uint32_t output = 0) const { return values_.get().items.size() - values_.get().taken[output]; }

 private:
  friend class Route;
  template <class...>
  friend class Machine;
  // put/take's queue; and the values the entries carry in the input queue, each stage's slot (line_'s order) and
  // each output queue.
  struct Values {
    std::deque<T> items;
    uint64_t taken[2] = {};
    std::deque<T> input, out[2];
    std::vector<T> line;
  };
  void reset_values() { values_.reset(); }
  void carry(T value) { values_.write().input.push_back(std::move(value)); }
  T carried_out(uint32_t output) {
    std::deque<T> &out = values_.write().out[output];
    T value = std::move(out.front());
    out.pop_front();
    return value;
  }
  void enter_value() override {
    Values &c = values_.write();
    if (c.line.size() != line_.size()) c.line.resize(line_.size());
    c.line[head_] = std::move(c.input.front());
    c.input.pop_front();
  }
  void leave_value() override {
    Values &c = values_.write();
    for (uint32_t q = 1; q < outputs(); q++) c.out[q].push_back(c.line[head_]);
    c.out[0].push_back(std::move(c.line[head_]));
    c.line[head_] = T{};
  }
  void drop_values() override {
    const Values &v = values_.get();
    if (v.input.empty() && v.out[0].empty() && v.out[1].empty() && v.line.empty()) return;
    Values &w = values_.write();
    w.input.clear(), w.out[0].clear(), w.out[1].clear(), w.line.clear();
  }
  void send_empty(uint64_t n) override {
    for (uint64_t i = 0; i < n; i++) carry(T{});
  }
  void take_unread(uint64_t n, uint32_t output) override {
    for (uint64_t i = 0; i < n; i++) carried_out(output);
  }
  // f(value, stage) for each entry in the stages, stage 0 the one it entered this cycle.
  template <class F>
  void each_stage(F &&f) {
    const uint32_t n = static_cast<uint32_t>(line_.size());
    if (!in_line_ || values_.get().line.size() != n) return;
    std::vector<T> &line = values_.write().line;
    for (uint32_t stage = 0; stage < n; stage++) {
      const uint32_t slot = (head_ + 2 * n - 1 - stage) % n;
      if (line_[slot]) f(line[slot], stage);
    }
  }
  Register<Values> values_;
};

// Issued instructions in program order; only the head works. Depth 0: no queue, every step at issue. A command
// arrives `arrival_cycles` after its issue and works from the cycle after; `free` says when its slot is given back.
class CommandQueue {
 public:
  void configure(uint32_t depth, Free free = Free::WHEN_DONE, uint32_t arrival_cycles = 1) {
    depth_ = depth, free_ = free, arrival_ = arrival_cycles;
    reset();
  }
  uint32_t depth() const { return depth_; }

 private:
  friend class Route;
  template <class...>
  friend class Machine;
  // A step after enqueue() as issue recorded it, its blocks named by their place in the unit's hardware() (-1: none).
  struct Move {
    uint32_t kind;
    uint64_t n;
    uint32_t weight, output;
    bool at_most, received;
    int32_t through, pipe, guard;
  };
  // `values`: the route Spike issued it from, alive until the command is done, which gives its moves their values.
  struct Command {
    Id id;
    Cycle arrives;
    uint32_t step = 0, count = 0;
    uint64_t done = 0;
    bool when_enqueued = false, when_idle = false, holds_slot = true, started = false;
    Route *values = nullptr;
    Move moves[MAX_STEPS];
  };
  // The ports and pipelines of the unit the queue belongs to, in hardware() order. A copy lists none until asked, so
  // a copy never reaches into the blocks of the unit it was copied from.
  struct Blocks {
    Blocks() = default;
    Blocks(const Blocks &) {}
    Blocks &operator=(const Blocks &) {
      ports.clear(), pipelines.clear(), listed = false;
      return *this;
    }
    bool listed = false;
    std::vector<Port *> ports;
    std::vector<PipelineTiming *> pipelines;
  };
  void reset() {
    list_.clear();
    waiters_.clear();
    used_ = 0;
  }
  void arrivals(Cycle now, std::vector<Id> &ready) const {
    for (const Command &c : list_)
      if (c.when_enqueued && c.arrives == now) ready.push_back(c.id);
  }
  void release_waiters(Cycle now, std::vector<Id> &ready) {
    if (waiters_.empty()) return;
    for (const Command &c : list_)
      if (c.arrives <= now) return;
    ready.insert(ready.end(), waiters_.begin(), waiters_.end());
    waiters_.clear();
  }
  void give_back(Command &c) {
    if (c.holds_slot) c.holds_slot = false, used_--;
  }
  uint32_t depth_ = 0, arrival_ = 1, used_ = 0;
  Free free_ = Free::WHEN_DONE;
  std::deque<Command> list_;
  std::vector<Id> waiters_;
  Blocks blocks_;
};

// The route one instruction takes, which its unit's function declares step by step. Steps before enqueue() happen in
// the issue cycle, all or nothing; steps after it at the queue head, in order, as much a cycle as the ports allow.
class Route {
 public:
  const Insn &insn() const { return insn_; }
  uint32_t vl() const { return insn_.vl; }
  uint32_t arg() const { return arg_; }

  // The unit never takes the instruction while `holds` is false; returns it.
  bool require(bool holds) {
    add(REQUIRE).holds = holds;
    return holds;
  }
  // n of the port's room this cycle.
  Route &use(Port &port, uint64_t n = 1) {
    Step &s = add(USE);
    s.port = &port, s.n = n;
    return *this;
  }
  Route &enqueue(CommandQueue &q) {
    add(ENQUEUE).queue = &q;
    return *this;
  }
  // n entries of `weight` elements into the pipeline's input queue.
  template <class T>
  Route &send(Pipeline<T> &p, uint64_t n, uint32_t weight = 1) {
    Step &s = add(SEND);
    s.pipe = &p, s.n = n, s.weight = weight;
    return *this;
  }
  // Exactly n of an output queue.
  template <class T>
  Route &receive(Pipeline<T> &p, uint64_t n) {
    Step &s = add(RECEIVE);
    s.pipe = &p, s.n = n;
    return *this;
  }
  // Up to n: once n are out, or with what is out once nothing more is on its way.
  template <class T>
  Route &receive_at_most(Pipeline<T> &p, uint64_t n) {
    receive(p, n);
    last().at_most = true;
    return *this;
  }
  // n beats into the unit's own registers or through a port, not into a pipeline.
  Route &transfer(uint64_t n) {
    add(TRANSFER).n = n;
    return *this;
  }
  // As send, with values (Spike): value_of(host, i) -> T is the i-th entry's, made as it enters the input queue. The
  // functions given to a route outlive its declaration: capture the route and the unit by reference, the rest by value.
  template <class T, class F, std::enable_if_t<std::is_invocable_r_v<T, F &, const Host &, uint64_t>, int> = 0>
  Route &send(Pipeline<T> &p, uint64_t n, F value_of) {
    send(p, n);
    return on_moves([&p, value_of](const Host &host, uint64_t i, uint32_t) mutable { p.carry(value_of(host, i)); });
  }
  // As receive and receive_at_most, with values (Spike): take(host, i, T &&) gets the i-th entry as it is taken.
  template <class T, class F, std::enable_if_t<std::is_invocable_v<F &, const Host &, uint64_t, T &&>, int> = 0>
  Route &receive(Pipeline<T> &p, uint64_t n, F take) {
    receive(p, n);
    return on_moves([&p, take](const Host &host, uint64_t i, uint32_t output) mutable {
      take(host, i, p.carried_out(output));
    });
  }
  template <class T, class F, std::enable_if_t<std::is_invocable_v<F &, const Host &, uint64_t, T &&>, int> = 0>
  Route &receive_at_most(Pipeline<T> &p, uint64_t n, F take) {
    receive(p, n, std::move(take));
    last().at_most = true;
    return *this;
  }
  // As transfer, with values (Spike): beat(host, i) as the i-th beat moves.
  template <class F, std::enable_if_t<std::is_invocable_v<F &, const Host &, uint64_t>, int> = 0>
  Route &transfer(uint64_t n, F beat) {
    transfer(n);
    return on_moves([beat](const Host &host, uint64_t i, uint32_t) mutable { beat(host, i); });
  }
  // finish(host) once the instruction is done (Spike).
  template <class F>
  Route &when_done(F finish) {
    if (keeps_values()) carry_->done = std::move(finish);
    has_values_ |= host_ != nullptr;
    return *this;
  }
  // Entries the receives of the instruction took so far (Spike).
  uint64_t received() const { return carry_ ? carry_->received : 0; }
  // The last transfer's n pass this port, at most its width a cycle; at issue the whole n must fit this cycle.
  Route &through(Port &port) {
    last().through = &port;
    return *this;
  }
  // As through(), for what the last receive_at_most actually takes rather than the n it asks.
  Route &through_received(Port &port) {
    through(port);
    last().received = true;
    return *this;
  }
  // Which output queue the last receive reads: 0 or 1.
  Route &from_output(uint32_t output) {
    last().output = output;
    return *this;
  }
  // The next step moves nothing until the pipeline's input queue and stages are empty.
  template <class T>
  Route &wait_until_empty(Pipeline<T> &p) {
    guard_ = &p;
    return *this;
  }
  Route &done_after(Cycle cycles) {
    latency_ = cycles;
    return *this;
  }
  // Done once no command that reached the queue is left unfinished.
  Route &done_when_idle() {
    when_idle_ = true;
    return *this;
  }
  // Done as soon as the queue takes it; its work goes on.
  Route &done_when_enqueued() {
    when_enqueued_ = true;
    return *this;
  }
  // Unit state the instruction changes as it is issued, before any of its moves; on Spike only for a route that gives
  // its moves values (send, receive or transfer with values, or when_done).
  template <class F>
  Route &at_issue(F &&change) {
    if (mode_ == ISSUE) change();
    return *this;
  }
  // The instruction's values (Spike only), at issue; takes no time.
  template <class F>
  Route &compute(F &&values) {
    if (mode_ == VALUE || keeps_values()) values(*host_);
    return *this;
  }

 private:
  template <class...>
  friend class Machine;
  enum Mode { CHECK, ISSUE, VALUE };
  enum Kind { REQUIRE, USE, ENQUEUE, SEND, RECEIVE, TRANSFER };
  struct Step {
    Kind kind;
    uint64_t n = 0;
    uint32_t weight = 1, output = 0;
    bool holds = true, at_most = false, received = false;
    Port *port = nullptr, *through = nullptr;
    PipelineTiming *pipe = nullptr, *guard = nullptr;
    CommandQueue *queue = nullptr;
  };
  struct Hook {
    uint32_t at;
    std::function<void(const Host &, uint64_t, uint32_t)> each;
  };
  // What Spike's issue of the route keeps for its moves: the functions it was given and what its receives took.
  struct Carry {
    std::vector<Hook> hooks;
    std::function<void(const Host &)> done;
    uint64_t received = 0;
  };
  Route(Mode mode, const Insn &insn, uint32_t arg, const Host *host) : mode_(mode), insn_(insn), arg_(arg), host_(host) {}
  Step &add(Kind k) {
    if (mode_ == VALUE) return scratch_ = Step{k};
    if (count_ == MAX_STEPS) {
      std::fprintf(stderr, "vcix_uarch: a route of more than %u steps\n", MAX_STEPS);
      std::abort();
    }
    Step &s = steps_[count_++] = Step{k};
    s.guard = guard_;
    guard_ = nullptr;
    return s;
  }
  Step &last() { return mode_ == VALUE || !count_ ? scratch_ : steps_[count_ - 1]; }
  CommandQueue *queue() const {
    for (uint32_t i = 0; i < count_; i++)
      if (steps_[i].kind == ENQUEUE && steps_[i].queue->depth_) return steps_[i].queue;
    return nullptr;
  }
  template <class B>
  static int32_t place(const std::vector<B *> &listed, const B *block) {
    if (!block) return -1;
    for (size_t i = 0; i < listed.size(); i++)
      if (listed[i] == block) return static_cast<int32_t>(i);
    std::fprintf(stderr, "vcix_uarch: a step after enqueue() names a block its unit's hardware() does not list\n");
    std::abort();
  }
  static CommandQueue::Move record(const Step &s, const CommandQueue::Blocks &b) {
    if (s.kind == USE || s.kind == REQUIRE || s.kind == ENQUEUE) {
      std::fprintf(stderr, "vcix_uarch: use(), require() and enqueue() come before enqueue()\n");
      std::abort();
    }
    return {s.kind, s.n, s.weight, s.output, s.at_most, s.received, place(b.ports, s.through), place(b.pipelines, s.pipe),
            place(b.pipelines, s.guard)};
  }
  uint32_t queued_from() const {
    for (uint32_t i = 0; i < count_; i++)
      if (steps_[i].kind == ENQUEUE && steps_[i].queue->depth_) return i + 1;
    return count_;
  }
  static bool open(const Step &s) { return !s.guard || s.guard->drained(); }
  // What a step moves at issue: n, or for a receive_at_most charged as received, what is out.
  static uint64_t charged(const Step &s) {
    return s.kind == RECEIVE && s.received ? std::min(s.n, s.pipe->out_[s.output]) : s.n;
  }

  // can_accept: every step before the queue holds now, and the queue has a slot.
  bool check(Cycle now) const {
    for (uint32_t i = 0; i < count_; i++) {
      const Step &s = steps_[i];
      if (!open(s)) return false;
      if (s.through && s.kind != USE && s.through->room(now) < charged(s)) return false;
      switch (s.kind) {
        case REQUIRE: if (!s.holds) return false; break;
        case USE: if (s.port->room(now) < s.n) return false; break;
        case ENQUEUE: if (s.queue->depth_) return s.queue->used_ < s.queue->depth_; break;
        case SEND: if (s.n * s.weight > s.pipe->depth_ - s.pipe->input_) return false; break;
        case RECEIVE: if (s.n > s.pipe->out_[s.output] && !(s.at_most && s.pipe->drained())) return false; break;
        default: break;
      }
    }
    return true;
  }
  // issue: the steps before the queue take effect; the rest becomes a command, or is nothing and done now.
  Cycle issue(Cycle now, Id id, std::vector<Id> &ready) {
    for (uint32_t i = 0; i < count_; i++) {
      Step &s = steps_[i];
      if (s.through && s.kind != USE) s.through->admit(charged(s), now);
      switch (s.kind) {
        case USE: s.port->admit(s.n, now); break;
        case SEND:
          if (s.n) s.pipe->input_ += s.n * s.weight, s.pipe->queue_run(s.n, s.weight);
          if (keeps_values()) deliver(i, 0, s.n, SEND, s.pipe, s.output);
          break;
        case RECEIVE: {
          const uint64_t taken = std::min(s.n, s.pipe->out_[s.output]);
          s.pipe->out_[s.output] -= taken;
          if (keeps_values()) deliver(i, 0, taken, RECEIVE, s.pipe, s.output);
          break;
        }
        case TRANSFER:
          if (keeps_values()) deliver(i, 0, s.n, TRANSFER, nullptr, 0);
          break;
        case ENQUEUE: {
          CommandQueue &q = *s.queue;
          if (!q.depth_) break;
          uint64_t work = q.free_ == Free::WHEN_STARTED;
          for (uint32_t j = i + 1; j < count_; j++) work += steps_[j].n;
          if (!work) {
            ready.push_back(id);
            return vcix_accel::Unknown;
          }
          CommandQueue::Command c{id, now + q.arrival_};
          c.when_enqueued = when_enqueued_, c.when_idle = when_idle_;
          if (keeps_values()) c.values = this;
          for (uint32_t j = i + 1; j < count_; j++) c.moves[c.count++] = record(steps_[j], q.blocks_);
          q.list_.push_back(c);
          q.used_++;
          return vcix_accel::Unknown;
        }
        default: break;
      }
    }
    return latency_;
  }

  // The values ride the moves: Spike issues the route with its host, and the functions it was given are kept.
  bool keeps_values() const { return carry_ != nullptr; }
  template <class F>
  Route &on_moves(F &&each) {
    if (keeps_values() && count_) carry_->hooks.push_back({count_ - 1, std::forward<F>(each)});
    has_values_ |= host_ != nullptr;
    return *this;
  }
  // n entries or beats of step `at`, from its from-th on, have moved: each gets its value, or a pipeline T{}.
  void deliver(uint32_t at, uint64_t from, uint64_t n, uint32_t kind, PipelineTiming *pipe, uint32_t output) {
    if (kind == RECEIVE) carry_->received += n;
    for (Hook &h : carry_->hooks)
      if (h.at == at) {
        for (uint64_t i = 0; i < n; i++) h.each(*host_, from + i, output);
        return;
      }
    if (kind == SEND) pipe->send_empty(n);
    else if (kind == RECEIVE) pipe->take_unread(n, output);
  }
  void finish() {
    if (carry_->done) carry_->done(*host_);
  }

  Mode mode_;
  const Insn &insn_;
  uint32_t arg_;
  const Host *host_;
  Step steps_[MAX_STEPS];
  Step scratch_{REQUIRE};
  uint32_t count_ = 0;
  PipelineTiming *guard_ = nullptr;
  Cycle latency_ = 1;
  Carry *carry_ = nullptr;
  bool when_idle_ = false, when_enqueued_ = false, has_values_ = false;
};

// An encoding, its name, and the unit's function that declares its route; `arg` tells a family's members apart.
template <class U>
struct Instruction {
  uint32_t match, mask;
  const char *name;
  void (U::*route)(Route &);
  uint32_t arg = 0;
};

// A model of units in order. A unit is constructed with its name, reads its keys in configure(Config), lists its blocks
// in hardware(h) and its encodings in INSTRUCTIONS; Machine derives tick, reset, the ports (the statistics), readiness,
// and Spike's execute, which clocks the machine. Squash and the commit counts are vcix_accel::Instance's.
template <class... U>
class Machine : public vcix_accel::Model {
 public:
  std::vector<Encoding> owns() const override {
    std::vector<Encoding> all;
    (add<U>(all), ...);
    return all;
  }
  void configure(const Config &c) override {
    std::apply([&](U &...u) { (u.configure(c), ...); }, units_);
    reset();
  }
  // Spike: a route that gives its moves values is issued as gem5 issues it, the machine ticked with the values on while
  // it is not taken and until it is done; the machine stands still between instructions. Else its compute() is all.
  void execute(const Host &host, const Insn &insn) override {
    if (!std::apply([&](U &...u) { return (execute_on(u, insn, host) || ...); }, units_)) {
      std::fprintf(stderr, "vcix_uarch: instruction %08x has no unit\n", insn.bits);
      std::abort();
    }
  }
  bool can_accept(const Insn &insn, Cycle now) const override {
    bool ok = false;
    auto &self = const_cast<Machine &>(*this);
    std::apply([&](U &...u) { (run(Route::CHECK, u, insn, nullptr, [&](Route &r) { ok = r.check(now); }) || ...); },
               self.units_);
    return ok;
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    Cycle latency = 1;
    std::apply([&](U &...u) {
      (run(Route::ISSUE, u, insn, nullptr, [&](Route &r) {
         if (CommandQueue *q = r.queue()) list_blocks(u, *q);
         latency = r.issue(now, id, ready_);
       }) || ...);
    }, units_);
    return latency;
  }
  void commit(const Insn &, Id id, Cycle) override {
    const auto it = std::find(ready_.begin(), ready_.end(), id);
    if (it != ready_.end()) ready_.erase(it);
  }
  bool ready(Id id, Cycle) const override { return std::find(ready_.begin(), ready_.end(), id) != ready_.end(); }
  void tick(Cycle now) override {
    std::apply([&](U &...u) { (tick_unit(u, now, false), ...); }, units_);
  }
  void reset() override {
    std::apply([&](U &...u) {
      (u.hardware(blocks([](Port &p) { p.reset(); },
                         [](auto &p, Port &) {
                           p.reset_timing();
                           p.reset_values();
                         },
                         [](CommandQueue &q) { q.reset(); }, [](auto &r) { r.reset(); })),
       ...);
    }, units_);
    ready_.clear();
  }
  std::vector<const Port *> ports() const override {
    std::vector<const Port *> all;
    auto &self = const_cast<Machine &>(*this);
    std::apply([&](U &...u) { (u.hardware(blocks([&](Port &p) { all.push_back(&p); })), ...); }, self.units_);
    return all;
  }

 protected:
  template <class... Name>
  explicit Machine(Name... names) : units_(names...) {
    static_assert(sizeof...(Name) == sizeof...(U), "a machine names each of its units");
  }
  // `what` on the first unit that owns the instruction; false when none does.
  template <class Do>
  bool with_owner(const Insn &insn, Do &&what) const {
    return std::apply([&](const U &...u) { return (owned(u, insn, what) || ...); }, units_);
  }
  // `what` on each pipeline the unit lists.
  template <class V, class Do>
  static void each_pipeline(const V &unit, Do &&what) {
    const_cast<V &>(unit).hardware(blocks(Ignore{}, [&](const PipelineTiming &p, const Port &) { what(p); }));
  }

 private:
  struct Ignore {
    template <class... A>
    void operator()(A &&...) const {}
  };
  // One pass over a unit's hardware(): what to do with each kind of block.
  template <class P, class L, class Q, class R>
  struct Blocks {
    P on_port;
    L on_pipeline;
    Q on_queue;
    R on_register;
    void port(Port &p) { on_port(p); }
    template <class T>
    void pipeline(Pipeline<T> &p, Port &entry) { on_pipeline(p, entry); }
    // A pipeline whose entries the unit's `stage(value, k)` works on in each stage k they reach, with the values on.
    template <class T, class V>
    void pipeline(Pipeline<T> &p, Port &entry, void (V::*stage)(T &, uint32_t)) {
      if constexpr (std::is_invocable_v<L &, Pipeline<T> &, Port &, decltype(stage)>) on_pipeline(p, entry, stage);
      else on_pipeline(p, entry);
    }
    void queue(CommandQueue &q) { on_queue(q); }
    template <class S>
    void reg(Register<S> &r) { on_register(r); }
  };
  template <class P = Ignore, class L = Ignore, class Q = Ignore, class R = Ignore>
  static Blocks<P, L, Q, R> blocks(P p = {}, L l = {}, Q q = {}, R r = {}) {
    return {p, l, q, r};
  }

  template <class V>
  static void add(std::vector<Encoding> &all) {
    for (const auto &i : V::INSTRUCTIONS) all.push_back({i.match, i.mask, i.name});
  }
  template <class V, class Do>
  static bool owned(const V &u, const Insn &insn, Do &&what) {
    for (const auto &i : V::INSTRUCTIONS)
      if ((insn.bits & i.mask) == i.match) return what(u), true;
    return false;
  }
  // The route of the unit's first instruction that matches, declared in `mode`, then `after` on it.
  template <class V, class After>
  static bool run(typename Route::Mode mode, V &u, const Insn &insn, const Host *host, After &&after) {
    for (const auto &i : V::INSTRUCTIONS) {
      if ((insn.bits & i.mask) != i.match) continue;
      Route r(mode, insn, i.arg, host);
      (u.*i.route)(r);
      after(r);
      return true;
    }
    return false;
  }
  // A cycle of one unit: commands done when enqueued that arrive are done; each queue's head works; the pipelines
  // move; then what everything moved becomes visible, and a command waiting for an idle queue may be done.
  template <class V>
  void tick_unit(V &u, Cycle now, bool values) {
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) { q.arrivals(now, ready_); }));
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) { head(u, q, now); }));
    u.hardware(blocks(Ignore{}, [&](auto &p, Port &entry, auto... stage) {
      const bool moved = p.advance(now, entry, values);
      entry.hold(p.in_line_);
      if constexpr (sizeof...(stage) == 1)
        if (values && moved) p.each_stage([&](auto &value, uint32_t k) { ((u.*stage)(value, k), ...); });
    }));
    u.hardware(blocks(Ignore{}, [](PipelineTiming &p, Port &) { p.apply(); },
                      [&](CommandQueue &q) { q.release_waiters(now, ready_); }));
  }
  template <class V>
  void head(V &u, CommandQueue &q, Cycle now) {
    if (q.list_.empty()) return;
    CommandQueue::Command &c = q.list_.front();
    if (c.arrives >= now) return;
    list_blocks(u, q);
    if (!advance(c, q, now)) return;
    if (c.when_idle) q.waiters_.push_back(c.id);
    else if (!c.when_enqueued) ready_.push_back(c.id);
    q.give_back(c);
    q.list_.pop_front();
  }

  template <class V>
  static void list_blocks(V &u, CommandQueue &q) {
    if (q.blocks_.listed) return;
    q.blocks_.listed = true;
    u.hardware(blocks([&](Port &p) { q.blocks_.ports.push_back(&p); },
                      [&](PipelineTiming &p, Port &) { q.blocks_.pipelines.push_back(&p); }));
  }
  // One cycle of the head command: its recorded step moves what the registered state allows. True when it is done.
  static bool advance(CommandQueue::Command &c, CommandQueue &q, Cycle now) {
    if (q.free_ == Free::WHEN_STARTED && !c.started) {
      c.started = true;
      q.give_back(c);
      return c.count == 0;
    }
    while (c.step < c.count) {
      const CommandQueue::Move &s = c.moves[c.step];
      Port *through = s.through < 0 ? nullptr : q.blocks_.ports[s.through];
      PipelineTiming *pipe = s.pipe < 0 ? nullptr : q.blocks_.pipelines[s.pipe];
      const bool open = s.guard < 0 || q.blocks_.pipelines[s.guard]->drained();
      const uint64_t left = s.n - c.done, width = through ? through->room(now) : UNLIMITED;
      uint64_t moved = 0;
      bool finished = false;
      if (s.kind == Route::SEND) {
        if (open) {
          moved = std::min({left, (pipe->depth_ - pipe->input_ - pipe->add_in_) / s.weight, width});
          if (moved) pipe->add_in_ += moved * s.weight, pipe->queue_run(moved, s.weight);
        }
      } else if (s.kind == Route::RECEIVE) {
        const uint64_t out = pipe->out_[s.output] - pipe->take_[s.output];
        if (open) moved = std::min({left, out, width});
        pipe->take_[s.output] += moved;
        finished = s.at_most && !out && pipe->drained();
      } else if (open) {
        moved = std::min(left, width);
      }
      if (c.values && moved) c.values->deliver(c.values->queued_from() + c.step, c.done, moved, s.kind, pipe, s.output);
      if (through && moved) through->admit(moved, now);
      c.done += moved;
      if (finished || c.done == s.n) c.step++, c.done = 0;
      else if (!moved) break;
      if (moved) break;
    }
    return c.step == c.count;
  }

  // Spike's execute on unit u, when it owns the instruction: the route declared with the host says whether its moves
  // carry values; if they do, it is clocked, else declared again for its compute().
  template <class V>
  bool execute_on(V &u, const Insn &insn, const Host &host) {
    for (const auto &i : V::INSTRUCTIONS) {
      if ((insn.bits & i.mask) != i.match) continue;
      Route probe(Route::CHECK, insn, i.arg, &host);
      (u.*i.route)(probe);
      if (probe.has_values_) clocked(u, i, insn, host);
      else run(Route::VALUE, u, insn, &host, [](Route &) {});
      return true;
    }
    return false;
  }
  template <class V, class I>
  void clocked(V &u, const I &instruction, const Insn &insn, const Host &host) {
    const auto accepts = [&] {
      Route r(Route::CHECK, insn, instruction.arg, nullptr);
      (u.*instruction.route)(r);
      return r.check(clock_);
    };
    while (!accepts()) spike_cycle(u, instruction.name, accepts);
    Route::Carry carry;
    Route r(Route::ISSUE, insn, instruction.arg, &host);
    r.carry_ = &carry;
    (u.*instruction.route)(r);
    if (CommandQueue *q = r.queue()) list_blocks(u, *q);
    const Id id = ++spike_id_;
    const Cycle issued = clock_, latency = r.issue(clock_, id, ready_);
    if (latency != vcix_accel::Unknown)
      while (clock_ < issued + latency) tick_values(clock_ + 1);
    const auto done = [&] {
      return (latency != vcix_accel::Unknown || Machine::ready(id, clock_)) && !queued(u, id);
    };
    while (!done()) spike_cycle(u, instruction.name, done);
    Machine::commit(insn, id, clock_);
    r.finish();
  }
  void tick_values(Cycle now) {
    clock_ = now;
    std::apply([&](U &...u) { (tick_unit(u, now, true), ...); }, units_);
  }
  // One cycle Spike waits for `until`; a cycle in which nothing moves, no command still on its way and `until` not
  // yet true, is the same every cycle after: the run ends there.
  template <class V, class Until>
  void spike_cycle(V &u, const char *name, Until &&until) {
    const std::vector<uint64_t> before = motion(u);
    tick_values(clock_ + 1);
    if (until() || motion(u) != before || arriving(u)) return;
    std::fprintf(stderr, "vcix_uarch: %s waits for the unit, and nothing in it moves any more (on Spike, which runs an "
                 "instruction to its end before the next: a push no later pop can make room for?)\n", name);
    std::exit(1);
  }
  // What a cycle of unit u can change in its pipelines and queues.
  template <class V>
  std::vector<uint64_t> motion(V &u) const {
    std::vector<uint64_t> m{ready_.size()};
    u.hardware(blocks(Ignore{},
                      [&](PipelineTiming &p, Port &) {
                        m.insert(m.end(), {p.input_, p.in_line_, p.out_[0], p.out_[1], p.runs_.size(),
                                           p.in_line_ ? p.head_ : 0});
                      },
                      [&](CommandQueue &q) {
                        m.insert(m.end(), {q.list_.size(), q.used_, q.waiters_.size()});
                        if (!q.list_.empty())
                          m.insert(m.end(), {q.list_.front().step, q.list_.front().done, q.list_.front().started});
                      }));
    return m;
  }
  template <class V>
  bool arriving(V &u) const {
    bool any = false;
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) {
      for (const CommandQueue::Command &c : q.list_) any |= c.arrives >= clock_;
    }));
    return any;
  }
  template <class V>
  static bool queued(V &u, Id id) {
    bool any = false;
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) {
      for (const CommandQueue::Command &c : q.list_) any |= c.id == id;
      any |= std::find(q.waiters_.begin(), q.waiters_.end(), id) != q.waiters_.end();
    }));
    return any;
  }

  std::tuple<U...> units_;
  std::vector<Id> ready_;
  Cycle clock_ = 0;
  Id spike_id_ = 0;
};

}  // namespace vcix_uarch

#endif
