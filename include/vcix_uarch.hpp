// vcix_uarch: a unit lists its hardware blocks (ports, pipelines, a command queue, registers) and, per instruction,
// the route it takes through them. Machine reads one route as gem5's can_accept and issue; Spike issues it the same way
// and ticks the same cycles, the same state carrying values where gem5's carries none. Moves are seen next cycle.
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

#include "vcix_cp_base.hpp"

namespace vcix_uarch {

using cp::Config;
using cp::Cycle;
using cp::Encoding;
using cp::Host;
using cp::Id;
using Insn = vcix_insn;
using Port = cp::Stats;
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
  // What get() reads now, kept as it is when the register is written later.
  std::shared_ptr<const S> share() const { return p_; }
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

// Entries in order, held as runs: a run is `count` entries of `weight` rows, the i-th tagged tag + i if it steps, else
// tag. On Spike each row of an entry also has its value; gem5's never do. The first runs sit inline; the rest and the
// values are a block that copies share until one writes, so a copy of a few runs allocates nothing.
class Slots {
 public:
  struct Run {
    uint32_t tag, count;
    uint16_t weight;
    bool steps;
  };
  bool empty() const { return !runs_; }
  const Run &front() const { return few_[0]; }
  const Run &back() const { return run(runs_ - 1); }
  uint64_t rows() const {
    uint64_t n = 0;
    for (uint32_t i = 0; i < runs_; i++) n += uint64_t{run(i).count} * run(i).weight;
    return n;
  }
  // The rows of the entries tagged `tag`, of runs that do not step.
  uint64_t rows(uint32_t tag) const {
    uint64_t n = 0;
    for (uint32_t i = 0; i < runs_; i++)
      if (!run(i).steps && run(i).tag == tag) n += uint64_t{run(i).count} * run(i).weight;
    return n;
  }
  bool valued() const { return more_ && more_->valued(); }
  // f(first row, rows, tag) for each entry.
  template <class F>
  void each(F &&f) const {
    uint64_t row = 0;
    for (uint32_t r = 0; r < runs_; r++)
      for (uint32_t k = 0; k < run(r).count; k++, row += run(r).weight) f(row, run(r).weight, run(r).tag + (run(r).steps ? k : 0));
  }
  void push(uint64_t count, uint32_t weight, uint32_t tag = 0, bool steps = false) {
    if (!count) return;
    if (count > UINT32_MAX || weight > UINT16_MAX) too_many(count, weight);
    const Run add{tag, static_cast<uint32_t>(count), static_cast<uint16_t>(weight), steps};
    Run *last = !runs_ ? nullptr : runs_ <= FEW ? &few_[runs_ - 1] : &own().runs.back();
    if (last && last->weight == weight && last->steps == steps && last->tag + (steps ? last->count : 0) == tag &&
        uint64_t{last->count} + count <= UINT32_MAX)
      last->count += add.count;
    else if (runs_ < FEW) few_[runs_++] = add;
    else own().runs.push_back(add), runs_++;
    if (valued()) own().add(count * weight);
  }
  void pop(uint64_t n) {
    uint64_t rows = 0;
    for (uint64_t left = n; left;) {
      Run &f = few_[0];
      if (left < f.count) {
        rows += left * f.weight;
        f.count -= static_cast<uint32_t>(left), f.tag += f.steps ? static_cast<uint32_t>(left) : 0;
        break;
      }
      left -= f.count, rows += uint64_t{f.count} * f.weight;
      erase(0);
    }
    if (valued()) own().drop(rows, !runs_);
  }
  // Runs that do not step: every entry tagged `from` is tagged `to`.
  void retag(uint32_t from, uint32_t to) {
    bool any = false;
    for (uint32_t i = 0; i < runs_; i++)
      if (!run(i).steps && run(i).tag == from) mut(i).tag = to, any = true;
    if (any) merge();
  }
  // Runs that do not step: the n entries after the front ones tagged `tag` are tagged `tag` too.
  void tag_front(uint64_t n, uint32_t tag) {
    uint32_t r = 0;
    while (r < runs_ && run(r).tag == tag) r++;
    for (; n && r < runs_; r++) {
      if (n < run(r).count) {
        Run rest = run(r);
        rest.count -= static_cast<uint32_t>(n), mut(r).count = static_cast<uint32_t>(n);
        insert(r + 1, rest);
      }
      n -= mut(r).count, mut(r).tag = tag;
    }
    merge();
  }
  void clear() {
    runs_ = 0;
    more_.reset();
  }
  // The value of row i, which a row of a Slots that carries no values yet gets as T{}.
  template <class T>
  T &value(uint64_t i) {
    Values<T> &v = values<T>();
    return v.items[v.first + i];
  }
  // The front entry into `to`, tagged `tag`, its values with it.
  void pass(Slots &to, uint32_t tag, bool steps) {
    const uint32_t weight = front().weight;
    to.push(1, weight, tag, steps);
    if (valued()) own().give(to, weight, false);
    pop(1);
  }
  // The front entry's rows into `to` as entries of one row; `copy`: their values stay here too.
  void give_rows(Slots &to, bool copy) {
    to.push(front().weight, 1);
    if (valued()) own().give(to, front().weight, copy);
  }

 private:
  static constexpr uint32_t FEW = 3;
  [[noreturn]] static void too_many(uint64_t count, uint32_t weight) {
    std::fprintf(stderr, "vcix_uarch: %llu entries of %u rows: a pipeline holds at most 2^32 - 1 entries of 65535\n",
                 static_cast<unsigned long long>(count), weight);
    std::abort();
  }
  // The runs past the first few, and with values (Values<T>) the rows', from `first` on.
  struct More {
    virtual ~More() = default;
    virtual std::shared_ptr<More> copy() const { return std::make_shared<More>(*this); }
    virtual bool valued() const { return false; }
    virtual void add(uint64_t) {}
    virtual void drop(uint64_t, bool) {}
    virtual void give(Slots &, uint64_t, bool) {}
    std::vector<Run> runs;
  };
  template <class T>
  struct Values : More {
    std::shared_ptr<More> copy() const override { return std::make_shared<Values>(*this); }
    bool valued() const override { return true; }
    void add(uint64_t n) override { items.resize(items.size() + n); }
    void drop(uint64_t n, bool all) override {
      first += n;
      if (all) items.clear(), first = 0;
      else if (first >= 64 && 2 * first >= items.size())
        items.erase(items.begin(), items.begin() + static_cast<long>(first)), first = 0;
    }
    // The front n rows' values to the last n rows of `to`.
    void give(Slots &to, uint64_t n, bool keep) override {
      Values &v = to.values<T>();
      T *at = &v.items[v.items.size() - n];
      for (uint64_t k = 0; k < n; k++) at[k] = keep ? items[first + k] : std::move(items[first + k]);
    }
    std::vector<T> items;
    uint64_t first = 0;
  };
  template <class T>
  Values<T> &values() {
    if (!valued()) {
      auto v = std::make_shared<Values<T>>();
      if (more_) v->runs = more_->runs;
      v->items.resize(rows());
      more_ = v;
    }
    return static_cast<Values<T> &>(own());
  }
  More &own() {
    if (!more_) more_ = std::make_shared<More>();
    else if (more_.use_count() > 1) more_ = more_->copy();
    return *more_;
  }
  const Run &run(uint32_t i) const { return i < FEW ? few_[i] : more_->runs[i - FEW]; }
  Run &mut(uint32_t i) { return i < FEW ? few_[i] : own().runs[i - FEW]; }
  void insert(uint32_t at, const Run &add) {
    if (runs_ >= FEW) {
      More &m = own();
      if (at >= FEW) m.runs.insert(m.runs.begin() + (at - FEW), add);
      else m.runs.insert(m.runs.begin(), few_[FEW - 1]);
    }
    if (at < FEW) {
      for (uint32_t i = std::min(runs_, FEW - 1); i > at; i--) few_[i] = few_[i - 1];
      few_[at] = add;
    }
    runs_++;
  }
  void erase(uint32_t at) {
    if (at >= FEW) {
      More &m = own();
      m.runs.erase(m.runs.begin() + (at - FEW));
    } else {
      for (uint32_t i = at + 1; i < FEW && i < runs_; i++) few_[i - 1] = few_[i];
      if (runs_ > FEW) {
        More &m = own();
        few_[FEW - 1] = m.runs.front(), m.runs.erase(m.runs.begin());
      }
    }
    runs_--;
  }
  // Neighbouring runs that continue one another become one.
  void merge() {
    for (uint32_t i = 1; i < runs_;) {
      const Run &a = run(i - 1), &b = run(i);
      if (a.weight == b.weight && a.steps == b.steps && a.tag + (a.steps ? a.count : 0) == b.tag &&
          uint64_t{a.count} + b.count <= UINT32_MAX) {
        const uint32_t count = b.count;
        mut(i - 1).count += count;
        erase(i);
      } else {
        i++;
      }
    }
  }
  Run few_[FEW] = {};
  uint32_t runs_ = 0;
  std::shared_ptr<More> more_;
};

// A pipeline, whatever its entries carry: an input queue -> `stages` stages -> 1 or 2 output queues, each `depth` rows
// deep, all slots. Rows that arrive in a cycle are tagged so and seen in the next; rows taken in a cycle are tagged so
// and hold their slots to its end. An entry of `weight` rows leaves the stages as that many rows, into every output.
class AnyPipeline {
 public:
  void configure(uint64_t stages, uint64_t depth, Stall stall, uint32_t outputs = 1) {
    if (!stages || stages > UINT32_MAX || !depth || outputs < 1 || outputs > 2) {
      std::fprintf(stderr, "vcix_uarch: a pipeline of %llu stages, %llu deep, %u outputs\n",
                   static_cast<unsigned long long>(stages), static_cast<unsigned long long>(depth), outputs);
      std::abort();
    }
    stages_ = static_cast<uint32_t>(stages), depth_ = depth, stall_ = stall, outputs_ = outputs;
    flush();
  }
  bool drained() const { return input_.empty() && line_.empty(); }
  uint64_t input_rows() const { return input_.rows(SEEN); }
  uint64_t output_rows(uint32_t output = 0) const { return out_[output].rows(); }
  // Forgets what is queued and in the stages.
  void flush() {
    input_.clear(), line_.clear(), out_[0].clear(), out_[1].clear();
  }

 protected:
  friend class Route;
  template <class...>
  friend class Machine;
  AnyPipeline() = default;
  uint64_t input_room() const { return depth_ - input_.rows(); }
  // The tag of an input queue's run (seen, or arrived this cycle) and of an output queue's (seen, or taken this cycle).
  static constexpr uint32_t SEEN = 0, ARRIVED = 1, TAKEN = 1;
  uint64_t out(uint32_t output) const { return out_[output].rows(SEEN); }
  void arrive(uint64_t entries, uint32_t weight) { input_.push(entries, weight, ARRIVED); }
  void take_rows(uint64_t rows, uint32_t output) { out_[output].tag_front(rows, TAKEN); }
  // One cycle of the stages; false when they hold still. An entry in the stages is tagged with the move it entered at
  // (mod 2^32); it leaves `stages` moves later.
  bool advance(Cycle now, Port &entry) {
    const uint32_t leaving = !line_.empty() && moves_ - line_.front().tag == stages_ ? line_.front().weight : 0;
    for (uint32_t q = 0; q < outputs_; q++)
      if (stall_ == Stall::WHEN_OUTPUT_FULL ? out_[q].rows() == depth_ : leaving > depth_ - out_[q].rows())
        return false;
    if (leaving) {
      for (uint32_t q = 0; q < outputs_; q++) line_.give_rows(out_[q], q + 1 < outputs_);
      line_.pop(1);
    }
    const uint32_t weight = input_.empty() || input_.front().tag != SEEN ? 0 : input_.front().weight;
    if (weight && weight <= entry.available(now)) {
      entry.admit(weight, now);
      input_.pass(line_, moves_, true);
    }
    moves_++;
    return true;
  }
  // Arrivals sit at the back of the input queue, what was taken at the front of an output queue.
  void end_cycle() {
    if (!input_.empty() && input_.back().tag == ARRIVED) input_.retag(ARRIVED, SEEN);
    for (uint32_t q = 0; q < outputs_; q++)
      if (!out_[q].empty() && out_[q].front().tag == TAKEN) out_[q].pop(out_[q].rows(TAKEN));
  }
  uint64_t depth_ = 1;
  uint32_t stages_ = 1, outputs_ = 1, moves_ = 0;
  Stall stall_ = Stall::WHEN_OUTPUT_FULL;
  Slots input_, line_, out_[2];
};

// A pipeline whose rows carry values of type T on Spike.
template <class T = char>
class Pipeline : public AnyPipeline {
 private:
  friend class Route;
  template <class...>
  friend class Machine;
  // Row `row` of the input queue, as it arrives; row `row` of an output queue, as it is taken.
  void arrived(uint64_t row, T value) { input_.value<T>(row) = std::move(value); }
  T taken(uint32_t output, uint64_t row) { return std::move(out_[output].value<T>(row)); }
  // f(value, stage) for each row in the stages, stage 0 the one it entered this cycle.
  template <class F>
  void each_stage(F &&f) {
    if (line_.valued())
      line_.each([&](uint64_t row, uint32_t rows, uint32_t tag) {
        for (uint32_t k = 0; k < rows; k++) f(line_.value<T>(row + k), moves_ - 1 - tag);
      });
  }
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
  bool idle() const { return list_.empty(); }

 private:
  friend class Route;
  template <class...>
  friend class Machine;
  // A step after enqueue() as issue recorded it, its blocks named by their place in the unit's hardware() (-1: none).
  struct Move {
    uint32_t kind;
    uint64_t n;
    uint32_t weight, output;
    bool received;
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
    std::vector<AnyPipeline *> pipelines;
  };
  void reset() {
    list_.clear();
    waiters_.clear();
  }
  // The slots its commands hold: all of them, but a head that gave its slot back as it started.
  uint64_t held() const { return list_.size() - (!list_.empty() && !list_.front().holds_slot); }
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
  uint32_t depth_ = 0, arrival_ = 1;
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
    if (!holds) when_stuck([](uint64_t) {});
    return holds;
  }
  // As require(holds), with never() as the reason when_stuck() gives.
  template <class F>
  bool require(bool holds, F never) {
    if (!holds) when_stuck([never](uint64_t) { never(); });
    return require(holds);
  }
  // On Spike, once the instruction waits and nothing in its unit moves, never(got) may say why and end the run; got:
  // the rows its waiting receive has. The first reason, or failing require(), decides; capture by value.
  template <class F>
  Route &when_stuck(F never) {
    if (host_ && !stuck_) stuck_ = std::move(never);
    return *this;
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
  // n beats into the unit's own registers or through a port, not into a pipeline.
  Route &transfer(uint64_t n) {
    add(TRANSFER).n = n;
    return *this;
  }
  // As send, with values (Spike): value_of(host, j) -> T is row j's, the entries' rows in order, made as its entry
  // enters the input queue. The functions given to a route outlive its declaration: capture the route and the unit by
  // reference, the rest by value.
  template <class T, class F, std::enable_if_t<std::is_invocable_r_v<T, F &, const Host &, uint64_t>, int> = 0>
  Route &send(Pipeline<T> &p, uint64_t n, uint32_t weight, F value_of) {
    send(p, n, weight);
    return on_moves([&p, weight, value_of](const Host &host, uint64_t i, uint64_t row, uint32_t) mutable {
      for (uint32_t k = 0; k < weight; k++) p.arrived(row + k, value_of(host, i * weight + k));
    });
  }
  template <class T, class F, std::enable_if_t<std::is_invocable_r_v<T, F &, const Host &, uint64_t>, int> = 0>
  Route &send(Pipeline<T> &p, uint64_t n, F value_of) {
    return send(p, n, 1, std::move(value_of));
  }
  // As receive, with values (Spike): take(host, i, T &&) gets the i-th row as it is taken.
  template <class T, class F, std::enable_if_t<std::is_invocable_v<F &, const Host &, uint64_t, T &&>, int> = 0>
  Route &receive(Pipeline<T> &p, uint64_t n, F take) {
    receive(p, n);
    return on_moves([&p, take](const Host &host, uint64_t i, uint64_t row, uint32_t output) mutable {
      take(host, i, p.taken(output, row));
    });
  }
  // As transfer, with values (Spike): beat(host, i) as the i-th beat moves.
  template <class F, std::enable_if_t<std::is_invocable_v<F &, const Host &, uint64_t>, int> = 0>
  Route &transfer(uint64_t n, F beat) {
    transfer(n);
    return on_moves([beat](const Host &host, uint64_t i, uint64_t, uint32_t) mutable { beat(host, i); });
  }
  // received(host) as the last receive ends (Spike).
  template <class F>
  Route &when_received(F received) {
    if (keeps_values() && count_) hooks_->received.push_back({count_ - 1, std::move(received)});
    return *this;
  }
  // The last step's n (sent, received or transferred) pass this port, at most its width a cycle; at issue the whole n
  // must fit this cycle.
  Route &through(Port &port) {
    last().through = &port;
    return *this;
  }
  // As through(), counting at most what is out: the same once the receive can issue, but a stuck receive of more than
  // the port's width still gets its when_stuck() reason on Spike.
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
  // What the instruction changes as it is issued, before any of its moves: change() unit state, on gem5 and Spike;
  // change(host) values, on Spike only.
  template <class F>
  Route &at_issue(F &&change) {
    if constexpr (std::is_invocable_v<F &, const Host &>) {
      if (keeps_values()) change(*host_);
    } else if (mode_ == ISSUE) {
      change();
    }
    return *this;
  }
  // done(host) as the instruction is done (Spike).
  template <class F>
  Route &when_done(F done) {
    if (keeps_values()) hooks_->done.push_back(std::move(done));
    return *this;
  }

 private:
  template <class...>
  friend class Machine;
  enum Mode { CHECK, ISSUE };
  enum Kind { REQUIRE, USE, ENQUEUE, SEND, RECEIVE, TRANSFER };
  // Trivial, so the steps a route does not declare cost nothing; made() is a declared one.
  struct Step {
    Kind kind;
    uint64_t n;
    uint32_t weight, output;
    bool holds, received;
    Port *port, *through;
    AnyPipeline *pipe, *guard;
    CommandQueue *queue;
    static Step made(Kind k) {
      Step s{k, 0, 1, 0, true, false, nullptr, nullptr, nullptr, nullptr, nullptr};
      return s;
    }
  };
  // each(host, i, row, output) for the i-th entry or beat of step `at`, row its first row in the queue it moved in.
  struct Move {
    uint32_t at;
    std::function<void(const Host &, uint64_t, uint64_t, uint32_t)> each;
  };
  struct Received {
    uint32_t at;
    std::function<void(const Host &)> done;
  };
  // The functions Spike's issue of the route keeps for its values until it is done.
  struct Hooks {
    std::vector<Move> moves;
    std::vector<Received> received;
    std::vector<std::function<void(const Host &)>> done;
  };
  Route(Mode mode, const Insn &insn, uint32_t arg, const Host *host) : mode_(mode), insn_(insn), arg_(arg), host_(host) {}
  Step &add(Kind k) {
    if (count_ == MAX_STEPS) {
      std::fprintf(stderr, "vcix_uarch: a route of more than %u steps\n", MAX_STEPS);
      std::abort();
    }
    Step &s = steps_[count_++] = Step::made(k);
    s.guard = guard_;
    guard_ = nullptr;
    return s;
  }
  Step &last() { return steps_[count_ - 1]; }
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
    return {s.kind, s.n, s.weight, s.output, s.received, place(b.ports, s.through), place(b.pipelines, s.pipe),
            place(b.pipelines, s.guard)};
  }
  uint32_t queued_from() const {
    for (uint32_t i = 0; i < count_; i++)
      if (steps_[i].kind == ENQUEUE && steps_[i].queue->depth_) return i + 1;
    return count_;
  }
  static bool open(const Step &s) { return !s.guard || s.guard->drained(); }
  // What a step's through() port counts: n, or for through_received() at most what is out.
  static uint64_t moved_at_issue(const Step &s) {
    return s.kind == RECEIVE && s.received ? std::min(s.n, s.pipe->out(s.output)) : s.n;
  }
  // The rows the first receive could take now.
  uint64_t got() const {
    for (uint32_t i = 0; i < count_; i++)
      if (steps_[i].kind == RECEIVE) return steps_[i].pipe->out(steps_[i].output);
    return 0;
  }

  // can_accept: every step before the queue holds now, and the queue has a slot. `settled`: requires aside, and a
  // receive from a drained pipeline taken as holding, so that a reason when_stuck() gave is the one that applies.
  bool check(Cycle now, bool settled = false) const {
    for (uint32_t i = 0; i < count_; i++) {
      const Step &s = steps_[i];
      if (!open(s)) return false;
      if (s.through && s.kind != USE && s.through->available(now) < moved_at_issue(s)) return false;
      switch (s.kind) {
        case REQUIRE: if (!s.holds && !settled) return false; break;
        case USE: if (s.port->available(now) < s.n) return false; break;
        case ENQUEUE: if (s.queue->depth_) return s.queue->held() < s.queue->depth_; break;
        case SEND: if (s.n * s.weight > s.pipe->input_room()) return false; break;
        case RECEIVE: if (s.n > s.pipe->out(s.output) && !(settled && s.pipe->drained())) return false; break;
        default: break;
      }
    }
    return true;
  }
  // issue: the steps before the queue take effect, seen at once (issue ends the cycle); the rest becomes a command, or
  // is nothing and done now.
  Cycle issue(Cycle now, Id id, std::vector<Id> &ready) {
    for (uint32_t i = 0; i < count_; i++) {
      Step &s = steps_[i];
      if (s.through && s.kind != USE) s.through->admit(moved_at_issue(s), now);
      switch (s.kind) {
        case USE: s.port->admit(s.n, now); break;
        case SEND:
          move(values(), i, 0, s.n, SEND, s.pipe, s.weight, s.output);
          s.pipe->end_cycle();
          break;
        case RECEIVE:
          move(values(), i, 0, s.n, RECEIVE, s.pipe, s.weight, s.output);
          received(values(), i);
          s.pipe->end_cycle();
          break;
        case TRANSFER: move(values(), i, 0, s.n, TRANSFER, nullptr, 0, 0); break;
        case ENQUEUE: {
          CommandQueue &q = *s.queue;
          if (!q.depth_) break;
          uint64_t work = q.free_ == Free::WHEN_STARTED;
          for (uint32_t j = i + 1; j < count_; j++) work += steps_[j].n;
          if (!work) {
            for (uint32_t j = i + 1; j < count_; j++)
              if (steps_[j].kind == RECEIVE) received(values(), j);
            ready.push_back(id);
            return cp::Unknown;
          }
          CommandQueue::Command c{id, now + q.arrival_};
          c.when_enqueued = when_enqueued_, c.when_idle = when_idle_;
          if (keeps_values()) c.values = this;
          for (uint32_t j = i + 1; j < count_; j++) c.moves[c.count++] = record(steps_[j], q.blocks_);
          q.list_.push_back(c);
          return cp::Unknown;
        }
        default: break;
      }
    }
    return latency_;
  }

  // The values ride the moves: Spike issues the route with its host, and the functions it was given are kept.
  bool keeps_values() const { return hooks_ != nullptr; }
  Route *values() { return keeps_values() ? this : nullptr; }
  template <class F>
  Route &on_moves(F &&each) {
    if (keeps_values() && count_) hooks_->moves.push_back({count_ - 1, std::forward<F>(each)});
    return *this;
  }
  // n entries or beats of step `at` move, from its from-th on: the same entries on gem5 and Spike, and on Spike, of route
  // `values`, the function it was given puts or takes their values.
  static void move(Route *values, uint32_t at, uint64_t from, uint64_t n, uint32_t kind, AnyPipeline *pipe,
                   uint32_t weight, uint32_t output) {
    if (!n) return;
    const Move *hook = nullptr;
    if (values)
      for (const Move &h : values->hooks_->moves)
        if (h.at == at) hook = &h;
    uint64_t row = 0;
    if (kind == SEND) {
      pipe->arrive(n, weight);
      if (hook) row = pipe->input_.rows() - n * weight;
    } else if (kind == RECEIVE) {
      if (hook) row = pipe->out_[output].rows(AnyPipeline::TAKEN);
      pipe->take_rows(n, output);
    }
    if (hook)
      for (uint64_t i = 0; i < n; i++) hook->each(*values->host_, from + i, row + i * (kind == SEND ? weight : 1), output);
  }
  static void received(Route *values, uint32_t at) {
    if (values)
      for (Received &r : values->hooks_->received)
        if (r.at == at) r.done(*values->host_);
  }
  void done() {
    if (keeps_values())
      for (auto &d : hooks_->done) d(*host_);
  }

  Mode mode_;
  const Insn &insn_;
  uint32_t arg_;
  const Host *host_;
  Step steps_[MAX_STEPS];
  uint32_t count_ = 0;
  AnyPipeline *guard_ = nullptr;
  Cycle latency_ = 1;
  Hooks *hooks_ = nullptr;
  std::function<void(uint64_t)> stuck_;
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
// and Spike's execute, which clocks the machine. Squash and the commit counts are cp::Instance's.
template <class... U>
class Machine : public cp::Model {
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
  // Spike: the route is issued as gem5 issues it, the machine ticked as gem5 ticks it while it is not taken and until it
  // is done, the route's functions giving the values; it stands still between instructions.
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
    std::apply([&](U &...u) { (tick_unit(u, now), ...); }, units_);
  }
  void reset() override {
    std::apply([&](U &...u) {
      (u.hardware(blocks([](Port &p) { p.reset(); },
                         [](auto &p, Port &) { p.flush(); },
                         [](CommandQueue &q) { q.reset(); }, [](auto &r) { r.reset(); })),
       ...);
    }, units_);
    ready_.clear();
    beats_.clear();
    clock_ = spike_id_ = 0;
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
    const_cast<V &>(unit).hardware(blocks(Ignore{}, [&](const AnyPipeline &p, const Port &) { what(p); }));
  }

 private:
  // A head's transfer of values in this cycle, given its values at the cycle's end.
  struct Beats {
    Route *route;
    uint32_t at;
    uint64_t from, n;
  };
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
    // A pipeline whose rows the unit's `stage(value, k)` works on in each stage k they reach, when they carry values.
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
  // A cycle of one unit: commands done when enqueued that arrive are done; each queue's head works; the pipelines with
  // anything in them move, their stages reading the registers as the cycle began; then the head's beats land, what
  // everything moved becomes visible, and a command waiting for an idle queue may be done.
  template <class V>
  void tick_unit(V &u, Cycle now) {
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) { q.arrivals(now, ready_); }));
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) { head(u, q, now); }));
    u.hardware(blocks(Ignore{}, [&](auto &p, Port &entry, auto... stage) {
      if (p.drained()) return;
      const bool moved = p.advance(now, entry);
      entry.hold(p.line_.rows());
      if constexpr (sizeof...(stage) == 1)
        if (moved) p.each_stage([&](auto &value, uint32_t k) { ((u.*stage)(value, k), ...); });
    }));
    for (const Beats &b : beats_) Route::move(b.route, b.at, b.from, b.n, Route::TRANSFER, nullptr, 0, 0);
    beats_.clear();
    u.hardware(blocks(Ignore{}, [](auto &p, Port &, auto...) { p.end_cycle(); },
                      [&](CommandQueue &q) { q.release_waiters(now, ready_); }));
  }
  template <class V>
  void head(V &u, CommandQueue &q, Cycle now) {
    if (q.list_.empty()) return;
    CommandQueue::Command &c = q.list_.front();
    if (c.arrives >= now) return;
    list_blocks(u, q);
    if (!advance(c, q, now, beats_)) return;
    if (c.when_idle) q.waiters_.push_back(c.id);
    else if (!c.when_enqueued) ready_.push_back(c.id);
    q.list_.pop_front();
  }

  template <class V>
  static void list_blocks(V &u, CommandQueue &q) {
    if (q.blocks_.listed) return;
    q.blocks_.listed = true;
    u.hardware(blocks([&](Port &p) { q.blocks_.ports.push_back(&p); },
                      [&](AnyPipeline &p, Port &) { q.blocks_.pipelines.push_back(&p); }));
  }
  // One cycle of the head command: its recorded step moves what the registered state allows. True when it is done.
  static bool advance(CommandQueue::Command &c, CommandQueue &q, Cycle now, std::vector<Beats> &beats) {
    if (q.free_ == Free::WHEN_STARTED && !c.started) {
      c.started = true, c.holds_slot = false;
      return c.count == 0;
    }
    while (c.step < c.count) {
      const CommandQueue::Move &s = c.moves[c.step];
      Port *through = s.through < 0 ? nullptr : q.blocks_.ports[s.through];
      AnyPipeline *pipe = s.pipe < 0 ? nullptr : q.blocks_.pipelines[s.pipe];
      const bool open = s.guard < 0 || q.blocks_.pipelines[s.guard]->drained();
      const uint64_t left = s.n - c.done, width = through ? through->available(now) : UNLIMITED;
      uint64_t moved = 0;
      if (s.kind == Route::SEND) {
        if (open) moved = std::min({left, pipe->input_room() / s.weight, width});
      } else if (s.kind == Route::RECEIVE) {
        if (open) moved = std::min({left, pipe->out(s.output), width});
      } else if (open) {
        moved = std::min(left, width);
      }
      const uint32_t at = c.values ? c.values->queued_from() + c.step : 0;
      if (s.kind == Route::TRANSFER && c.values && moved) beats.push_back({c.values, at, c.done, moved});
      else Route::move(c.values, at, c.done, moved, s.kind, pipe, s.weight, s.output);
      if (through && moved) through->admit(moved, now);
      c.done += moved;
      if (c.done == s.n && s.kind == Route::RECEIVE) Route::received(c.values, at);
      if (c.done == s.n) c.step++, c.done = 0;
      else if (!moved) break;
      if (moved) break;
    }
    return c.step == c.count;
  }

  // Spike's execute on unit u, when it owns the instruction.
  template <class V>
  bool execute_on(V &u, const Insn &insn, const Host &host) {
    for (const auto &i : V::INSTRUCTIONS)
      if ((insn.bits & i.mask) == i.match) return clocked(u, i, insn, host), true;
    return false;
  }
  template <class V, class I>
  void clocked(V &u, const I &instruction, const Insn &insn, const Host &host) {
    std::function<void(uint64_t)> why;
    uint64_t got = 0;
    const auto accepts = [&] {
      Route r(Route::CHECK, insn, instruction.arg, &host);
      (u.*instruction.route)(r);
      why = r.check(clock_, true) ? r.stuck_ : nullptr, got = r.got();
      return r.check(clock_);
    };
    while (!accepts()) spike_cycle(u, instruction.name, accepts, [&] { if (why) why(got); });
    Route::Hooks hooks;
    Route r(Route::ISSUE, insn, instruction.arg, &host);
    r.hooks_ = &hooks;
    (u.*instruction.route)(r);
    if (CommandQueue *q = r.queue()) list_blocks(u, *q);
    const Id id = ++spike_id_;
    const Cycle issued = clock_, latency = r.issue(clock_, id, ready_);
    if (latency != cp::Unknown)
      while (clock_ < issued + latency) tick(++clock_);
    const auto done = [&] {
      return (latency != cp::Unknown || Machine::ready(id, clock_)) && !queued(u, id);
    };
    while (!done()) spike_cycle(u, instruction.name, done, [&] {
      uint64_t got = 0;
      if (r.stuck_ && drained_receive(u, id, got)) r.stuck_(got);
    });
    r.done();
    Machine::commit(insn, id, clock_);
  }
  // One cycle Spike waits for `until`; a cycle in which nothing moves, no command still on its way and `until` not
  // yet true, is the same every cycle after: the run ends there, with the reason `stuck` gives if it gives one.
  template <class V, class Until, class Stuck>
  void spike_cycle(V &u, const char *name, Until &&until, Stuck &&stuck) {
    motion(u, before_);
    tick(++clock_);
    if (until()) return;
    motion(u, after_);
    if (after_ != before_ || arriving(u)) return;
    stuck();
    std::fprintf(stderr, "vcix_uarch: %s waits, and nothing in its unit moves any more: on Spike, which runs each "
                 "instruction to its end before the next, it never ends\n", name);
    std::exit(1);
  }
  // What a cycle of unit u can change in its pipelines and queues, into m.
  template <class V>
  void motion(V &u, std::vector<uint64_t> &m) const {
    m.assign(1, ready_.size());
    const auto add = [&m](auto... v) { (m.push_back(static_cast<uint64_t>(v)), ...); };
    u.hardware(blocks(Ignore{},
                      [&](auto &p, Port &, auto...) {
                        add(p.input_.rows(), p.line_.rows(), p.out_[0].rows(), p.out_[1].rows(),
                            p.line_.empty() ? 0 : p.moves_);
                      },
                      [&](CommandQueue &q) {
                        add(q.list_.size(), q.waiters_.size());
                        if (!q.list_.empty()) add(q.list_.front().step, q.list_.front().done, q.list_.front().started);
                      }));
  }
  template <class V>
  bool arriving(V &u) const {
    bool any = false;
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) {
      for (const CommandQueue::Command &c : q.list_) any |= c.arrives >= clock_;
    }));
    return any;
  }
  // Whether command `id` is at its queue's head in a receive from a drained pipeline; got, the rows it has taken.
  template <class V>
  static bool drained_receive(V &u, Id id, uint64_t &got) {
    bool at = false;
    u.hardware(blocks(Ignore{}, Ignore{}, [&](CommandQueue &q) {
      if (q.list_.empty() || q.list_.front().id != id) return;
      const CommandQueue::Command &c = q.list_.front();
      if (c.step == c.count || c.moves[c.step].kind != Route::RECEIVE) return;
      const CommandQueue::Move &m = c.moves[c.step];
      const AnyPipeline &p = *q.blocks_.pipelines[m.pipe];
      at = p.drained(), got = c.done + p.out(m.output);
    }));
    return at;
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
  std::vector<Beats> beats_;
  std::vector<uint64_t> before_, after_;
  Cycle clock_ = 0;
  Id spike_id_ = 0;
};

}  // namespace vcix_uarch

#endif
