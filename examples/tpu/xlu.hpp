// The tpu model's cross-lane unit: one element a cycle enters its delay line, one instruction a cycle the unit.
// The pushes between two pops are one tile, as in tpu::Functional: the first push after a pop forgets the tile before.
#ifndef TPU_XLU_HPP
#define TPU_XLU_HPP

#include <vector>

#include "vcix_accel.hpp"

#include "stream.hpp"

namespace tpu {

class Xlu {
 public:
  using Config = vcix_accel::Config;
  using Cycle = vcix_accel::Cycle;
  using Encoding = vcix_accel::Encoding;
  using Id = vcix_accel::Id;
  using Insn = vcix_accel::Insn;

  static std::vector<Encoding> encodings() {
    return {{PUSH, FORM, "xlu_push"}, {PUSH_PATTERN, FORM, "xlu_push_pattern"}, {POP, FORM, "xlu_pop"}};
  }
  bool owns(const Insn &insn) const { return is_push(insn) || is(insn, POP); }

  void configure(const Config &config) {
    const uint64_t latency = config.uint(LATENCY_KEY, DEFAULT_LATENCY);
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    if (latency < 1 || latency > UINT32_MAX)
      throw vcix_accel::ConfigError(LATENCY_KEY, config.get(LATENCY_KEY), "is not a latency the unit can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    stream_.configure(static_cast<uint32_t>(latency), static_cast<uint32_t>(entries));
  }

  bool can_accept(const Insn &insn, Cycle now) const {
    if (!issue_.room(now)) return false;
    if (is_push(insn)) return stream_.has_room(insn.vl);
    return stream_.gives(insn.vl);
  }
  Cycle issue(const Insn &insn, Id, Cycle now) {
    issue_.admit(1, now);
    if (is_push(insn)) {
      if (popped_) stream_.flush();
      stream_.push(insn.vl);
    } else {
      stream_.pop(insn.vl);
    }
    popped_ = !is_push(insn);
    return 1;
  }
  void commit(const Insn &, Id, Cycle) {}
  void tick(Cycle now) { stream_.tick(now); }
  void reset() {
    stream_.reset();
    issue_.reset();
    popped_ = false;
  }

  std::vector<const vcix_accel::Port *> ports() const { return {&stream_.entry(), &issue_}; }

  uint32_t input_entries() const { return stream_.input_entries(); }
  uint32_t output_entries() const { return stream_.output_entries(); }

 private:
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr uint32_t PUSH = 0x2E00305B;
  static constexpr uint32_t PUSH_PATTERN = 0xAE00305B;
  static constexpr uint32_t POP = 0x0400305B;
  // TODO: the unit's latency is not known; 10 is a placeholder.
  static constexpr uint32_t DEFAULT_LATENCY = 10;
  static constexpr uint32_t DEFAULT_ENTRIES = 256;
  static constexpr const char *LATENCY_KEY = "tpu_xlu_latency_cycles";
  static constexpr const char *QUEUE_KEY = "tpu_xlu_queue_entries";

  static bool is(const Insn &insn, uint32_t match) { return (insn.bits & FORM) == match; }
  static bool is_push(const Insn &insn) { return is(insn, PUSH) || is(insn, PUSH_PATTERN); }

  Stream stream_{"Xlu", "input", "elements", vcix_accel::Port::PRIMARY, DEFAULT_LATENCY, DEFAULT_ENTRIES};
  vcix_accel::Port issue_{"Xlu", "issue", "instructions", 1};
  // A pop was issued since the last push: the next push starts a tile.
  bool popped_ = false;
};

}  // namespace tpu

#endif
