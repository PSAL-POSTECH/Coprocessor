// The tpu_mxu model's matrix unit: an array twice as wide and twice as deep as the machine has lanes, so an operand
// is two registers. A vmatprep holds the upper half of the columns until the vmatpush (weights) or the vmatmul
// (inputs) that brings the lower half. A word of 32 bits holds one single or two rows of 16 bits, and a cell has
// two accumulators: two rows a cycle enter the array. A result leaves as singles, one half of the columns a vpop.
#ifndef TPU_MXU_HPP
#define TPU_MXU_HPP

#include <vector>

#include "vcix_accel.hpp"

namespace tpu {

class Mxu {
 public:
  using Config = vcix_accel::Config;
  using Cycle = vcix_accel::Cycle;
  using Encoding = vcix_accel::Encoding;
  using Id = vcix_accel::Id;
  using Insn = vcix_accel::Insn;

  static std::vector<Encoding> encodings() {
    return {{MATMUL, FORM, "vmatmul"}, {PUSH, FORM, "vmatpush"}, {PREP, FORM, "vmatprep"}, {POP, FORM, "vpop"}};
  }
  bool owns(const Insn &insn) const { return is(insn, MATMUL) || is(insn, PUSH) || is(insn, PREP) || is(insn, POP); }

  // The field at 19:15: [1:0] what a word holds (0 a single, 1 two halves, 3 two bfloat16), and [2] which
  // operand a vmatprep is for (0 weights, 1 inputs) or which half of the columns a vpop takes (0 lower, 1 upper).
  static constexpr uint32_t FORMAT = 0x3, SECOND = 0x4;
  static uint32_t rows_in_word(uint32_t field) { return (field & FORMAT) ? 2 : 1; }

  void configure(const Config &config) {
    const uint64_t lanes = config.uint(LANES_KEY, 128);
    const uint64_t entries = config.uint(QUEUE_KEY, 256);
    if (lanes < 1 || lanes > MAX_LANES)
      throw vcix_accel::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    // Width + depth - 1 of an array of 2 * lanes each way, unless the machine description says what was measured.
    const uint64_t slots = config.uint(DELAY_KEY, 4 * lanes - 1);
    if (slots < 1 || slots > UINT32_MAX)
      throw vcix_accel::ConfigError(DELAY_KEY, config.get(DELAY_KEY), "is not a delay in cycles");
    const uint64_t rate = config.uint(RATE_KEY, 2);
    if (rate < 1 || rate > 255 || rate > entries)
      throw vcix_accel::ConfigError(RATE_KEY, config.get(RATE_KEY), "is not a number of rows a cycle the queues can hold");
    capacity_ = static_cast<uint32_t>(entries);
    pop_.set_capacity(entries);
    entry_.set_capacity(rate);
    line_.assign(slots, 0);
    head_ = 0;
    flush();
  }

  bool can_accept(const Insn &insn, Cycle now) const {
    if (!issue_.room(now)) return false;
    if (is(insn, MATMUL)) return rows(insn) <= capacity_ - input_;
    if (is(insn, PUSH)) return weight_push_.room(now) != 0;
    if (is(insn, POP)) return insn.vl <= out(insn) || drained();
    return true;
  }
  Cycle issue(const Insn &insn, Id, Cycle now) {
    issue_.admit(1, now);
    if (is(insn, MATMUL)) input_ += rows(insn);
    if (is(insn, PUSH)) weight_push_.admit(1, now);
    if (is(insn, POP)) {
      uint32_t &left = upper(insn) ? out_upper_ : out_lower_;
      const uint32_t taken = insn.vl < left ? insn.vl : left;
      left -= taken;
      pop_.admit(taken, now);
    }
    return 1;
  }
  void commit(const Insn &, Id, Cycle) {}
  // The line moves unless what it would put out has no room in either half's queue.
  void tick(Cycle now) {
    const uint32_t most = out_lower_ > out_upper_ ? out_lower_ : out_upper_;
    if (line_[head_] <= capacity_ - most) {
      out_lower_ += line_[head_];
      out_upper_ += line_[head_];
      in_line_ -= line_[head_];
      const uint64_t room = entry_.room(now);
      const uint32_t entering = input_ < room ? input_ : static_cast<uint32_t>(room);
      if (entering) entry_.admit(entering, now);
      line_[head_] = static_cast<uint8_t>(entering);
      input_ -= entering;
      in_line_ += entering;
      head_ = head_ + 1 == line_.size() ? 0 : head_ + 1;
    }
    entry_.hold(in_line_);
  }
  void reset() {
    entry_.reset();
    issue_.reset();
    weight_push_.reset();
    pop_.reset();
    flush();
    head_ = 0;
  }

  std::vector<const vcix_accel::Port *> ports() const { return {&entry_, &issue_, &weight_push_, &pop_}; }

  uint32_t input_entries() const { return input_; }
  uint32_t lower_entries() const { return out_lower_; }
  uint32_t upper_entries() const { return out_upper_; }

 private:
  // funct6, vm and funct3 of custom-2: sf.vc.iv 0, 1 and 2, and sf.vc.v.i 2. The tpu model's two arrays have these.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr uint32_t MATMUL = 0x2200305B;
  static constexpr uint32_t PUSH = 0x2600305B;
  static constexpr uint32_t PREP = 0x2A00305B;
  static constexpr uint32_t POP = 0x0800305B;
  static constexpr uint64_t MAX_LANES = 1u << 19;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_systolic_queue_entries";
  static constexpr const char *DELAY_KEY = "tpu_mxu_delay_cycles";
  static constexpr const char *RATE_KEY = "tpu_mxu_rows_per_cycle";

  static bool is(const Insn &insn, uint32_t match) { return (insn.bits & FORM) == match; }
  static bool upper(const Insn &insn) { return (vcix_accel::rs1(insn) & SECOND) != 0; }
  static uint32_t rows(const Insn &insn) { return insn.vl * rows_in_word(vcix_accel::rs1(insn)); }
  uint32_t out(const Insn &insn) const { return upper(insn) ? out_upper_ : out_lower_; }
  bool drained() const { return !input_ && !in_line_; }
  void flush() {
    input_ = out_lower_ = out_upper_ = in_line_ = 0;
    line_.assign(line_.size(), 0);
  }

  vcix_accel::Port entry_{"Mxu", "input", "rows", 2, vcix_accel::Port::PRIMARY};
  vcix_accel::Port issue_{"Mxu", "issue", "instructions", 1};
  vcix_accel::Port weight_push_{"Mxu", "weight_push", "instructions", 1};
  vcix_accel::Port pop_{"Mxu", "pop", "rows", 256};
  uint32_t capacity_ = 256;
  uint32_t input_ = 0;       // rows pushed and not in the line
  uint32_t out_lower_ = 0;   // rows out and not popped, of each half of the columns
  uint32_t out_upper_ = 0;
  uint32_t in_line_ = 0;
  uint32_t head_ = 0;
  std::vector<uint8_t> line_ = std::vector<uint8_t>(511, 0);  // rows in each slot of the delay
};

}  // namespace tpu

#endif
