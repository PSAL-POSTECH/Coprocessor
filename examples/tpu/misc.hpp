// A TPU's instructions with no timing of their own: one cycle each, tpu_misc_issue_width of them a cycle. Their values:
// the lane number, the bf16 pack and unpack, and the DMA (dma.hpp), an untimed side path whose time TOGSim counts.
#ifndef TPU_MISC_HPP
#define TPU_MISC_HPP

#include <cstring>
#include <vector>

#include "vcix_uarch.hpp"

#include "dma.hpp"
#include "values.hpp"

namespace tpu {

class Misc {
  using Host = vcix_uarch::Host;
  using Insn = vcix_uarch::Insn;
  using Route = vcix_uarch::Route;
  using Values = void (*)(const Host &, const Insn &);
  using DmaValues = void (Dma::*)(const Host &, const Insn &);

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Misc(const char *name) : issue_{name, "issue", "instructions", 2, vcix_uarch::PRIMARY} {}
  void configure(const vcix_uarch::Config &config) {
    const uint64_t width = config.uint(WIDTH_KEY, 2);
    if (width == 0) throw vcix_accel::ConfigError(WIDTH_KEY, "0", "is not an issue width: at least 1");
    issue_.set_capacity(width);
    Dma configured;
    configured.configure(config);
    dma_.configure(configured);
  }
  template <class H>
  void hardware(H &&h) {
    h.port(issue_);
    h.reg(dma_);
  }

  // A slot of the issue width, done the next cycle; the values of V.
  template <Values V>
  void one(Route &r) {
    r.use(issue_).compute([&](const Host &host) { V(host, r.insn()); });
  }
  // The same slot; the DMA's V on its descriptor and counts.
  template <DmaValues V>
  void dma(Route &r) {
    r.use(issue_).compute([&](const Host &host) { (dma_.write().*V)(host, r.insn()); });
  }

 private:
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

  vcix_uarch::Port issue_;
  vcix_uarch::Register<Dma> dma_;

 public:
  // custom-2: lane number, compute. custom-1: DMA. pack is custom-2 sf.vc.v.vv funct6 10 (vd's vl words from vs2's vl
  // elements then vs1's, each rounded to bf16, two to a word); unpack_lo / unpack_hi sf.vc.v.iv funct6 10 with the
  // field at 19:15 8 / 9 (vd's vl f32 from the first or the last vl of vs2's 2 * vl bf16).
  static constexpr uint32_t FUNCTION = 0xFE00707F, FIELD = 0xFE0FF07F;
  static constexpr vcix_uarch::Instruction<Misc> INSTRUCTIONS[] = {
      {0x0000305B, FUNCTION, "vlane_idx", &Misc::one<&Misc::vlane_idx>},
      {0x0600305B, FUNCTION, "compute", &Misc::one<&Misc::nothing>},
      {0x0200302B, FUNCTION, "mvin2", &Misc::dma<&Dma::mvin>},
      {0x0400302B, FUNCTION, "mvin", &Misc::dma<&Dma::mvin>},
      {0x0600302B, FUNCTION, "mvout", &Misc::dma<&Dma::mvout>},
      {0x0E00302B, FUNCTION, "dma_config_desc", &Misc::dma<&Dma::dma_config_desc>},
      {0x1000302B, FUNCTION, "dma_index_key", &Misc::dma<&Dma::dma_index_key>},
      {0x1C00302B, FUNCTION, "mvin3", &Misc::dma<&Dma::mvin>},
      {0x2800005B, FUNCTION, "pack", &Misc::one<&Misc::pack>},
      {0x2804305B, FIELD, "unpack_lo", &Misc::one<&Misc::unpack_lo>},
      {0x2804B05B, FIELD, "unpack_hi", &Misc::one<&Misc::unpack_hi>},
  };
};

}  // namespace tpu

#endif
