// A TPU as one model: vcix_uarch::Machine of its units, plus tpu_trace, a line at every issue and every commit. A model
// derives from it with its units and the names they report, as tpu.cc does. An issue's line ends with the unit's queues:
// its trace_queues(insn) when it has one, else the input and output queues of its one pipeline, if it has one.
#ifndef TPU_MACHINE_HPP
#define TPU_MACHINE_HPP

#include <cinttypes>
#include <cstdio>
#include <deque>

#include "vcix_uarch.hpp"

namespace tpu {

template <class... Unit>
class Machine : public vcix_uarch::Machine<Unit...> {
  using Base = vcix_uarch::Machine<Unit...>;
  using Cycle = vcix_uarch::Cycle;
  using Id = vcix_uarch::Id;
  using Insn = vcix_uarch::Insn;

 public:
  void configure(const vcix_uarch::Config &config) override {
    trace_ = config.uint("tpu_trace", 0) != 0;
    Base::configure(config);
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_issue(insn, now);
    return Base::issue(insn, id, now);
  }
  void commit(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_commit(insn, now);
    Base::commit(insn, id, now);
  }
  void reset() override {
    Base::reset();
    issued_any_ = false;
    last_issue_ = 0;
    in_flight_.clear();
  }

 protected:
  using Base::Base;

 private:
  template <class U>
  static auto queues(const U &unit, const Insn &insn, int) -> decltype(unit.trace_queues(insn)) {
    unit.trace_queues(insn);
  }
  template <class U>
  static void queues(const U &unit, const Insn &, long) {
    uint32_t pipelines = 0;
    Base::each_pipeline(unit, [&](const vcix_uarch::PipelineTiming &) { pipelines++; });
    if (pipelines != 1) return;
    Base::each_pipeline(unit, [](const vcix_uarch::PipelineTiming &p) {
      std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(p.input_entries()),
                  static_cast<uint32_t>(p.output_entries()));
    });
  }
  const char *name_of(const Insn &insn) const {
    for (const vcix_uarch::Encoding &e : this->owns())
      if ((insn.bits & e.mask) == e.match) return e.name;
    return "?";
  }
  void trace_issue(const Insn &insn, Cycle now) {
    std::printf("[tpu] issue %s: ", name_of(insn));
    if (issued_any_)
      std::printf("%" PRIu64 " cycles after the last issue, %zu in flight", now - last_issue_, in_flight_.size());
    else
      std::printf("the first");
    this->with_owner(insn, [&](const auto &unit) { queues(unit, insn, 0); });
    std::printf("\n");
    std::fflush(stdout);
    issued_any_ = true;
    last_issue_ = now;
    in_flight_.push_back(now);
  }
  void trace_commit(const Insn &insn, Cycle now) {
    const Cycle issued = in_flight_.front();
    in_flight_.pop_front();
    if (this->replaying()) return;
    std::printf("[tpu] commit %s: %" PRIu64 " cycles after its issue\n", name_of(insn), now - issued);
    std::fflush(stdout);
  }

  bool trace_ = false;
  bool issued_any_ = false;
  Cycle last_issue_ = 0;
  std::deque<Cycle> in_flight_;
};

}  // namespace tpu

#endif
