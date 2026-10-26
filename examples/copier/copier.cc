// copier.sv behind rtl_model.hpp as a vcix_cp component, libcopier.so: a command enters the unit's queue of 4 the cycle
// after its issue and is ready then; the head is taken while no copy runs, a copy then makes 2 requests a word of 8
// bytes, one a cycle; a wait is ready once nothing is queued or copying. On Spike a copy writes memory as it enters.
#include <vector>

#include "vcix_cp.hpp"

namespace {

// custom-2, funct3 4, as examples/rtl/copier: the source, a copy, a wait.
class Copier : public cp::Component {
 public:
  static constexpr uint64_t BYTES = 256, DEPTH = 4;
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr cp::Encoding COPY_SOURCE{0x0600405B, FORM, "copy source"};
  static constexpr cp::Encoding COPY{0x0A00405B, FORM, "copy"};
  static constexpr cp::Encoding COPY_WAIT{0x0E00405B, FORM, "copy wait"};

  explicit Copier(cp::Coprocessor &cp) : Component(cp, "COPIER") {
    register_insn(COPY_SOURCE);
    register_insn(COPY);
    register_insn(COPY_WAIT);
  }
  // insns() is the wrapper's one pending command; its default 2 deep, so a slot freed in a cycle is taken again in
  // that cycle, as the wrapper takes it.
  void configure(const cp::Config &) override { commands_.configure(DEPTH); }
  void reset() override {
    source_ = left_ = 0;
    waits_.clear();
  }
  // As rtl_model.hpp's can_accept: nothing pending, and a slot in the unit's queue as this cycle left it.
  bool valid(const cp::Insn &) const override { return !insns().size() && commands_.size() < DEPTH; }
  // One clock edge of copier.sv: a request of the running copy or the head taken, the pending command in, then the
  // waits if the unit is no longer busy.
  void cycle() override {
    if (left_) {
      if (memory_.utilize(1)) left_--;
    } else if (commands_.length() && is(COPY, commands_.pop())) {
      left_ = 2 * BYTES / 8;
    }
    if (insns().length() && commands_.push(insns().front())) enter(insns().pop());
    if (left_ || commands_.size()) return;
    for (const cp::Insn &wait : waits_) complete(wait);
    waits_.clear();
  }

 private:
  static bool is(const cp::Encoding &e, const cp::Insn &insn) { return (insn.bits & e.mask) == e.match; }
  // A source or a copy is ready as it enters; on Spike its values happen then: x[rs1] is the source of the copies after
  // it, a copy moves BYTES from the source to x[rs1], a word at a time, and the source past them.
  void enter(const cp::Insn &insn) {
    values([&](const cp::Host &host) {
      if (is(COPY_SOURCE, insn)) source_ = host.xreg(cp::rs1(insn));
      if (!is(COPY, insn)) return;
      const uint64_t to = host.xreg(cp::rs1(insn));
      for (uint64_t i = 0; i < BYTES; i += 8) {
        uint64_t word = 0;
        host.mem_read(source_ + i, &word, 8);
        host.mem_write(to + i, &word, 8);
      }
      source_ += BYTES;
    });
    instructions_.add(1);
    if (is(COPY_WAIT, insn)) waits_.push_back(insn);
    else complete(insn);
  }

  cp::Counter instructions_{*this, "instructions", 1};
  cp::Port memory_{*this, "memory", 1};
  cp::Queue<cp::Insn> commands_{*this, "commands"};
  uint64_t source_ = 0, left_ = 0;
  std::vector<cp::Insn> waits_;
};

class CopierCP final : public cp::Coprocessor {
 public:
  const char *name() const override { return "copier"; }

 private:
  Copier copier_{*this};
};

}

VCIX_CP_REGISTER(CopierCP)
