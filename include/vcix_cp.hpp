// vcix_cp: a model is a cp::Coprocessor whose members are cp::Components. Each component writes cycle(), its timing
// and (through values()) Spike's values, over optional blocks the framework advances: Port, Queue<T>, Pipeline<T>.
// The model keeps the cycle, now(); blocks and components are found by byte offset, so a copy (gem5's squash) reaches
// its own.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "vcix_cp_base.hpp"

namespace cp {

constexpr uint64_t UNLIMITED = UINT64_MAX;

// A name kept for the life of the program (never destroyed): gem5 and Stats hold it as a const char *.
inline const char *intern(const std::string &name) {
  static auto *names = new std::set<std::string>;
  return names->insert(name).first->c_str();
}

// An issued instruction: the simulator's fields and the id ready() and commit name it by (0 in valid()).
struct Insn : vcix_insn {
  Id id = 0;
};

class Coprocessor;
class Component;

// A component's part, a member of it, kept as an offset to its component; label_ "<component>.<name>" names it in
// messages. Its port counts under the component's unit; only a PRIMARY Port is a statistic. The framework calls
// cycle() after every component's cycle(), clear() at reset, and stat() for the statistics.
class Block {
 public:
  virtual ~Block() = default;

 protected:
  Block(Component &c, const char *name);
  Component &owner() const;
  Cycle now() const;
  bool functional_model() const;
  void moved() const;
  [[noreturn]] void broken(const char *what) const {
    std::fprintf(stderr, "vcix_cp: %s %s in cycle %llu\n", label_, what, static_cast<unsigned long long>(now()));
    std::abort();
  }
  const char *label_, *name_;

 private:
  friend class Coprocessor;
  virtual void cycle() {}
  virtual void clear() = 0;
  virtual const Stats *stat() const = 0;
  ptrdiff_t to_owner_;
};

// Per-cycle bandwidth: utilize(n) takes available(), which is width() again in every new cycle. The component's one
// PRIMARY port is the component's unit in gem5's stats; a REPORTING port only times.
class Port : public Block {
 public:
  using Role = Stats::Role;
  static constexpr Role REPORTING = Stats::REPORTING, PRIMARY = Stats::PRIMARY;
  Port(Component &c, const char *name, const char *unit_of_work, uint64_t width, Role role = REPORTING);
  // In configure only.
  void set_width(uint64_t width) { stats_.set_capacity(width); }
  uint64_t width() const { return stats_.capacity(); }
  uint64_t available() const { return stats_.available(now()); }
  // More than available() stops the run (Stats::admit).
  void utilize(uint64_t n) {
    if (!n) return;
    stats_.admit(n, now());
    moved();
  }
  // Adds n to its OCCUPANCY statistic.
  void occupy(uint64_t n) { stats_.hold(n); }

 private:
  void clear() override { stats_.reset(); }
  const Stats *stat() const override { return &stats_; }
  Stats stats_;
};

// A FIFO of at most capacity() rows, at most per_cycle pushes a cycle. A row pushed in cycle t is visible (length,
// front, pop) from t + 1; a slot a pop frees in t is free from t + 1. A push when full(), or a read past the visible
// rows, stops the run. Depth 2W with W a cycle sustains W pushes a cycle under one pop of each visible row a cycle.
template <class T>
class Queue : public Block {
 public:
  Queue(Component &c, const char *name, const char *unit_of_work = "rows")
      : Block(c, name), stats_(c.name(), name_, unit_of_work, 1) {}
  // In configure: the capacity, at least 1, and the pushes a cycle (UNLIMITED: no bound); empties the queue.
  void configure(uint64_t capacity, uint64_t per_cycle = UNLIMITED) {
    if (!capacity || !per_cycle) throw std::invalid_argument(std::string("vcix_cp: queue ") + label_ + ": capacity or per-cycle limit 0");
    stats_.set_capacity(std::min(capacity, per_cycle));
    capacity_ = capacity, per_cycle_ = per_cycle;
    clear();
  }
  uint64_t capacity() const { return capacity_; }
  // Every row held, visible or not.
  uint64_t size() const { return rows_.size(); }
  uint64_t length() const { return rows_.size() - pushed(); }
  // The pushes still allowed in this cycle: free slots (a slot freed now still counts as taken) and per_cycle.
  uint64_t available() const {
    const uint64_t slots = capacity_ == UNLIMITED ? UNLIMITED : capacity_ - rows_.size() - freed();
    return std::min(slots, per_cycle_ == UNLIMITED ? UNLIMITED : per_cycle_ - pushed());
  }
  bool full() const { return !available(); }
  void push(T row) {
    if (full()) broken("pushed to while full");
    if (pushed_at_ != now()) pushed_at_ = now(), pushed_ = 0;
    rows_.push_back(std::move(row));
    pushed_++;
    stats_.admit(1, now());
    moved();
  }
  const T &front() const { return visible(), rows_.front(); }
  T &front() { return visible(), rows_.front(); }
  T pop() {
    visible();
    T row = std::move(rows_.front());
    rows_.pop_front();
    if (freed_at_ != now()) freed_at_ = now(), freed_ = 0;
    freed_++;
    moved();
    return row;
  }

 private:
  uint64_t pushed() const { return pushed_at_ == now() ? pushed_ : 0; }
  uint64_t freed() const { return freed_at_ == now() ? freed_ : 0; }
  void visible() const {
    if (!length()) broken("read with no visible row");
  }
  // OCCUPANCY: the rows held as this cycle began.
  void cycle() override { stats_.hold(length() + freed()); }
  void clear() override {
    rows_.clear();
    pushed_ = freed_ = 0;
    pushed_at_ = freed_at_ = Unknown;
    stats_.reset();
  }
  const Stats *stat() const override { return &stats_; }
  std::deque<T> rows_;
  uint64_t capacity_ = 1, per_cycle_ = UNLIMITED;
  Cycle pushed_at_ = Unknown, freed_at_ = Unknown;
  uint64_t pushed_ = 0, freed_ = 0;
  Stats stats_;
};

// A part of the model: encodings, blocks, plain copyable members (all state copied by value: no pointer to state, no
// reading another component), and cycle(), which moves only when it has work. An instruction it owns is accepted
// when valid() and insns() has room, and pushed there; complete() ends it, fault() ends it as a wrong program. Time
// spent waiting lives in a Pipeline.
class Component {
 public:
  Component(Coprocessor &cp, const char *name);
  virtual ~Component() = default;
  const char *name() const { return name_; }
  // The cycle the simulator is in; what every block reads.
  Cycle now() const;

  // Reads its keys; sizes insns() (default: depth 2, one a cycle) and its blocks. Before any other call.
  virtual void configure(const Config &) {}
  // Back to what configure left, for the plain members; the framework empties the blocks.
  virtual void reset() {}
  // Flow control only, as an RTL ready: false while the component cannot take the instruction this cycle. A wrong
  // instruction is accepted and ends in fault().
  virtual bool valid(const Insn &) const { return true; }
  virtual void cycle() = 0;

 protected:
  // In the constructor: instructions with (bits & mask) == match go to this component.
  void register_insn(const Encoding &encoding) { layout().encodings.push_back(encoding); }
  Queue<Insn> &insns() { return insns_; }
  const Queue<Insn> &insns() const { return insns_; }
  // ready(insn.id) is true from this cycle on (gem5); Spike's execute of it returns after this cycle.
  void complete(const Insn &insn);
  // The instruction can never end (a wrong field, an order the program got wrong): completes it as failed. gem5
  // prints why and exits with exit_code when it commits (a squashed one never does); Spike as its execute() returns.
  void fault(const Insn &insn, const std::string &why, int exit_code = 1);
  bool functional_model() const;
  // Spike only: the run stops when asked on gem5.
  const Host &host() const;
  // f(host()) on Spike, nothing on gem5: values only, never timing. Not const, so valid() cannot.
  template <class F>
  void values(F &&f) {
    if (functional_model()) f(host());
  }

 private:
  friend class Block;
  friend class Coprocessor;
  struct Layout {
    std::vector<Encoding> encodings;
    std::vector<ptrdiff_t> blocks;
  };
  Layout &layout() {
    if (layout_.use_count() > 1) layout_ = std::make_shared<Layout>(*layout_);
    return *layout_;
  }
  template <class F>
  void each_block(F &&f) const {
    for (ptrdiff_t at : layout_->blocks) f(*reinterpret_cast<Block *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + at));
  }
  const Encoding *encoding(const vcix_insn &insn) const {
    for (const Encoding &e : layout_->encodings)
      if ((insn.bits & e.mask) == e.match) return &e;
    return nullptr;
  }
  Coprocessor &cp() const {
    return *reinterpret_cast<Coprocessor *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + to_cp_);
  }

  const char *name_;
  ptrdiff_t to_cp_;
  std::shared_ptr<Layout> layout_ = std::make_shared<Layout>();
  uint64_t motion_ = 0;
  Queue<Insn> insns_{*this, "insns", "instructions"};
};

// A pipelined unit, as gem5's MinorFU: push(row) when available() (interval cycles since the last push, and the
// finished head popped); a row pushed in cycle t is at the head, length() == 1, from t + latency unless the pipeline
// stalled. After each cycle() it advances one stage, or holds still entirely while its finished head is not popped.
template <class T>
class Pipeline : public Block {
 public:
  using Stage = void (Component::*)(T &, uint32_t);
  Pipeline(Component &c, const char *name, const char *unit_of_work = "rows")
      : Block(c, name), stats_(c.name(), name_, unit_of_work, 1) {}
  // In D's constructor: on Spike, fn(row, k) for each row each cycle it advances, k = 0 the stage it was pushed to.
  template <class D>
  void on_stage(void (D::*fn)(T &, uint32_t)) {
    if (!dynamic_cast<D *>(&owner())) throw std::logic_error(std::string("vcix_cp: ") + label_ + ": on_stage of another class");
    stage_ = static_cast<Stage>(fn);
  }
  // In configure: latency and interval from 1 to UINT32_MAX; empties the stages.
  void configure(uint64_t latency, uint64_t interval) {
    if (!latency || latency > UINT32_MAX || !interval || interval > UINT32_MAX)
      throw std::invalid_argument(std::string("vcix_cp: pipeline ") + label_ + ": latency " + std::to_string(latency) +
                                  ", interval " + std::to_string(interval));
    latency_ = static_cast<uint32_t>(latency), interval_ = interval;
    clear();
  }
  uint64_t available() const { return !blocked() && (!pushed_ || now() - last_ >= interval_); }
  void push(T row) {
    if (!available()) broken("pushed to while it cannot take a row");
    rows_.push_back({std::move(row), 0, Unknown});
    pushed_ = true, last_ = now();
    stats_.admit(1, now());
    moved();
  }
  uint64_t length() const { return blocked() && rows_.front().done_at < now(); }
  const T &front() const { return finished(), rows_.front().row; }
  T pop() {
    finished();
    T row = std::move(rows_.front().row);
    rows_.pop_front();
    moved();
    return row;
  }
  bool idle() const { return rows_.empty(); }
  uint64_t size() const { return rows_.size(); }

 private:
  struct Slot {
    T row;
    uint32_t age;
    Cycle done_at;
  };
  bool blocked() const { return !rows_.empty() && rows_.front().age == latency_; }
  void finished() const {
    if (!length()) broken("has no finished row");
  }
  // A stage on every row, or none while the head waits; an interval still running is motion too.
  void cycle() override {
    if (!blocked() && !rows_.empty()) {
      for (Slot &s : rows_) {
        if (++s.age == latency_) s.done_at = now();
        if (stage_ && functional_model()) (owner().*stage_)(s.row, s.age - 1);
      }
      moved();
    }
    if (pushed_ && now() - last_ < interval_) moved();
    stats_.hold(rows_.size());
  }
  void clear() override {
    rows_.clear();
    pushed_ = false;
    stats_.reset();
  }
  const Stats *stat() const override { return &stats_; }
  std::deque<Slot> rows_;
  uint32_t latency_ = 1;
  uint64_t interval_ = 1;
  bool pushed_ = false;
  Cycle last_ = 0;
  Stage stage_ = nullptr;
  Stats stats_;
};

// The model: routes each instruction by encoding to its component's insns(), ticks every cycle() then every block,
// answers ready() from complete(), and runs Spike's execute on the same cycles. Squash: Instance's copies;
// a Spike trap: export_model's copy. check_layout refuses a block or component outside the model object.
class Coprocessor : public Model {
 public:
  std::vector<Encoding> owns() const override {
    std::vector<Encoding> all;
    each([&](const Component &c) { all.insert(all.end(), c.layout_->encodings.begin(), c.layout_->encodings.end()); });
    return all;
  }
  void configure(const Config &config) override {
    each([&](Component &c) { c.configure(config); });
    reset();
  }
  // The simulator's cycle is the model's clock: every call sets now(), and the blocks read it.
  bool can_accept(const vcix_insn &insn, Cycle now) const override {
    now_ = now;
    const Component *c = owner(insn);
    return c && c->valid(Insn{insn}) && !c->insns_.full();
  }
  Cycle issue(const vcix_insn &insn, Id id, Cycle now) override {
    now_ = now;
    unit(insn).insns_.push(Insn{insn, id});
    return Unknown;
  }
  void commit(const vcix_insn &, Id id, Cycle now) override {
    now_ = now;
    failed(id);
    const auto it = std::find(done_.begin(), done_.end(), id);
    if (it != done_.end()) done_.erase(it);
  }
  bool ready(Id id, Cycle now) const override {
    now_ = now;
    return std::find(done_.begin(), done_.end(), id) != done_.end();
  }
  // Every component's cycle(), then every block's cycle(): pipelines advance, queues count.
  void tick(Cycle now) override {
    now_ = now;
    each([&](Component &c) { c.cycle(); });
    each([&](Component &c) { c.each_block([&](Block &b) { b.cycle(); }); });
  }
  // Spike: tick until can_accept, push into insns(), tick until complete(); a tick that moves nothing in the
  // instruction's component ends the run.
  void execute(const Host &host, const vcix_insn &insn) override {
    struct Leave {
      const Host *&host;
      ~Leave() { host = nullptr; }
    } leave{host_ = &host};
    Component &c = unit(insn);
    executing_ = Insn{insn, ++spike_id_};
    until(c, [&] { return Coprocessor::can_accept(executing_, now_); });
    c.insns_.push(executing_);
    until(c, [&] { return Coprocessor::ready(executing_.id, now_); });
    Coprocessor::commit(insn, executing_.id, now_);
  }
  void reset() override {
    each([](Component &c) {
      c.each_block([](Block &b) { b.clear(); });
      c.reset();
    });
    done_.clear();
    faults_.clear();
    now_ = spike_id_ = 0;
  }
  std::vector<const Stats *> ports() const override {
    std::vector<const Stats *> all;
    each([&](const Component &c) { c.each_block([&](const Block &b) { all.push_back(b.stat()); }); });
    return all;
  }

 protected:
  Coprocessor() = default;
  // The component that owns the instruction, nullptr if none (for a trace).
  const Component *owner(const vcix_insn &insn) const {
    const Component *found = nullptr;
    each([&](const Component &c) {
      if (!found && c.encoding(insn)) found = &c;
    });
    return found;
  }
  // Every component and block lies in [this, this + bytes): a copy of the model then reaches its own.
  void check_layout(size_t bytes) const override {
    const auto outside = [&](ptrdiff_t at) { return at < 0 || static_cast<size_t>(at) >= bytes; };
    for (ptrdiff_t at : *components_) {
      const Component &c = *reinterpret_cast<const Component *>(reinterpret_cast<const char *>(this) + at);
      if (outside(at)) throw std::invalid_argument(std::string("vcix_cp: component ") + c.name_ + " is not a member of the model");
      for (ptrdiff_t b : c.layout_->blocks)
        if (outside(at + b))
          throw std::invalid_argument(std::string("vcix_cp: a block of ") + c.name_ + " lies outside the model object");
    }
  }

 private:
  friend class Block;
  friend class Component;
  template <class F>
  void each(F &&f) const {
    for (ptrdiff_t at : *components_)
      f(*reinterpret_cast<Component *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + at));
  }
  Component &unit(const vcix_insn &insn) const {
    if (const Component *c = owner(insn)) return const_cast<Component &>(*c);
    std::fprintf(stderr, "vcix_cp: instruction %08x has no unit\n", insn.bits);
    std::abort();
  }
  // Ticks the clock until done(); a tick after which done() is false and c moved nothing repeats forever.
  template <class Done>
  void until(const Component &c, Done &&done) {
    while (!done()) {
      const uint64_t before = c.motion_;
      tick(now_ + 1);
      if (done() || c.motion_ != before) continue;
      std::fprintf(stderr, "vcix_cp: %s waits, and nothing in its unit moves any more: on Spike, which runs each "
                   "instruction to its end before the next, it never ends\n", c.encoding(executing_)->name);
      std::exit(1);
    }
  }

  // A faulted instruction, as it commits: its why, then the run ends with its exit code.
  void failed(Id id) {
    for (const Fault &f : faults_)
      if (f.id == id) {
        std::fprintf(stderr, "%s\n", f.why.c_str());
        std::exit(f.code);
      }
  }

  struct Fault {
    Id id;
    std::string why;
    int code;
  };
  std::shared_ptr<std::vector<ptrdiff_t>> components_ = std::make_shared<std::vector<ptrdiff_t>>();
  std::vector<Id> done_;
  std::vector<Fault> faults_;
  const Host *host_ = nullptr;
  Insn executing_{};
  mutable Cycle now_ = 0;
  Id spike_id_ = 0;
};

inline Block::Block(Component &c, const char *name)
    : label_(intern(std::string(c.name()) + "." + name)), name_(intern(name)),
      to_owner_(reinterpret_cast<char *>(&c) - reinterpret_cast<char *>(this)) {
  c.layout().blocks.push_back(-to_owner_);
}
inline Component &Block::owner() const {
  return *reinterpret_cast<Component *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + to_owner_);
}
inline Cycle Block::now() const { return owner().now(); }
inline bool Block::functional_model() const { return owner().functional_model(); }
inline void Block::moved() const { owner().motion_++; }

inline Port::Port(Component &c, const char *name, const char *unit_of_work, uint64_t width, Role role)
    : Block(c, name), stats_(c.name(), Block::name_, unit_of_work, width, role) {}

inline Component::Component(Coprocessor &cp, const char *name)
    : name_(intern(name)), to_cp_(reinterpret_cast<char *>(&cp) - reinterpret_cast<char *>(this)) {
  cp.components_->push_back(-to_cp_);
  insns_.configure(2, 1);
}
inline Cycle Component::now() const { return cp().now_; }
inline void Component::complete(const Insn &insn) {
  cp().done_.push_back(insn.id);
  motion_++;
}
inline void Component::fault(const Insn &insn, const std::string &why, int exit_code) {
  cp().faults_.push_back({insn.id, why, exit_code});
  complete(insn);
}
inline bool Component::functional_model() const { return cp().host_ != nullptr; }
inline const Host &Component::host() const {
  if (!functional_model()) {
    std::fprintf(stderr, "vcix_cp: %s asked for Spike's host on gem5\n", name_);
    std::abort();
  }
  return *cp().host_;
}

}  // namespace cp
