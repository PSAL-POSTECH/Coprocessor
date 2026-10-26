// A TPU model's matrix unit: `tpu_mxu_units` arrays of tpu_mxu_registers x lanes each way (lanes x lanes under
// tpu_mxu_bound_to_lanes: 1), an operand that many registers; tpu_mxu_issue_slots and tpu_mxu_word_a_cycle set its
// issue slots and its rows a cycle. A key the machine leaves out is the model's Shape.
// Each array: an input queue -> a line of tpu_mxu_delay_cycles -> one output queue per half of its columns.
#ifndef TPU_MXU_HPP
#define TPU_MXU_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "machine.hpp"
#include "values.hpp"

namespace tpu {

template <class Shape>
class Mxu : public Component {
  static_assert(Shape::REGISTERS == 1 || Shape::REGISTERS == 2, "an operand is one register, or an upper and a lower half");
  using Config = cp::Config;
  using Cycle = cp::Cycle;
  using Host = cp::Host;
  using Insn = cp::Insn;
  using Counter = cp::Counter;
  using Port = cp::Port;
  using Row = std::vector<float>;
  using Rows = std::vector<Row>;

 public:
  // `name` is the component as its statistics report it; the model that holds the component says it.
  Mxu(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    for (const cp::Encoding &e : {MATMUL, PUSH, PREP, POP}) register_insn(e);
  }

  // The field at 19:15: [1:0] what a word holds (0 a single, 1 two halves, 3 two bfloat16); [2] which operand a
  // vmatprep is for (0 weights, 1 inputs), whether a vmatmul latches the staged weights, or which half of the columns
  // a vpop takes (0 lower, 1 upper); [4:3] which half of the depth a vmatpush fills (1 the lower, 2 the upper).
  static constexpr uint32_t FORMAT = 0x3, SECOND = 0x4, LATCH = 0x4, DEPTH = 0x18;
  static uint32_t rows_in_word(uint32_t field) { return (field & FORMAT) ? 2 : 1; }
  static constexpr uint64_t MAX_ARRAYS = 32;

  // The first tpu_mxu_units arrays are made here, their blocks the component's; an array's line defaults to its
  // width + depth - 1 stages. insns() takes as many a cycle as the slots and pops can carry out, twice that deep.
  void configure(const Config &config) override {
    const uint64_t lanes = config.uint(LANES_KEY, 128), entries = config.uint(QUEUE_KEY, 256), arrays = config.uint(ARRAYS_KEY, 1);
    if (lanes < 1)
      throw cp::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < 2 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size: a word of two rows needs 2");
    if (arrays < 1 || arrays > MAX_ARRAYS)
      throw cp::ConfigError(ARRAYS_KEY, config.get(ARRAYS_KEY),
                                    "is not a number of arrays an instruction's field of 5 bits can name: 1 to 32");
    const uint64_t bound = config.uint(BOUND_KEY, 0);
    if (bound > 1)
      throw cp::ConfigError(BOUND_KEY, config.get(BOUND_KEY), "is not 0 (the model's array) or 1 (an array of lanes x lanes)");
    const uint64_t registers = config.uint(REGISTERS_KEY, bound ? 1 : Shape::REGISTERS);
    if (registers < 1 || registers > 2)
      throw cp::ConfigError(REGISTERS_KEY, config.get(REGISTERS_KEY), "is not 1 or 2: an operand is one register, or an upper and a lower half");
    if (bound && registers != 1)
      throw cp::ConfigError(REGISTERS_KEY, config.get(REGISTERS_KEY), "is not 1, and tpu_mxu_bound_to_lanes: 1 makes the array lanes x lanes");
    const uint64_t slots = config.uint(SLOTS_KEY, Shape::SLOTS), word = config.uint(WORD_KEY, Shape::WORD_A_CYCLE);
    if (slots < 1 || slots > UINT32_MAX)
      throw cp::ConfigError(SLOTS_KEY, config.get(SLOTS_KEY), "is not a number of instructions a cycle");
    if (word > 1)
      throw cp::ConfigError(WORD_KEY, config.get(WORD_KEY), "is not 0 (a row a cycle) or 1 (a word of two rows a cycle)");
    registers_ = static_cast<uint32_t>(registers), word_a_cycle_ = word;
    const uint64_t stages = config.uint(DELAY_KEY, 2 * registers_ * lanes - 1);
    if (stages < 1 || stages > UINT32_MAX)
      throw cp::ConfigError(DELAY_KEY, config.get(DELAY_KEY), "is not a delay in cycles");
    insns().configure(2 * (slots + arrays), slots + arrays);
    entry_.set_width((word_a_cycle_ ? 2 : 1) * arrays);
    rows_.set_capacity(entry_.width());
    issue_.set_width(slots);
    pop_.set_width(entries);
    units_ = arrays;
    for (size_t i = 0; i < units_; i++) {
      if (!arrays_[i]) arrays_[i].emplace(*this, i, registers_);
      arrays_[i]->configure(stages, entries);
    }
  }
  void reset() override {
    for (size_t i = 0; i < units_; i++) arrays_[i]->state = {}, arrays_[i]->values = {}, arrays_[i]->in_line = 0;
  }
  // Each array's line hands its finished word on and takes its next; then insns() in order, each carried out whole
  // (or faulted). active_cycles counts the cycles some line holds a row.
  void cycle() override {
    uint64_t in_line = 0;
    for (size_t i = 0; i < units_; i++) in_line += arrays_[i]->flow(entry_, rows_);
    while (insns().length() && carry_out(insns().front())) insns().pop();
    rows_in_flight_.add(in_line);
    active_cycles_.add(in_line != 0);
  }

  // What the [tpu] trace adds to an issue: the array when there are several, and its queues.
  void trace_queues(const vcix_insn &insn) const override {
    const uint32_t which = is_pop(insn) ? cp::rs2(insn) : cp::rd(insn);
    if (units_ > 1) std::printf(", array %u", which);
    if (which >= units_) return;
    const Array &a = *arrays_[which];
    std::printf(", input queue %u", static_cast<uint32_t>(a.input.size()));
    if (a.upper)
      std::printf(", output queues %u and %u", static_cast<uint32_t>(a.lower.size()), static_cast<uint32_t>(a.upper->size()));
    else
      std::printf(", output queue %u", static_cast<uint32_t>(a.lower.size()));
  }

  // funct6, vm and funct3 of custom-2: sf.vc.iv 0, 1 and 2, and sf.vc.v.i 2, as the tpu model's two arrays have them.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr cp::Encoding MATMUL{0x2200305B, FORM, "vmatmul"};
  static constexpr cp::Encoding PUSH{0x2600305B, FORM, "vmatpush"};
  static constexpr cp::Encoding PREP{0x2A00305B, FORM, "vmatprep"};
  static constexpr cp::Encoding POP{0x0800305B, FORM, "vpop"};

 private:
  enum Operand : uint32_t { WEIGHTS, INPUTS };
  // One array's state: whether each operand's upper half is held, and of how many rows; whether a push has staged
  // weights, and a latch made them active.
  struct State {
    bool held[2] = {false, false};
    uint64_t rows[2] = {0, 0};
    bool pushed = false, latched = false;
  };
  // One array's values (Spike): the upper half each operand's vmatprep holds; per column, of registers x lanes, its
  // weight at each depth as the pushes left them (staged) and as the last latch took them (active).
  struct Values {
    Rows upper[2];
    Rows staged, active;
  };
  // A row in an input queue: its outputs (Spike), and on a word's first row the rows that enter the line with it.
  struct Input {
    uint32_t word;
    Row out;
  };
  // An entry of a line: a word's rows, entered and left together.
  struct Word {
    uint32_t rows;
    Row out[2];
  };
  // One array: its issue slots, one instruction a cycle each; its queues and line, named array<i>_<block>; its state,
  // its values, and the rows in its line.
  struct Array {
    Array(Mxu &mxu, size_t i, uint32_t halves)
        : weight_issue{mxu, "weight_issue", 1},
          input_issue{mxu, "input_issue", 1},
          pop_issue{mxu, "pop_issue", 1},
          input{mxu, named(i, "input")},
          line{mxu, named(i, "line")},
          lower{mxu, named(i, halves > 1 ? "lower" : "output")} {
      if (halves > 1) upper.emplace(mxu, named(i, "upper"));
    }
    void configure(uint64_t stages, uint64_t entries) {
      input.configure(entries);
      line.configure(stages, 1);
      lower.configure(entries);
      if (upper) upper->configure(entries);
    }
    cp::Queue<Row> &out(uint32_t half) { return half ? *upper : lower; }
    const cp::Queue<Row> &out(uint32_t half) const { return half ? *upper : lower; }
    // The finished word into every output queue, if each has room for all its rows; then the next word in, if the
    // shared input port has room for its rows, counted in `rows_in`. Returns the rows in the line.
    uint64_t flow(Port &entry, Counter &rows_in) {
      if (line.length()) {
        const uint32_t rows = line.front().rows;
        if (lower.available() >= rows && (!upper || upper->available() >= rows)) {
          Word w = line.pop();
          for (uint32_t k = 0; k < rows; k++)
            if ((upper && !upper->push(w.out[k])) || !lower.push(std::move(w.out[k]))) std::abort();
          in_line -= rows;
        }
      }
      if (input.length() && line.available() && entry.utilize(input.front().word)) {
        Word w{input.front().word, {}};
        for (uint32_t k = 0; k < w.rows; k++) w.out[k] = input.pop().out;
        rows_in.add(w.rows);
        in_line += w.rows;
        if (!line.push(std::move(w))) std::abort();
      }
      return in_line;
    }
    static const char *named(size_t i, const char *block) { return cp::intern("array" + std::to_string(i) + "_" + block); }

    Port weight_issue, input_issue, pop_issue;
    cp::Queue<Input> input;
    cp::Pipeline<Word> line;
    cp::Queue<Row> lower;
    std::optional<cp::Queue<Row>> upper;
    State state;
    Values values;
    uint64_t in_line = 0;
  };

  static bool is(const vcix_insn &insn, const cp::Encoding &e) { return (insn.bits & e.mask) == e.match; }
  static bool is_pop(const vcix_insn &insn) { return is(insn, POP); }
  // A failed check, in `why`.
  template <class... A>
  static bool no(std::string &why, const char *format, A... a) {
    why = message(format, a...);
    return false;
  }
  // Whether the instruction is ever carried out, its checks (of its fields, then of its array's state) in their order;
  // the first that fails is `why`. A vmatprep while its operand's half is held fails with no reason: it waits.
  bool carried(const Insn &insn, std::string &why) const {
    const uint32_t field = cp::rs1(insn), sew = insn.sew_bits, format = field & FORMAT;
    const uint32_t which = is_pop(insn) ? cp::rs2(insn) : cp::rd(insn);
    const bool prep = is(insn, PREP), push = is(insn, PUSH);
    const char *what = is_pop(insn) ? "vpop" : prep ? "vmatprep" : push ? "vmatpush" : "vmatmul";
    if (which >= units_) return no(why, "%s: array %u, and the machine has %zu (tpu_mxu_units)", what, which, units_);
    if (is_pop(insn)) {
      if (sew != 32 || format) return no(why, "vpop: a result is a single, and format %u of %u bits was asked", format, sew);
      return !(field & SECOND) || registers_ > 1 ||
             no(why, "vpop: [2] names the upper half of the columns, which an array of lanes x lanes has not");
    }
    const State &s = arrays_[which]->state;
    const Operand operand = !prep && !push ? INPUTS : prep && (field & SECOND) ? INPUTS : WEIGHTS;
    if (prep && s.held[operand]) return false;
    if (prep && registers_ < 2)
      return no(why, "vmatprep: an operand of an array of lanes x lanes is one register, with no upper half");
    if (sew != 32) return no(why, "%s: its elements are words of 32 bits, not of %u", what, sew);
    if (format == 2) return no(why, "%s: format 2 names nothing a word holds", what);
    if (prep) return true;
    const uint32_t half = (field & DEPTH) >> 3;
    if (push && half == 0) return no(why, "vmatpush: field %u names no half of the depth: [4:3] is 1 or 2", field);
    if (push && half == 3) return no(why, "vmatpush: [4:3] 3 is the v6e's vmatpush3, which is not modelled");
    if (push && half == 2 && registers_ < 2)
      return no(why, "vmatpush: [4:3] 2 names a second half of the depth, which an array of lanes x lanes has not");
    const uint64_t rows = uint64_t{insn.vl} * rows_in_word(field);
    if (registers_ > 1 && !s.held[operand]) return no(why, "%s: no vmatprep of its upper half came before it", what);
    if (registers_ > 1 && s.rows[operand] != rows)
      return no(why, "%s: %zu rows, and the vmatprep before it held %zu", what, static_cast<size_t>(rows),
                static_cast<size_t>(s.rows[operand]));
    if (push) return true;
    if (field & LATCH) return s.pushed || no(why, "vmatmul: no weight was pushed before it");
    return s.latched || no(why, "vmatmul: no weights were latched before it, and it does not latch");
  }
  // The head of insns(), carried out whole in this cycle when its slots, its rows and its room allow: a vpop takes
  // its rows of one output queue through the pop port, a vmatmul puts its rows in the input queue, both at once. True
  // once it completed, or faulted: a failed check, or a vpop asking more rows than its idle array and queue hold.
  bool carry_out(const Insn &insn) {
    std::string why;
    if (!carried(insn, why)) return why.empty() ? false : (fault(insn, why), true);
    const uint32_t field = cp::rs1(insn), vl = insn.vl;
    Array &a = *arrays_[is_pop(insn) ? cp::rs2(insn) : cp::rd(insn)];
    if (is_pop(insn)) {
      const uint32_t half = field & SECOND ? 1 : 0;
      cp::Queue<Row> &out = a.out(half);
      if (out.size() < vl && !a.input.size() && a.line.idle())
        return fault(insn, message("vpop: %u rows asked of the %s half, %zu computed", vl, half ? "upper" : "lower",
                                   static_cast<size_t>(out.size()))),
               true;
      if (out.length() < vl || pop_.available() < vl || !a.pop_issue.utilize(1) || !pop_.utilize(vl)) return false;
      for (uint32_t i = 0; i < vl; i++) {
        const Row row = out.pop();
        values([&](const Host &host) {
          for (uint32_t lane = 0, first = half ? host.lanes() : 0; lane < host.lanes(); lane++)
            put<float>(host, lane, cp::rd(insn), i, row[first + lane]);
        });
      }
      complete(insn);
      return true;
    }
    const bool weights = is(insn, PUSH) || (is(insn, PREP) && !(field & SECOND));
    Port &slot = weights ? a.weight_issue : a.input_issue;
    const uint64_t rows = uint64_t{vl} * rows_in_word(field);
    if (!issue_.available() || (is(insn, MATMUL) && a.input.available() < rows) || !slot.utilize(1) || !issue_.utilize(1))
      return false;
    if (is(insn, PREP)) prep(insn, a, weights ? WEIGHTS : INPUTS, rows);
    else if (is(insn, PUSH)) push(insn, a);
    else matmul(insn, a, rows);
    complete(insn);
    return true;
  }
  // vmatprep holds the upper half of its operand until the next vmatpush or vmatmul of that operand takes it.
  void prep(const Insn &insn, Array &a, Operand operand, uint64_t rows) {
    a.state.held[operand] = true, a.state.rows[operand] = rows;
    values([&](const Host &host) { a.values.upper[operand] = operand_rows(host, insn); });
  }
  // vmatpush stages a weight per row in every column in one half of the depth: what it held moves down as many
  // depths, past its last is gone, and the rows take its top in their order. Nothing enters the array.
  void push(const Insn &insn, Array &a) {
    const uint32_t half = (cp::rs1(insn) & DEPTH) >> 3;
    values([&](const Host &host) {
      const Rows lower = operand_rows(host, insn);
      Values &v = a.values;
      const Rows upper = registers_ > 1 ? v.upper[WEIGHTS] : Rows{};
      const uint32_t lanes = host.lanes(), width = registers_ * lanes;
      const size_t rows = lower[0].size(), top = half == 1 ? 0 : lanes;
      if (v.staged.empty()) v.staged.assign(width, Row(width));
      for (uint32_t column = 0; column < width; column++) {
        float *weight = &v.staged[column][top];
        for (size_t depth = lanes; depth-- > rows;) weight[depth] = weight[depth - rows];
        for (size_t row = 0; row < rows && row < lanes; row++) weight[row] = column < lanes ? lower[column][row] : upper[column - lanes][row];
      }
    });
    a.state.held[WEIGHTS] = false, a.state.pushed = true;
  }
  // vmatmul: one that latches first makes the staged weights the active; then its vl words of 1 or 2 rows enter the
  // input queue, each row's outputs made as it enters (output()), a word one entry under tpu_mxu_word_a_cycle.
  void matmul(const Insn &insn, Array &a, uint64_t rows) {
    const uint32_t field = cp::rs1(insn), word = word_a_cycle_ ? rows_in_word(field) : 1;
    const bool latch = field & LATCH;
    values([&](const Host &) {
      if (latch) a.values.active = a.values.staged;
    });
    for (uint64_t row = 0; row < rows; row++) {
      Input in{row % word ? 0 : word, {}};
      values([&](const Host &host) { in.out = output(host, insn, a, row); });
      if (!a.input.push(std::move(in))) std::abort();
    }
    a.state.held[INPUTS] = false, a.state.latched |= latch;
  }
  static float word_element(uint32_t word, uint32_t slot, uint32_t format) {
    if (format == 0) {
      float value;
      std::memcpy(&value, &word, sizeof value);
      return value;
    }
    const uint16_t part = static_cast<uint16_t>(word >> (16 * slot));
    return format == 3 ? bf16_to_float(part) : half_to_float(part);
  }
  // The rows of one register group, per lane.
  static Rows operand_rows(const Host &host, const Insn &insn) {
    const uint32_t format = cp::rs1(insn) & FORMAT, pack = rows_in_word(format);
    Rows rows(host.lanes(), Row(uint64_t{insn.vl} * pack));
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++)
        for (uint32_t slot = 0; slot < pack; slot++)
          rows[lane][i * pack + slot] = word_element(get<uint32_t>(host, lane, cp::rs2(insn), i), slot, format);
    return rows;
  }
  // Row `row` of a vmatmul's input, as it enters: a column's output is its active weights times the row, one input
  // vector of registers x lanes, the lower lanes from vs2 and the upper from the vmatprep the vmatmul took.
  Row output(const Host &host, const Insn &insn, const Array &array, uint64_t row) const {
    const Values &v = array.values;
    const uint32_t lanes = host.lanes(), width = registers_ * lanes, format = cp::rs1(insn) & FORMAT;
    const uint32_t pack = rows_in_word(format), word = static_cast<uint32_t>(row / pack), slot = static_cast<uint32_t>(row % pack);
    Row input(width), out(width);
    for (uint32_t k = 0; k < width; k++)
      input[k] = k < lanes ? word_element(get<uint32_t>(host, k, cp::rs2(insn), word), slot, format) : v.upper[INPUTS][k - lanes][row];
    for (uint32_t column = 0; column < width; column++) {
      float sum = 0;
      for (uint32_t k = 0; k < width; k++) sum = multiply_add(sum, input[k], v.active[column][k]);
      out[column] = sum;
    }
    return out;
  }

  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *QUEUE_KEY = "tpu_mxu_queue_entries";
  static constexpr const char *DELAY_KEY = "tpu_mxu_delay_cycles";
  static constexpr const char *ARRAYS_KEY = "tpu_mxu_units";
  static constexpr const char *BOUND_KEY = "tpu_mxu_bound_to_lanes";
  static constexpr const char *REGISTERS_KEY = "tpu_mxu_registers";
  static constexpr const char *SLOTS_KEY = "tpu_mxu_issue_slots";
  static constexpr const char *WORD_KEY = "tpu_mxu_word_a_cycle";

  Port entry_{*this, "input", Shape::WORD_A_CYCLE ? 2 : 1};
  Counter rows_{*this, "rows", Shape::WORD_A_CYCLE ? 2 : 1};
  Counter rows_in_flight_{*this, "rows_in_flight"};
  Counter active_cycles_{*this, "active_cycles"};
  Port issue_{*this, "issue", Shape::SLOTS};
  std::optional<Array> arrays_[MAX_ARRAYS];
  Port pop_{*this, "pop", 256};
  uint32_t registers_ = Shape::REGISTERS;
  bool word_a_cycle_ = Shape::WORD_A_CYCLE;
  size_t units_ = 1;
};

}

#endif
