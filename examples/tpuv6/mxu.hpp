// The tpuv6 model's matrix unit: `tpu_mxu_units` arrays, each twice as wide and twice as deep as the machine has
// lanes, so an operand is two registers. A vmatprep holds the upper half of the columns, each operand's and each
// array's apart, until the vmatpush (weights) or the vmatmul (inputs) that brings the lower half. A word of 32 bits
// holds one single or two rows of 16 bits, and a word a cycle enters an array: two rows of 16 bits a cycle, one
// single. A result leaves as singles, one half of the columns a vpop. The array an instruction is for is a field
// the encodings leave free: bits 11:7 of a vmatprep, vmatpush or vmatmul (sf.vc.iv's rd, which names no register)
// and bits 24:20 of a vpop (sf.vc.v.i's rs2, which names none); with one array both are 0.
#ifndef TPUV6_MXU_HPP
#define TPUV6_MXU_HPP

#include <cstdio>
#include <deque>
#include <vector>

#include "vcix_accel.hpp"

namespace tpuv6 {

class Mxu {
 public:
  using Config = vcix_accel::Config;
  using Cycle = vcix_accel::Cycle;
  using Encoding = vcix_accel::Encoding;
  using Id = vcix_accel::Id;
  using Insn = vcix_accel::Insn;

  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Mxu(const char *name)
      : entry_{name, "input", "rows", 2, vcix_accel::Port::PRIMARY},
        issue_{name, "issue", "instructions", MXU_SLOTS},
        pop_{name, "pop", "rows", 256},
        arrays_(1, Array(name)) {}

  static std::vector<Encoding> encodings() {
    return {{MATMUL, FORM, "vmatmul"}, {PUSH, FORM, "vmatpush"}, {PREP, FORM, "vmatprep"}, {POP, FORM, "vpop"}};
  }
  bool owns(const Insn &insn) const { return is(insn, MATMUL) || is(insn, PUSH) || is(insn, PREP) || is(insn, POP); }

  // The field at 19:15: [1:0] what a word holds (0 a single, 1 two halves, 3 two bfloat16); [2] which operand a
  // vmatprep is for (0 weights, 1 inputs), whether a vmatmul latches the staged weights, or which half of the columns
  // a vpop takes (0 lower, 1 upper); [4:3] which half of the depth a vmatpush fills (1 the lower, 2 the upper).
  static constexpr uint32_t FORMAT = 0x3, SECOND = 0x4, LATCH = 0x4, DEPTH = 0x18;
  static uint32_t rows_in_word(uint32_t field) { return (field & FORMAT) ? 2 : 1; }
  // Which array an instruction is for: 0 to tpu_mxu_units - 1 names one.
  static uint32_t array_of(const Insn &insn) {
    return is(insn, POP) ? vcix_accel::rs2(insn) : vcix_accel::rd(insn);
  }
  static constexpr uint64_t MAX_ARRAYS = 32;  // what a field of 5 bits names

  void configure(const Config &config) {
    const uint64_t lanes = config.uint(LANES_KEY, 128);
    const uint64_t entries = config.uint(QUEUE_KEY, 256);
    const uint64_t arrays = config.uint(ARRAYS_KEY, 1);
    if (lanes < 1 || lanes > MAX_LANES)
      throw vcix_accel::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 2 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size: a word of two rows needs 2");
    if (arrays < 1 || arrays > MAX_ARRAYS)
      throw vcix_accel::ConfigError(ARRAYS_KEY, config.get(ARRAYS_KEY),
                                    "is not a number of arrays an instruction's field of 5 bits can name: 1 to 32");
    // Width + depth - 1 of an array of 2 * lanes each way, unless the machine description says what was measured.
    const uint64_t slots = config.uint(DELAY_KEY, 4 * lanes - 1);
    if (slots < 1 || slots > UINT32_MAX)
      throw vcix_accel::ConfigError(DELAY_KEY, config.get(DELAY_KEY), "is not a delay in cycles");
    capacity_ = static_cast<uint32_t>(entries);
    entry_.set_capacity(2 * arrays);  // a word a cycle into each array
    pop_.set_capacity(entries);
    arrays_.assign(arrays, Array(entry_.unit()));
    for (Array &array : arrays_) array.line.assign(slots, 0);
    flush();
  }

  // A cycle takes MXU_SLOTS of vmatprep, vmatpush and vmatmul over all the arrays and, of each array, one of the
  // weights' (vmatprep of weights, vmatpush), one of the inputs' (vmatprep of inputs, vmatmul) and one vpop. One for
  // an array the machine does not have is never accepted; the functional face ends the run with the reason.
  bool can_accept(const Insn &insn, Cycle now) const {
    if (array_of(insn) >= arrays_.size()) return false;
    const Array &array = arrays_[array_of(insn)];
    if (!slot(array, insn).room(now)) return false;
    if (is(insn, POP)) return insn.vl <= out(array, insn) || drained(array);
    if (!issue_.room(now)) return false;
    if (is(insn, MATMUL)) return rows(insn) <= capacity_ - array.input;
    return true;
  }
  Cycle issue(const Insn &insn, Id, Cycle now) {
    Array &array = arrays_[array_of(insn)];
    slot(array, insn).admit(1, now);
    if (!is(insn, POP)) issue_.admit(1, now);
    if (is(insn, MATMUL) && rows(insn)) {
      array.input += rows(insn);
      array.queued.push_back({rows(insn), rows_in_word(vcix_accel::rs1(insn))});
    }
    if (is(insn, POP)) {
      uint32_t &left = upper(insn) ? array.out_upper : array.out_lower;
      const uint32_t taken = insn.vl < left ? insn.vl : left;
      left -= taken;
      pop_.admit(taken, now);
    }
    return 1;
  }
  void commit(const Insn &, Id, Cycle) {}
  // Each array's line moves unless what it would put out has no room in either half's queue; as it moves, the
  // array's input queue's next word enters, of as many rows as its vmatmul's format puts in a word. The arrays
  // share the entry port, which takes a word for each of them a cycle.
  void tick(Cycle now) {
    uint32_t in_line = 0;
    for (Array &array : arrays_) {
      step(array, now);
      in_line += array.in_line;
    }
    entry_.hold(in_line);
  }
  void reset() {
    entry_.reset();
    issue_.reset();
    for (Array &array : arrays_) {
      array.weight_issue.reset();
      array.input_issue.reset();
      array.pop_issue.reset();
    }
    pop_.reset();
    flush();
  }

  std::vector<const vcix_accel::Port *> ports() const {
    std::vector<const vcix_accel::Port *> all = {&entry_, &issue_};
    for (const Array &array : arrays_) all.insert(all.end(), {&array.weight_issue, &array.input_issue, &array.pop_issue});
    all.push_back(&pop_);
    return all;
  }

  uint32_t arrays() const { return static_cast<uint32_t>(arrays_.size()); }
  uint32_t input_entries(uint32_t array = 0) const { return arrays_[array].input; }
  uint32_t lower_entries(uint32_t array = 0) const { return arrays_[array].out_lower; }
  uint32_t upper_entries(uint32_t array = 0) const { return arrays_[array].out_upper; }
  // What the [tpu] trace adds to an issue: the array, when there is more than one, and its queues, when it has them.
  void trace_queues(const Insn &insn) const {
    const uint32_t array = array_of(insn);
    if (arrays() > 1) std::printf(", array %u", array);
    if (array < arrays())
      std::printf(", input queue %u, output queues %u and %u", input_entries(array), lower_entries(array),
                  upper_entries(array));
  }

 private:
  // funct6, vm and funct3 of custom-2: sf.vc.iv 0, 1 and 2, and sf.vc.v.i 2, as the tpu model's two arrays have them.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr uint32_t MATMUL = 0x2200305B;
  static constexpr uint32_t PUSH = 0x2600305B;
  static constexpr uint32_t PREP = 0x2A00305B;
  static constexpr uint32_t POP = 0x0800305B;
  static constexpr uint64_t MAX_LANES = 1u << 19;
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_mxu_queue_entries";
  static constexpr const char *DELAY_KEY = "tpu_mxu_delay_cycles";
  static constexpr const char *ARRAYS_KEY = "tpu_mxu_units";
  // The v6e bundle's MXU slots, which its two MXUs share: the capacity line of its static per-bundle utilization.
  static constexpr uint64_t MXU_SLOTS = 2;

  struct Queued {
    uint32_t rows, per_word;
  };
  // One array: its issue slots, one instruction a cycle each, its input queue, its delay line and its output queues.
  struct Array {
    explicit Array(const char *unit)
        : weight_issue{unit, "weight_issue", "instructions", 1},
          input_issue{unit, "input_issue", "instructions", 1},
          pop_issue{unit, "pop_issue", "instructions", 1} {}
    vcix_accel::Port weight_issue;  // a vmatprep of weights or a vmatpush
    vcix_accel::Port input_issue;   // a vmatprep of inputs or a vmatmul
    vcix_accel::Port pop_issue;     // a vpop
    uint32_t input = 0;        // rows pushed and not in the line
    std::deque<Queued> queued;  // per vmatmul in the input queue, oldest first: its rows left, and how many a word holds
    uint32_t out_lower = 0;    // rows out and not popped, of each half of the columns
    uint32_t out_upper = 0;
    uint32_t in_line = 0;
    uint32_t head = 0;
    std::vector<uint8_t> line = std::vector<uint8_t>(511, 0);  // rows in each slot of the delay
  };

  static bool is(const Insn &insn, uint32_t match) { return (insn.bits & FORM) == match; }
  static bool upper(const Insn &insn) { return (vcix_accel::rs1(insn) & SECOND) != 0; }
  static bool of_inputs(const Insn &insn) {
    return is(insn, MATMUL) || (is(insn, PREP) && (vcix_accel::rs1(insn) & SECOND));
  }
  static vcix_accel::Port &slot(Array &array, const Insn &insn) {
    return is(insn, POP) ? array.pop_issue : of_inputs(insn) ? array.input_issue : array.weight_issue;
  }
  static const vcix_accel::Port &slot(const Array &array, const Insn &insn) {
    return is(insn, POP) ? array.pop_issue : of_inputs(insn) ? array.input_issue : array.weight_issue;
  }
  static uint32_t rows(const Insn &insn) { return insn.vl * rows_in_word(vcix_accel::rs1(insn)); }
  static uint32_t out(const Array &array, const Insn &insn) { return upper(insn) ? array.out_upper : array.out_lower; }
  static bool drained(const Array &array) { return !array.input && !array.in_line; }
  void step(Array &array, Cycle now) {
    const uint32_t most = array.out_lower > array.out_upper ? array.out_lower : array.out_upper;
    if (array.line[array.head] > capacity_ - most) return;
    array.out_lower += array.line[array.head];
    array.out_upper += array.line[array.head];
    array.in_line -= array.line[array.head];
    const uint32_t word = array.queued.empty() ? 0 : array.queued.front().per_word;
    const uint32_t entering = word <= entry_.room(now) ? word : 0;
    if (entering) {
      entry_.admit(entering, now);
      if ((array.queued.front().rows -= entering) == 0) array.queued.pop_front();
    }
    array.line[array.head] = static_cast<uint8_t>(entering);
    array.input -= entering;
    array.in_line += entering;
    array.head = array.head + 1 == array.line.size() ? 0 : array.head + 1;
  }
  void flush() {
    for (Array &array : arrays_) {
      array.input = array.out_lower = array.out_upper = array.in_line = array.head = 0;
      array.queued.clear();
      array.line.assign(array.line.size(), 0);
    }
  }

  vcix_accel::Port entry_;
  vcix_accel::Port issue_;
  vcix_accel::Port pop_;
  uint32_t capacity_ = 256;
  std::vector<Array> arrays_;
};

}  // namespace tpuv6

#endif
