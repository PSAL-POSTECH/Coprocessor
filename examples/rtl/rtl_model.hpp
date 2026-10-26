// A vcix model made of one Verilated unit with the standard ports and a table of the encodings it owns; nothing here
// knows the unit. Spike is a core infinitely faster than the unit: execute clocks it only while its own command, beats
// and results need it (RTL_SCHEDULE=slow or random:<seed>:<max gap> clock it more, as checks). See README.md.
#ifndef RTL_MODEL_HPP
#define RTL_MODEL_HPP

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "vcix_cp_base.hpp"
#include "verilated.h"
#include "verilated_save.h"

namespace rtl {

using cp::Cycle;
using cp::Id;
using Insn = vcix_insn;

// Verilator's save and restore, to and from memory.
class MemSave final : public VerilatedSerialize {
 public:
  explicit MemSave(std::vector<uint8_t> &out) : out_(out) {
    m_isOpen = true;
    header();
  }
  ~MemSave() override {
    trailer();
    flush();
  }
  void flush() override {
    out_.insert(out_.end(), m_bufp, m_cp);
    m_cp = m_bufp;
  }

 private:
  std::vector<uint8_t> &out_;
};

class MemRestore final : public VerilatedDeserialize {
 public:
  explicit MemRestore(const std::vector<uint8_t> &in) : in_(in) {
    m_isOpen = true;
    m_endp = m_bufp;
    header();
  }
  ~MemRestore() override { trailer(); }
  void fill() override {
    if (pos_ == in_.size()) return;
    uint8_t *rp = m_bufp;
    for (const uint8_t *sp = m_cp; sp < m_endp; *rp++ = *sp++) {}
    m_endp = m_bufp + (m_endp - m_cp);
    m_cp = m_bufp;
    const size_t n = std::min(in_.size() - pos_, static_cast<size_t>(m_bufp + bufferSize() - m_endp));
    std::memcpy(m_endp, in_.data() + pos_, n);
    pos_ += n;
    m_endp += n;
  }

 private:
  const std::vector<uint8_t> &in_;
  size_t pos_ = 0;
};

// What the process did with its units, on stderr at exit if anything: clocks (the cycles gem5 runs again after a squash
// included) and copies (gem5's before each issue, Spike's before each execute).
struct Counters {
  uint64_t clocks = 0, copies = 0;
  ~Counters() {
    if (clocks || copies) fprintf(stderr, "[rtl] %" PRIu64 " clocks, %" PRIu64 " copies\n", clocks, copies);
  }
};
inline Counters counters;

// One instance of V in a context of one thread, reset on creation. A copy is a save restored into a new instance; a
// move swaps the two.
template <class V>
class Unit {
 public:
  Unit() {
    ctx_.reset(new VerilatedContext);
    ctx_->threads(1);
    top_.reset(new V{ctx_.get()});
    top_->rst = 1;
    half(0);
    half(1);
    top_->rst = 0;
    half(0);
  }
  Unit(const Unit &other) : Unit() { *this = other; }
  Unit &operator=(const Unit &other) {
    if (this == &other) return *this;
    std::vector<uint8_t> state;
    {
      MemSave out(state);
      out << *other.top_;
    }
    {
      MemRestore in(state);
      in >> *top_;
    }
    half(0);
    counters.copies++;
    return *this;
  }
  Unit(Unit &&other) noexcept { swap(other); }
  Unit &operator=(Unit &&other) noexcept {
    swap(other);
    return *this;
  }
  void swap(Unit &other) noexcept {
    std::swap(ctx_, other.ctx_);
    std::swap(top_, other.top_);
  }
  V *operator->() const { return top_.get(); }
  // Inputs are set; settle() shows the outputs they give before the edge, edge() takes the clock edge.
  void settle() { half(0); }
  void edge() {
    half(1);
    counters.clocks++;
  }

 private:
  void half(int clk) {
    top_->clk = clk;
    top_->eval();
  }
  std::unique_ptr<VerilatedContext> ctx_;
  std::unique_ptr<V> top_;
};

// Lane-major bytes of a beat port of any width.
template <class P>
void put_byte(P &port, uint32_t i, uint8_t v) {
  if constexpr (sizeof(P) <= 8)
    port = (port & ~(static_cast<P>(0xFF) << (8 * i))) | (static_cast<P>(v) << (8 * i));
  else
    port[i / 4] = (port[i / 4] & ~(0xFFu << (8 * (i % 4)))) | (static_cast<uint32_t>(v) << (8 * (i % 4)));
}
template <class P>
uint8_t get_byte(const P &port, uint32_t i) {
  if constexpr (sizeof(P) <= 8)
    return static_cast<uint8_t>(port >> (8 * i));
  else
    return static_cast<uint8_t>(port[i / 4] >> (8 * (i % 4)));
}

// One owned encoding: the command it is, whether vl beats come in from vs2, and whether vl beats go out to vd (one or
// both). An op with vl 0 is not sent to the unit.
struct Op {
  uint32_t match, mask;
  const char *name;
  uint32_t cmd_op;
  bool beats_in, beats_out;
};

// T says NAME, LANES, OPS, TAGS (the command tags the unit tells apart) and check(config). The unit's ports: cmd_valid,
// cmd_ready, cmd_op, cmd_vl, cmd_tag; in_valid, in_ready, in_data; out_valid, out_ready, out_data, out_tag, out_last; busy.
template <class V, class T>
class Model : public cp::Model {
 public:
  const char *name() const override { return T::NAME; }
  std::vector<cp::Encoding> owns() const override {
    std::vector<cp::Encoding> e;
    for (const Op &op : T::OPS) e.push_back({op.match, op.mask, op.name});
    return e;
  }
  void configure(const cp::Config &config) override {
    T::check(config);
    const char *s = std::getenv("RTL_SCHEDULE");
    if (!s || !*s) return;
    if (!std::strcmp(s, "slow")) {
      schedule_ = SLOW;
    } else if (int end = 0; !std::strncmp(s, "random:", 7) &&
               std::sscanf(s + 7, "%" SCNu64 ":%u%n", &seed0_, &max_gap_, &end) == 2 && !s[7 + end] && seed0_ &&
               max_gap_ != UINT_MAX) {
      schedule_ = RANDOM;
      seed_ = seed0_;
    } else {
      throw std::runtime_error(std::string("RTL_SCHEDULE=") + s + " is not slow or random:<seed, not 0>:<max gap>");
    }
  }
  void reset() override {
    unit_ = Unit<V>();
    seed_ = seed0_;
    idle_ = 0;
    issued_any_ = false;
    tag_used_.assign(T::TAGS, false);
    pending_.clear();
    feeding_.clear();
    draining_.clear();
    ready_.clear();
  }

  // ---- Spike: the command, its beats and results, then the schedule's gap, then vd.
  void execute(const cp::Host &host, const Insn &insn) override {
    const Op &op = find(insn);
    const uint32_t lanes = T::LANES, per_reg = host.vlen_bits() / 8, vl = insn.vl;
    if (insn.sew_bits != 8 || cp::rs1(insn) != 0) fail("only SEW 8, format 0 is in this RTL");
    if (vl == 0) return;
    std::vector<std::vector<uint8_t>> in(op.beats_in ? vl : 0, std::vector<uint8_t>(lanes));
    for (uint32_t i = 0; i < in.size(); i++)
      for (uint32_t lane = 0; lane < lanes; lane++)
        in[i][lane] = host.vreg<uint8_t>(lane, cp::rs2(insn) + i / per_reg)[i % per_reg];
    std::vector<std::vector<uint8_t>> out;
    bool cmd_fired = false;
    uint32_t sent = 0;
    for (uint64_t n = 0; !(cmd_fired && sent == in.size() && out.size() == (op.beats_out ? vl : 0)); n++) {
      if (n > LIMIT) fail("no end");
      unit_->cmd_valid = !cmd_fired;
      unit_->cmd_op = op.cmd_op;
      unit_->cmd_vl = vl;
      unit_->cmd_tag = 0;
      unit_->in_valid = cmd_fired && sent < in.size();
      if (unit_->in_valid)
        for (uint32_t lane = 0; lane < lanes; lane++) put_byte(unit_->in_data, lane, in[sent][lane]);
      unit_->out_ready = op.beats_out;
      const Fired f = cycle();
      if (f.out) {
        if (f.tag != 0) wrong_tag(f.tag, Cmd{0, &op, vl, 0});
        out.push_back(f.out_bytes);
      }
      if (!f.any() && !f.busy) stuck(op, cmd_fired, sent, in.size(), out.size());
      cmd_fired |= f.cmd;
      sent += f.in;
    }
    unit_->cmd_valid = 0;
    unit_->in_valid = 0;
    unit_->out_ready = 0;
    uint64_t gap = 0;
    if (schedule_ == SLOW)
      gap = LIMIT;
    else if (schedule_ == RANDOM)
      gap = next_random() % (uint64_t{max_gap_} + 1);
    for (uint64_t g = 0; g < gap; g++)
      if (!cycle().busy && schedule_ == SLOW) break;
    for (uint32_t i = 0; i < out.size(); i++)
      for (uint32_t lane = 0; lane < lanes; lane++)
        host.vreg_mut<uint8_t>(lane, cp::rd(insn) + i / per_reg)[i % per_reg] = out[i][lane];
  }

  // ---- gem5: one command presented at a time. Ready: a command with beats out at its last beat, one with only beats
  // in when they have all fired; a command with vl 0 at issue.
  bool can_accept(const Insn &, Cycle now) const override {
    return !(issued_any_ && last_issue_ == now) && pending_.empty() && unit_->cmd_ready && free_tag() >= 0;
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    const Op &op = find(insn);
    issued_any_ = true;
    last_issue_ = now;
    Cmd c{id, &op, insn.vl, now};
    if (insn.vl == 0) {
      c.ready = now;
      ready_.emplace(id, c);
      return cp::Unknown;
    }
    c.tag = static_cast<uint32_t>(free_tag());
    tag_used_[c.tag] = true;
    pending_.push_back(c);
    return cp::Unknown;
  }
  void tick(Cycle now) override {
    unit_->cmd_valid = !pending_.empty();
    if (!pending_.empty()) {
      unit_->cmd_op = pending_.front().op->cmd_op;
      unit_->cmd_vl = pending_.front().vl;
      unit_->cmd_tag = pending_.front().tag;
    }
    unit_->in_valid = !feeding_.empty();
    unit_->out_ready = 1;
    const Fired f = cycle();
    if (f.in && --feeding_.front().left == 0) {
      if (!feeding_.front().op->beats_out) done(feeding_.front(), now);
      feeding_.pop_front();
    }
    if (f.out) {
      if (draining_.empty()) fail("a beat came out while no command waits for one");
      if (f.tag != draining_.front().tag) wrong_tag(f.tag, draining_.front());
      if (f.last) {
        done(draining_.front(), now);
        draining_.pop_front();
      }
    }
    if (f.cmd) {
      Cmd c = pending_.front();
      pending_.pop_front();
      c.left = c.vl;
      if (c.op->beats_in) feeding_.push_back(c);
      if (c.op->beats_out) draining_.push_back(c);
    }
    const bool owed = !pending_.empty() || !feeding_.empty() || !draining_.empty();
    idle_ = owed && !f.any() && !f.busy ? idle_ + 1 : 0;
    if (idle_ > LIMIT) fail("deadlock: commands are left, nothing has fired and the unit is not busy");
  }
  bool ready(Id id, Cycle) const override { return ready_.count(id) != 0; }
  // The timing line is printed here, so an instruction gem5 squashes prints nothing.
  void commit(const Insn &, Id id, Cycle) override {
    const auto it = ready_.find(id);
    if (it == ready_.end()) return;
    const Cmd &c = it->second;
    if (!replaying()) {
      printf("[rtl] %s vl=%u: issued @%" PRIu64 ", ready @%" PRIu64 " (%" PRIu64 " cycles)\n", c.op->name, c.vl,
             c.issued, c.ready, c.ready - c.issued);
      fflush(stdout);
    }
    ready_.erase(it);
  }

 private:
  enum Schedule { DEFAULT, SLOW, RANDOM };
  static constexpr uint64_t LIMIT = 1u << 22;
  struct Cmd {
    Id id;
    const Op *op;
    uint32_t vl;
    Cycle issued;
    uint32_t tag = 0, left = 0;
    Cycle ready = 0;
  };
  struct Fired {
    bool cmd = false, in = false, out = false, last = false, busy = false;
    uint32_t tag = 0;
    std::vector<uint8_t> out_bytes;
    bool any() const { return cmd || in || out; }
  };

  // One clock: settle, sample what fires, take the edge, settle again.
  Fired cycle() {
    Fired f;
    unit_.settle();
    f.cmd = unit_->cmd_valid && unit_->cmd_ready;
    f.in = unit_->in_valid && unit_->in_ready;
    f.out = unit_->out_valid && unit_->out_ready;
    f.last = unit_->out_last;
    f.tag = unit_->out_tag;
    if (f.out)
      for (uint32_t lane = 0; lane < T::LANES; lane++) f.out_bytes.push_back(get_byte(unit_->out_data, lane));
    f.busy = unit_->busy;
    unit_.edge();
    unit_.settle();
    return f;
  }

  const Op &find(const Insn &insn) const {
    for (const Op &op : T::OPS)
      if ((insn.bits & op.mask) == op.match) return op;
    fail("an instruction no entry of the table owns");
  }
  int free_tag() const {
    for (uint32_t t = 0; t < T::TAGS; t++)
      if (!tag_used_[t]) return static_cast<int>(t);
    return -1;
  }
  [[noreturn]] void fail(const char *what) const {
    fprintf(stderr, "%s: %s\n", T::NAME, what);
    std::exit(1);
  }
  [[noreturn]] void wrong_tag(uint32_t tag, const Cmd &c) const {
    fprintf(stderr, "%s: a beat came out with out_tag %u, but the oldest command with beats out is %s vl=%u, tag %u\n",
            T::NAME, tag, c.op->name, c.vl, c.tag);
    std::exit(1);
  }
  [[noreturn]] void stuck(const Op &op, bool cmd, uint32_t sent, size_t in, size_t out) const {
    fprintf(stderr, "%s: deadlock in %s: cmd %s, in %u of %zu beats, out %zu beats; the unit is not busy\n", T::NAME,
            op.name, cmd ? "taken" : "waiting (cmd_ready low)", sent, in, out);
    std::exit(1);
  }
  void done(Cmd c, Cycle now) {
    tag_used_[c.tag] = false;
    c.ready = now;
    ready_.emplace(c.id, c);
  }
  uint64_t next_random() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 7;
    seed_ ^= seed_ << 17;
    return seed_;
  }

  Unit<V> unit_;
  Schedule schedule_ = DEFAULT;
  uint64_t seed0_ = 1, seed_ = 1, idle_ = 0;
  uint32_t max_gap_ = 0;
  bool issued_any_ = false;
  Cycle last_issue_ = 0;
  std::vector<bool> tag_used_ = std::vector<bool>(T::TAGS, false);
  std::deque<Cmd> pending_, feeding_, draining_;
  std::map<Id, Cmd> ready_;
};

}  // namespace rtl

#endif
