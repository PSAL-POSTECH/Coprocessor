// The cross-lane all-reduce sum unit as a cp component, for 128 lanes: sum takes vs2 from every lane, serializes it
// into rows (row i = element i of every lane) in an input buffer, sums each row across the lanes in a tree of 7
// levels (log2 128), broadcasts the row's sum to every lane and collects the rows in an output buffer; vd[i] of every
// lane is r[i]. Hands-on 1 uncomments its steps; answer/xlane_all_reduce.hpp has them uncommented.
#ifndef XLANE_ALL_REDUCE_HPP
#define XLANE_ALL_REDUCE_HPP

#include <deque>
#include <utility>
#include <vector>

#include "vcix_cp.hpp"

namespace xlane {

class XlaneAllReduce : public cp::Component {
 public:
  static constexpr uint32_t LANES = 128, LEVELS = 7;  // a tree of log2(128) = 7 levels

  // >>> Step 1: the encoding this unit owns, custom-2 sf.vc.v.iv with opcode 0 (sum)
  static constexpr cp::Encoding SUM{
      0x2000305B, 0xFE00707F, "all-reduce sum"};
  XlaneAllReduce(cp::Coprocessor &cp,
                 const char *name)
      : Component(cp, name) {
    register_insn(SUM);
  }
  // <<< Step 1

  void configure(const cp::Config &config) override {
    if (config.uint("vpu_num_lanes", LANES) != LANES)
      throw cp::ConfigError("vpu_num_lanes", config.get("vpu_num_lanes"),
                            "is not the 128 lanes this unit is built for");
    rows_ = config.uint("xlane_all_reduce_buffer_rows", 16);
    if (rows_ == 0) throw cp::ConfigError("xlane_all_reduce_buffer_rows", "0", "is not a buffer: at least 1");
    // >>> Step 2b: size the blocks: the buffers hold rows_ rows; the tree takes LEVELS cycles, the broadcast 1
    input_.configure(rows_);
    output_.configure(rows_);
    tree_.configure(LEVELS, 1);
    broadcast_.configure(1, 1);
    // <<< Step 2b
  }
  void reset() override { done_.clear(); }

  // Step 3: one cycle of the unit: drain from the exit first, then fill from the entry.
  void cycle() override {
    // >>> Step 3 (1/3): the output buffer: once every row of the head instruction is there, vd goes back to the lanes
    if (output_.length() &&
        output_.length() >=
            output_.front().insn.vl) {
      const cp::Insn in = output_.front().insn;
      for (uint32_t i = 0; i < in.vl; i++)
        output_.pop();
      values([&](const cp::Host &host) {
        reduce(host, in); });
      if (!functional_model())
        done_.push_back({in.id, now()});
      complete(in);
    }
    // <<< Step 3 (1/3)
    // >>> Step 3 (2/3): the broadcast network into the output buffer, the tree into the broadcast
    if (broadcast_.length() &&
        output_.push(
            std::move(broadcast_.front())))
      broadcast_.pop();
    if (tree_.length() &&
        broadcast_.push(std::move(tree_.front())))
      tree_.pop();
    // <<< Step 3 (2/3)
    // >>> Step 3 (3/3): the input buffer into the tree, then the next instruction
    if (input_.length() &&
        tree_.push(std::move(input_.front())))
      input_.pop();
    if (insns().length() &&
        input_.available() >=
            insns().front().vl) {
      const cp::Insn in = insns().pop();
      instructions_.add(1);
      for (uint32_t i = 0; i < in.vl; i++)
        (void)input_.push({in, i});
    }
    // <<< Step 3 (3/3)
  }

  // The cycle the instruction with this id completed, once, for the commit's trace line.
  cp::Cycle take_done(cp::Id id) {
    for (auto it = done_.begin(); it != done_.end(); ++it)
      if (it->first == id) {
        const cp::Cycle at = it->second;
        done_.erase(it);
        return at;
      }
    return 0;
  }

 private:
  struct Row {
    cp::Insn insn;
    uint32_t i;
  };

  // r[i] = vs2[i] of every lane, added in pairs a level at a time (an odd last one goes up), to vd[i] of every lane.
  static void reduce(const cp::Host &host, const cp::Insn &in) {
    std::vector<float> r(in.vl);
    for (uint32_t i = 0; i < in.vl; i++) {
      std::vector<float> level;
      for (uint32_t lane = 0; lane < host.lanes(); lane++) level.push_back(host.vreg<float>(lane, cp::rs2(in))[i]);
      while (level.size() > 1) {
        std::vector<float> up;
        for (size_t k = 0; k + 1 < level.size(); k += 2)
          up.push_back(level[k] + level[k + 1]);
        if (level.size() % 2) up.push_back(level.back());
        level = std::move(up);
      }
      r[i] = level[0];
    }
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < in.vl; i++) host.vreg_mut<float>(lane, cp::rd(in))[i] = r[i];
  }

  uint64_t rows_ = 16;
  // The VCIX interface, the input buffer, the adder tree (LEVELS deep), the broadcast network, the output buffer.
  // >>> Step 2a: the blocks, one per block of the unit
  cp::Counter instructions_{
      *this, "instructions", 1};
  cp::Queue<Row> input_{*this, "input"};
  cp::Pipeline<Row> tree_{*this, "tree"};
  cp::Pipeline<Row> broadcast_{
      *this, "broadcast"};
  cp::Queue<Row> output_{*this, "output"};
  // <<< Step 2a
  std::deque<std::pair<cp::Id, cp::Cycle>> done_;
};

}  // namespace xlane

#endif
