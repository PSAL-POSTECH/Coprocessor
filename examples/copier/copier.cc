// copier.sv's timing as a vcix_uarch unit, libcopier.so: commands done as a queue of 4 takes them, the head freeing its
// slot as it starts, a copy then 2 requests a word of 8 bytes, one a cycle; a wait done once all are finished. On Spike
// a copy writes memory as it issues, the RTL's only as its unit is clocked: without a wait the two programs differ.
#include "vcix_uarch.hpp"

namespace {

class Copier {
  using Host = vcix_uarch::Host;
  using Route = vcix_uarch::Route;

 public:
  static constexpr uint64_t BYTES = 256;
  explicit Copier(const char *name)
      : issue_{name, "issue", "instructions", 1, vcix_uarch::PRIMARY}, memory_{name, "memory", "requests", 1} {}
  void configure(const vcix_uarch::Config &) { commands_.configure(4, vcix_uarch::Free::WHEN_STARTED); }
  template <class H>
  void hardware(H &&h) {
    h.port(issue_), h.port(memory_);
    h.queue(commands_);
    h.reg(source_);
  }

  // x[rs1] is the source of the copies after it.
  void set_source(Route &r) {
    r.use(issue_).enqueue(commands_).done_when_enqueued();
    r.at_issue([&](const Host &host) { source_.write() = host.xreg(cp::rs1(r.insn())); });
  }
  // BYTES from the source to x[rs1], a read and a write a word; the source moves past them.
  void copy(Route &r) {
    r.use(issue_).enqueue(commands_).done_when_enqueued().transfer(2 * BYTES / 8).through(memory_);
    r.at_issue([&](const Host &host) {
      const uint64_t to = host.xreg(cp::rs1(r.insn()));
      uint64_t &from = source_.write();
      for (uint64_t i = 0; i < BYTES; i += 8) {
        uint64_t word = 0;
        host.mem_read(from + i, &word, 8);
        host.mem_write(to + i, &word, 8);
      }
      from += BYTES;
    });
  }
  void wait(Route &r) { r.use(issue_).enqueue(commands_).done_when_idle(); }

  // custom-2, funct3 4, as examples/rtl/copier: the source, a copy, a wait.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr vcix_uarch::Instruction<Copier> INSTRUCTIONS[] = {
      {0x0600405B, FORM, "copy source", &Copier::set_source},
      {0x0A00405B, FORM, "copy", &Copier::copy},
      {0x0E00405B, FORM, "copy wait", &Copier::wait},
  };

 private:
  vcix_uarch::Port issue_, memory_;
  vcix_uarch::CommandQueue commands_;
  vcix_uarch::Register<uint64_t> source_;
};

class Model final : public vcix_uarch::Machine<Copier> {
 public:
  Model() : Machine("COPIER") {}
  const char *name() const override { return "copier"; }
};

}  // namespace

VCIX_CP_REGISTER(Model)
