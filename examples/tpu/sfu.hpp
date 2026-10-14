// A TPU's special-function unit: one instruction per cycle, a fixed latency.
#ifndef TPU_SFU_HPP
#define TPU_SFU_HPP

#include <iterator>
#include <vector>

#include "vcix_accel.hpp"

namespace tpu {

class Sfu {
 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Sfu(const char *name) : entry_{name, "entry", "instructions", 1, vcix_accel::Port::PRIMARY} {}

  static std::vector<vcix_accel::Encoding> encodings() { return {std::begin(ENCODINGS), std::end(ENCODINGS)}; }
  bool owns(const vcix_accel::Insn &insn) const {
    for (const vcix_accel::Encoding &e : ENCODINGS)
      if ((insn.bits & e.mask) == e.match) return true;
    return false;
  }
  void configure(const vcix_accel::Config &config) {
    latency_ = config.uint(LATENCY_KEY, 10);
    if (latency_ == 0) throw vcix_accel::ConfigError(LATENCY_KEY, "0", "is not a pipeline depth: at least 1");
  }
  bool can_accept(const vcix_accel::Insn &, vcix_accel::Cycle now) const { return entry_.room(now) != 0; }
  vcix_accel::Cycle issue(const vcix_accel::Insn &, vcix_accel::Id, vcix_accel::Cycle now) {
    entry_.admit(1, now);
    return latency_;
  }
  void commit(const vcix_accel::Insn &, vcix_accel::Id, vcix_accel::Cycle) {}
  void tick(vcix_accel::Cycle) {}
  void reset() { entry_.reset(); }
  // What the [tpu] trace adds to an issue: a pipeline has no queue to show.
  void trace_queues(const vcix_accel::Insn &) const {}

  std::vector<const vcix_accel::Port *> ports() const { return {&entry_}; }

 private:
  // sf.vc.v.iv on custom-2: opcode, funct3, funct6 and vm; FUNCTION_FIELD adds the field at 19:15.
  static constexpr uint32_t FUNCTION = 0xFE00707F, FUNCTION_FIELD = 0xFE0FF07F;
  static constexpr vcix_accel::Encoding ENCODINGS[] = {
      {0x2000305B, FUNCTION, "verf"},
      {0x2400305B, FUNCTION, "vtanh"},
      {0x2800305B, FUNCTION_FIELD, "vsin"},
      {0x2800B05B, FUNCTION_FIELD, "vcos"},
      {0x2801305B, FUNCTION_FIELD, "vlog"},
      {0x2801B05B, FUNCTION_FIELD, "vatan"},
      {0x2C00305B, FUNCTION, "vexp"},
  };
  static constexpr const char *LATENCY_KEY = "tpu_sfu_latency_cycles";

  vcix_accel::Cycle latency_ = 10;
  vcix_accel::Port entry_;
};

}  // namespace tpu

#endif
