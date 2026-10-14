// The functional face of the tpu model, libtpu.so: the functional core (functional_core.hpp) and the model's two
// arrays, the systolic array and the multi-precision array, as the Spike these units came from did them.
#ifndef TPU_FUNCTIONAL_HPP
#define TPU_FUNCTIONAL_HPP

#include <cstring>
#include <deque>
#include <vector>

#include "vcix_accel.hpp"

#include "functional_core.hpp"
#include "msa.hpp"
#include "systolic.hpp"

namespace tpu {

class Functional final : public FunctionalCore<Functional> {
 public:
  // Back to what configure left: the units hold nothing.
  void reset() {
    FunctionalCore::reset();
    sa_input_.clear();
    sa_weight_input_.clear();
    sa_output_.clear();
    sa_weight_.clear();
    sa_ready_ = 0;
    msa_weight_.clear();
    msa_output_.clear();
    msa_format_ = 0;
    msa_shift_ = 0;
  }

 private:
  friend class FunctionalCore<Functional>;

  // The handlers of the two arrays, and the units whose encodings they have.
  static std::vector<Named> array_handlers() {
    return {
        {"systolic input push", &Functional::systolic_input_push},
        {"systolic weight push", &Functional::systolic_weight_push},
        {"systolic pop", &Functional::systolic_pop},
        {"msa push", &Functional::msa_push},
        {"msa pop", &Functional::msa_pop},
    };
  }
  static std::vector<std::vector<Encoding>> array_units() { return {Systolic::encodings(), Msa::encodings()}; }

  // ---- The systolic array: a matrix of weights times each pushed input vector, a float per lane.
  // What an element is rides the push or the pop in its rs1 field, one table at every width: 0 the width's own
  // (an integer of 8 bits, a half, a single; 64 bits are zeros), 1 an E4M3, 2 an E5M2, 3 a bfloat16.
  // A code its width cannot hold ends the run.
  enum class Element { INTEGER, E4M3, E5M2, HALF, BF16, SINGLE, ZERO };
  static Element systolic_format(const Insn &insn, uint32_t sew) {
    const uint32_t code = vcix_accel::rs1(insn);
    if (code == 0) return sew == 8 ? Element::INTEGER : sew == 16 ? Element::HALF : sew == 32 ? Element::SINGLE : Element::ZERO;
    if (sew == 8 && (code == 1 || code == 2)) return code == 1 ? Element::E4M3 : Element::E5M2;
    if (sew == 16 && code == 3) return Element::BF16;
    fail("systolic: format %u names no element of %u bits", code, sew);
  }
  static float systolic_element(const Host &host, const Insn &insn, uint32_t lane, uint32_t reg, uint32_t i) {
    switch (systolic_format(insn, insn.sew_bits)) {
      case Element::INTEGER: return static_cast<float>(get<int8_t>(host, lane, reg, i));
      case Element::E4M3: return fp8_to_float(get<uint8_t>(host, lane, reg, i), E4M3);
      case Element::E5M2: return fp8_to_float(get<uint8_t>(host, lane, reg, i), E5M2);
      case Element::HALF: return half_to_float(get<uint16_t>(host, lane, reg, i));
      case Element::BF16: return bf16_to_float(get<uint16_t>(host, lane, reg, i));
      case Element::SINGLE: return get<float>(host, lane, reg, i);
      case Element::ZERO: return 0.0f;
    }
    return 0.0f;
  }
  void systolic_size(const Host &host) {
    if (sa_input_.size() == host.lanes()) return;
    sa_input_.assign(host.lanes(), {});
    sa_weight_input_.assign(host.lanes(), {});
    sa_output_.assign(host.lanes(), {});
  }
  // A weight push adds a column per pushed element; a row keeps its last `lanes` weights.
  void systolic_weight_push(const Host &host, const Insn &insn) {
    systolic_size(host);
    const uint32_t lanes = host.lanes();
    for (uint32_t lane = 0; lane < lanes; lane++)
      for (uint32_t i = 0; i < insn.vl; i++)
        sa_weight_input_[lane].push_back(systolic_element(host, insn, lane, vcix_accel::rs2(insn), i));
    if (sa_weight_.empty()) sa_weight_.assign(lanes, {});
    for (uint32_t lane = 0; lane < lanes; lane++)
      for (uint32_t i = 0; i < insn.vl; i++) {
        if (sa_weight_[lane].size() == lanes) sa_weight_[lane].pop_front();
        sa_weight_[lane].push_back(sa_weight_input_[lane].front());
        sa_weight_input_[lane].pop_front();
      }
  }
  // An input push computes at once: element i of every lane is one input vector, and lane j's output is its weights times it.
  void systolic_input_push(const Host &host, const Insn &insn) {
    systolic_size(host);
    const uint32_t lanes = host.lanes();
    for (uint32_t lane = 0; lane < lanes; lane++)
      for (uint32_t i = 0; i < insn.vl; i++)
        sa_input_[lane].push_back(systolic_element(host, insn, lane, vcix_accel::rs2(insn), i));
    if (sa_weight_.empty() || sa_weight_[0].empty()) fail("systolic input push: no weight was pushed before it");
    std::vector<float> input(lanes);
    for (uint32_t i = 0; i < insn.vl; i++) {
      for (uint32_t lane = 0; lane < lanes; lane++) {
        input[lane] = sa_input_[lane].front();
        sa_input_[lane].pop_front();
      }
      for (uint32_t lane = 0; lane < lanes; lane++) {
        float output = 0;
        uint32_t k = 0;
        for (float weight : sa_weight_[lane]) output += input[k++] * weight;
        sa_output_[lane].push_back(output);
      }
    }
    sa_ready_ += insn.vl;
  }
  // A pop reads rs1 as a push does, and a code its width cannot hold ends the run. A pop to fewer bits rounds
  // by frm; a pop of 64 bits writes singles; a bfloat16 result is not modeled.
  void systolic_pop(const Host &host, const Insn &insn) {
    systolic_size(host);
    const uint64_t mode = host.csr(CSR_FRM);
    const Element format = systolic_format(insn, insn.sew_bits);
    if (format == Element::BF16) fail("systolic pop: a pop to bfloat16 is not modeled");
    if (sa_ready_ < insn.vl)
      fail("systolic pop: %u elements asked, %llu computed", insn.vl, static_cast<unsigned long long>(sa_ready_));
    const uint32_t vd = vcix_accel::rd(insn);
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++) {
        if (sa_output_[lane].empty()) break;
        const float value = sa_output_[lane].front();
        sa_output_[lane].pop_front();
        switch (format) {
          case Element::INTEGER: put<int8_t>(host, lane, vd, i, static_cast<int8_t>(value)); break;
          case Element::E4M3: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(narrow(value, E4M3, mode))); break;
          case Element::E5M2: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(narrow(value, E5M2, mode))); break;
          case Element::HALF: put<uint16_t>(host, lane, vd, i, static_cast<uint16_t>(narrow(value, HALF, mode))); break;
          default: put<float>(host, lane, vd, i, value); break;
        }
      }
    sa_ready_ -= insn.vl;
  }
  // ---- The multi-precision array (riscv-isa-sim branch spike-multi-precision-sa, ea9ec10): the systolic
  // array on words of 32 bits that hold 1 single, 2 halves or 4 floats of 8 bits, so a column has
  // lanes x that many weights. The field at 19:15 of a push: [4] weights or inputs, [3:2] how many columns
  // compute (all of them shifted right by it), [1:0] what a word holds.
  enum MsaFormat : uint32_t { MSA_SINGLE = 0, MSA_HALF = 1, MSA_E4M3 = 2, MSA_E5M2 = 3 };
  static uint32_t msa_pack(uint32_t format) { return format == MSA_SINGLE ? 1 : format == MSA_HALF ? 2 : 4; }
  // Element `slot` of a word, from its low bits up.
  static float msa_element(uint32_t word, uint32_t slot, uint32_t format) {
    const uint32_t bits = 32 / msa_pack(format), part = word >> (slot * bits);
    switch (format) {
      case MSA_SINGLE: {
        float value;
        std::memcpy(&value, &word, sizeof value);
        return value;
      }
      case MSA_HALF: return half_to_float(static_cast<uint16_t>(part));
      default: return fp8_to_float(static_cast<uint8_t>(part), format == MSA_E5M2 ? E5M2 : E4M3);
    }
  }
  void msa_size(const Host &host) {
    if (msa_weight_.size() == host.lanes()) return;
    msa_weight_.assign(host.lanes(), {});
    msa_output_.assign(host.lanes(), {});
  }
  void msa_push(const Host &host, const Insn &insn) {
    msa_size(host);
    if (insn.sew_bits != 32) fail("msa push: its elements are words of 32 bits, not of %u", insn.sew_bits);
    const uint32_t lanes = host.lanes(), field = vcix_accel::rs1(insn), vs = vcix_accel::rs2(insn);
    const uint32_t format = field & 3, pack = msa_pack(format);
    if (field & 0x10) {
      // A weight push says the format and the width; changing either empties the matrix.
      const uint32_t shift = (field >> 2) & 3;
      if (lanes >> shift == 0) fail("msa push: no column is left of %u when shifted by %u", lanes, shift);
      if (format != msa_format_ || shift != msa_shift_)
        for (std::deque<float> &column : msa_weight_) column.clear();
      msa_format_ = format;
      msa_shift_ = shift;
      for (uint32_t lane = 0; lane < lanes; lane++)
        for (uint32_t i = 0; i < insn.vl; i++)
          for (uint32_t slot = 0; slot < pack; slot++) {
            if (msa_weight_[lane].size() == lanes * pack) msa_weight_[lane].pop_front();
            msa_weight_[lane].push_back(msa_element(get<uint32_t>(host, lane, vs, i), slot, format));
          }
      return;
    }
    if (pack != msa_pack(msa_format_))
      fail("msa push: inputs of %u to a word, weights of %u", pack, msa_pack(msa_format_));
    // Element i of every lane is one input row; a column past the width computes 0.
    std::vector<float> row(lanes * pack);
    for (uint32_t i = 0; i < insn.vl; i++) {
      for (uint32_t lane = 0; lane < lanes; lane++)
        for (uint32_t slot = 0; slot < pack; slot++)
          row[lane * pack + slot] = msa_element(get<uint32_t>(host, lane, vs, i), slot, format);
      for (uint32_t lane = 0; lane < lanes; lane++) {
        float output = 0;
        if (lane < lanes >> msa_shift_) {
          uint32_t k = 0;
          for (float weight : msa_weight_[lane]) output += row[k++] * weight;
        }
        msa_output_[lane].push_back(output);
      }
    }
  }
  // A pop takes what is there and no more. To fewer bits it rounds to nearest even; 8 bits are E5M2 when the
  // field's [1:0] says so and E4M3 otherwise.
  void msa_pop(const Host &host, const Insn &insn) {
    msa_size(host);
    const uint32_t vd = vcix_accel::rd(insn);
    const Narrow &byte = (vcix_accel::rs1(insn) & 3) == MSA_E5M2 ? E5M2 : E4M3;
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++) {
        if (msa_output_[lane].empty()) break;
        const float value = msa_output_[lane].front();
        msa_output_[lane].pop_front();
        switch (insn.sew_bits) {
          case 8: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(narrow(value, byte, 0))); break;
          case 16: put<uint16_t>(host, lane, vd, i, static_cast<uint16_t>(narrow(value, HALF, 0))); break;
          default: put<float>(host, lane, vd, i, value); break;
        }
      }
  }

  static constexpr uint32_t CSR_FRM = 0x002;

  Queues sa_input_, sa_weight_input_, sa_output_;
  Queues sa_weight_;  // per lane, its row of the matrix; empty until the first weight push
  uint64_t sa_ready_ = 0;

  Queues msa_weight_, msa_output_;  // per lane: its column of the matrix, and what it computed
  uint32_t msa_format_ = 0, msa_shift_ = 0;  // as the last weight push said
};

}  // namespace tpu

#endif
