// A TPU's instructions with no timing of their own: one cycle each, tpu_misc_issue_width of them a cycle.
#ifndef TPU_MISC_HPP
#define TPU_MISC_HPP

#include <iterator>
#include <vector>

#include "vcix_accel.hpp"

namespace tpu {

class Misc {
 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Misc(const char *name) : issue_{name, "issue", "instructions", 2, vcix_accel::Port::PRIMARY} {}

  static std::vector<vcix_accel::Encoding> encodings() { return {std::begin(ENCODINGS), std::end(ENCODINGS)}; }
  bool owns(const vcix_accel::Insn &insn) const {
    for (const vcix_accel::Encoding &e : ENCODINGS)
      if ((insn.bits & e.mask) == e.match) return true;
    return false;
  }
  void configure(const vcix_accel::Config &config) {
    const uint64_t width = config.uint(WIDTH_KEY, 2);
    if (width == 0) throw vcix_accel::ConfigError(WIDTH_KEY, "0", "is not an issue width: at least 1");
    issue_.set_capacity(width);
  }
  bool can_accept(const vcix_accel::Insn &, vcix_accel::Cycle now) const { return issue_.room(now) != 0; }
  vcix_accel::Cycle issue(const vcix_accel::Insn &, vcix_accel::Id, vcix_accel::Cycle now) {
    issue_.admit(1, now);
    return 1;
  }
  void commit(const vcix_accel::Insn &, vcix_accel::Id, vcix_accel::Cycle) {}
  void tick(vcix_accel::Cycle) {}
  void reset() { issue_.reset(); }
  // What the [tpu] trace adds to an issue: these instructions have no queue to show.
  void trace_queues(const vcix_accel::Insn &) const {}

  std::vector<const vcix_accel::Port *> ports() const { return {&issue_}; }

 private:
  // custom-2: lane number, compute. custom-1: DMA.
  static constexpr uint32_t FUNCTION = 0xFE00707F, FIELD = 0xFE0FF07F;
  static constexpr vcix_accel::Encoding ENCODINGS[] = {
      {0x0000305B, FUNCTION, "vlane_idx"},
      {0x0600305B, FUNCTION, "compute"},
      {0x0200302B, FUNCTION, "mvin2"},
      {0x0400302B, FUNCTION, "mvin"},
      {0x0600302B, FUNCTION, "mvout"},
      {0x0E00302B, FUNCTION, "dma_config_desc"},
      {0x1000302B, FUNCTION, "dma_index_key"},
      {0x1C00302B, FUNCTION, "mvin3"},
      // custom-2, sf.vc.v.vv (funct3 000) funct6 10: a bf16 pack of two f32 registers, as the v6e's VPU packs
      // (vpack): vd's vl words from vs2's vl elements then vs1's, each rounded to bf16 (to nearest, ties to even),
      // two to a word, element 2w in the low half. No 16-bit vtype is set for it.
      {0x2800005B, FUNCTION, "pack"},
      // custom-2, sf.vc.v.iv funct6 10 with the field at 19:15 8 / 9 (the SFU's vsin..vatan use 0..3): the bf16
      // unpack, the pack the other way, as the v6e's vunpack.c.l / vunpack.c.h: vs2's vl words hold 2 * vl bf16,
      // two to a word; unpack_lo writes vd's vl f32 from the first vl of them, unpack_hi from the last vl.
      {0x2804305B, FIELD, "unpack_lo"},
      {0x2804B05B, FIELD, "unpack_hi"},
  };
  static constexpr const char *WIDTH_KEY = "tpu_misc_issue_width";

  vcix_accel::Port issue_;
};

}  // namespace tpu

#endif
