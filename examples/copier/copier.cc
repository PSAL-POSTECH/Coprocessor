// copier.sv's timing as a vcix_cp component, libcopier.so: commands done as a queue of 4 takes them, the head freeing
// its slot as it starts, a copy then 2 requests a word of 8 bytes, one a cycle; a wait done once all are finished. On
// Spike a copy writes memory as it issues, the RTL's only as its unit is clocked: without a wait the two programs differ.
#include "vcix_cp.hpp"

namespace {

// custom-2, funct3 4, as examples/rtl/copier: the source, a copy, a wait.
class Copier : public cp::Component {
 public:
  static constexpr uint64_t BYTES = 256;
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr cp::Encoding COPY_SOURCE{0x0600405B, FORM, "copy source"};
  static constexpr cp::Encoding COPY{0x0A00405B, FORM, "copy"};
  static constexpr cp::Encoding COPY_WAIT{0x0E00405B, FORM, "copy wait"};

  explicit Copier(cp::Coprocessor &cp) : Component(cp, "COPIER") {
    handle(COPY_SOURCE, &Copier::set_source);
    handle(COPY, &Copier::copy);
    handle(COPY_WAIT, &Copier::wait);
  }
  void configure(const cp::Config &) override { commands_.configure(4, cp::Free::WHEN_STARTED); }

 private:
  // x[rs1] is the source of the copies after it.
  void set_source(cp::Instr &in) {
    in.use(issue_);
    in.on_issue([&](const cp::Host &host) { source_.write() = host.xreg(in.rs1()); });
    in.enqueue(commands_, cp::Done::WhenEnqueued);
  }
  // BYTES from the source to x[rs1], a read and a write a word; the source moves past them.
  void copy(cp::Instr &in) {
    in.use(issue_);
    in.on_issue([&](const cp::Host &host) {
      const uint64_t to = host.xreg(in.rs1());
      uint64_t &from = source_.write();
      for (uint64_t i = 0; i < BYTES; i += 8) {
        uint64_t word = 0;
        host.mem_read(from + i, &word, 8);
        host.mem_write(to + i, &word, 8);
      }
      from += BYTES;
    });
    in.enqueue(commands_, cp::Done::WhenEnqueued);
    in.transfer(2 * BYTES / 8, cp::Through(memory_));
  }
  void wait(cp::Instr &in) {
    in.use(issue_);
    in.enqueue(commands_, cp::Done::WhenIdle);
  }

  cp::Port issue_{*this, "issue", "instructions", 1, cp::Port::PRIMARY};
  cp::Port memory_{*this, "memory", "requests", 1};
  cp::CmdQueue commands_{*this};
  cp::Register<uint64_t> source_{*this};
};

class CopierCP final : public cp::Coprocessor {
 public:
  const char *name() const override { return "copier"; }

 private:
  Copier copier_{*this};
};

}

VCIX_ACCEL_REGISTER(CopierCP)
