// A TPU's cross-lane unit, a black box: pushed elements pass the stages tpu_xlu_elements_per_cycle a cycle into its
// buffer; its first pop after pushes moves (or folds) the tile across the lanes at once, the whole result leaving at once
// into the output queue. The first push after a pop forgets what is left of the tile before. Values on Spike, counts on gem5.
#ifndef TPU_XLU_HPP
#define TPU_XLU_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "machine.hpp"

#include "values.hpp"

namespace tpu {

// The v6e's vmax.f32, as MLIR's arith.maximumf: a NaN on either side is the result, and -0 is below +0.
inline float maximum(float a, float b) {
  if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<float>::quiet_NaN();
  if (a == b) return std::signbit(a) ? b : a;
  return a > b ? a : b;
}
// vmin.f32 the same way, as arith.minimumf: a NaN on either side is the result, and -0 is below +0.
inline float minimum(float a, float b) {
  if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<float>::quiet_NaN();
  if (a == b) return std::signbit(a) ? a : b;
  return a < b ? a : b;
}

// stages_ takes a fold's or an exchange's elements (tpu_xlu_latency_cycles), moves_ a move's (tpu_xlu_move_latency_cycles);
// left_: the result slots the pops have not taken; keep_: whether a push keeps them (a move's, not a fold's or an
// exchange's); pattern_: the set pattern, kept across pops (values: Spike only), pattern_width_ its width (both).
class Xlu : public Component {
  using Host = cp::Host;
  using Insn = cp::Insn;
  using Tile = std::vector<std::vector<uint32_t>>;
  using Elements = std::vector<uint32_t>;
  using Column = std::vector<std::optional<uint32_t>>;
  // fold: 0 none, else the fold kind + 1, of fold_bits elements; moves: its first push was a move, its elements take moves_.
  struct Box {
    uint32_t operation = 0, fold = 0, fold_bits = 0;
    bool moves = false;
    uint64_t elements = 0, before_entries = 0, after_entries = 0;
    Tile before, after;
    std::vector<Elements> tile;
    std::deque<Column> result;
  };

 public:
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr cp::Encoding XLU_PUSH{0x2E00305B, FORM, "xlu_push"};
  static constexpr cp::Encoding XLU_SET_PATTERN{0xAE00305B, FORM, "xlu_set_pattern"};
  static constexpr cp::Encoding XLU_PUSH_REDUCE{0xAA00305B, FORM, "xlu_push_reduce"};
  static constexpr cp::Encoding XLU_POP{0x0400305B, FORM, "xlu_pop"};

  // `name` is the component as its statistics report it; the model that holds the component says it.
  Xlu(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    handle(XLU_PUSH, &Xlu::push);
    handle(XLU_SET_PATTERN, &Xlu::set_pattern);
    handle(XLU_PUSH_REDUCE, &Xlu::push, 2);
    handle(XLU_POP, &Xlu::pop);
  }
  void configure(const cp::Config &config) override {
    const uint64_t latency = config.uint(LATENCY_KEY, DEFAULT_LATENCY);
    const uint64_t move_latency = config.uint(MOVE_LATENCY_KEY, latency);
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    const uint64_t lanes = config.uint(LANES_KEY, 128);
    const uint64_t width = config.uint(WIDTH_KEY, 1), issue = config.uint(ISSUE_KEY, 1);
    if (latency < 1 || latency > UINT32_MAX)
      throw vcix_accel::ConfigError(LATENCY_KEY, config.get(LATENCY_KEY), "is not a latency the unit can have");
    if (move_latency < 1 || move_latency > UINT32_MAX)
      throw vcix_accel::ConfigError(MOVE_LATENCY_KEY, config.get(MOVE_LATENCY_KEY), "is not a latency the unit can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    if (lanes < 1)
      throw vcix_accel::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the unit can have");
    if (lanes > MAX_SLOTS)
      throw vcix_accel::ConfigError(LANES_KEY, config.get(LANES_KEY),
                                    "is more than 65535 lanes: an exchange's result of that many slots does not fit one entry");
    if (width < 1 || width > entries)
      throw vcix_accel::ConfigError(WIDTH_KEY, config.get(WIDTH_KEY), "is not between 1 and the queue size");
    if (issue < 1 || issue > UINT32_MAX)
      throw vcix_accel::ConfigError(ISSUE_KEY, config.get(ISSUE_KEY), "is not an issue rate");
    entry_.set_capacity(width), issue_.set_capacity(issue);
    width_ = width;
    stages_.configure(latency, cp::UNLIMITED, cp::Stall::WHEN_OUTPUT_FULL);
    moves_.configure(move_latency, cp::UNLIMITED, cp::Stall::WHEN_OUTPUT_FULL);
    out_.configure(1, cp::UNLIMITED, cp::Stall::WHEN_OUTPUT_FULL);
    pops_.configure(1, cp::Free::WHEN_DONE, 0);
    lanes_ = lanes, entries_ = entries;
  }
  // Field 19:15: [4:3] shuffle before, [2] exchange, [1:0] shuffle after; [4:3] 3 loads the after's pattern from rs2. vl
  // elements wait for the pop before and for queue room, at most MAX_SLOTS a tile, element i taking set pattern entry i mod
  // its width. arg() 2: xlu_push_reduce, a fold of the field (0 max, 1 sum, 2 min, 3 argmax, 4 argmin; f32, or bf16 for max and min).
  void push(cp::Instr &in) {
    const uint32_t field = in.rs1(), bits = in.sew_bits();
    const bool load = !in.arg() && field >> 3 == LOAD, fold = in.arg() == 2;
    if (fold && field > FOLD_ARGMIN)
      in.require(false, [field] {
        fail("xlu push_reduce: fold %u, which is none of max (0), sum (1), min (2), argmax (3), argmin (4)", field);
      });
    if (fold && bits != 32 && !(bits == 16 && (field == FOLD_MAX || field == FOLD_MIN)))
      in.require(false, [field, bits] {
        fail("xlu push_reduce: fold %u of %u-bit elements; every fold takes f32, max and min also bf16", field, bits);
      });
    if (!load) {
      const Box &box = box_.get();
      const bool open = box.elements;
      const uint32_t kind = fold ? field + 1 : 0;
      in.require(!open || (box.fold == kind && (!fold || box.fold_bits == bits)), [kind] {
        fail("xlu push: a tile of one kind takes a push of another (fold %u, or another width)", kind);
      });
    }
    if (!load) in.require(pops_.idle() && stages_.input_rows() + moves_.input_rows() + in.vl() <= entries_);
    const bool through_moves = box_.get().elements ? box_.get().moves : !fold && !(field & EXCHANGE);
    if (!load) {
      const uint64_t held = box_.get().elements;
      in.require(held + in.vl() <= MAX_SLOTS, [held] {
        fail("xlu push: %llu elements held, and a result holds at most 65535; a pop must take them first",
             static_cast<unsigned long long>(held));
      });
    }
    in.use(issue_);
    in.on_issue([&] {
      Box &box = box_.write();
      if (load) {
        box.after_entries += in.vl();
        return;
      }
      if (left_.get() && !keep_.get()) stages_.flush(), moves_.flush(), out_.flush(), left_.write() = 0;
      if (!box.elements) box.moves = through_moves;
      if (fold)
        box.fold = field + 1, box.fold_bits = in.sew_bits();
      else
        box.operation = field, box.fold = 0;
      box.elements += in.vl();
      if (!fold && pattern_width_.get()) box.before_entries += in.vl();
    });
    in.on_issue([&](const Host &host) {
      if (fold) return;
      if (!load) {
        const Tile &set = pattern_.get();
        if (set.empty() || set[0].empty()) return;
        Tile &before = sized(box_.write().before, host);
        for (uint32_t lane = 0; lane < host.lanes(); lane++)
          for (uint32_t i = 0; i < in.vl(); i++) before[lane].push_back(set[lane][i % set[lane].size()]);
        return;
      }
      Tile &pattern = sized(box_.write().after, host);
      const uint32_t from = in.rs2();
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < in.vl(); i++) pattern[lane].push_back(get<uint32_t>(host, lane, from, i));
    });
    const uint32_t weight = static_cast<uint32_t>(std::gcd<uint64_t>(in.vl(), width_));
    if (!load)
      in.send(through_moves ? moves_ : stages_, in.vl() / weight, weight,
              [&in](const Host &host, uint64_t i) { return element(host, in.insn(), i); });
  }
  // The before's pattern, as the v6e's vset.pattern.permute: rs2's vl words in each lane, a lane number per element,
  // kept until the next set; each push after it takes it as it issues, so a set between pushes of one tile changes
  // only the pushes after it.
  void set_pattern(cp::Instr &in) {
    const uint32_t bits = in.sew_bits();
    in.require(bits == 32, [bits] { fail("xlu set_pattern: lane numbers are words of 32 bits, not of %u", bits); });
    in.use(issue_);
    in.on_issue([&] { pattern_width_.write() = in.vl(); });
    in.on_issue([&](const Host &host) {
      Tile &set = pattern_.write();
      set.assign(host.lanes(), {});
      const uint32_t from = in.rs2();
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < in.vl(); i++) set[lane].push_back(get<uint32_t>(host, lane, from, i));
    });
  }
  // vl slots of the output queue, slot i to element i of vd in its lanes; asking more than is left never ends. A pop with
  // no slots left makes the pushes since one tile, its result's slots (an element each, vpu_num_lanes for an exchange) one
  // entry. A push among a move's pops starts the next tile behind them; one after a fold's or an exchange's drops them.
  void pop(cp::Instr &in) {
    const bool first = !left_.get();
    const uint64_t pushed = first ? box_.get().elements : 0;
    const uint64_t out = !pushed ? 0 : !box_.get().fold && box_.get().operation & EXCHANGE ? lanes_ : pushed;
    if (pushed && !box_.get().fold) patterned(in, pushed, out);
    in.use(issue_);
    in.on_issue([&] {
      const uint64_t left = first ? out : left_.get();
      left_.write() = left > in.vl() ? left - in.vl() : 0;
      if (!first) return;
      keep_.write() = !box_.get().fold && !(box_.get().operation & EXCHANGE);
      Box &box = box_.write();
      box.elements = 0;
      if (pushed) box.before_entries = box.after_entries = 0;
    });
    in.enqueue(pops_);
    if (first) {
      auto &line = box_.get().moves ? moves_ : stages_;
      in.wait_empty(line);
      in.receive(line, pushed, [&](const Host &, uint64_t, Elements &&slot) {
        box_.write().tile.push_back(std::move(slot));
      });
      in.on_received([&, out](const Host &host) { transform(host, out); });
      in.send(out_, out ? 1 : 0, out ? out : 1, [&](const Host &, uint64_t) {
        Box &box = box_.write();
        Column slot = std::move(box.result.front());
        box.result.pop_front();
        return slot;
      });
    }
    const uint32_t vl = in.vl();
    in.receive(out_, vl, [&in](const Host &host, uint64_t i, Column &&slot) {
      const uint32_t vd = in.rd(), at = static_cast<uint32_t>(i);
      for (uint32_t lane = 0; lane < slot.size() && lane < host.lanes(); lane++) {
        if (!slot[lane]) continue;
        switch (in.sew_bits()) {
          case 8: put<uint8_t>(host, lane, vd, at, static_cast<uint8_t>(*slot[lane])); break;
          case 16: put<uint16_t>(host, lane, vd, at, static_cast<uint16_t>(*slot[lane])); break;
          default: put<uint32_t>(host, lane, vd, at, *slot[lane]); break;
        }
      }
    });
    in.on_stuck([vl](uint64_t got) {
      fail("xlu pop: %u elements asked, %llu left", vl, static_cast<unsigned long long>(got));
    });
  }
  // What the [tpu] trace adds to an issue: the stages' input queue and the buffer they leave into.
  void trace_queues(const Insn &) const override {
    std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(stages_.input_rows() + moves_.input_rows()),
                static_cast<uint32_t>(stages_.output_rows() + moves_.output_rows()));
  }

 private:
  enum Shuffle : uint32_t { BYPASS = 0, REPLICATE = 1, ARBITRARY = 2, LOAD = 3 };
  enum Fold : uint32_t { FOLD_MAX = 0, FOLD_SUM = 1, FOLD_MIN = 2, FOLD_ARGMAX = 3, FOLD_ARGMIN = 4 };
  static constexpr uint32_t EXCHANGE = 4;
  static constexpr int INVALID_XLU_PATTERN = 201;
  static Tile &sized(Tile &tile, const Host &host) {
    if (tile.size() != host.lanes()) tile.assign(host.lanes(), {});
    return tile;
  }
  // Element i of rs2 in every lane, at the push's width.
  static Elements element(const Host &host, const Insn &insn, uint64_t i) {
    Elements slot(host.lanes());
    const uint32_t reg = vcix_accel::rs2(insn), at = static_cast<uint32_t>(i);
    for (uint32_t lane = 0; lane < host.lanes(); lane++) switch (insn.sew_bits) {
        case 8: slot[lane] = get<uint8_t>(host, lane, reg, at); break;
        case 16: slot[lane] = get<uint16_t>(host, lane, reg, at); break;
        case 32: slot[lane] = get<uint32_t>(host, lane, reg, at); break;
        default: slot[lane] = 0; break;
      }
    return slot;
  }
  // The box's tile shuffled, exchanged and shuffled by the last push's operation (both patterns taken), or folded over the
  // lanes in order (argmax / argmin: the first lane holding it, NaN above and below all), as the result's `out` slots.
  // With no element, nothing, and the patterns stay.
  void transform(const Host &host, uint64_t out) {
    Box &box = box_.write();
    if (box.tile.empty()) return;
    const uint32_t lanes = host.lanes(), operation = box.operation;
    Tile tile(lanes);
    for (const Elements &slot : box.tile)
      for (uint32_t lane = 0; lane < lanes; lane++) tile[lane].push_back(slot[lane]);
    box.tile.clear();
    if (box.fold) {
      const uint32_t kind = box.fold - 1;
      const bool bf16 = box.fold_bits == 16;
      for (uint64_t k = 0; k < out; k++) {
        float acc = 0.0f;
        uint32_t at = 0;
        for (uint32_t lane = 0; lane < lanes; lane++) {
          float x;
          const uint32_t raw = bf16 ? tile[lane][k] << 16 : tile[lane][k];
          std::memcpy(&x, &raw, sizeof x);
          if (!lane) {
            acc = x;
            continue;
          }
          switch (kind) {
            case FOLD_MAX: acc = maximum(acc, x); break;
            case FOLD_MIN: acc = minimum(acc, x); break;
            case FOLD_SUM: acc = acc + x; break;
            default:
              if (!std::isnan(acc) && (std::isnan(x) || (kind == FOLD_ARGMAX ? x > acc : x < acc))) acc = x, at = lane;
          }
        }
        uint32_t bits;
        std::memcpy(&bits, &acc, sizeof bits);
        box.result.emplace_back(lanes, kind >= FOLD_ARGMAX ? at : bf16 ? bits >> 16 : bits);
      }
      box.fold = 0;
      return;
    }
    Tile got = shuffle(operation >> 3, tile, sized(box.before, host));
    if (operation & EXCHANGE) got = cross(got, tile[0].size());
    got = shuffle(operation & 3, got, sized(box.after, host));
    box.before.clear(), box.after.clear();
    for (uint64_t k = 0; k < out; k++) {
      Column &slot = box.result.emplace_back(lanes);
      for (uint32_t lane = 0; lane < lanes; lane++)
        if (k < got[lane].size()) slot[lane] = got[lane][k];
    }
  }
  // A first pop of `pushed` elements whose shuffle is ARBITRARY needs a lane number per column of the tile it walks,
  // `pushed` before (each push took the set pattern) and `out` after; it is never accepted otherwise, and on Spike exits
  // INVALID_XLU_PATTERN.
  void patterned(cp::Instr &in, uint64_t pushed, uint64_t out) const {
    const Box &box = box_.get();
    const uint32_t operation = box.operation;
    const uint64_t walks[2] = {pushed, out}, entries[2] = {box.before_entries, box.after_entries};
    const uint32_t shuffles[2] = {operation >> 3, operation & 3};
    for (int k = 0; k < 2; k++) {
      const char *stage = k ? "post" : "pre";
      const size_t width = walks[k], carried = entries[k];
      in.require(shuffles[k] != ARBITRARY || carried == width, [stage, width, carried, operation] {
        std::fprintf(stderr, "XLU ERROR: the %s stage walks %zu columns but lane 0 carries %zu pattern entries (SIMM5 %u)\n",
                     stage, width, carried, operation);
        std::exit(INVALID_XLU_PATTERN);
      });
    }
  }
  // Every lane reads some lane's row: the lane its pattern names for ARBITRARY, a lane number per column, and lane 0
  // otherwise. What is missing reads as 0.
  static Tile shuffle(uint32_t what, const Tile &tile, const Tile &pattern) {
    if (what == BYPASS) return tile;
    const size_t lanes = tile.size();
    size_t width = 0;
    for (const std::vector<uint32_t> &row : tile) width = row.size() > width ? row.size() : width;
    Tile got(lanes);
    for (size_t lane = 0; lane < lanes; lane++)
      for (size_t k = 0; k < width; k++) {
        const size_t from = what == ARBITRARY ? pattern[lane][k] : 0;
        got[lane].push_back(from < lanes && k < tile[from].size() ? tile[from][k] : 0u);
      }
    return got;
  }
  // Column k becomes lane k's row; only `depth` lanes receive anything.
  static Tile cross(const Tile &tile, uint64_t depth) {
    const size_t lanes = tile.size();
    Tile got(lanes);
    for (uint64_t k = 0; k < depth && k < lanes; k++)
      for (size_t lane = 0; lane < lanes; lane++) got[k].push_back(k < tile[lane].size() ? tile[lane][k] : 0u);
    return got;
  }

  // TODO: the unit's latency is not known; 10 is a placeholder.
  static constexpr uint32_t DEFAULT_LATENCY = 10;
  static constexpr uint32_t DEFAULT_ENTRIES = 256;
  static constexpr uint64_t MAX_SLOTS = UINT16_MAX;
  static constexpr const char *LATENCY_KEY = "tpu_xlu_latency_cycles";
  static constexpr const char *MOVE_LATENCY_KEY = "tpu_xlu_move_latency_cycles";
  static constexpr const char *QUEUE_KEY = "tpu_xlu_queue_entries";
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *WIDTH_KEY = "tpu_xlu_elements_per_cycle";
  static constexpr const char *ISSUE_KEY = "tpu_xlu_issue_per_cycle";

  cp::Port entry_{*this, "input", "elements", 1, cp::Port::PRIMARY};
  cp::Port issue_{*this, "issue", "instructions", 1};
  cp::Port exit_{*this, "output", "elements", cp::UNLIMITED};
  cp::Pipeline<Elements> stages_{*this, entry_};
  cp::Pipeline<Elements> moves_{*this, entry_};
  cp::Pipeline<Column> out_{*this, exit_};
  cp::CmdQueue pops_{*this};
  cp::Register<Box> box_{*this};
  cp::Register<uint64_t> left_{*this};
  cp::Register<bool> keep_{*this};
  cp::Register<Tile> pattern_{*this};
  cp::Register<uint64_t> pattern_width_{*this};
  uint64_t lanes_ = 128, entries_ = DEFAULT_ENTRIES, width_ = 1;
};

}

#endif
