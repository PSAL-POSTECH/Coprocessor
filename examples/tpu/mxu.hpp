// A TPU model's matrix unit: `tpu_mxu_units` arrays of tpu_mxu_registers x lanes each way (lanes x lanes under
// tpu_mxu_bound_to_lanes: 1), an operand that many registers; tpu_mxu_issue_slots and tpu_mxu_word_a_cycle set its
// issue slots and its rows a cycle. A key the machine leaves out is the model's Shape.
#ifndef TPU_MXU_HPP
#define TPU_MXU_HPP

#include <cstdio>
#include <cstring>
#include <vector>

#include "vcix_uarch.hpp"

#include "values.hpp"

namespace tpu {

template <class Shape>
class Mxu {
  static_assert(Shape::REGISTERS == 1 || Shape::REGISTERS == 2, "an operand is one register, or an upper and a lower half");
  using Config = vcix_uarch::Config;
  using Host = vcix_uarch::Host;
  using Insn = vcix_uarch::Insn;
  using Route = vcix_uarch::Route;
  using Port = vcix_uarch::Port;
  using Rows = std::vector<std::vector<float>>;
  using Row = std::vector<float>;

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Mxu(const char *name)
      : entry_{name, "input", "rows", Shape::WORD_A_CYCLE ? 2 : 1, vcix_uarch::PRIMARY},
        issue_{name, "issue", "instructions", Shape::SLOTS},
        pop_{name, "pop", "rows", 256},
        arrays_(1, Array(name)) {}

  // The field at 19:15: [1:0] what a word holds (0 a single, 1 two halves, 3 two bfloat16); [2] which operand a
  // vmatprep is for (0 weights, 1 inputs), whether a vmatmul latches the staged weights, or which half of the columns
  // a vpop takes (0 lower, 1 upper); [4:3] which half of the depth a vmatpush fills (1 the lower, 2 the upper).
  static constexpr uint32_t FORMAT = 0x3, SECOND = 0x4, LATCH = 0x4, DEPTH = 0x18;
  static uint32_t rows_in_word(uint32_t field) { return (field & FORMAT) ? 2 : 1; }
  static constexpr uint64_t MAX_ARRAYS = 32;

  // An array's stages default to its width + depth - 1.
  void configure(const Config &config) {
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
    registers_ = static_cast<uint32_t>(registers), per_cycle_ = word ? 2 : 1, word_a_cycle_ = word;
    issue_.set_capacity(slots);
    const uint64_t stages = config.uint(DELAY_KEY, 2 * registers_ * lanes - 1);
    if (stages < 1 || stages > UINT32_MAX)
      throw cp::ConfigError(DELAY_KEY, config.get(DELAY_KEY), "is not a delay in cycles");
    entry_.set_capacity(per_cycle_ * arrays);
    pop_.set_capacity(entries);
    arrays_.assign(arrays, Array(entry_.unit()));
    for (Array &array : arrays_) array.line.configure(stages, entries, vcix_uarch::Stall::WHEN_HEAD_BLOCKED, registers_);
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_), h.port(issue_);
    for (Array &array : arrays_) h.port(array.weight_issue), h.port(array.input_issue), h.port(array.pop_issue);
    h.port(pop_);
    for (Array &array : arrays_) h.pipeline(array.line, entry_), h.reg(array.state), h.reg(array.values);
  }

  // vmatprep: one of the tpu_mxu_issue_slots a cycle, and of its array's weight or input slot; it holds the upper half of its
  // operand until the next vmatpush or vmatmul of that operand takes it, and waits while the half held before is not.
  void prep(Route &r) {
    const uint32_t operand = cp::rs1(r.insn()) & SECOND ? INPUTS : WEIGHTS;
    Array *array = array_for(r, cp::rd(r.insn()), "vmatprep");
    if (!array) return;
    r.require(!array->state.get().held[operand]);
    r.require(registers_ > 1, [] { fail("vmatprep: an operand of an array of lanes x lanes is one register, with no upper half"); });
    words(r, "vmatprep");
    r.use(operand == INPUTS ? array->input_issue : array->weight_issue).use(issue_);
    const uint64_t rows = uint64_t{r.vl()} * rows_in_word(cp::rs1(r.insn()));
    r.at_issue([&] {
      State &s = array->state.write();
      s.held[operand] = true, s.rows[operand] = rows;
    });
    r.at_issue([&](const Host &host) { array->values.write().upper[operand] = operand_rows(host, r.insn()); });
  }
  // vmatpush stages a weight per row in every column in one half of the depth, as it issues: what it held moves down as
  // many depths, past its last is gone, and the rows take its top in their order. Nothing enters the array.
  void push(Route &r) {
    Array *array = array_for(r, cp::rd(r.insn()), "vmatpush");
    if (!array) return;
    const uint32_t field = cp::rs1(r.insn()), half = (field & DEPTH) >> 3;
    words(r, "vmatpush");
    r.require(half != 0, [field] { fail("vmatpush: field %u names no half of the depth: [4:3] is 1 or 2", field); });
    r.require(half != 3, [] { fail("vmatpush: [4:3] 3 is the v6e's vmatpush3, which is not modelled"); });
    r.require(half != 2 || registers_ > 1,
              [] { fail("vmatpush: [4:3] 2 names a second half of the depth, which an array of lanes x lanes has not"); });
    if (registers_ > 1) taken(r, *array, "vmatpush", WEIGHTS, uint64_t{r.vl()} * rows_in_word(field));
    r.use(array->weight_issue).use(issue_);
    r.at_issue([&](const Host &host) {
      const Rows lower = operand_rows(host, r.insn());
      Values &v = array->values.write();
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
    r.at_issue([&] {
      State &s = array->state.write();
      s.held[WEIGHTS] = false, s.pushed = true;
    });
  }
  // vmatmul: vl words of 1 or 2 rows into the array, a word (tpu_mxu_word_a_cycle) or a row a cycle, each row's outputs
  // made as it enters (output()). One that latches first makes the staged weights the active, as it issues.
  void matmul(Route &r) {
    Array *array = array_for(r, cp::rd(r.insn()), "vmatmul");
    if (!array) return;
    const uint32_t in_word = rows_in_word(cp::rs1(r.insn())), weight = word_a_cycle_ ? in_word : 1;
    const bool latch = cp::rs1(r.insn()) & LATCH;
    words(r, "vmatmul");
    if (registers_ > 1) taken(r, *array, "vmatmul", INPUTS, uint64_t{r.vl()} * in_word);
    if (latch) r.require(array->state.get().pushed, [] { fail("vmatmul: no weight was pushed before it"); });
    else r.require(array->state.get().latched, [] { fail("vmatmul: no weights were latched before it, and it does not latch"); });
    r.use(array->input_issue).use(issue_);
    r.send(array->line, uint64_t{r.vl()} * in_word / weight, weight,
           [this, array, &r](const Host &host, uint64_t row) { return output(host, r.insn(), *array, row); });
    r.at_issue([&](const Host &) {
      Values &v = array->values.write();
      if (latch) v.active = v.staged;
    });
    r.at_issue([&] {
      State &s = array->state.write();
      s.held[INPUTS] = false, s.latched |= latch;
    });
  }
  // vpop: vl rows of the lower or the upper half of the columns, row i to element i of vd; the pops of all arrays share
  // the pop port's rows. As singles; one that asks more than is computed never ends.
  void pop(Route &r) {
    Array *array = array_for(r, cp::rs2(r.insn()), "vpop");
    if (!array) return;
    const uint32_t half = cp::rs1(r.insn()) & SECOND ? 1 : 0, format = cp::rs1(r.insn()) & FORMAT;
    const uint32_t sew = r.insn().sew_bits, vl = r.vl();
    r.require(sew == 32 && !format,
              [format, sew] { fail("vpop: a result is a single, and format %u of %u bits was asked", format, sew); });
    r.require(!half || registers_ > 1,
              [] { fail("vpop: [2] names the upper half of the columns, which an array of lanes x lanes has not"); });
    r.use(array->pop_issue);
    r.receive(array->line, vl, [&r, half](const Host &host, uint64_t i, Row &&row) {
      const uint32_t lanes = host.lanes(), first = half ? lanes : 0, vd = cp::rd(r.insn());
      for (uint32_t lane = 0; lane < lanes; lane++) put<float>(host, lane, vd, static_cast<uint32_t>(i), row[first + lane]);
    });
    r.from_output(half).through_received(pop_);
    r.when_stuck([vl, half](uint64_t got) {
      fail("vpop: %u rows asked of the %s half, %zu computed", vl, half ? "upper" : "lower", static_cast<size_t>(got));
    });
  }

  // What the [tpu] trace adds to an issue: the array when there are several, and its queues.
  void trace_queues(const Insn &insn) const {
    const uint32_t which = is_pop(insn) ? cp::rs2(insn) : cp::rd(insn);
    if (arrays_.size() > 1) std::printf(", array %u", which);
    if (which >= arrays_.size()) return;
    const vcix_uarch::AnyPipeline &line = arrays_[which].line;
    if (registers_ > 1)
      std::printf(", input queue %u, output queues %u and %u", static_cast<uint32_t>(line.input_rows()),
                  static_cast<uint32_t>(line.output_rows(0)), static_cast<uint32_t>(line.output_rows(1)));
    else
      std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(line.input_rows()),
                  static_cast<uint32_t>(line.output_rows(0)));
  }

  // funct6, vm and funct3 of custom-2: sf.vc.iv 0, 1 and 2, and sf.vc.v.i 2, as the tpu model's two arrays have them.
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr vcix_uarch::Instruction<Mxu> INSTRUCTIONS[] = {{0x2200305B, FORM, "vmatmul", &Mxu::matmul},
                                                                  {0x2600305B, FORM, "vmatpush", &Mxu::push},
                                                                  {0x2A00305B, FORM, "vmatprep", &Mxu::prep},
                                                                  {0x0800305B, FORM, "vpop", &Mxu::pop}};

 private:
  enum Operand : uint32_t { WEIGHTS, INPUTS };
  // One array's state: whether each operand's upper half is held, and of how many rows; whether a push has staged
  // weights, and a latch made them active.
  struct State {
    bool held[2] = {false, false};
    uint64_t rows[2] = {0, 0};
    bool pushed = false, latched = false;
  };
  // One array's values: the upper half each operand's vmatprep holds; per column, of registers x lanes, its weight at
  // each depth as the pushes left them (staged) and as the last latch took them (active), empty until the first.
  struct Values {
    Rows upper[2];
    Rows staged, active;
  };
  // One array: its issue slots, one instruction a cycle each; its pipeline, whose rows carry its outputs; its state;
  // its values.
  struct Array {
    explicit Array(const char *unit)
        : weight_issue{unit, "weight_issue", "instructions", 1},
          input_issue{unit, "input_issue", "instructions", 1},
          pop_issue{unit, "pop_issue", "instructions", 1} {}
    Port weight_issue, input_issue, pop_issue;
    vcix_uarch::Pipeline<Row> line;
    vcix_uarch::Register<State> state;
    vcix_uarch::Register<Values> values;
  };

  static bool is_pop(const Insn &insn) { return (insn.bits & FORM) == 0x0800305B; }
  // The array an instruction names; one the machine lacks is never accepted.
  Array *array_for(Route &r, uint32_t which, const char *what) {
    const size_t arrays = arrays_.size();
    if (r.require(which < arrays, [what, which, arrays] { fail("%s: array %u, and the machine has %zu (tpu_mxu_units)", what, which, arrays); }))
      return &arrays_[which];
    return nullptr;
  }
  // An operand is words of 32 bits of format 0, 1 or 3; one of others is never accepted.
  static void words(Route &r, const char *what) {
    const uint32_t sew = r.insn().sew_bits, format = cp::rs1(r.insn()) & FORMAT;
    r.require(sew == 32, [what, sew] { fail("%s: its elements are words of 32 bits, not of %u", what, sew); });
    r.require(format != 2, [what] { fail("%s: format 2 names nothing a word holds", what); });
  }
  // The upper half a vmatprep of the operand holds, of as many rows, is taken; never accepted otherwise.
  static void taken(Route &r, const Array &array, const char *what, Operand operand, uint64_t rows) {
    const State &s = array.state.get();
    const uint64_t held = s.rows[operand];
    if (r.require(s.held[operand], [what] { fail("%s: no vmatprep of its upper half came before it", what); }))
      r.require(held == rows, [what, rows, held] {
        fail("%s: %zu rows, and the vmatprep before it held %zu", what, static_cast<size_t>(rows), static_cast<size_t>(held));
      });
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
    const Values &v = array.values.get();
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

  Port entry_, issue_, pop_;
  uint32_t registers_ = Shape::REGISTERS, per_cycle_ = Shape::WORD_A_CYCLE ? 2 : 1;
  bool word_a_cycle_ = Shape::WORD_A_CYCLE;
  std::vector<Array> arrays_;
};

}  // namespace tpu

#endif
