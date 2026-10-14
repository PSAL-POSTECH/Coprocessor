// The tpu model's systolic array: input queue, delay line of width + height - 1, output queue.
// One row a cycle enters the array and one instruction a cycle the unit; a weight push enters nothing.
#ifndef TPU_SYSTOLIC_HPP
#define TPU_SYSTOLIC_HPP

#include <cstdio>
#include <string>
#include <vector>

#include "vcix_accel.hpp"

#include "stream.hpp"

namespace tpu {

class Systolic {
 public:
  using Config = vcix_accel::Config;
  using Cycle = vcix_accel::Cycle;
  using Encoding = vcix_accel::Encoding;
  using Id = vcix_accel::Id;
  using Insn = vcix_accel::Insn;

  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Systolic(const char *name)
      : stream_{name, "input", "rows", vcix_accel::Port::PRIMARY, 255, 256},
        issue_{name, "issue", "instructions", 1},
        weight_push_{name, "weight_push", "instructions", 1},
        pop_{name, "pop", "rows", 256} {}

  static std::vector<Encoding> encodings() {
    return {{INPUT_PUSH, FORM, "systolic input push"},
            {WEIGHT_PUSH, FORM, "systolic weight push"},
            {POP, FORM, "systolic pop"}};
  }
  bool owns(const Insn &insn) const { return is(insn, INPUT_PUSH) || is(insn, WEIGHT_PUSH) || is(insn, POP); }

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
    if (is(insn, INPUT_PUSH)) return stream_.has_room(insn.vl);
    if (is(insn, WEIGHT_PUSH)) return weight_push_.room(now) != 0;
    return stream_.gives(insn.vl) && insn.vl <= pop_.room(now);
  }
  Cycle issue(const Insn &insn, Id, Cycle now) {
    issue_.admit(1, now);
    if (is(insn, INPUT_PUSH)) stream_.push(insn.vl);
    if (is(insn, WEIGHT_PUSH)) weight_push_.admit(1, now);
    if (is(insn, POP)) {
      pop_.admit(insn.vl, now);
      stream_.pop(insn.vl);
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
  uint32_t slots() const { return stream_.slots(); }
  uint32_t queue_capacity() const { return stream_.queue_capacity(); }

 private:
  // funct6, vm and funct3 of custom-2: sf.vc.iv 0, sf.vc.iv 1 and sf.vc.v.i 2.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr uint32_t INPUT_PUSH = 0x2200305B;
  static constexpr uint32_t WEIGHT_PUSH = 0x2600305B;
  static constexpr uint32_t POP = 0x0800305B;
  static constexpr uint64_t MAX_LANES = 1u << 20;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_systolic_queue_entries";

  static bool is(const Insn &insn, uint32_t match) { return (insn.bits & FORM) == match; }

  Stream stream_;
  vcix_accel::Port issue_;
  vcix_accel::Port weight_push_;
  vcix_accel::Port pop_;
};

}  // namespace tpu

#endif
