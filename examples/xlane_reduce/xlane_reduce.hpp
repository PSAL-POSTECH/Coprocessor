// The cross-lane reduction unit as a cp component: sum and max take vs2 from every lane, serialize it into rows (row i
// = element i of every lane) in an input buffer, reduce each row across the lanes in a tree of ceil(log2 lanes)
// levels, broadcast the row's result to every lane and collect the rows in an output buffer; vd[i] of every lane is r[i].
#ifndef XLANE_REDUCE_HPP
#define XLANE_REDUCE_HPP

#include <array>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "vcix_cp.hpp"

namespace xlane {

class XlaneReduce : public cp::Component {
 public:
  static constexpr uint32_t MAX_LEVELS = 7;  // up to 128 lanes

  // >>> Step 1: the encodings this unit owns, custom-2 sf.vc.v.iv with opcode 0 (sum) or 1 (max)
  static constexpr cp::Encoding SUM{0x2000305B, 0xFE00707F, "xlane sum"};
  static constexpr cp::Encoding MAX{0x2400305B, 0xFE00707F, "xlane max"};
  XlaneReduce(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    register_insn(SUM);
    register_insn(MAX);
  }
  // <<< Step 1

  void configure(const cp::Config &config) override {
    const uint64_t lanes = config.uint("vpu_num_lanes", 4);
    levels_ = 1;
    while ((uint64_t{1} << levels_) < lanes) levels_++;
    if (lanes == 0 || levels_ > MAX_LEVELS)
      throw cp::ConfigError("vpu_num_lanes", std::to_string(lanes), "is not 1 to 128 lanes");
    rows_ = config.uint("xlane_reduce_buffer_rows", 16);
    if (rows_ == 0) throw cp::ConfigError("xlane_reduce_buffer_rows", "0", "is not a buffer: at least 1");
    // >>> Step 2b: size the blocks: the buffers hold rows_ rows; each tree level and the broadcast take 1 cycle
    input_.configure(rows_);
    output_.configure(rows_);
    for (auto &level : tree_) level.configure(1, 1);
    broadcast_.configure(1, 1);
    // <<< Step 2b
  }
  void reset() override { done_.clear(); }

  void cycle() override {
    // >>> Step 3: one cycle of the unit: drain from the exit first, then fill from the entry
    // The output buffer: once every row of the head instruction is there, vd goes back to the lanes.
    if (output_.length() && output_.length() >= output_.front().insn.vl) {
      const cp::Insn in = output_.front().insn;
      for (uint32_t i = 0; i < in.vl; i++) output_.pop();
      values([&](const cp::Host &host) { reduce(host, in); });
      if (!functional_model()) done_.push_back({in.id, now()});
      complete(in);
    }
    // The broadcast network into the output buffer, the last level into the broadcast, each level up one.
    if (broadcast_.length() && !output_.full()) output_.push(broadcast_.pop());
    if (tree_[levels_ - 1].length() && broadcast_.available()) broadcast_.push(tree_[levels_ - 1].pop());
    for (uint32_t k = levels_ - 1; k-- > 0;)
      if (tree_[k].length() && tree_[k + 1].available()) tree_[k + 1].push(tree_[k].pop());
    // The input buffer into the tree, one row a cycle; the head instruction's vl rows into the input buffer.
    if (input_.length() && tree_[0].available()) tree_[0].push(input_.pop());
    if (insns().length() && (insns().front().vl > rows_ || input_.available() >= insns().front().vl)) {
      const cp::Insn in = insns().pop();
      vcix_.utilize(1);
      if (in.sew_bits != 32 || in.lmul_log2 > 0)
        fault(in, "xlane_reduce: only 32-bit elements in one register (LMUL <= 1) are supported");
      else if (in.vl > rows_)
        fault(in, "xlane_reduce: vl " + std::to_string(in.vl) + " is more rows than the input buffer holds");
      else
        for (uint32_t i = 0; i < in.vl; i++) input_.push({in, i});
    }
    // <<< Step 3
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

  // r[i] = vs2[i] of every lane, reduced in pairs a level at a time (an odd last one goes up), to vd[i] of every lane.
  static void reduce(const cp::Host &host, const cp::Insn &in) {
    const bool is_max = (in.bits & MAX.mask) == MAX.match;
    std::vector<float> r(in.vl);
    for (uint32_t i = 0; i < in.vl; i++) {
      std::vector<float> level;
      for (uint32_t lane = 0; lane < host.lanes(); lane++) level.push_back(host.vreg<float>(lane, cp::rs2(in))[i]);
      while (level.size() > 1) {
        std::vector<float> up;
        for (size_t k = 0; k + 1 < level.size(); k += 2)
          up.push_back(is_max ? (level[k + 1] > level[k] ? level[k + 1] : level[k]) : level[k] + level[k + 1]);
        if (level.size() % 2) up.push_back(level.back());
        level = std::move(up);
      }
      r[i] = level[0];
    }
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < in.vl; i++) host.vreg_mut<float>(lane, cp::rd(in))[i] = r[i];
  }

  uint32_t levels_ = 1;
  uint64_t rows_ = 16;
  // >>> Step 2a: the blocks, one per box of the picture
  cp::Port vcix_{*this, "vcix", "instructions", 1, cp::Port::PRIMARY};  // the VCIX interface
  cp::Queue<Row> input_{*this, "input", "rows"};                        // the input buffer
  std::array<cp::Pipeline<Row>, MAX_LEVELS> tree_{{{*this, "level1", "rows"},
                                                   {*this, "level2", "rows"},
                                                   {*this, "level3", "rows"},
                                                   {*this, "level4", "rows"},
                                                   {*this, "level5", "rows"},
                                                   {*this, "level6", "rows"},
                                                   {*this, "level7", "rows"}}};  // the tree's levels
  cp::Pipeline<Row> broadcast_{*this, "broadcast", "rows"};             // the broadcast network
  cp::Queue<Row> output_{*this, "output", "rows"};                      // the output buffer
  // <<< Step 2a
  std::deque<std::pair<cp::Id, cp::Cycle>> done_;
};

}  // namespace xlane

#endif
