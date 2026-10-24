// A TPU's instructions with no timing of their own: one cycle each, tpu_misc_issue_width of them a cycle. Their values:
// the lane number, the bf16 pack and unpack, written as the instruction is done, and the DMA (dma.hpp), an untimed side
// path whose time TOGSim counts, carried out as it issues.
#ifndef TPU_MISC_HPP
#define TPU_MISC_HPP

#include <cstring>
#include <vector>

#include "vcix_cp.hpp"

#include "dma.hpp"
#include "values.hpp"

namespace tpu {

class Misc : public cp::Component {
  using Host = cp::Host;
  using Insn = cp::Insn;
  using Values = void (*)(const Host &, const Insn &);
  using DmaValues = void (Dma::*)(const Host &, const Insn &);

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
    handle(VLANE_IDX, &Misc::one<&Misc::vlane_idx>);
    handle(COMPUTE, &Misc::one<&Misc::nothing>);
    handle(MVIN2, &Misc::transfer<&Dma::mvin, MVIN>);
    handle(MVIN, &Misc::transfer<&Dma::mvin, MVIN>);
    handle(MVOUT, &Misc::transfer<&Dma::mvout, MVOUT>);
    handle(DMA_CONFIG_DESC, &Misc::dma<&Dma::dma_config_desc>);
    handle(DMA_INDEX_KEY, &Misc::dma<&Dma::dma_index_key>);
    handle(MVIN3, &Misc::transfer<&Dma::mvin, MVIN>);
    handle(PACK, &Misc::one<&Misc::pack>);
    handle(UNPACK_LO, &Misc::one<&Misc::unpack_lo>);
    handle(UNPACK_HI, &Misc::one<&Misc::unpack_hi>);
  }
  void configure(const cp::Config &config) override {
    const uint64_t width = config.uint(WIDTH_KEY, 2);
    if (width == 0) throw vcix_accel::ConfigError(WIDTH_KEY, "0", "is not an issue width: at least 1");
    issue_.set_capacity(width);
    Dma configured;
    configured.configure(config);
    dma_.configure(configured);
  }

 private:
  // A slot of the issue width, done the next cycle, when V writes its values.
  template <Values V>
  void one(cp::Instr &in) {
    in.use(issue_);
    in.on_done([&in](const Host &host) { V(host, in.insn()); });
  }
  // The same slot; the DMA's V on its descriptor and counts, as it issues.
  template <DmaValues V>
  void dma(cp::Instr &in) {
    in.use(issue_);
    in.on_issue([&](const Host &host) { (dma_.write().*V)(host, in.insn()); });
  }
  // The same, for a transfer, E's: never accepted on a machine with no scratchpad.
  template <DmaValues V, const cp::Encoding &E>
  void transfer(cp::Instr &in) {
    in.require(dma_.get().scratchpad(),
               [] { fail("%s: the machine description has no %s", E.name, Dma::SPAD_LANE_KEY); });
    dma<V>(in);
  }

  static void nothing(const Host &, const Insn &) {}

  // pack: f32 registers packed into bf16, two to a 32-bit word, as the v6e's VPU packs. vd's vl words are
  // vs2's vl elements then vs1's, element 2w in the low half of word w, rounded to nearest-even.
  static uint32_t bf16_of(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu)) return (u >> 16) | 0x40u;
    return (u + 0x7FFFu + ((u >> 16) & 1u)) >> 16;
  }
  static void pack(const Host &host, const Insn &insn) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      std::vector<float> both;
      for (uint32_t i = 0; i < insn.vl; i++) both.push_back(get<float>(host, lane, vcix_accel::rs2(insn), i));
      for (uint32_t i = 0; i < insn.vl; i++) both.push_back(get<float>(host, lane, vcix_accel::rs1(insn), i));
      for (uint32_t w = 0; w < insn.vl; w++)
        put<uint32_t>(host, lane, vcix_accel::rd(insn), w, bf16_of(both[2 * w]) | (bf16_of(both[2 * w + 1]) << 16));
    }
  }

  // unpack_lo / unpack_hi: the pack the other way. vs2's vl 32-bit words hold 2 * vl bf16, element 2w in the
  // low half of word w and 2w + 1 in the high half; unpack_lo writes vd's vl f32 from elements 0 .. vl - 1,
  // unpack_hi from vl .. 2 * vl - 1. A bf16 widens exactly (its bits are the f32's upper half).
  static float f32_of_bf16(uint32_t h) {
    const uint32_t u = (h & 0xFFFFu) << 16;
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
  }
  static void unpack(const Host &host, const Insn &insn, uint32_t first) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      std::vector<uint32_t> words(insn.vl);
      for (uint32_t w = 0; w < insn.vl; w++) words[w] = get<uint32_t>(host, lane, vcix_accel::rs2(insn), w);
      for (uint32_t i = 0; i < insn.vl; i++) {
        const uint32_t e = first + i;
        put<float>(host, lane, vcix_accel::rd(insn), i, f32_of_bf16(words[e / 2] >> (16 * (e & 1))));
      }
    }
  }
  static void unpack_lo(const Host &host, const Insn &insn) { unpack(host, insn, 0); }
  static void unpack_hi(const Host &host, const Insn &insn) { unpack(host, insn, insn.vl); }

  // vlane_idx: every element of vd is the number of its lane, as 64 bits whatever the element width.
  static void vlane_idx(const Host &host, const Insn &insn) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++) put<int64_t>(host, lane, vcix_accel::rd(insn), i, lane);
  }

  static constexpr const char *WIDTH_KEY = "tpu_misc_issue_width";

  cp::Port issue_{*this, "issue", "instructions", 2, cp::Port::PRIMARY};
  cp::Register<Dma> dma_{*this};
};

}

#endif
