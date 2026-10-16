// vcix_uarch: a unit lists its hardware blocks (ports, pipelines, a command queue, registers) and, per instruction,
// the route it takes through them. Machine reads one route as gem5's can_accept and issue, whose record of the steps
// after enqueue() the queue head replays, and as Spike's values. What moves in a cycle is seen the next.
#ifndef VCIX_UARCH_HPP
#define VCIX_UARCH_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <tuple>
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

 private:
  friend class Route;
  template <class...>
  friend class Machine;
  struct Run {
    uint64_t items;
    uint32_t weight;
  };
  void reset_timing() {
    input_ = in_line_ = add_in_ = 0;
    out_[0] = out_[1] = take_[0] = take_[1] = 0;
    runs_.clear();
    std::fill(line_.begin(), line_.end(), 0);
  }
  void queue_run(uint64_t items, uint32_t weight) {
    if (!runs_.empty() && runs_.back().weight == weight) runs_.back().items += items;
    else runs_.push_back({items, weight});
  }
  void advance(Cycle now, Port &entry) {
    const uint8_t leaving = line_[head_];
    for (uint32_t q = 0; q < outputs_; q++)
      if (stall_ == Stall::WHEN_OUTPUT_FULL ? out_[q] == depth_ : leaving > depth_ - out_[q]) return;
    for (uint32_t q = 0; q < outputs_; q++) out_[q] += leaving;
    in_line_ -= leaving;
    const uint32_t w = runs_.empty() ? 0 : runs_.front().weight;
    const uint32_t entering = w && w <= input_ && w <= entry.room(now) ? w : 0;
    if (entering) {
      entry.admit(entering, now);
      if (--runs_.front().items == 0) runs_.erase(runs_.begin());
    }
    line_[head_] = static_cast<uint8_t>(entering);
    input_ -= entering;
    in_line_ += entering;
    head_ = head_ + 1 == line_.size() ? 0 : head_ + 1;
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

// A pipeline whose entries carry values of type T. For Spike it is a plain queue (the unit is infinitely fast): put
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
  template <class...>
  friend class Machine;
  struct Values {
    std::deque<T> items;
    uint64_t taken[2] = {};
  };
  void reset_values() { values_.reset(); }
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
  struct Command {
    Id id;
    Cycle arrives;
    uint32_t step = 0, count = 0;
    uint64_t done = 0;
    bool when_enqueued = false, when_idle = false, holds_slot = true, started = false;
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
  // Unit state the instruction changes as it is issued, before any of its moves; never for Spike.
  template <class F>
  Route &at_issue(F &&change) {
    if (mode_ == ISSUE) change();
    return *this;
  }
  // The instruction's values (Spike only); takes no time.
  template <class F>
  Route &compute(F &&values) {
    if (mode_ == VALUE) values(*host_);
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
          break;
        case RECEIVE: s.pipe->out_[s.output] -= std::min(s.n, s.pipe->out_[s.output]); break;
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
  Mode mode_;
  const Insn &insn_;
  uint32_t arg_;
  const Host *host_;
  Step steps_[MAX_STEPS];
  Step scratch_{REQUIRE};
  uint32_t count_ = 0;
  PipelineTiming *guard_ = nullptr;
  Cycle latency_ = 1;
  bool when_idle_ = false, when_enqueued_ = false;
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
// and Spike's execute. Squash and the commit counts are vcix_accel::Instance's.
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
  void execute(const Host &host, const Insn &insn) override {
    if (!std::apply([&](U &...u) { return (run(Route::VALUE, u, insn, &host, [](Route &) {}) || ...); }, units_)) {
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
    std::apply([&](U &...u) { (tick_unit(u, now), ...); }, units_);
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
  void tick_unit(V &u, Cycle now) {
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) { q.arrivals(now, ready_); }));
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) { head(u, q, now); }));
    u.hardware(blocks(Ignore{}, [&](PipelineTiming &p, Port &entry) {
      p.advance(now, entry);
      entry.hold(p.in_line_);
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
      if (through && moved) through->admit(moved, now);
      c.done += moved;
      if (finished || c.done == s.n) c.step++, c.done = 0;
      else if (!moved) break;
      if (moved) break;
    }
    return c.step == c.count;
  }

  std::tuple<U...> units_;
  std::vector<Id> ready_;
};

}  // namespace vcix_uarch

#endif
