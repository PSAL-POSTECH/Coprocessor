// A TPU's cross-lane unit, a black box: pushed elements pass the stages tpu_xlu_elements_per_cycle a cycle into its
// buffer; its first pop after pushes moves the tile across the lanes at once (or folds it across them), and the whole
// result leaves at once into the output queue.
// The first element push after a pop forgets what is left of the tile before. Values on Spike, counts on gem5.
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

#include "vcix_uarch.hpp"

#include "values.hpp"

namespace tpu {

// The v6e's vmax.f32, as MLIR's arith.maximumf: a NaN on either side is the result, and -0 is below +0.
inline float maximum(float a, float b) {
  if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<float>::quiet_NaN();
  if (a == b) return std::signbit(a) ? b : a;
  return a > b ? a : b;
}

class Xlu {
  using Host = vcix_uarch::Host;
  using Insn = vcix_uarch::Insn;
  using Route = vcix_uarch::Route;
  using Tile = std::vector<std::vector<uint32_t>>;
  using Elements = std::vector<uint32_t>;
  using Column = std::vector<std::optional<uint32_t>>;
  struct Box {
    uint32_t operation = 0, fold = 0;  // fold: 0 none, else the fold kind + 1
    uint64_t elements = 0, before_entries = 0, after_entries = 0;
    Tile before, after;
    std::vector<Elements> tile;
    std::deque<Column> result;
  };

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Xlu(const char *name)
      : entry_{name, "input", "elements", 1, vcix_uarch::PRIMARY}, issue_{name, "issue", "instructions", 1},
        exit_{name, "output", "elements", vcix_uarch::UNLIMITED} {}
  void configure(const vcix_uarch::Config &config) {
    const uint64_t latency = config.uint(LATENCY_KEY, DEFAULT_LATENCY);
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    const uint64_t lanes = config.uint(LANES_KEY, 128);
    const uint64_t width = config.uint(WIDTH_KEY, 1), issue = config.uint(ISSUE_KEY, 1);
    if (latency < 1 || latency > UINT32_MAX)
      throw vcix_accel::ConfigError(LATENCY_KEY, config.get(LATENCY_KEY), "is not a latency the unit can have");
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
    stages_.configure(latency, vcix_uarch::UNLIMITED, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
    out_.configure(1, vcix_uarch::UNLIMITED, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
    pops_.configure(1, vcix_uarch::Free::WHEN_DONE, 0);
    lanes_ = lanes, entries_ = entries;
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_), h.port(issue_), h.port(exit_);
    h.pipeline(stages_, entry_), h.pipeline(out_, exit_);
    h.queue(pops_);
    h.reg(box_), h.reg(popped_);
  }

  // Field 19:15: [4:3] shuffle before, [2] exchange, [1:0] shuffle after; [4:3] 3 only loads the after's pattern from
  // rs2. Elements (32 raw bits each) wait for the pop before them and for room in the input queue, and take vl slots;
  // the box holds at most MAX_SLOTS between pops. arg() 1: xlu_push_pattern, whose rd holds the before's pattern.
  // arg() 2: xlu_push_reduce, a tile of f32 folded across the lanes, the field the fold (0 max, 1 sum), as the v6e's
  // vmax.xlane / vadd.xlane; a tile is one kind, and a fold pushed into a move, or a move into a fold, stops the run.
  void push(Route &r) {
    const uint32_t field = vcix_accel::rs1(r.insn());
    const bool load = !r.arg() && field >> 3 == LOAD, fold = r.arg() == 2;
    if (fold && field > FOLD_SUM)
      r.require(false, [field] { fail("xlu push_reduce: fold %u, which is neither max (0) nor sum (1)", field); });
    if (!load) {
      const Box &box = box_.get();
      const bool open = box.elements && !popped_.get();
      const uint32_t kind = fold ? field + 1 : 0;
      r.require(!open || box.fold == kind, [kind] {
        fail("xlu push: a tile of one kind takes a push of another (fold %u)", kind);
      });
    }
    if (!load) r.require(pops_.idle() && stages_.input_rows() + r.vl() <= entries_);
    if (!load) {
      const uint64_t held = box_.get().elements;
      r.require(held + r.vl() <= MAX_SLOTS, [held] {
        fail("xlu push: %llu elements held, and a result holds at most 65535; a pop must take them first",
             static_cast<unsigned long long>(held));
      });
    }
    r.use(issue_).at_issue([&] {
      Box &box = box_.write();
      if (load) {
        box.after_entries += r.vl();
        return;
      }
      if (popped_.get()) stages_.flush(), out_.flush(), popped_.write() = false;
      if (fold)
        box.fold = field + 1;
      else
        box.operation = field, box.fold = 0;
      box.elements += r.vl();
      if (r.arg() == 1) box.before_entries += r.vl();
    });
    r.at_issue([&](const Host &host) {
      if (!load && r.arg() != 1) return;
      Tile &pattern = sized(load ? box_.write().after : box_.write().before, host);
      const uint32_t from = load ? vcix_accel::rs2(r.insn()) : vcix_accel::rd(r.insn());
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < r.vl(); i++) pattern[lane].push_back(get<uint32_t>(host, lane, from, i));
    });
    // as many elements an entry as divides vl and the unit takes a cycle: tpu_xlu_elements_per_cycle when it divides vl
    const uint32_t weight = static_cast<uint32_t>(std::gcd<uint64_t>(r.vl(), width_));
    if (!load)
      r.send(stages_, r.vl() / weight, weight, [&r](const Host &host, uint64_t i) { return element(host, r.insn(), i); });
  }
  // vl slots of the output queue, slot i to element i of vd in the lanes it holds; one asking more than is left never
  // ends. The first pop after pushes takes the whole tile at once and makes the result's slots, one an element or
  // vpu_num_lanes for an exchange, which leave the box together as one entry of that many rows.
  void pop(Route &r) {
    const bool first = !popped_.get();
    const uint64_t in = first ? box_.get().elements : 0;
    const uint64_t out = !in ? 0 : !box_.get().fold && box_.get().operation & EXCHANGE ? lanes_ : in;
    if (in && !box_.get().fold) patterned(r, in, out);
    r.use(issue_).at_issue([&] {
      if (!first) return;
      popped_.write() = true;
      Box &box = box_.write();
      box.elements = 0;
      if (in) box.before_entries = box.after_entries = 0;
    });
    r.enqueue(pops_);
    if (first) {
      r.wait_until_empty(stages_).receive(stages_, in, [&](const Host &, uint64_t, Elements &&slot) { box_.write().tile.push_back(std::move(slot)); })
          .when_received([&, out](const Host &host) { transform(host, out); });
      r.send(out_, out ? 1 : 0, out ? out : 1, [&](const Host &, uint64_t) {
        Box &box = box_.write();
        Column slot = std::move(box.result.front());
        box.result.pop_front();
        return slot;
      });
    }
    const uint32_t vl = r.vl();
    r.receive(out_, vl, [&r](const Host &host, uint64_t i, Column &&slot) {
      const uint32_t vd = vcix_accel::rd(r.insn()), at = static_cast<uint32_t>(i);
      for (uint32_t lane = 0; lane < slot.size() && lane < host.lanes(); lane++) {
        if (!slot[lane]) continue;
        switch (r.insn().sew_bits) {
          case 8: put<uint8_t>(host, lane, vd, at, static_cast<uint8_t>(*slot[lane])); break;
          case 16: put<uint16_t>(host, lane, vd, at, static_cast<uint16_t>(*slot[lane])); break;
          default: put<uint32_t>(host, lane, vd, at, *slot[lane]); break;
        }
      }
    });
    r.when_stuck([vl](uint64_t got) {
      fail("xlu pop: %u elements asked, %llu left", vl, static_cast<unsigned long long>(got));
    });
  }
  // What the [tpu] trace adds to an issue: the stages' input queue and the buffer they leave into.
  void trace_queues(const Insn &) const {
    std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(stages_.input_rows()),
                static_cast<uint32_t>(stages_.output_rows()));
  }

  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr vcix_uarch::Instruction<Xlu> INSTRUCTIONS[] = {{0x2E00305B, FORM, "xlu_push", &Xlu::push, 0},
                                                                  {0xAE00305B, FORM, "xlu_push_pattern", &Xlu::push, 1},
                                                                  {0xAA00305B, FORM, "xlu_push_reduce", &Xlu::push, 2},
                                                                  {0x0400305B, FORM, "xlu_pop", &Xlu::pop}};

 private:
  enum Shuffle : uint32_t { BYPASS = 0, REPLICATE = 1, ARBITRARY = 2, LOAD = 3 };
  enum Fold : uint32_t { FOLD_MAX = 0, FOLD_SUM = 1 };
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
  // The box's tile shuffled, exchanged and shuffled by the last push's operation, as the `out` slots of the result;
  // both patterns are taken whether or not the operation reads them. With no element, nothing, and the patterns stay.
  void transform(const Host &host, uint64_t out) {
    Box &box = box_.write();
    if (box.tile.empty()) return;
    const uint32_t lanes = host.lanes(), operation = box.operation;
    Tile tile(lanes);
    for (const Elements &slot : box.tile)
      for (uint32_t lane = 0; lane < lanes; lane++) tile[lane].push_back(slot[lane]);
    box.tile.clear();
    if (box.fold) {
      // each column's fold over the lanes in lane order, the same in every lane
      for (uint64_t k = 0; k < out; k++) {
        float acc = 0.0f;
        for (uint32_t lane = 0; lane < lanes; lane++) {
          float x;
          std::memcpy(&x, &tile[lane][k], sizeof x);
          acc = !lane ? x : box.fold - 1 == FOLD_MAX ? maximum(acc, x) : acc + x;
        }
        uint32_t bits;
        std::memcpy(&bits, &acc, sizeof bits);
        box.result.emplace_back(lanes, bits);
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
  // A first pop of `in` elements whose shuffle is ARBITRARY needs a lane number per column of the tile it walks, `in`
  // before and `out` after; it is never accepted otherwise, and on Spike exits INVALID_XLU_PATTERN.
  void patterned(Route &r, uint64_t in, uint64_t out) const {
    const Box &box = box_.get();
    const uint32_t operation = box.operation;
    const uint64_t walks[2] = {in, out}, entries[2] = {box.before_entries, box.after_entries};
    const uint32_t shuffles[2] = {operation >> 3, operation & 3};
    for (int k = 0; k < 2; k++) {
      const char *stage = k ? "post" : "pre";
      const size_t width = walks[k], carried = entries[k];
      r.require(shuffles[k] != ARBITRARY || carried == width, [stage, width, carried, operation] {
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
  static constexpr const char *QUEUE_KEY = "tpu_xlu_queue_entries";
  static constexpr const char *LANES_KEY = "vpu_num_lanes";
  static constexpr const char *WIDTH_KEY = "tpu_xlu_elements_per_cycle";
  static constexpr const char *ISSUE_KEY = "tpu_xlu_issue_per_cycle";

  vcix_uarch::Port entry_, issue_, exit_;
  vcix_uarch::Pipeline<Elements> stages_;
  vcix_uarch::Pipeline<Column> out_;
  vcix_uarch::CommandQueue pops_;
  vcix_uarch::Register<Box> box_;
  vcix_uarch::Register<bool> popped_;
  uint64_t lanes_ = 128, entries_ = DEFAULT_ENTRIES, width_ = 1;
};

}  // namespace tpu

#endif
