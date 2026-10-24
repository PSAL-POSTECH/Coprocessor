// vcix_cp: a model derives from cp::Coprocessor and holds its components as members; a component holds its blocks
// (ports, pipelines, a command queue, registers) and, per encoding, a handler that declares the instruction's steps.
// gem5's can_accept and issue read those steps; Spike issues them the same way and ticks the same cycles.
#ifndef VCIX_CP_HPP
#define VCIX_CP_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "vcix_accel.hpp"

namespace cp {

using vcix_accel::Config;
using vcix_accel::Cycle;
using vcix_accel::Encoding;
using vcix_accel::Host;
using vcix_accel::Id;
using vcix_accel::Insn;
constexpr uint64_t UNLIMITED = UINT64_MAX;
constexpr uint32_t MAX_STEPS = 12;

// When a pipeline holds still: as soon as an output queue is full, or only when the entry leaving does not fit.
enum class Stall { WHEN_OUTPUT_FULL, WHEN_HEAD_BLOCKED };
// When a command gives back its queue slot: once its work is done, or as the head starts it (a cycle of its own).
enum class Free { WHEN_DONE, WHEN_STARTED };
// When an enqueued instruction is done: once its work is, as soon as the queue takes it, or once no command is left.
enum class Done { WhenFinished, WhenEnqueued, WhenIdle };

class Coprocessor;
class Instr;
class Port;
class AnyPipeline;
class CmdQueue;
class AnyRegister;

// A coprocessor's part, registered with it as it is made, as its blocks are with it: each by its byte offset, so a
// copy reaches its own. A component lies inside its coprocessor object, a block inside its component's (a member, or
// in-object storage such as std::optional); the offsets, copied as they are, are shared by copies.
class Component {
 public:
  Component(Coprocessor &cp, const char *name);
  virtual ~Component() = default;
  virtual void configure(const Config &) {}
  const char *name() const { return name_; }

 protected:
  // In the constructor: `fn` declares the steps of an instruction of `encoding`; `arg` tells a family's members apart.
  template <class D>
  void handle(const Encoding &encoding, void (D::*fn)(Instr &), uint32_t arg = 0) {
    layout().handlers.push_back({encoding, static_cast<void (Component::*)(Instr &)>(fn), arg});
  }

 private:
  friend class Coprocessor;
  friend class Instr;
  friend class Port;
  friend class AnyPipeline;
  friend class CmdQueue;
  friend class AnyRegister;
  struct Handler {
    Encoding encoding;
    void (Component::*fn)(Instr &);
    uint32_t arg;
  };
  struct Layout {
    std::vector<Handler> handlers;
    std::vector<ptrdiff_t> ports, pipelines, queues, registers;
  };
  Layout &layout() {
    if (layout_.use_count() > 1) layout_ = std::make_shared<Layout>(*layout_);
    return *layout_;
  }
  ptrdiff_t offset_of(const void *block) const {
    return static_cast<const char *>(block) - reinterpret_cast<const char *>(this);
  }
  template <class B>
  B &block(ptrdiff_t offset) const {
    return *reinterpret_cast<B *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + offset);
  }
  template <class B, class F>
  void each(const std::vector<ptrdiff_t> &blocks, F &&f) const {
    for (ptrdiff_t at : blocks) f(block<B>(at));
  }
  Port &port(int32_t i) const { return block<Port>(layout_->ports[i]); }
  AnyPipeline &pipeline(int32_t i) const { return block<AnyPipeline>(layout_->pipelines[i]); }
  const Handler *owner(const Insn &insn) const {
    for (const Handler &h : layout_->handlers)
      if ((insn.bits & h.encoding.mask) == h.encoding.match) return &h;
    return nullptr;
  }

  const char *name_;
  std::shared_ptr<Layout> layout_ = std::make_shared<Layout>();
};

// A vcix_accel::Port of its component, which reports it under the component's name.
class Port : public vcix_accel::Port {
 public:
  Port(Component &c, const char *name, const char *unit_of_work, uint64_t capacity_per_cycle, Role role = REPORTING)
      : vcix_accel::Port(c.name(), name, unit_of_work, capacity_per_cycle, role) {
    c.layout().ports.push_back(c.offset_of(this));
  }
};

// What reset() needs of a register, whatever it holds.
class AnyRegister {
 public:
  virtual void reset() = 0;

 protected:
  explicit AnyRegister(Component &c) { c.layout().registers.push_back(c.offset_of(this)); }
};

// Component state: get() reads it, write() changes it, reset puts back what configure gave. Spike's values keep theirs
// in program order; gem5's copies share it until one writes.
template <class S>
class Register : public AnyRegister {
 public:
  explicit Register(Component &c) : AnyRegister(c) {}
  void configure(S at_reset) { p_ = reset_ = std::make_shared<S>(std::move(at_reset)); }
  const S &get() const { return p_ ? *p_ : none(); }
  // What get() reads now, kept as it is when the register is written later.
  std::shared_ptr<const S> share() const { return p_; }
  S &write() {
    if (!p_) p_ = std::make_shared<S>();
    else if (p_.use_count() > 1) p_ = std::make_shared<S>(*p_);
    return *p_;
  }
  void reset() override { p_ = reset_; }

 private:
  static const S &none() {
    static const S s{};
    return s;
  }
  std::shared_ptr<S> p_, reset_;
};

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
    std::fprintf(stderr, "vcix_cp: %llu entries of %u rows: a pipeline holds at most 2^32 - 1 entries of 65535\n",
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
      std::fprintf(stderr, "vcix_cp: a pipeline of %llu stages, %llu deep, %u outputs\n",
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
  friend class Instr;
  friend class Coprocessor;
  // `entry` admits the entries that enter the stages; it is kept, as the pipeline is, as an offset.
  AnyPipeline(Component &c, Port &entry) : entry_(reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(this)) {
    c.layout().pipelines.push_back(c.offset_of(this));
  }
  virtual void stages(Component &) {}
  Port &entry() { return *reinterpret_cast<Port *>(reinterpret_cast<char *>(this) + entry_); }
  uint64_t input_room() const { return depth_ - input_.rows(); }
  static constexpr uint32_t SEEN = 0, ARRIVED = 1, TAKEN = 1;
  uint64_t out(uint32_t output) const { return out_[output].rows(SEEN); }
  void arrive(uint64_t entries, uint32_t weight) { input_.push(entries, weight, ARRIVED); }
  void take_rows(uint64_t rows, uint32_t output) { out_[output].tag_front(rows, TAKEN); }
  // One cycle of the stages; false when they hold still. An entry in the stages is tagged with the move it entered at
  // (mod 2^32); it leaves `stages` moves later.
  bool advance(Cycle now) {
    Port &entry = this->entry();
    const uint32_t leaving = !line_.empty() && moves_ - line_.front().tag == stages_ ? line_.front().weight : 0;
    for (uint32_t q = 0; q < outputs_; q++)
      if (stall_ == Stall::WHEN_OUTPUT_FULL ? out_[q].rows() == depth_ : leaving > depth_ - out_[q].rows())
        return false;
    if (leaving) {
      for (uint32_t q = 0; q < outputs_; q++) line_.give_rows(out_[q], q + 1 < outputs_);
      line_.pop(1);
    }
    const uint32_t weight = input_.empty() || input_.front().tag != SEEN ? 0 : input_.front().weight;
    if (weight && weight <= entry.room(now)) {
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
  ptrdiff_t entry_;
  uint64_t depth_ = 1;
  uint32_t stages_ = 1, outputs_ = 1, moves_ = 0;
  Stall stall_ = Stall::WHEN_OUTPUT_FULL;
  Slots input_, line_, out_[2];
};

// A pipeline whose rows carry values of type T on Spike; with `stage`, the component's stage(value, k) works on each
// row in each stage k it reaches, stage 0 the one it entered this cycle.
template <class T = char>
class Pipeline : public AnyPipeline {
 public:
  Pipeline(Component &c, Port &entry) : AnyPipeline(c, entry) {}
  template <class D>
  Pipeline(Component &c, Port &entry, void (D::*stage)(T &, uint32_t))
      : AnyPipeline(c, entry), stage_(static_cast<void (Component::*)(T &, uint32_t)>(stage)) {}

 private:
  friend class Instr;
  friend class Coprocessor;
  void arrived(uint64_t row, T value) { input_.value<T>(row) = std::move(value); }
  T taken(uint32_t output, uint64_t row) { return std::move(out_[output].value<T>(row)); }
  void stages(Component &c) override {
    if (stage_ && line_.valued())
      line_.each([&](uint64_t row, uint32_t rows, uint32_t tag) {
        for (uint32_t k = 0; k < rows; k++) (c.*stage_)(line_.value<T>(row + k), moves_ - 1 - tag);
      });
  }
  void (Component::*stage_)(T &, uint32_t) = nullptr;
};

// Issued instructions in program order; only the head works. Depth 0: no queue, every step at issue. A command
// arrives `arrival_cycles` after its issue and works from the cycle after; `free` says when its slot is given back.
class CmdQueue {
 public:
  explicit CmdQueue(Component &c) { c.layout().queues.push_back(c.offset_of(this)); }
  void configure(uint32_t depth, Free free = Free::WHEN_DONE, uint32_t arrival_cycles = 1) {
    depth_ = depth, free_ = free, arrival_ = arrival_cycles;
    reset();
  }
  uint32_t depth() const { return depth_; }
  bool idle() const { return list_.empty(); }

 private:
  friend class Instr;
  friend class Coprocessor;
  // A step after enqueue() as issue recorded it, its blocks named by their place in the component's (-1: none).
  struct Move {
    uint32_t kind;
    uint64_t n;
    uint32_t weight, output;
    bool received;
    int32_t through, pipe, guard;
  };
  // `values`: the instruction Spike issued it from, alive until the command is done, which gives its moves their values.
  struct Command {
    Id id;
    Cycle arrives;
    uint32_t step = 0, count = 0;
    uint64_t done = 0;
    bool when_enqueued = false, when_idle = false, holds_slot = true, started = false;
    Instr *values = nullptr;
    Move moves[MAX_STEPS];
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
};

// The step before: its n (sent, received or transferred) pass this port, at most its width a cycle; at issue the
// whole n must fit this cycle.
struct Through {
  explicit Through(Port &p) : port(p) {}
  Port &port;
};
// As Through, counting at most what is out: the same once the receive can issue, but a stuck receive of more than
// the port's width still gets its on_stuck() reason on Spike.
struct ThroughReceived {
  explicit ThroughReceived(Port &p) : port(p) {}
  Port &port;
};
// Which output queue a receive reads: 0 or 1.
struct Output {
  explicit Output(uint32_t k) : output(k) {}
  uint32_t output;
};

// The steps one instruction takes, which its handler declares one a statement. Steps before enqueue() happen in the
// issue cycle, all or nothing; steps after it at the queue head, in order, as much a cycle as the ports allow. The
// handler runs to check and to issue, maybe more than once: only what on_issue() is given changes state.
class Instr {
  template <class M>
  static constexpr bool modifier = std::is_same_v<M, Through> || std::is_same_v<M, ThroughReceived> || std::is_same_v<M, Output>;
  template <class... M>
  using Modifiers = std::enable_if_t<(modifier<M> && ...), int>;
  template <class T, class F>
  using Sent = std::enable_if_t<std::is_invocable_r_v<T, F &, const Host &, uint64_t>, int>;

 public:
  const Insn &insn() const { return insn_; }
  uint32_t vl() const { return insn_.vl; }
  uint32_t arg() const { return handler_.arg; }
  const char *name() const { return handler_.encoding.name; }
  uint32_t rd() const { return vcix_accel::rd(insn_); }
  uint32_t rs1() const { return vcix_accel::rs1(insn_); }
  uint32_t rs2() const { return vcix_accel::rs2(insn_); }
  uint32_t sew_bits() const { return insn_.sew_bits; }

  // The component never takes the instruction while `holds` is false; returns it.
  bool require(bool holds) {
    add(REQUIRE).holds = holds;
    if (!holds) on_stuck([](uint64_t) {});
    return holds;
  }
  // As require(holds), with never() as the reason on_stuck() gives.
  template <class F>
  bool require(bool holds, F never) {
    if (!holds) on_stuck([never](uint64_t) { never(); });
    return require(holds);
  }
  // On Spike, once the instruction waits and nothing in its component moves, never(got) may say why and end the run;
  // got: the rows its waiting receive has. The first reason, or failing require(), decides; capture by value.
  template <class F>
  void on_stuck(F never) {
    if (host_ && !stuck_) stuck_ = std::move(never);
  }
  // n of the port's room this cycle.
  void use(Port &port, uint64_t n = 1) {
    Step &s = add(USE);
    s.port = &port, s.n = n;
  }
  void enqueue(CmdQueue &q, Done done = Done::WhenFinished) {
    add(ENQUEUE).queue = &q;
    when_enqueued_ = done == Done::WhenEnqueued, when_idle_ = done == Done::WhenIdle;
  }
  // n entries of `weight` elements into the pipeline's input queue.
  template <class T, class... M, Modifiers<M...> = 0>
  void send(Pipeline<T> &p, uint64_t n, M... m) {
    send(p, n, 1, m...);
  }
  template <class T, class... M, Modifiers<M...> = 0>
  void send(Pipeline<T> &p, uint64_t n, uint32_t weight, M... m) {
    Step &s = add(SEND);
    s.pipe = &p, s.n = n, s.weight = weight;
    (modify(m), ...);
  }
  // As send, with values (Spike): value_of(host, j) -> T is row j's, the entries' rows in order, made as its entry
  // enters the input queue. The functions given to an instruction outlive its handler: capture the Instr and the
  // component by reference, the rest by value.
  template <class T, class F, class... M, Sent<T, F> = 0, Modifiers<M...> = 0>
  void send(Pipeline<T> &p, uint64_t n, uint32_t weight, F value_of, M... m) {
    send(p, n, weight, m...);
    on_moves([&p, weight, value_of](const Host &host, uint64_t i, uint64_t row, uint32_t) mutable {
      for (uint32_t k = 0; k < weight; k++) p.arrived(row + k, value_of(host, i * weight + k));
    });
  }
  template <class T, class F, class... M, Sent<T, F> = 0, Modifiers<M...> = 0>
  void send(Pipeline<T> &p, uint64_t n, F value_of, M... m) {
    send(p, n, 1, std::move(value_of), m...);
  }
  // Exactly n of an output queue.
  template <class T, class... M, Modifiers<M...> = 0>
  void receive(Pipeline<T> &p, uint64_t n, M... m) {
    Step &s = add(RECEIVE);
    s.pipe = &p, s.n = n;
    (modify(m), ...);
  }
  // As receive, with values (Spike): take(host, i, T &&) gets the i-th row as it is taken.
  template <class T, class F, class... M, std::enable_if_t<std::is_invocable_v<F &, const Host &, uint64_t, T &&>, int> = 0,
            Modifiers<M...> = 0>
  void receive(Pipeline<T> &p, uint64_t n, F take, M... m) {
    receive(p, n, m...);
    on_moves([&p, take](const Host &host, uint64_t i, uint64_t row, uint32_t output) mutable {
      take(host, i, p.taken(output, row));
    });
  }
  // n beats into the component's own registers or through a port, not into a pipeline.
  template <class... M, Modifiers<M...> = 0>
  void transfer(uint64_t n, M... m) {
    add(TRANSFER).n = n;
    (modify(m), ...);
  }
  // As transfer, with values (Spike): beat(host, i) as the i-th beat moves.
  template <class F, class... M, std::enable_if_t<std::is_invocable_v<F &, const Host &, uint64_t>, int> = 0,
            Modifiers<M...> = 0>
  void transfer(uint64_t n, F beat, M... m) {
    transfer(n, m...);
    on_moves([beat](const Host &host, uint64_t i, uint64_t, uint32_t) mutable { beat(host, i); });
  }
  // received(host) as the last receive ends (Spike).
  template <class F>
  void on_received(F received) {
    if (keeps_values() && count_) hooks_->received.push_back({count_ - 1, std::move(received)});
  }
  // The next step moves nothing until the pipeline's input queue and stages are empty.
  void wait_empty(AnyPipeline &p) { guard_ = &p; }
  void done_after(Cycle cycles) { latency_ = cycles; }
  // What the instruction changes as it is issued, before any of its moves: change() component state, on gem5 and
  // Spike; change(host) values, on Spike only.
  template <class F>
  void on_issue(F &&change) {
    if constexpr (std::is_invocable_v<F &, const Host &>) {
      if (keeps_values()) change(*host_);
    } else if (mode_ == ISSUE) {
      change();
    }
  }
  // done(host) as the instruction is done (Spike).
  template <class F>
  void on_done(F done) {
    if (keeps_values()) hooks_->done.push_back(std::move(done));
  }

 private:
  friend class Coprocessor;
  enum Mode { CHECK, ISSUE };
  enum Kind { REQUIRE, USE, ENQUEUE, SEND, RECEIVE, TRANSFER };
  // Trivial, so the steps an instruction does not declare cost nothing; made() is a declared one.
  struct Step {
    Kind kind;
    uint64_t n;
    uint32_t weight, output;
    bool holds, received;
    Port *port, *through;
    AnyPipeline *pipe, *guard;
    CmdQueue *queue;
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
  // The functions Spike's issue of the instruction keeps for its values until it is done.
  struct Hooks {
    std::vector<Move> moves;
    std::vector<Received> received;
    std::vector<std::function<void(const Host &)>> done;
  };
  Instr(Mode mode, const Insn &insn, const Component::Handler &handler, const Host *host, const Component &owner)
      : mode_(mode), insn_(insn), handler_(handler), host_(host), owner_(owner) {}
  Step &add(Kind k) {
    if (count_ == MAX_STEPS) {
      std::fprintf(stderr, "vcix_cp: an instruction of more than %u steps\n", MAX_STEPS);
      std::abort();
    }
    Step &s = steps_[count_++] = Step::made(k);
    s.guard = guard_;
    guard_ = nullptr;
    return s;
  }
  Step &last() { return steps_[count_ - 1]; }
  void modify(const Through &t) { last().through = &t.port; }
  void modify(const ThroughReceived &t) { last().through = &t.port, last().received = true; }
  void modify(const Output &o) { last().output = o.output; }
  int32_t place(const std::vector<ptrdiff_t> &held, const void *block) const {
    if (!block) return -1;
    const auto at = std::find(held.begin(), held.end(), owner_.offset_of(block));
    if (at != held.end()) return static_cast<int32_t>(at - held.begin());
    std::fprintf(stderr, "vcix_cp: a step after enqueue() names a block its component does not hold\n");
    std::abort();
  }
  CmdQueue::Move record(const Step &s) const {
    if (s.kind == USE || s.kind == REQUIRE || s.kind == ENQUEUE) {
      std::fprintf(stderr, "vcix_cp: use(), require() and enqueue() come before enqueue()\n");
      std::abort();
    }
    return {s.kind, s.n, s.weight, s.output, s.received, place(owner_.layout_->ports, s.through),
            place(owner_.layout_->pipelines, s.pipe), place(owner_.layout_->pipelines, s.guard)};
  }
  uint32_t queued_from() const {
    for (uint32_t i = 0; i < count_; i++)
      if (steps_[i].kind == ENQUEUE && steps_[i].queue->depth_) return i + 1;
    return count_;
  }
  static bool open(const Step &s) { return !s.guard || s.guard->drained(); }
  // What a step's Through port counts: n, or for ThroughReceived at most what is out.
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
  // receive from a drained pipeline taken as holding, so that a reason on_stuck() gave is the one that applies.
  bool check(Cycle now, bool settled = false) const {
    for (uint32_t i = 0; i < count_; i++) {
      const Step &s = steps_[i];
      if (!open(s)) return false;
      if (s.through && s.kind != USE && s.through->room(now) < moved_at_issue(s)) return false;
      switch (s.kind) {
        case REQUIRE: if (!s.holds && !settled) return false; break;
        case USE: if (s.port->room(now) < s.n) return false; break;
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
          CmdQueue &q = *s.queue;
          if (!q.depth_) break;
          uint64_t work = q.free_ == Free::WHEN_STARTED;
          for (uint32_t j = i + 1; j < count_; j++) work += steps_[j].n;
          if (!work) {
            for (uint32_t j = i + 1; j < count_; j++)
              if (steps_[j].kind == RECEIVE) received(values(), j);
            ready.push_back(id);
            return vcix_accel::Unknown;
          }
          CmdQueue::Command c{id, now + q.arrival_};
          c.when_enqueued = when_enqueued_, c.when_idle = when_idle_;
          if (keeps_values()) c.values = this;
          for (uint32_t j = i + 1; j < count_; j++) c.moves[c.count++] = record(steps_[j]);
          q.list_.push_back(c);
          return vcix_accel::Unknown;
        }
        default: break;
      }
    }
    return latency_;
  }

  // The values ride the moves: Spike issues the instruction with its host, and the functions it was given are kept.
  bool keeps_values() const { return hooks_ != nullptr; }
  Instr *values() { return keeps_values() ? this : nullptr; }
  template <class F>
  void on_moves(F &&each) {
    if (keeps_values() && count_) hooks_->moves.push_back({count_ - 1, std::forward<F>(each)});
  }
  // n entries or beats of step `at` move, from its from-th on: the same entries on gem5 and Spike, and on Spike, of
  // instruction `values`, the function it was given puts or takes their values.
  static void move(Instr *values, uint32_t at, uint64_t from, uint64_t n, uint32_t kind, AnyPipeline *pipe,
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
  static void received(Instr *values, uint32_t at) {
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
  const Component::Handler &handler_;
  const Host *host_;
  const Component &owner_;
  Step steps_[MAX_STEPS];
  uint32_t count_ = 0;
  AnyPipeline *guard_ = nullptr;
  Cycle latency_ = 1;
  Hooks *hooks_ = nullptr;
  std::function<void(uint64_t)> stuck_;
  bool when_idle_ = false, when_enqueued_ = false;
};

// A model of components in their order. Each reads its keys in configure(Config) and declares its encodings with
// handle(); Coprocessor derives tick, reset, the ports (the statistics), readiness, and Spike's execute, which clocks
// it. Squash and the commit counts are vcix_accel::Instance's, from copies of the whole model.
class Coprocessor : public vcix_accel::Model {
 public:
  std::vector<Encoding> owns() const override {
    std::vector<Encoding> all;
    for (size_t i = 0; i < components_->size(); i++)
      for (const Component::Handler &h : component(i).layout_->handlers) all.push_back(h.encoding);
    return all;
  }
  void configure(const Config &c) override {
    for (size_t i = 0; i < components_->size(); i++) component(i).configure(c);
    reset();
  }
  // Spike: the instruction is issued as gem5 issues it, the coprocessor ticked as gem5 ticks it while it is not taken
  // and until it is done, the instruction's functions giving the values; it stands still between instructions.
  void execute(const Host &host, const Insn &insn) override {
    for (size_t i = 0; i < components_->size(); i++)
      if (const Component::Handler *h = component(i).owner(insn)) return clocked(component(i), *h, insn, host);
    std::fprintf(stderr, "vcix_cp: instruction %08x has no unit\n", insn.bits);
    std::abort();
  }
  bool can_accept(const Insn &insn, Cycle now) const override {
    bool ok = false;
    run(Instr::CHECK, insn, [&](Component &, Instr &in) { ok = in.check(now); });
    return ok;
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    Cycle latency = 1;
    run(Instr::ISSUE, insn, [&](Component &, Instr &in) { latency = in.issue(now, id, ready_); });
    return latency;
  }
  void commit(const Insn &, Id id, Cycle) override {
    const auto it = std::find(ready_.begin(), ready_.end(), id);
    if (it != ready_.end()) ready_.erase(it);
  }
  bool ready(Id id, Cycle) const override { return std::find(ready_.begin(), ready_.end(), id) != ready_.end(); }
  void tick(Cycle now) override {
    for (size_t i = 0; i < components_->size(); i++) tick_component(component(i), now);
  }
  void reset() override {
    for (size_t i = 0; i < components_->size(); i++) {
      const Component &comp = component(i);
      comp.each<Port>(comp.layout_->ports, [](Port &p) { p.reset(); });
      comp.each<AnyPipeline>(comp.layout_->pipelines, [](AnyPipeline &p) { p.flush(); });
      comp.each<CmdQueue>(comp.layout_->queues, [](CmdQueue &q) { q.reset(); });
      comp.each<AnyRegister>(comp.layout_->registers, [](AnyRegister &r) { r.reset(); });
    }
    ready_.clear();
    beats_.clear();
    clock_ = spike_id_ = 0;
  }
  std::vector<const vcix_accel::Port *> ports() const override {
    std::vector<const vcix_accel::Port *> all;
    for (size_t i = 0; i < components_->size(); i++)
      component(i).each<Port>(component(i).layout_->ports, [&](Port &p) { all.push_back(&p); });
    return all;
  }

 protected:
  Coprocessor() = default;
  // `what` on the first component that owns the instruction; false when none does.
  template <class Do>
  bool with_owner(const Insn &insn, Do &&what) const {
    for (size_t i = 0; i < components_->size(); i++)
      if (component(i).owner(insn)) return what(static_cast<const Component &>(component(i))), true;
    return false;
  }
  // `what` on each pipeline the component holds.
  template <class Do>
  static void each_pipeline(const Component &c, Do &&what) {
    c.each<AnyPipeline>(c.layout_->pipelines, [&](const AnyPipeline &p) { what(p); });
  }

 private:
  friend class Component;
  // A head's transfer of values in this cycle, given its values at the cycle's end.
  struct Beats {
    Instr *instr;
    uint32_t at;
    uint64_t from, n;
  };
  Component &component(size_t i) const {
    return *reinterpret_cast<Component *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + (*components_)[i]);
  }
  // The steps of the first component's first handler that matches, declared in `mode`, then `after` on them.
  template <class After>
  bool run(Instr::Mode mode, const Insn &insn, After &&after) const {
    for (size_t i = 0; i < components_->size(); i++) {
      Component &comp = component(i);
      const Component::Handler *h = comp.owner(insn);
      if (!h) continue;
      Instr in(mode, insn, *h, nullptr, comp);
      (comp.*h->fn)(in);
      after(comp, in);
      return true;
    }
    return false;
  }
  // A cycle of one component: commands done when enqueued that arrive are done; each queue's head works; the pipelines
  // with anything in them move, their stages reading the registers as the cycle began; then the head's beats land,
  // what everything moved becomes visible, and a command waiting for an idle queue may be done.
  void tick_component(Component &comp, Cycle now) {
    const Component::Layout &l = *comp.layout_;
    comp.each<CmdQueue>(l.queues, [&](CmdQueue &q) { q.arrivals(now, ready_); });
    comp.each<CmdQueue>(l.queues, [&](CmdQueue &q) { head(comp, q, now); });
    comp.each<AnyPipeline>(l.pipelines, [&](AnyPipeline &p) {
      if (p.drained()) return;
      const bool moved = p.advance(now);
      p.entry().hold(p.line_.rows());
      if (moved) p.stages(comp);
    });
    for (const Beats &b : beats_) Instr::move(b.instr, b.at, b.from, b.n, Instr::TRANSFER, nullptr, 0, 0);
    beats_.clear();
    comp.each<AnyPipeline>(l.pipelines, [](AnyPipeline &p) { p.end_cycle(); });
    comp.each<CmdQueue>(l.queues, [&](CmdQueue &q) { q.release_waiters(now, ready_); });
  }
  void head(const Component &comp, CmdQueue &q, Cycle now) {
    if (q.list_.empty()) return;
    CmdQueue::Command &c = q.list_.front();
    if (c.arrives >= now) return;
    if (!advance(comp, c, q, now, beats_)) return;
    if (c.when_idle) q.waiters_.push_back(c.id);
    else if (!c.when_enqueued) ready_.push_back(c.id);
    q.list_.pop_front();
  }
  // One cycle of the head command: its recorded step moves what the registered state allows. True when it is done.
  static bool advance(const Component &comp, CmdQueue::Command &c, CmdQueue &q, Cycle now, std::vector<Beats> &beats) {
    if (q.free_ == Free::WHEN_STARTED && !c.started) {
      c.started = true, c.holds_slot = false;
      return c.count == 0;
    }
    while (c.step < c.count) {
      const CmdQueue::Move &s = c.moves[c.step];
      Port *through = s.through < 0 ? nullptr : &comp.port(s.through);
      AnyPipeline *pipe = s.pipe < 0 ? nullptr : &comp.pipeline(s.pipe);
      const bool open = s.guard < 0 || comp.pipeline(s.guard).drained();
      const uint64_t left = s.n - c.done, width = through ? through->room(now) : UNLIMITED;
      uint64_t moved = 0;
      if (s.kind == Instr::SEND) {
        if (open) moved = std::min({left, pipe->input_room() / s.weight, width});
      } else if (s.kind == Instr::RECEIVE) {
        if (open) moved = std::min({left, pipe->out(s.output), width});
      } else if (open) {
        moved = std::min(left, width);
      }
      const uint32_t at = c.values ? c.values->queued_from() + c.step : 0;
      if (s.kind == Instr::TRANSFER && c.values && moved) beats.push_back({c.values, at, c.done, moved});
      else Instr::move(c.values, at, c.done, moved, s.kind, pipe, s.weight, s.output);
      if (through && moved) through->admit(moved, now);
      c.done += moved;
      if (c.done == s.n && s.kind == Instr::RECEIVE) Instr::received(c.values, at);
      if (c.done == s.n) c.step++, c.done = 0;
      else if (!moved) break;
      if (moved) break;
    }
    return c.step == c.count;
  }

  // Spike's execute of handler h of component comp.
  void clocked(Component &comp, const Component::Handler &h, const Insn &insn, const Host &host) {
    std::function<void(uint64_t)> why;
    uint64_t got = 0;
    const auto accepts = [&] {
      Instr in(Instr::CHECK, insn, h, &host, comp);
      (comp.*h.fn)(in);
      why = in.check(clock_, true) ? in.stuck_ : nullptr, got = in.got();
      return in.check(clock_);
    };
    while (!accepts()) spike_cycle(comp, h.encoding.name, accepts, [&] { if (why) why(got); });
    Instr::Hooks hooks;
    Instr in(Instr::ISSUE, insn, h, &host, comp);
    in.hooks_ = &hooks;
    (comp.*h.fn)(in);
    const Id id = ++spike_id_;
    const Cycle issued = clock_, latency = in.issue(clock_, id, ready_);
    if (latency != vcix_accel::Unknown)
      while (clock_ < issued + latency) tick(++clock_);
    const auto done = [&] {
      return (latency != vcix_accel::Unknown || Coprocessor::ready(id, clock_)) && !queued(comp, id);
    };
    while (!done()) spike_cycle(comp, h.encoding.name, done, [&] {
      uint64_t got = 0;
      if (in.stuck_ && drained_receive(comp, id, got)) in.stuck_(got);
    });
    in.done();
    Coprocessor::commit(insn, id, clock_);
  }
  // One cycle Spike waits for `until`; a cycle in which nothing moves, no command still on its way and `until` not
  // yet true, is the same every cycle after: the run ends there, with the reason `stuck` gives if it gives one.
  template <class Until, class Stuck>
  void spike_cycle(const Component &comp, const char *name, Until &&until, Stuck &&stuck) {
    motion(comp, before_);
    tick(++clock_);
    if (until()) return;
    motion(comp, after_);
    if (after_ != before_ || arriving(comp)) return;
    stuck();
    std::fprintf(stderr, "vcix_cp: %s waits, and nothing in its unit moves any more: on Spike, which runs each "
                 "instruction to its end before the next, it never ends\n", name);
    std::exit(1);
  }
  // What a cycle of component comp can change in its pipelines and queues, into m.
  void motion(const Component &comp, std::vector<uint64_t> &m) const {
    m.assign(1, ready_.size());
    const auto add = [&m](auto... v) { (m.push_back(static_cast<uint64_t>(v)), ...); };
    comp.each<AnyPipeline>(comp.layout_->pipelines, [&](AnyPipeline &p) {
      add(p.input_.rows(), p.line_.rows(), p.out_[0].rows(), p.out_[1].rows(), p.line_.empty() ? 0 : p.moves_);
    });
    comp.each<CmdQueue>(comp.layout_->queues, [&](CmdQueue &q) {
      add(q.list_.size(), q.waiters_.size());
      if (!q.list_.empty()) add(q.list_.front().step, q.list_.front().done, q.list_.front().started);
    });
  }
  bool arriving(const Component &comp) const {
    bool any = false;
    comp.each<CmdQueue>(comp.layout_->queues, [&](CmdQueue &q) {
      for (const CmdQueue::Command &c : q.list_) any |= c.arrives >= clock_;
    });
    return any;
  }
  // Whether command `id` is at its queue's head in a receive from a drained pipeline; got, the rows it has taken.
  static bool drained_receive(const Component &comp, Id id, uint64_t &got) {
    bool at = false;
    comp.each<CmdQueue>(comp.layout_->queues, [&](CmdQueue &q) {
      if (q.list_.empty() || q.list_.front().id != id) return;
      const CmdQueue::Command &c = q.list_.front();
      if (c.step == c.count || c.moves[c.step].kind != Instr::RECEIVE) return;
      const CmdQueue::Move &m = c.moves[c.step];
      const AnyPipeline &p = comp.pipeline(m.pipe);
      at = p.drained(), got = c.done + p.out(m.output);
    });
    return at;
  }
  static bool queued(const Component &comp, Id id) {
    bool any = false;
    comp.each<CmdQueue>(comp.layout_->queues, [&](CmdQueue &q) {
      for (const CmdQueue::Command &c : q.list_) any |= c.id == id;
      any |= std::find(q.waiters_.begin(), q.waiters_.end(), id) != q.waiters_.end();
    });
    return any;
  }

  std::shared_ptr<std::vector<ptrdiff_t>> components_ = std::make_shared<std::vector<ptrdiff_t>>();
  std::vector<Id> ready_;
  std::vector<Beats> beats_;
  std::vector<uint64_t> before_, after_;
  Cycle clock_ = 0;
  Id spike_id_ = 0;
};

inline Component::Component(Coprocessor &cp, const char *name) : name_(name) {
  cp.components_->push_back(reinterpret_cast<char *>(this) - reinterpret_cast<char *>(&cp));
}

}

#endif
