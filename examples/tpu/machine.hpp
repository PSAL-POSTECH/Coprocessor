// A TPU's timing as one model: the units it holds, in their order, and its functional face. Each call goes to the
// unit that owns the instruction; configure, tick, reset and the ports go to every unit in that order. A model
// derives from it with its units and the names they report, as tpu.cc does.
#ifndef TPU_MACHINE_HPP
#define TPU_MACHINE_HPP

#include <cinttypes>
#include <cstdio>
#include <deque>
#include <tuple>
#include <vector>

#include "vcix_accel.hpp"

namespace tpu {

template <class Functional, class... Unit>
class Machine : public vcix_accel::Model {
 public:
  using Config = vcix_accel::Config;
  using Cycle = vcix_accel::Cycle;
  using Encoding = vcix_accel::Encoding;
  using Host = vcix_accel::Host;
  using Id = vcix_accel::Id;
  using Insn = vcix_accel::Insn;
  using Port = vcix_accel::Port;

  std::vector<Encoding> owns() const override {
    std::vector<Encoding> all;
    for (const std::vector<Encoding> &unit : {Unit::encodings()...}) all.insert(all.end(), unit.begin(), unit.end());
    return all;
  }

  void configure(const Config &config) override {
    trace_ = config.uint("tpu_trace", 0) != 0;
    each([&](auto &unit) { unit.configure(config); });
    functional_.configure(config);
  }

  void execute(const Host &host, const Insn &insn) override { functional_.execute(host, insn); }

  bool can_accept(const Insn &insn, Cycle now) const override {
    bool accepts = false;
    first_owner(insn, [&](const auto &unit) { accepts = unit.can_accept(insn, now); });
    return accepts;
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_issue(insn, now);
    Cycle latency = 1;
    first_owner(insn, [&](auto &unit) { latency = unit.issue(insn, id, now); });
    return latency;
  }
  void commit(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_commit(insn, now);
    each([&](auto &unit) {
      if (unit.owns(insn)) unit.commit(insn, id, now);
    });
  }
  void tick(Cycle now) override {
    each([&](auto &unit) { unit.tick(now); });
  }
  std::vector<const Port *> ports() const override {
    std::vector<const Port *> all;
    each([&](const auto &unit) {
      const std::vector<const Port *> own = unit.ports();
      all.insert(all.end(), own.begin(), own.end());
    });
    return all;
  }
  void reset() override {
    each([&](auto &unit) { unit.reset(); });
    functional_.reset();
    issued_any_ = false;
    last_issue_ = 0;
    in_flight_.clear();
  }

 protected:
  // Each unit's name, in the order of the units: the name its statistics report.
  template <class... Name>
  explicit Machine(Name... names) : units_(names...) {
    static_assert(sizeof...(Name) == sizeof...(Unit), "a machine names each of its units");
  }

 private:
  template <class Do>
  void each(Do &&what) {
    std::apply([&](Unit &...unit) { (what(unit), ...); }, units_);
  }
  template <class Do>
  void each(Do &&what) const {
    std::apply([&](const Unit &...unit) { (what(unit), ...); }, units_);
  }
  // `what` runs on the first unit that owns the instruction, if one does.
  template <class Do>
  void first_owner(const Insn &insn, Do &&what) {
    std::apply([&](Unit &...unit) { ((unit.owns(insn) && (what(unit), true)) || ...); }, units_);
  }
  template <class Do>
  void first_owner(const Insn &insn, Do &&what) const {
    std::apply([&](const Unit &...unit) { ((unit.owns(insn) && (what(unit), true)) || ...); }, units_);
  }

  const char *name_of(const Insn &insn) const {
    for (const Encoding &e : owns())
      if ((insn.bits & e.mask) == e.match) return e.name;
    return "?";
  }
  void trace_issue(const Insn &insn, Cycle now) {
    std::printf("[tpu] issue %s: ", name_of(insn));
    if (issued_any_)
      std::printf("%" PRIu64 " cycles after the last issue, %zu in flight", now - last_issue_, in_flight_.size());
    else
      std::printf("the first");
    each([&](const auto &unit) {
      if (unit.owns(insn)) unit.trace_queues(insn);
    });
    std::printf("\n");
    std::fflush(stdout);
    issued_any_ = true;
    last_issue_ = now;
    in_flight_.push_back(now);
  }
  void trace_commit(const Insn &insn, Cycle now) {
    const Cycle issued = in_flight_.front();
    in_flight_.pop_front();
    if (replaying()) return;
    std::printf("[tpu] commit %s: %" PRIu64 " cycles after its issue\n", name_of(insn), now - issued);
    std::fflush(stdout);
  }

  std::tuple<Unit...> units_;
  Functional functional_;

  bool trace_ = false;
  // Kept only under trace_; no answer to the simulator reads them.
  bool issued_any_ = false;
  Cycle last_issue_ = 0;
  // Issue cycles of what is not committed, oldest first.
  std::deque<Cycle> in_flight_;
};

}  // namespace tpu

#endif
