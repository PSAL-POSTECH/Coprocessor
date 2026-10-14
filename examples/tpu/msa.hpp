// The tpu model's multi-precision array: the systolic array's queues and delay line, one push for weights and inputs.
// One row a cycle enters the array, whatever its active width, and one instruction a cycle the unit.
#ifndef TPU_MSA_HPP
#define TPU_MSA_HPP

#include <cstdio>
#include <vector>

#include "vcix_accel.hpp"

#include "stream.hpp"

namespace tpu {

class Msa {
 public:
  using Config = vcix_accel::Config;
  using Cycle = vcix_accel::Cycle;
  using Encoding = vcix_accel::Encoding;
  using Id = vcix_accel::Id;
  using Insn = vcix_accel::Insn;

  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Msa(const char *name)
      : stream_{name, "input", "rows", vcix_accel::Port::PRIMARY, 255, 256},
        issue_{name, "issue", "instructions", 1},
        weight_push_{name, "weight_push", "instructions", 1},
        pop_{name, "pop", "rows", 256} {}

  static std::vector<Encoding> encodings() { return {{PUSH, FORM, "msa push"}, {POP, FORM, "msa pop"}}; }
  bool owns(const Insn &insn) const { return is(insn, PUSH) || is(insn, POP); }

  void configure(const Config &config) {
    const uint64_t lanes = config.uint(LANES_KEY, 128);
    const uint64_t entries = config.uint(QUEUE_KEY, 256);
    if (lanes < 1 || lanes > MAX_LANES)
      throw vcix_accel::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    stream_.configure(static_cast<uint32_t>(2 * lanes - 1), static_cast<uint32_t>(entries));
    pop_.set_capacity(entries);
  }

  bool can_accept(const Insn &insn, Cycle now) const {
    if (!issue_.room(now)) return false;
    if (is(insn, POP)) return stream_.gives(insn.vl) && insn.vl <= pop_.room(now);
    if (is_input(insn)) return stream_.has_room(insn.vl);
    return weight_push_.room(now) != 0;
  }
  Cycle issue(const Insn &insn, Id, Cycle now) {
    issue_.admit(1, now);
    if (is(insn, POP)) {
      pop_.admit(insn.vl, now);
      stream_.pop(insn.vl);
    } else if (is_input(insn)) {
      stream_.push(insn.vl);
    } else {
      weight_push_.admit(1, now);
    }
    return 1;
  }
  void commit(const Insn &, Id, Cycle) {}
  void tick(Cycle now) { stream_.tick(now); }
  void reset() {
    stream_.reset();
    issue_.reset();
    weight_push_.reset();
    pop_.reset();
  }

  std::vector<const vcix_accel::Port *> ports() const { return {&stream_.entry(), &issue_, &weight_push_, &pop_}; }

  uint32_t input_entries() const { return stream_.input_entries(); }
  uint32_t output_entries() const { return stream_.output_entries(); }
  // What the [tpu] trace adds to an issue.
  void trace_queues(const Insn &) const { std::printf(", input queue %u, output queue %u", input_entries(), output_entries()); }

 private:
  // funct6, vm and funct3 of custom-2: sf.vc.iv 2 and sf.vc.v.i 3.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr uint32_t PUSH = 0x2A00305B;
  static constexpr uint32_t POP = 0x0C00305B;
  // The field at 19:15 of a push: [4] a weight, [3:2] the active-width shift, [1:0] the format. The timing reads [4].
  static constexpr uint32_t WEIGHT = 0x10;
  static constexpr uint64_t MAX_LANES = 1u << 20;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_systolic_queue_entries";

  static bool is(const Insn &insn, uint32_t match) { return (insn.bits & FORM) == match; }
  static bool is_input(const Insn &insn) { return is(insn, PUSH) && !(vcix_accel::rs1(insn) & WEIGHT); }

  Stream stream_;
  vcix_accel::Port issue_;
  vcix_accel::Port weight_push_;
  vcix_accel::Port pop_;
};

}  // namespace tpu

#endif
