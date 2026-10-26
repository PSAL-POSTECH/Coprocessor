// A TPU's cross-lane unit, a black box: a push's elements wait in its input queue, then pass the stages
// tpu_xlu_elements_per_cycle a cycle into its buffer; the first pop after pushes moves (or folds) the whole tile, which
// leaves the output line at once into the output queue. The first push after a pop forgets what is left of the tile.
#ifndef TPU_XLU_HPP
#define TPU_XLU_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
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

// insns() in order, only the head working. box_: the tile pushed since the last first pop; left_: the result's slots
// no pop has taken; keep_: whether a push keeps them (a move's); pop_: the head pop once started; pattern_: the set
// pattern (values, Spike only) and pattern_width_ its width; staged_: the elements in the stages.
class Xlu : public Component {
  using Host = cp::Host;
  using Insn = cp::Insn;
  using Cycle = cp::Cycle;
  using Tile = std::vector<std::vector<uint32_t>>;
  using Elements = std::vector<uint32_t>;
  using Column = std::vector<std::optional<uint32_t>>;
  struct Element {
    uint32_t group;
    bool moves;
    Elements lanes;
  };
  struct Group {
    uint32_t count;
    std::vector<Elements> elements;
  };
  struct Result {
    uint64_t count;
    std::vector<Column> slots;
  };
  struct Box {
    uint32_t operation = 0, fold = 0, fold_bits = 0;
    bool moves = false;
    uint64_t elements = 0, before_entries = 0, after_entries = 0;
    Tile before, after;
  };
  struct Pop {
    bool started = false;
    uint64_t pushed = 0, out = 0;
  };

 public:
  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr cp::Encoding XLU_PUSH{0x2E00305B, FORM, "xlu_push"};
  static constexpr cp::Encoding XLU_SET_PATTERN{0xAE00305B, FORM, "xlu_set_pattern"};
  static constexpr cp::Encoding XLU_PUSH_REDUCE{0xAA00305B, FORM, "xlu_push_reduce"};
  static constexpr cp::Encoding XLU_POP{0x0400305B, FORM, "xlu_pop"};

  // `name` is the component as its statistics report it; the model that holds the component says it.
  Xlu(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    for (const cp::Encoding &e : {XLU_PUSH, XLU_SET_PATTERN, XLU_PUSH_REDUCE, XLU_POP}) register_insn(e);
  }
  void configure(const cp::Config &config) override {
    const uint64_t latency = config.uint(LATENCY_KEY, DEFAULT_LATENCY);
    const uint64_t move_latency = config.uint(MOVE_LATENCY_KEY, latency);
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    const uint64_t lanes = config.uint(LANES_KEY, 128);
    const uint64_t width = config.uint(WIDTH_KEY, 1), issue = config.uint(ISSUE_KEY, 1);
    if (latency < 1 || latency > UINT32_MAX)
      throw cp::ConfigError(LATENCY_KEY, config.get(LATENCY_KEY), "is not a latency the unit can have");
    if (move_latency < 1 || move_latency > UINT32_MAX)
      throw cp::ConfigError(MOVE_LATENCY_KEY, config.get(MOVE_LATENCY_KEY), "is not a latency the unit can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    if (lanes < 1)
      throw cp::ConfigError(LANES_KEY, config.get(LANES_KEY), "is not a lane count the unit can have");
    if (lanes > MAX_SLOTS)
      throw cp::ConfigError(LANES_KEY, config.get(LANES_KEY),
                                    "is more than 65535 lanes: an exchange's result of that many slots does not fit one entry");
    if (width < 1 || width > entries)
      throw cp::ConfigError(WIDTH_KEY, config.get(WIDTH_KEY), "is not between 1 and the queue size");
    if (issue < 1 || issue > UINT32_MAX)
      throw cp::ConfigError(ISSUE_KEY, config.get(ISSUE_KEY), "is not an issue rate");
    elements_.set_capacity(width), issue_.set_width(issue);
    insns().configure(2 * issue, issue);
    queue_.configure(entries), buffer_.configure(MAX_SLOTS), out_.configure(MAX_SLOTS);
    stages_.configure(latency, 1), moves_.configure(move_latency, 1), line_.configure(1, 1);
    lanes_ = lanes, width_ = width;
  }
  void reset() override {
    box_ = {}, pop_ = {}, pattern_.clear();
    pattern_width_ = left_ = staged_ = 0, keep_ = false;
  }
  // The output line's result into the output queue, finished elements into the buffer, the input queue's head into
  // its stages, then the instructions at the head of insns(), at most tpu_xlu_issue_per_cycle starting.
  void cycle() override {
    if (line_.length()) {
      Result r = line_.pop();
      for (uint64_t k = 0; k < r.count; k++)
        if (!out_.push(k < r.slots.size() ? std::move(r.slots[k]) : Column{})) std::abort();
    }
    for (cp::Pipeline<Group> *line : {&stages_, &moves_}) {
      if (!line->length()) continue;
      Group g = line->pop();
      staged_ -= g.count;
      for (uint32_t k = 0; k < g.count; k++)
        if (!buffer_.push(k < g.elements.size() ? std::move(g.elements[k]) : Elements{})) std::abort();
    }
    if (queue_.length()) {
      const Element &head = queue_.front();
      cp::Pipeline<Group> &line = head.moves ? moves_ : stages_;
      if (line.available()) {
        Group g{head.group, {}};
        for (uint32_t k = 0; k < g.count; k++) g.elements.push_back(queue_.pop().lanes);
        elements_.add(g.count);
        staged_ += g.count;
        if (!line.push(std::move(g))) std::abort();
      }
    }
    while (insns().length() && step(insns().front())) insns().pop();
    elements_in_flight_.add(staged_);
  }
  // What the [tpu] trace adds to an issue: the input queue and the buffer the stages leave into.
  void trace_queues(const vcix_insn &) const override {
    std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(queue_.size()), static_cast<uint32_t>(buffer_.size()));
  }

 private:
  enum Shuffle : uint32_t { BYPASS = 0, REPLICATE = 1, ARBITRARY = 2, LOAD = 3 };
  enum Fold : uint32_t { FOLD_MAX = 0, FOLD_SUM = 1, FOLD_MIN = 2, FOLD_ARGMAX = 3, FOLD_ARGMIN = 4 };
  static constexpr uint32_t EXCHANGE = 4;
  static constexpr int INVALID_XLU_PATTERN = 201;
  static bool is(const Insn &in, const cp::Encoding &e) { return (in.bits & e.mask) == e.match; }
  // Field 19:15 of a push: [4:3] shuffle before, [2] exchange, [1:0] shuffle after; [4:3] 3 loads the after's pattern.
  static bool loads(const Insn &in) { return is(in, XLU_PUSH) && cp::rs1(in) >> 3 == LOAD; }
  uint64_t slots(uint64_t pushed) const { return !pushed ? 0 : !box_.fold && box_.operation & EXCHANGE ? lanes_ : pushed; }
  // One step of the head; true when it leaves: completed, or faulted for a field the unit has no form of. A started pop
  // needs no issue slot; everything else starts and ends now.
  bool step(const Insn &in) {
    if (const std::string why = fields(in); !why.empty()) return fault(in, why), true;
    if (is(in, XLU_POP)) return pop(in);
    if (!issue_.available()) return false;
    if (is(in, XLU_SET_PATTERN)) return issue_.utilize(1) && (set_pattern(in), complete(in), true);
    return push(in);
  }
  // Why the unit has no form of the instruction's fields, or "".
  std::string fields(const Insn &in) const {
    const uint32_t field = cp::rs1(in), bits = in.sew_bits;
    if (is(in, XLU_SET_PATTERN) && bits != 32) return message("xlu set_pattern: lane numbers are words of 32 bits, not of %u", bits);
    if (!is(in, XLU_PUSH_REDUCE)) return "";
    if (field > FOLD_ARGMIN)
      return message("xlu push_reduce: fold %u, which is none of max (0), sum (1), min (2), argmax (3), argmin (4)", field);
    if (bits != 32 && !(bits == 16 && (field == FOLD_MAX || field == FOLD_MIN)))
      return message("xlu push_reduce: fold %u of %u-bit elements; every fold takes f32, max and min also bf16", field, bits);
    return "";
  }
  // Why a push never fits the tile, or "": of another kind or width, or past the 65535 elements one result holds.
  std::string fits(const Insn &in) const {
    const bool fold = is(in, XLU_PUSH_REDUCE);
    const uint32_t kind = fold ? cp::rs1(in) + 1 : 0;
    if (box_.elements && (box_.fold != kind || (fold && box_.fold_bits != in.sew_bits)))
      return message("xlu push: a tile of one kind takes a push of another (fold %u, or another width)", kind);
    if (box_.elements + in.vl > MAX_SLOTS)
      return message("xlu push: %llu elements held, and a result holds at most 65535; a pop must take them first",
                     static_cast<unsigned long long>(box_.elements));
    return "";
  }
  // vl elements into the input queue once it has room, element i taking set pattern entry i mod its width;
  // xlu_push_reduce: a fold of the field (0 max, 1 sum, 2 min, 3 argmax, 4 argmin; f32, or bf16 for max and min).
  // A push that never fits the tile is a fault.
  bool push(const Insn &in) {
    const uint32_t field = cp::rs1(in);
    const bool fold = is(in, XLU_PUSH_REDUCE);
    if (loads(in)) {
      if (!issue_.utilize(1)) return false;
      box_.after_entries += in.vl;
      values([&](const Host &host) {
        Tile &pattern = sized(box_.after, host);
        for (uint32_t lane = 0; lane < host.lanes(); lane++)
          for (uint32_t i = 0; i < in.vl; i++) pattern[lane].push_back(get<uint32_t>(host, lane, cp::rs2(in), i));
      });
      complete(in);
      return true;
    }
    if (const std::string why = fits(in); !why.empty()) return fault(in, why), true;
    if (queue_.available() < in.vl || !issue_.utilize(1)) return false;
    if (left_ && !keep_) {
      while (out_.length()) out_.pop();
      left_ = 0;
    }
    if (!box_.elements) box_.moves = !fold && !(field & EXCHANGE);
    if (fold)
      box_.fold = field + 1, box_.fold_bits = in.sew_bits;
    else
      box_.operation = field, box_.fold = 0;
    box_.elements += in.vl;
    if (!fold && pattern_width_) box_.before_entries += in.vl;
    values([&](const Host &host) {
      if (fold || pattern_.empty() || pattern_[0].empty()) return;
      Tile &before = sized(box_.before, host);
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < in.vl; i++) before[lane].push_back(pattern_[lane][i % pattern_[lane].size()]);
    });
    const uint32_t group = static_cast<uint32_t>(std::gcd<uint64_t>(in.vl, width_));
    for (uint32_t i = 0; i < in.vl; i++) {
      Element e{group, box_.moves, {}};
      values([&](const Host &host) { e.lanes = element(host, in, i); });
      if (!queue_.push(std::move(e))) std::abort();
    }
    complete(in);
    return true;
  }
  // The before's pattern, as the v6e's vset.pattern.permute: rs2's vl words in each lane, a lane number per element,
  // kept until the next set; each push after it takes it, so a set between pushes of one tile changes the later ones.
  void set_pattern(const Insn &in) {
    pattern_width_ = in.vl;
    values([&](const Host &host) {
      pattern_.assign(host.lanes(), {});
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < in.vl; i++) pattern_[lane].push_back(get<uint32_t>(host, lane, cp::rs2(in), i));
    });
  }
  // vl slots of the output queue, slot i to element i of vd. A first pop (no slot left) waits for its whole tile in
  // the buffer and sends its result (an element a slot, vpu_num_lanes for an exchange) down the output line. One
  // asking more slots than are left once nothing is on its way is a fault, as is one its pattern cannot walk.
  bool pop(const Insn &in) {
    if (!pop_.started) {
      const uint64_t pushed = left_ ? 0 : box_.elements, out = slots(pushed), left = left_ ? left_ : out;
      if (!issue_.available()) return false;
      if (pushed && !box_.fold)
        if (const std::string why = patterned(pushed, out); !why.empty()) return fault(in, why, INVALID_XLU_PATTERN), true;
      if (!issue_.utilize(1)) return false;
      if (!left_) {
        keep_ = !box_.fold && !(box_.operation & EXCHANGE);
        box_.elements = 0;
        if (pushed) box_.before_entries = box_.after_entries = 0;
      }
      left_ = left > in.vl ? left - in.vl : 0;
      pop_ = {true, pushed, out};
    }
    if (pop_.pushed) {
      if (buffer_.length() < pop_.pushed) return false;
      std::vector<Elements> tile;
      for (uint64_t k = 0; k < pop_.pushed; k++) tile.push_back(buffer_.pop());
      Result r{pop_.out, {}};
      values([&](const Host &host) { r.slots = transform(host, tile, pop_.out); });
      if (!line_.push(std::move(r))) std::abort();
      pop_.pushed = 0;
      return false;
    }
    if (line_.idle() && out_.size() < in.vl) {
      fault(in, message("xlu pop: %u elements asked, %llu left", in.vl, static_cast<unsigned long long>(out_.size())));
      pop_ = {};
      return true;
    }
    if (!line_.idle() || out_.length() < in.vl) return false;
    for (uint32_t i = 0; i < in.vl; i++) {
      const Column slot = out_.pop();
      values([&](const Host &host) {
        for (uint32_t lane = 0; lane < slot.size() && lane < host.lanes(); lane++) {
          if (!slot[lane]) continue;
          switch (in.sew_bits) {
            case 8: put<uint8_t>(host, lane, cp::rd(in), i, static_cast<uint8_t>(*slot[lane])); break;
            case 16: put<uint16_t>(host, lane, cp::rd(in), i, static_cast<uint16_t>(*slot[lane])); break;
            default: put<uint32_t>(host, lane, cp::rd(in), i, *slot[lane]); break;
          }
        }
      });
    }
    pop_ = {};
    complete(in);
    return true;
  }
  static Tile &sized(Tile &tile, const Host &host) {
    if (tile.size() != host.lanes()) tile.assign(host.lanes(), {});
    return tile;
  }
  // Element i of rs2 in every lane, at the push's width.
  static Elements element(const Host &host, const Insn &insn, uint64_t i) {
    Elements slot(host.lanes());
    const uint32_t reg = cp::rs2(insn), at = static_cast<uint32_t>(i);
    for (uint32_t lane = 0; lane < host.lanes(); lane++) switch (insn.sew_bits) {
        case 8: slot[lane] = get<uint8_t>(host, lane, reg, at); break;
        case 16: slot[lane] = get<uint16_t>(host, lane, reg, at); break;
        case 32: slot[lane] = get<uint32_t>(host, lane, reg, at); break;
        default: slot[lane] = 0; break;
      }
    return slot;
  }
  // The tile shuffled, exchanged and shuffled by the last push's operation (both patterns taken), or folded over the
  // lanes in order (argmax / argmin: the first lane holding it, NaN above and below all), as the result's `out` slots.
  std::vector<Column> transform(const Host &host, const std::vector<Elements> &elements, uint64_t out) {
    const uint32_t lanes = host.lanes(), operation = box_.operation;
    std::vector<Column> result;
    Tile tile(lanes);
    for (const Elements &slot : elements)
      for (uint32_t lane = 0; lane < lanes; lane++) tile[lane].push_back(slot[lane]);
    if (box_.fold) {
      const uint32_t kind = box_.fold - 1;
      const bool bf16 = box_.fold_bits == 16;
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
        result.emplace_back(lanes, kind >= FOLD_ARGMAX ? at : bf16 ? bits >> 16 : bits);
      }
      return result;
    }
    Tile got = shuffle(operation >> 3, tile, sized(box_.before, host));
    if (operation & EXCHANGE) got = cross(got, tile[0].size());
    got = shuffle(operation & 3, got, sized(box_.after, host));
    box_.before.clear(), box_.after.clear();
    for (uint64_t k = 0; k < out; k++) {
      Column &slot = result.emplace_back(lanes);
      for (uint32_t lane = 0; lane < lanes; lane++)
        if (k < got[lane].size()) slot[lane] = got[lane][k];
    }
    return result;
  }
  // Why a first pop of `pushed` elements cannot walk its tile, or "": an ARBITRARY shuffle needs a lane number per
  // column, `pushed` before (each push took the set pattern) and `out` after. The entries are counted on both hosts,
  // so the fault (exit INVALID_XLU_PATTERN) comes on gem5 too, where the pop waited for good before.
  std::string patterned(uint64_t pushed, uint64_t out) const {
    const uint32_t operation = box_.operation;
    const uint64_t walks[2] = {pushed, out}, entries[2] = {box_.before_entries, box_.after_entries};
    const uint32_t shuffles[2] = {operation >> 3, operation & 3};
    for (int k = 0; k < 2; k++) {
      if (shuffles[k] != ARBITRARY || entries[k] == walks[k]) continue;
      char text[192];
      std::snprintf(text, sizeof text, "XLU ERROR: the %s stage walks %zu columns but lane 0 carries %zu pattern entries (SIMM5 %u)",
                    k ? "post" : "pre", static_cast<size_t>(walks[k]), static_cast<size_t>(entries[k]), operation);
      return text;
    }
    return "";
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

  cp::Counter elements_{*this, "elements", 1};
  cp::Counter elements_in_flight_{*this, "elements_in_flight"};
  cp::Port issue_{*this, "issue", 1};
  cp::Queue<Element> queue_{*this, "queue"};
  cp::Pipeline<Group> stages_{*this, "stages"}, moves_{*this, "moves"};
  cp::Queue<Elements> buffer_{*this, "buffer"};
  cp::Pipeline<Result> line_{*this, "line"};
  cp::Queue<Column> out_{*this, "out"};
  Box box_;
  Pop pop_;
  Tile pattern_;
  uint64_t pattern_width_ = 0, left_ = 0, staged_ = 0, lanes_ = 128, width_ = 1;
  bool keep_ = false;
};

}

#endif
