// A TPU as one model: a cp::Coprocessor of its components, plus tpu_trace, a line at every issue and every commit. A model
// derives from it and holds its components as members, as tpu.cc does. An issue's line ends with the component's queues:
// its trace_queues(insn) when it is a tpu::Component, else the input and output queues of its one pipeline, if any.
#ifndef TPU_MACHINE_HPP
#define TPU_MACHINE_HPP

#include <cinttypes>
#include <cstdio>
#include <deque>

#include "vcix_cp.hpp"

namespace tpu {

// A component that says itself what the [tpu] trace adds to an issue of its instructions.
class Component : public cp::Component {
 public:
  using cp::Component::Component;
  virtual void trace_queues(const cp::Insn &insn) const = 0;
};

class Coprocessor : public cp::Coprocessor {
  using Cycle = cp::Cycle;
  using Id = cp::Id;
  using Insn = cp::Insn;

 public:
  void configure(const cp::Config &config) override {
    trace_ = config.uint("tpu_trace", 0) != 0;
    cp::Coprocessor::configure(config);
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_issue(insn, now);
    return cp::Coprocessor::issue(insn, id, now);
  }
  void commit(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_commit(insn, now);
    cp::Coprocessor::commit(insn, id, now);
  }
  void reset() override {
    cp::Coprocessor::reset();
    issued_any_ = false;
    last_issue_ = 0;
    in_flight_.clear();
  }

 protected:
  Coprocessor() = default;

 private:
  static void queues(const cp::Component &comp, const Insn &insn) {
    if (const auto *own = dynamic_cast<const Component *>(&comp)) return own->trace_queues(insn);
    uint32_t pipelines = 0;
    each_pipeline(comp, [&](const cp::AnyPipeline &) { pipelines++; });
    if (pipelines != 1) return;
    each_pipeline(comp, [](const cp::AnyPipeline &p) {
      std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(p.input_rows()),
                  static_cast<uint32_t>(p.output_rows()));
    });
  }
  const char *name_of(const Insn &insn) const {
    for (const cp::Encoding &e : owns())
      if ((insn.bits & e.mask) == e.match) return e.name;
    return "?";
  }
  void trace_issue(const Insn &insn, Cycle now) {
    std::printf("[tpu] issue %s: ", name_of(insn));
    if (issued_any_)
      std::printf("%" PRIu64 " cycles after the last issue, %zu in flight", now - last_issue_, in_flight_.size());
    else
      std::printf("the first");
    with_owner(insn, [&](const cp::Component &comp) { queues(comp, insn); });
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

  bool trace_ = false;
  bool issued_any_ = false;
  Cycle last_issue_ = 0;
  std::deque<Cycle> in_flight_;
};

}

#endif
