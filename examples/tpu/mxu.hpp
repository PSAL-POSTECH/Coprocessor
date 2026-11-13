// A TPU model's matrix unit: `tpu_mxu_units` arrays of tpu_mxu_registers x lanes each way (lanes x lanes under
// tpu_mxu_bound_to_lanes: 1), an operand that many registers; tpu_mxu_issue_slots, tpu_mxu_word_a_cycle and
// tpu_mxu_int8_macs_per_pe set its issue slots and its rows a cycle. A key the machine leaves out is the model's Shape.
// Each array: an input queue -> a line of tpu_mxu_delay_cycles -> one output queue per half of its columns.
#ifndef TPU_MXU_HPP
#define TPU_MXU_HPP

#include <algorithm>
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

  // The field at 19:15: [1:0] what a word holds (0 a single, 1 four int8, 2 four 8-bit floats, 3 two bfloat16), and
  // what a vpop writes (0 singles, 1 int32); [2] which operand a vmatprep is for (0 weights, 1 inputs), whether a
  // vmatmul latches the staged weights, or which half of the columns a vpop takes (0 lower, 1 upper); [4:3] which half
  // of the depth a vmatpush fills (1 lower, 2 upper).
  static constexpr uint32_t FORMAT = 0x3, SECOND = 0x4, LATCH = 0x4, DEPTH = 0x18;
  // A vpop's [3]: each row is its array's plus the next array's, as the v6e's vpop.add of mxu0 and mxu1.
  static constexpr uint32_t ADD = 0x8;
  // Format 1: four int8 a word, summed as int32, which a vpop of format 1 writes.
  static constexpr uint32_t INT8 = 1;
  // sf.vc.iv's rd: [3:0] the array, [4] the 8-bit float of format 2 is E5M2 (else E4M3).
  static constexpr uint32_t ARRAY = 0xF, E5M2_RD = 0x10;
  // Rows that enter the line together: a word of four rows at most.
  static constexpr uint32_t LINE_ROWS = 4;
  static uint32_t rows_in_word(uint32_t field) { return (field & FORMAT) == 3 ? 2 : (field & FORMAT) ? 4 : 1; }
  static uint32_t array_of(const vcix_insn &insn) { return is_pop(insn) ? cp::rs2(insn) : cp::rd(insn) & ARRAY; }
  static constexpr uint64_t MAX_ARRAYS = 16;

  // The first tpu_mxu_units arrays are made here, their blocks the component's; an array's line defaults to its
  // width + depth - 1 stages. insns() takes as many a cycle as the slots and pops can carry out, twice that deep.
  void configure(const Config &config) override {
    const uint64_t lanes = config.uint(LANES_KEY, 128), entries = config.uint(QUEUE_KEY, 256), arrays = config.uint(ARRAYS_KEY, 1);
    if (lanes < 1)
      throw cp::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the array can have");
    if (entries < LINE_ROWS || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size: a word of four rows needs 4");
    if (arrays < 1 || arrays > MAX_ARRAYS)
      throw cp::ConfigError(ARRAYS_KEY, config.get(ARRAYS_KEY),
                                    "is not a number of arrays the instruction's rd[3:0] can name: 1 to 16");
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
      throw cp::ConfigError(WORD_KEY, config.get(WORD_KEY), "is not 0 (a row a cycle) or 1 (a word a cycle)");
    const uint64_t macs = config.uint(INT8_MACS_KEY, Shape::INT8_MACS);
    if (macs < 1 || macs > 2)
      throw cp::ConfigError(INT8_MACS_KEY, config.get(INT8_MACS_KEY),
                            "is not 1 or 2: an int8 row a cycle is the work of one or two 16-bit rows");
    registers_ = static_cast<uint32_t>(registers), word_a_cycle_ = word, int8_macs_ = static_cast<uint32_t>(macs);
    const uint64_t stages = config.uint(DELAY_KEY, 2 * registers_ * lanes - 1);
    if (stages < 1 || stages > UINT32_MAX)
      throw cp::ConfigError(DELAY_KEY, config.get(DELAY_KEY), "is not a delay in cycles");
    insns().configure(2 * (slots + arrays), slots + arrays);
    uint32_t widest = 1;
    for (uint32_t format = 0; format <= FORMAT; format++) widest = std::max(widest, rows_a_cycle(format));
    entry_.set_width(widest * arrays);
    rows_.set_capacity(counted_rows(word_a_cycle_) * arrays);
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
    for (size_t i = 0; i < units_; i++) in_line += arrays_[i]->flow(entry_, rows_, counted_rows(word_a_cycle_));
    while (insns().length() && carry_out(insns().front())) insns().pop();
    rows_in_flight_.add(in_line);
    active_cycles_.add(in_line != 0);
  }

  // What the [tpu] trace adds to an issue: the array when there are several, and its queues.
  void trace_queues(const vcix_insn &insn) const override {
    const uint32_t which = array_of(insn);
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
  // One array's state: whether each operand's upper half is held, of how many rows and of which kind; whether a push
  // has staged weights, and a latch made them active; and whether the staged and the active weights are int8.
  struct State {
    bool held[2] = {false, false};
    uint64_t rows[2] = {0, 0};
    uint32_t kind[2] = {0, 0};
    bool pushed = false, latched = false;
    bool staged_int8 = false, active_int8 = false;
  };
  // One array's values (Spike): the upper half each operand's vmatprep holds; per column, of registers x lanes, its
  // weight at each depth as the pushes left them (staged) and as the last latch took them (active).
  struct Values {
    Rows upper[2];
    Rows staged, active;
  };
  // A row's outputs: whether its inputs were int8, and (Spike) each column's 32 bits, a single's or an int32's.
  struct Out {
    bool int8 = false;
    std::vector<uint32_t> bits;
  };
  // A row in an input queue: its outputs, and on the first row of an entry the rows that enter the line with it.
  struct Input {
    uint32_t word;
    Out out;
  };
  // An entry of a line: rows entered and left together.
  struct Word {
    uint32_t rows;
    Out out[LINE_ROWS];
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
    cp::Queue<Out> &out(uint32_t half) { return half ? *upper : lower; }
    const cp::Queue<Out> &out(uint32_t half) const { return half ? *upper : lower; }
    // The finished word into every output queue, if each has room for all its rows; then the next word in, if the
    // shared input port has room for its rows, counted in `rows_in` up to `counted` an entry. Returns the rows in the
    // line.
    uint64_t flow(Port &entry, Counter &rows_in, uint32_t counted) {
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
        rows_in.add(std::min(w.rows, counted));
        in_line += w.rows;
        if (!line.push(std::move(w))) std::abort();
      }
      return in_line;
    }
    static const char *named(size_t i, const char *block) { return cp::intern("array" + std::to_string(i) + "_" + block); }

    Port weight_issue, input_issue, pop_issue;
    cp::Queue<Input> input;
    cp::Pipeline<Word> line;
    cp::Queue<Out> lower;
    std::optional<cp::Queue<Out>> upper;
    State state;
    Values values;
    uint64_t in_line = 0;
  };

  static bool is(const vcix_insn &insn, const cp::Encoding &e) { return (insn.bits & e.mask) == e.match; }
  static bool is_pop(const vcix_insn &insn) { return is(insn, POP); }
  static bool e5m2(const vcix_insn &insn) { return !is_pop(insn) && (cp::rd(insn) & E5M2_RD); }
  // What an operand's words hold: the format, and E5M2 apart from E4M3 (4 + 2); its name.
  static uint32_t kind(const vcix_insn &insn) { return (cp::rs1(insn) & FORMAT) | (e5m2(insn) ? 4 : 0); }
  static const char *kind_name(uint32_t kind) {
    static const char *const names[] = {"singles", "int8", "E4M3", "bfloat16", "", "", "E5M2", ""};
    return names[kind & 7];
  }
  // Rows of `format` that enter the line together, one entry a cycle: a word under tpu_mxu_word_a_cycle, else a row;
  // int8's tpu_mxu_int8_macs_per_pe times a 16-bit format's.
  uint32_t rows_a_cycle(uint32_t format) const {
    if (format == INT8) return (word_a_cycle_ ? 2 : 1) * int8_macs_;
    return word_a_cycle_ ? rows_in_word(format) : 1;
  }
  // Two results of one column added as a vpop of [3] adds them: int32 as an int32 accumulator wraps, singles as floats.
  static uint32_t sum(uint32_t a, uint32_t b, bool int8) {
    if (int8) return a + b;
    float x, y;
    std::memcpy(&x, &a, sizeof x), std::memcpy(&y, &b, sizeof y);
    const float z = x + y;
    uint32_t bits;
    std::memcpy(&bits, &z, sizeof bits);
    return bits;
  }
  // The rows counter's rows an entry at most, and each array's capacity: a 16-bit word's rows under
  // tpu_mxu_word_a_cycle, else one. Rows over capacity is then the cycles a line took entries, whatever the format,
  // a word of four 8-bit rows counted as two.
  static constexpr uint32_t counted_rows(bool word_a_cycle) { return word_a_cycle ? 2 : 1; }
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
    const uint32_t which = array_of(insn);
    const bool prep = is(insn, PREP), push = is(insn, PUSH);
    const char *what = is_pop(insn) ? "vpop" : prep ? "vmatprep" : push ? "vmatpush" : "vmatmul";
    if (which >= units_) return no(why, "%s: array %u, and the machine has %zu (tpu_mxu_units)", what, which, units_);
    if (is_pop(insn)) {
      if (sew != 32 || (format != 0 && format != INT8))
        return no(why, "vpop: a result is a single or an int32 (format 0 or 1), and format %u of %u bits was asked",
                  format, sew);
      if ((field & ADD) && which + 1 >= units_)
        return no(why, "vpop: [3] adds array %u's rows, and the machine has %zu (tpu_mxu_units)", which + 1, units_);
      return !(field & SECOND) || registers_ > 1 ||
             no(why, "vpop: [2] names the upper half of the columns, which an array of lanes x lanes has not");
    }
    const State &s = arrays_[which]->state;
    const Operand operand = !prep && !push ? INPUTS : prep && (field & SECOND) ? INPUTS : WEIGHTS;
    if (prep && s.held[operand]) return false;
    if (prep && registers_ < 2)
      return no(why, "vmatprep: an operand of an array of lanes x lanes is one register, with no upper half");
    if (sew != 32) return no(why, "%s: its elements are words of 32 bits, not of %u", what, sew);
    if ((cp::rd(insn) & E5M2_RD) && format != 2)
      return no(why, "%s: rd[4] names E5M2, and format %u is no 8-bit float", what, format);
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
    if (registers_ > 1 && s.kind[operand] != kind(insn))
      return no(why, "%s: words of %s, and the vmatprep before it held %s", what, kind_name(kind(insn)),
                kind_name(s.kind[operand]));
    if (push) return true;
    const bool int8 = format == INT8, latch = field & LATCH;
    if (latch && !s.pushed) return no(why, "vmatmul: no weight was pushed before it");
    if (!latch && !s.latched) return no(why, "vmatmul: no weights were latched before it, and it does not latch");
    const bool weights_int8 = latch ? s.staged_int8 : s.active_int8;
    return weights_int8 == int8 || no(why, "vmatmul: inputs of %s, and the weights it computes with are %s",
                                      kind_name(kind(insn)), weights_int8 ? "int8" : "not int8");
  }
  // The head of insns(), carried out whole in this cycle when its slots, its rows and its room allow: a vpop takes
  // its rows of one output queue through the pop port, a vmatmul puts its rows in the input queue, both at once. True
  // once it completed, or faulted: a failed check, or a vpop asking more rows than its idle array and queue hold.
  bool carry_out(const Insn &insn) {
    std::string why;
    if (!carried(insn, why)) return why.empty() ? false : (fault(insn, why), true);
    const uint32_t field = cp::rs1(insn), vl = insn.vl;
    Array &a = *arrays_[array_of(insn)];
    if (is_pop(insn)) {
      const uint32_t half = field & SECOND ? 1 : 0;
      const bool int8 = (field & FORMAT) == INT8;
      cp::Queue<Out> &out = a.out(half);
      if (out.size() < vl && !a.input.size() && a.line.idle())
        return fault(insn, message("vpop: %u rows asked of the %s half, %zu computed", vl, half ? "upper" : "lower",
                                   static_cast<size_t>(out.size()))),
               true;
      Array *const next = field & ADD ? &*arrays_[array_of(insn) + 1] : nullptr;
      cp::Queue<Out> *const more = next ? &next->out(half) : nullptr;
      if (more && more->size() < vl && !next->input.size() && next->line.idle())
        return fault(insn, message("vpop: %u rows asked of the next array's %s half, %zu computed", vl,
                                   half ? "upper" : "lower", static_cast<size_t>(more->size()))),
               true;
      if (out.length() < vl || (more && more->length() < vl) || pop_.available() < vl || !a.pop_issue.available() ||
          (next && !next->pop_issue.available()))
        return false;
      if (!a.pop_issue.utilize(1) || (next && !next->pop_issue.utilize(1)) || !pop_.utilize(vl)) std::abort();
      for (uint32_t i = 0; i < vl; i++) {
        const Out row = out.pop();
        const Out added = more ? more->pop() : Out{int8, {}};
        if (row.int8 != int8 || added.int8 != int8)
          return fault(insn, message("vpop: format %u writes %s, and its row %u is of %s inputs", field & FORMAT,
                                     int8 ? "int32" : "singles", i, int8 ? "float" : "int8")),
                 true;
        values([&](const Host &host) {
          for (uint32_t lane = 0, first = half ? host.lanes() : 0; lane < host.lanes(); lane++)
            put<uint32_t>(host, lane, cp::rd(insn), i,
                          more ? sum(row.bits[first + lane], added.bits[first + lane], int8) : row.bits[first + lane]);
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
    a.state.held[operand] = true, a.state.rows[operand] = rows, a.state.kind[operand] = kind(insn);
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
    a.state.held[WEIGHTS] = false, a.state.pushed = true, a.state.staged_int8 = (cp::rs1(insn) & FORMAT) == INT8;
  }
  // vmatmul: one that latches first makes the staged weights the active; then its vl words of 1, 2 or 4 rows enter the
  // input queue, each row's outputs made as it enters (output()), rows_a_cycle of them one entry.
  void matmul(const Insn &insn, Array &a, uint64_t rows) {
    const uint32_t field = cp::rs1(insn), word = rows_a_cycle(field & FORMAT);
    const bool latch = field & LATCH;
    values([&](const Host &) {
      if (latch) a.values.active = a.values.staged;
    });
    for (uint64_t row = 0; row < rows; row++) {
      Input in{row % word ? 0 : word, {(field & FORMAT) == INT8, {}}};
      values([&](const Host &host) { in.out.bits = output(host, insn, a, row); });
      if (!a.input.push(std::move(in))) std::abort();
    }
    a.state.held[INPUTS] = false, a.state.latched |= latch;
    if (latch) a.state.active_int8 = a.state.staged_int8;
  }
  static float word_element(uint32_t word, uint32_t slot, uint32_t format, bool e5m2) {
    if (format == 0) {
      float value;
      std::memcpy(&value, &word, sizeof value);
      return value;
    }
    if (format == INT8) return static_cast<int8_t>(word >> (8 * slot));
    if (format == 2) return fp8_to_float(static_cast<uint8_t>(word >> (8 * slot)), e5m2 ? E5M2 : E4M3);
    return bf16_to_float(static_cast<uint16_t>(word >> (16 * slot)));
  }
  // An int8 value as word_element holds it; 0 for what no int8 is (a weight a float tile left at a depth no int8
  // push reached).
  static int32_t int8_of(float value) { return value >= -128 && value <= 127 ? static_cast<int32_t>(value) : 0; }
  // The rows of one register group, per lane.
  static Rows operand_rows(const Host &host, const Insn &insn) {
    const uint32_t format = cp::rs1(insn) & FORMAT, pack = rows_in_word(format);
    Rows rows(host.lanes(), Row(uint64_t{insn.vl} * pack));
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++)
        for (uint32_t slot = 0; slot < pack; slot++)
          rows[lane][i * pack + slot] = word_element(get<uint32_t>(host, lane, cp::rs2(insn), i), slot, format, e5m2(insn));
    return rows;
  }
  // Row `row` of a vmatmul's input, as it enters: a column's output is its active weights times the row, one input
  // vector of registers x lanes, the lower lanes from vs2 and the upper from the vmatprep the vmatmul took. Each
  // column's 32 bits: a single, or of int8 inputs an int32, the sum wrapping as an int32 accumulator does.
  std::vector<uint32_t> output(const Host &host, const Insn &insn, const Array &array, uint64_t row) const {
    const Values &v = array.values;
    const uint32_t lanes = host.lanes(), width = registers_ * lanes, format = cp::rs1(insn) & FORMAT;
    const uint32_t pack = rows_in_word(format), word = static_cast<uint32_t>(row / pack), slot = static_cast<uint32_t>(row % pack);
    Row input(width);
    std::vector<uint32_t> out(width);
    for (uint32_t k = 0; k < width; k++)
      input[k] = k < lanes ? word_element(get<uint32_t>(host, k, cp::rs2(insn), word), slot, format, e5m2(insn)) : v.upper[INPUTS][k - lanes][row];
    for (uint32_t column = 0; column < width; column++) {
      if (format == INT8) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < width; k++) sum += static_cast<uint32_t>(int8_of(input[k]) * int8_of(v.active[column][k]));
        out[column] = sum;
        continue;
      }
      float sum = 0;
      for (uint32_t k = 0; k < width; k++) sum = multiply_add(sum, input[k], v.active[column][k]);
      std::memcpy(&out[column], &sum, sizeof sum);
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
  static constexpr const char *INT8_MACS_KEY = "tpu_mxu_int8_macs_per_pe";

  Port entry_{*this, "input", std::max<uint32_t>(Shape::WORD_A_CYCLE ? LINE_ROWS : 1,
                                                 counted_rows(Shape::WORD_A_CYCLE) * Shape::INT8_MACS)};
  Counter rows_{*this, "rows", counted_rows(Shape::WORD_A_CYCLE)};
  Counter rows_in_flight_{*this, "rows_in_flight"};
  Counter active_cycles_{*this, "active_cycles"};
  Port issue_{*this, "issue", Shape::SLOTS};
  std::optional<Array> arrays_[MAX_ARRAYS];
  Port pop_{*this, "pop", 256};
  uint32_t registers_ = Shape::REGISTERS;
  bool word_a_cycle_ = Shape::WORD_A_CYCLE;
  uint32_t int8_macs_ = Shape::INT8_MACS;
  size_t units_ = 1;
};

}

#endif
