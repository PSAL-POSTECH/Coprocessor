// A TPU's instructions with no timing of their own: tpu_misc_issue_width of them a cycle, each done the cycle after its
// issue, when it writes its values: the lane number, the bf16 pack and unpack, and the DMA (dma.hpp), an untimed side
// path whose time TOGSim counts.
#ifndef TPU_MISC_HPP
#define TPU_MISC_HPP

#include <cstring>
#include <string>
#include <vector>

#include "machine.hpp"

#include "dma.hpp"
#include "values.hpp"

namespace tpu {

class Misc : public cp::Component {
  using Cycle = cp::Cycle;
  using Host = cp::Host;
  using Insn = cp::Insn;

 public:
  // custom-2: lane number, compute. custom-1: DMA. pack is custom-2 sf.vc.v.vv funct6 10 (vd's vl words from vs2's vl
  // elements then vs1's, each rounded to bf16, two to a word); unpack_lo / unpack_hi sf.vc.v.iv funct6 10 with the
  // field at 19:15 8 / 9 (vd's vl f32 from the first or the last vl of vs2's 2 * vl bf16).
  static constexpr uint32_t FUNCTION = 0xFE00707F, FIELD = 0xFE0FF07F;
  static constexpr cp::Encoding VLANE_IDX{0x0000305B, FUNCTION, "vlane_idx"};
  static constexpr cp::Encoding COMPUTE{0x0600305B, FUNCTION, "compute"};
  static constexpr cp::Encoding MVIN2{0x0200302B, FUNCTION, "mvin2"};
  static constexpr cp::Encoding MVIN{0x0400302B, FUNCTION, "mvin"};
  static constexpr cp::Encoding MVOUT{0x0600302B, FUNCTION, "mvout"};
  static constexpr cp::Encoding DMA_CONFIG_DESC{0x0E00302B, FUNCTION, "dma_config_desc"};
  static constexpr cp::Encoding DMA_INDEX_KEY{0x1000302B, FUNCTION, "dma_index_key"};
  static constexpr cp::Encoding MVIN3{0x1C00302B, FUNCTION, "mvin3"};
  static constexpr cp::Encoding PACK{0x2800005B, FUNCTION, "pack"};
  static constexpr cp::Encoding UNPACK_LO{0x2804305B, FIELD, "unpack_lo"};
  static constexpr cp::Encoding UNPACK_HI{0x2804B05B, FIELD, "unpack_hi"};

  // `name` is the component as its statistics report it; the model that holds the component says it.
  Misc(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    for (const cp::Encoding *e : {&VLANE_IDX, &COMPUTE, &MVIN2, &MVIN, &MVOUT, &DMA_CONFIG_DESC, &DMA_INDEX_KEY, &MVIN3,
                                  &PACK, &UNPACK_LO, &UNPACK_HI})
      register_insn(*e);
  }
  // width a cycle; a slot is free the cycle after its pop, so 2 x width slots sustain it. A fresh Dma each time.
  void configure(const cp::Config &config) override {
    const uint64_t width = config.uint(WIDTH_KEY, 2);
    if (width == 0) throw cp::ConfigError(WIDTH_KEY, "0", "is not an issue width: at least 1");
    issue_.set_width(width);
    insns().configure(2 * width, width);
    dma_ = Dma{};
    dma_.configure(config);
  }
  void reset() override { dma_.reset(); }
  // Up to the issue width of the instructions issued before this cycle: each writes its values and is done. A
  // transfer on a machine with no scratchpad is a fault (mvin2 and mvin3 are mvin's); one that leaves the scratchpad
  // (Spike only, where the values run) a fault with Dma::INVALID_SPAD_ACCESS.
  void cycle() override {
    while (insns().length() && issue_.available()) {
      const Insn in = insns().pop();
      issue_.utilize(1);
      if (transfer(in) && !dma_.scratchpad()) {
        fault(in, message("%s: the machine description has no %s", is(in, MVOUT) ? MVOUT.name : MVIN.name, Dma::SPAD_LANE_KEY));
        continue;
      }
      std::string why;
      values([&](const Host &host) { why = apply(host, in); });
      if (!why.empty())
        fault(in, why, Dma::INVALID_SPAD_ACCESS);
      else
        complete(in);
    }
  }

 private:
  static bool is(const vcix_insn &insn, const cp::Encoding &e) { return (insn.bits & e.mask) == e.match; }
  static bool transfer(const vcix_insn &insn) {
    return is(insn, MVIN) || is(insn, MVIN2) || is(insn, MVIN3) || is(insn, MVOUT);
  }
  // Its values, and why a transfer left the scratchpad ("" if it did not): compute writes nothing, and dma_index_key's
  // x[rs1], which once named the indirect transfers' index dumps, is read by nothing now.
  std::string apply(const Host &host, const Insn &insn) {
    if (is(insn, VLANE_IDX)) vlane_idx(host, insn);
    else if (is(insn, PACK)) pack(host, insn);
    else if (is(insn, UNPACK_LO)) unpack(host, insn, 0);
    else if (is(insn, UNPACK_HI)) unpack(host, insn, insn.vl);
    else if (is(insn, DMA_CONFIG_DESC)) dma_.dma_config_desc(host, insn);
    else if (is(insn, MVOUT)) return dma_.mvout(host, insn);
    else if (transfer(insn)) return dma_.mvin(host, insn);
    return "";
  }

  // A float rounded to bf16, to nearest-even; a NaN stays a quiet NaN.
  static uint32_t bf16_of(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu)) return (u >> 16) | 0x40u;
    return (u + 0x7FFFu + ((u >> 16) & 1u)) >> 16;
  }
  // pack: f32 registers packed into bf16, two to a 32-bit word, as the v6e's VPU packs. vd's vl words are
  // vs2's vl elements then vs1's, element 2w in the low half of word w.
  static void pack(const Host &host, const Insn &insn) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      std::vector<float> both;
      for (uint32_t i = 0; i < insn.vl; i++) both.push_back(get<float>(host, lane, cp::rs2(insn), i));
      for (uint32_t i = 0; i < insn.vl; i++) both.push_back(get<float>(host, lane, cp::rs1(insn), i));
      for (uint32_t w = 0; w < insn.vl; w++)
        put<uint32_t>(host, lane, cp::rd(insn), w, bf16_of(both[2 * w]) | (bf16_of(both[2 * w + 1]) << 16));
    }
  }

  // A bf16 widened to f32, exactly: its bits are the f32's upper half.
  static float f32_of_bf16(uint32_t h) {
    const uint32_t u = (h & 0xFFFFu) << 16;
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
  }
  // unpack_lo / unpack_hi: the pack the other way. vs2's vl words hold 2 * vl bf16, 2w low and 2w + 1 high in word w;
  // vd's vl f32 are elements first .. first + vl - 1: 0 for unpack_lo, vl for unpack_hi.
  static void unpack(const Host &host, const Insn &insn, uint32_t first) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      std::vector<uint32_t> words(insn.vl);
      for (uint32_t w = 0; w < insn.vl; w++) words[w] = get<uint32_t>(host, lane, cp::rs2(insn), w);
      for (uint32_t i = 0; i < insn.vl; i++) {
        const uint32_t e = first + i;
        put<float>(host, lane, cp::rd(insn), i, f32_of_bf16(words[e / 2] >> (16 * (e & 1))));
      }
    }
  }

  // vlane_idx: every element of vd is the number of its lane, as 64 bits whatever the element width.
  static void vlane_idx(const Host &host, const Insn &insn) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++) put<int64_t>(host, lane, cp::rd(insn), i, lane);
  }

  static constexpr const char *WIDTH_KEY = "tpu_misc_issue_width";

  cp::Port issue_{*this, "issue", "instructions", 2, cp::Port::PRIMARY};
  Dma dma_;
};

}  // namespace tpu

#endif
