// Example: the tpu model with the matrix unit (mxu.hpp) as its array, in place of the systolic array and the
// multi-precision array, whose encodings it has. A different machine, so a different library: libtpu_mxu.so.
#define TPU_MXU 1
#include <cinttypes>
#include <cstdio>
#include <deque>
#include <vector>

#include "vcix_accel.hpp"

#include "functional.hpp"
#include "misc.hpp"
#include "mxu.hpp"
#include "sfu.hpp"
#include "xlu.hpp"

namespace {

using namespace vcix_accel;

class TpuMxu : public Model {
 public:
  const char *name() const override { return "tpu_mxu"; }
  std::vector<Encoding> owns() const override {
    std::vector<Encoding> all;
    for (const std::vector<Encoding> &unit :
         {tpu::Sfu::encodings(), tpu::Misc::encodings(), tpu::Xlu::encodings(), tpu::Mxu::encodings()})
      all.insert(all.end(), unit.begin(), unit.end());
    return all;
  }

  void configure(const Config &config) override {
    trace_ = config.uint("tpu_trace", 0) != 0;
    sfu_.configure(config);
    misc_.configure(config);
    xlu_.configure(config);
    mxu_.configure(config);
    functional_.configure(config);
  }

  void execute(const Host &host, const Insn &insn) override { functional_.execute(host, insn); }

  bool can_accept(const Insn &insn, Cycle now) const override {
    if (sfu_.owns(insn)) return sfu_.can_accept(insn, now);
    if (misc_.owns(insn)) return misc_.can_accept(insn, now);
    if (xlu_.owns(insn)) return xlu_.can_accept(insn, now);
    if (mxu_.owns(insn)) return mxu_.can_accept(insn, now);
    return false;
  }
  Cycle issue(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_issue(insn, now);
    if (sfu_.owns(insn)) return sfu_.issue(insn, id, now);
    if (misc_.owns(insn)) return misc_.issue(insn, id, now);
    if (xlu_.owns(insn)) return xlu_.issue(insn, id, now);
    if (mxu_.owns(insn)) return mxu_.issue(insn, id, now);
    return 1;
  }
  void commit(const Insn &insn, Id id, Cycle now) override {
    if (trace_) trace_commit(insn, now);
    if (sfu_.owns(insn)) sfu_.commit(insn, id, now);
    if (misc_.owns(insn)) misc_.commit(insn, id, now);
    if (xlu_.owns(insn)) xlu_.commit(insn, id, now);
    if (mxu_.owns(insn)) mxu_.commit(insn, id, now);
  }
  void tick(Cycle now) override {
    sfu_.tick(now);
    misc_.tick(now);
    xlu_.tick(now);
    mxu_.tick(now);
  }
  std::vector<const Port *> ports() const override {
    std::vector<const Port *> all;
    for (const std::vector<const Port *> &unit : {sfu_.ports(), misc_.ports(), xlu_.ports(), mxu_.ports()})
      all.insert(all.end(), unit.begin(), unit.end());
    return all;
  }
  void reset() override {
    sfu_.reset();
    misc_.reset();
    xlu_.reset();
    mxu_.reset();
    functional_.reset();
    issued_any_ = false;
    last_issue_ = 0;
    in_flight_.clear();
  }

 private:
  const char *name_of(const Insn &insn) const {
    for (const Encoding &e : owns())
      if ((insn.bits & e.mask) == e.match) return e.name;
    return "?";
  }
  void trace_issue(const Insn &insn, Cycle now) {
    printf("[tpu] issue %s: ", name_of(insn));
    if (issued_any_)
      printf("%" PRIu64 " cycles after the last issue, %zu in flight", now - last_issue_, in_flight_.size());
    else
      printf("the first");
    if (xlu_.owns(insn)) printf(", input queue %u, output queue %u", xlu_.input_entries(), xlu_.output_entries());
    if (mxu_.owns(insn)) {
      const uint32_t array = tpu::Mxu::array_of(insn);
      if (mxu_.arrays() > 1) printf(", array %u", array);
      if (array < mxu_.arrays())
        printf(", input queue %u, output queues %u and %u", mxu_.input_entries(array), mxu_.lower_entries(array),
               mxu_.upper_entries(array));
    }
    printf("\n");
    fflush(stdout);
    issued_any_ = true;
    last_issue_ = now;
    in_flight_.push_back(now);
  }
  void trace_commit(const Insn &insn, Cycle now) {
    const Cycle issued = in_flight_.front();
    in_flight_.pop_front();
    if (replaying()) return;
    printf("[tpu] commit %s: %" PRIu64 " cycles after its issue\n", name_of(insn), now - issued);
    fflush(stdout);
  }

  tpu::Sfu sfu_;
  tpu::Misc misc_;
  tpu::Xlu xlu_;
  tpu::Mxu mxu_;
  tpu::Functional functional_;

  bool trace_ = false;
  // Kept only under trace_; no answer to the simulator reads them.
  bool issued_any_ = false;
  Cycle last_issue_ = 0;
  // Issue cycles of what is not committed, oldest first.
  std::deque<Cycle> in_flight_;
};

}  // namespace

VCIX_ACCEL_REGISTER(TpuMxu)
