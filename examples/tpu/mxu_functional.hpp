// The functional face of a TPU model with a matrix unit: the functional core (functional_core.hpp) and the matrix
// unit (mxu.hpp) of the model's Shape.
#ifndef TPU_MXU_FUNCTIONAL_HPP
#define TPU_MXU_FUNCTIONAL_HPP

#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "vcix_accel.hpp"

#include "functional_core.hpp"

#include "mxu.hpp"

namespace tpu {

template <class Shape>
class MxuFunctional final : public FunctionalCore<MxuFunctional<Shape>> {
  using Core = FunctionalCore<MxuFunctional>;
  using Mxu = tpu::Mxu<Shape>;
  using typename Core::Config;
  using typename Core::Encoding;
  using typename Core::Host;
  using typename Core::Insn;
  using typename Core::Named;
  using typename Core::Queues;

 public:
  // What the core reads, how many arrays the matrix unit has, and how many registers an operand is.
  void configure(const Config &config) {
    Core::configure(config);
    mxu_.assign(config.uint(MXU_ARRAYS_KEY, 1), {});
    registers_ = Mxu::registers(config);
  }
  // Back to what configure left: the units hold nothing.
  void reset() {
    Core::reset();
    for (MxuArray &array : mxu_) array = {};
  }

 private:
  friend class FunctionalCore<MxuFunctional>;

  // The handlers of the matrix unit, and the unit whose encodings they have.
  static std::vector<Named> array_handlers() {
    return {
        {"vmatprep", &MxuFunctional::mxu_prep},
        {"vmatpush", &MxuFunctional::mxu_push},
        {"vmatmul", &MxuFunctional::mxu_matmul},
        {"vpop", &MxuFunctional::mxu_pop},
    };
  }
  static std::vector<std::vector<Encoding>> array_units() { return {Mxu::encodings()}; }

  // ---- The matrix unit.
  // Of the weights and of the inputs: whether a vmatprep's upper half waits to be taken, and per lane its rows.
  struct MxuHalf {
    bool held = false;
    std::vector<std::vector<float>> rows;
  };
  // One array of the matrix unit: its held halves; per column, of registers_ x lanes, its weight at each depth as the
  // pushes left them and as the last latch took them, each empty until the first push or latch; and per column
  // what it computed.
  struct MxuArray {
    MxuHalf held[2];
    std::vector<std::vector<float>> staged, active;
    Queues output;
  };
  enum MxuOperand : uint32_t { MXU_WEIGHTS, MXU_INPUTS };
  static constexpr const char *mxu_operand[] = {"weights", "inputs"}, *mxu_taker[] = {"vmatpush", "vmatmul"};
  static float mxu_element(uint32_t word, uint32_t slot, uint32_t format) {
    if (format == 0) {
      float value;
      std::memcpy(&value, &word, sizeof value);
      return value;
    }
    const uint16_t part = static_cast<uint16_t>(word >> (16 * slot));
    return format == 3 ? bf16_to_float(part) : half_to_float(part);
  }
  // The rows of one register group, per lane.
  static std::vector<std::vector<float>> mxu_rows(const Host &host, const Insn &insn, const char *what) {
    if (insn.sew_bits != 32) fail("%s: its elements are words of 32 bits, not of %u", what, insn.sew_bits);
    const uint32_t format = vcix_accel::rs1(insn) & Mxu::FORMAT, pack = Mxu::rows_in_word(format);
    if (format == 2) fail("%s: format 2 names nothing a word holds", what);
    std::vector<std::vector<float>> rows(host.lanes(), std::vector<float>(uint64_t{insn.vl} * pack));
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++)
        for (uint32_t slot = 0; slot < pack; slot++)
          rows[lane][i * pack + slot] =
              mxu_element(Core::template get<uint32_t>(host, lane, vcix_accel::rs2(insn), i), slot, format);
    return rows;
  }
  MxuArray &mxu_array(const Insn &insn, const char *what) {
    const uint32_t which = Mxu::array_of(insn);
    if (which >= mxu_.size())
      fail("%s: array %u, and the machine has %zu (tpu_mxu_units)", what, which, mxu_.size());
    return mxu_[which];
  }
  void mxu_size(const Host &host, MxuArray &array) const {
    if (array.output.size() == registers_ * host.lanes()) return;
    array.output.assign(registers_ * host.lanes(), {});
  }
  // A vmatprep holds the upper half of its operand until the next vmatpush or vmatmul of that operand takes it; what
  // comes between, the other operand's included, leaves it. A second before the first is taken ends the run.
  void mxu_prep(const Host &host, const Insn &insn) {
    const MxuOperand operand = vcix_accel::rs1(insn) & Mxu::SECOND ? MXU_INPUTS : MXU_WEIGHTS;
    MxuArray &array = mxu_array(insn, "vmatprep");
    if (registers_ == 1) fail("vmatprep: an operand of an array of lanes x lanes is one register, with no upper half");
    std::vector<std::vector<float>> rows = mxu_rows(host, insn, "vmatprep");
    MxuHalf &held = array.held[operand];
    if (held.held)
      fail("vmatprep: the upper half of the %s a vmatprep held before it was taken by no %s", mxu_operand[operand],
           mxu_taker[operand]);
    held = {true, std::move(rows)};
  }
  // The upper half a vmatprep of this operand holds, which must be of as many rows.
  static std::vector<std::vector<float>> mxu_take(MxuArray &array, const char *what, MxuOperand operand, size_t rows) {
    MxuHalf &held = array.held[operand];
    if (!held.held) fail("%s: no vmatprep of its upper half came before it", what);
    if (held.rows[0].size() != rows) fail("%s: %zu rows, and the vmatprep before it held %zu", what, rows, held.rows[0].size());
    held.held = false;
    return std::move(held.rows);
  }
  // A vmatpush stages a weight per row in every column in one half of the depth ([4:3]): what it held moves down
  // as many depths, past its last is gone, and the rows take its top in their order.
  void mxu_push(const Host &host, const Insn &insn) {
    MxuArray &array = mxu_array(insn, "vmatpush");
    mxu_size(host, array);
    const std::vector<std::vector<float>> lower = mxu_rows(host, insn, "vmatpush");
    const uint32_t half = (vcix_accel::rs1(insn) & Mxu::DEPTH) >> 3;
    if (half == 0) fail("vmatpush: field %u names no half of the depth: [4:3] is 1 or 2", vcix_accel::rs1(insn));
    if (half == 3) fail("vmatpush: [4:3] 3 is the v6e's vmatpush3, which is not modelled");
    if (half == 2 && registers_ == 1)
      fail("vmatpush: [4:3] 2 names a second half of the depth, which an array of lanes x lanes has not");
    const std::vector<std::vector<float>> upper =
        registers_ > 1 ? mxu_take(array, "vmatpush", MXU_WEIGHTS, lower[0].size()) : std::vector<std::vector<float>>{};
    const uint32_t lanes = host.lanes(), width = registers_ * lanes;
    const size_t rows = lower[0].size(), top = half == 1 ? 0 : lanes;
    if (array.staged.empty()) array.staged.assign(width, std::vector<float>(width));
    for (uint32_t column = 0; column < width; column++) {
      float *weight = &array.staged[column][top];
      for (size_t depth = lanes; depth-- > rows;) weight[depth] = weight[depth - rows];
      for (size_t row = 0; row < rows && row < lanes; row++)
        weight[row] = column < lanes ? lower[column][row] : upper[column - lanes][row];
    }
  }
  // A vmatmul computes at once: a row is one input vector of registers_ x lanes, and a column's output is its
  // active weights times it. One that latches first makes the staged weights the active.
  void mxu_matmul(const Host &host, const Insn &insn) {
    MxuArray &array = mxu_array(insn, "vmatmul");
    mxu_size(host, array);
    const std::vector<std::vector<float>> lower = mxu_rows(host, insn, "vmatmul");
    const std::vector<std::vector<float>> upper =
        registers_ > 1 ? mxu_take(array, "vmatmul", MXU_INPUTS, lower[0].size()) : std::vector<std::vector<float>>{};
    if (vcix_accel::rs1(insn) & Mxu::LATCH) {
      if (array.staged.empty()) fail("vmatmul: no weight was pushed before it");
      array.active = array.staged;
    }
    if (array.active.empty()) fail("vmatmul: no weights were latched before it, and it does not latch");
    const uint32_t lanes = host.lanes(), width = registers_ * lanes;
    std::vector<float> input(width);
    for (size_t row = 0; row < lower[0].size(); row++) {
      for (uint32_t k = 0; k < width; k++) input[k] = k < lanes ? lower[k][row] : upper[k - lanes][row];
      for (uint32_t column = 0; column < width; column++) {
        float output = 0;
        for (uint32_t k = 0; k < width; k++) output += input[k] * array.active[column][k];
        array.output[column].push_back(output);
      }
    }
  }
  // A vpop takes vl rows of one half of the columns, as singles.
  void mxu_pop(const Host &host, const Insn &insn) {
    MxuArray &array = mxu_array(insn, "vpop");
    mxu_size(host, array);
    if (insn.sew_bits != 32 || (vcix_accel::rs1(insn) & Mxu::FORMAT))
      fail("vpop: a result is a single, and format %u of %u bits was asked", vcix_accel::rs1(insn) & Mxu::FORMAT, insn.sew_bits);
    const uint32_t lanes = host.lanes(), first = vcix_accel::rs1(insn) & Mxu::SECOND ? lanes : 0;
    if (first && registers_ == 1)
      fail("vpop: [2] names the upper half of the columns, which an array of lanes x lanes has not");
    if (array.output[first].size() < insn.vl)
      fail("vpop: %u rows asked of the %s half, %zu computed", insn.vl, first ? "upper" : "lower", array.output[first].size());
    for (uint32_t lane = 0; lane < lanes; lane++)
      for (uint32_t i = 0; i < insn.vl; i++) {
        Core::template put<float>(host, lane, vcix_accel::rd(insn), i, array.output[first + lane].front());
        array.output[first + lane].pop_front();
      }
  }

  static constexpr const char *MXU_ARRAYS_KEY = "tpu_mxu_units";

  std::vector<MxuArray> mxu_ = std::vector<MxuArray>(1);  // tpu_mxu_units of them
  uint32_t registers_ = Shape::REGISTERS;
};

}  // namespace tpu

#endif
