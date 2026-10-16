// A TPU's cross-lane unit: one element a cycle enters its stages, one instruction a cycle the unit. The pushes between
// two pops are one tile: the first push after a pop forgets the tile before, in time and in value.
#ifndef TPU_XLU_HPP
#define TPU_XLU_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#include "vcix_uarch.hpp"

#include "values.hpp"

namespace tpu {

// What the cross-lane unit holds, as the Spike it came from kept it.
class XluValues {
  using Host = vcix_accel::Host;
  using Insn = vcix_accel::Insn;

 public:
  // A tile is pushed a register at a time (32 raw bits an element), the first pop after a push moves it across the
  // lanes. The push's field at 19:15: [4:3] the shuffle before, [2] depth and lane exchanged, [1:0] the shuffle after;
  // [4:3] 3 pushes no tile but loads the pattern of the shuffle after. A bad pattern exits INVALID_XLU_PATTERN.
  using Tile = std::vector<std::vector<uint32_t>>;
  enum Shuffle : uint32_t { BYPASS = 0, REPLICATE = 1, ARBITRARY = 2, LOAD = 3 };
  static constexpr int INVALID_XLU_PATTERN = 201;
  static uint32_t xlu_element(const Host &host, const Insn &insn, uint32_t lane, uint32_t reg, uint32_t i) {
    switch (insn.sew_bits) {
      case 8: return get<uint8_t>(host, lane, reg, i);
      case 16: return get<uint16_t>(host, lane, reg, i);
      case 32: return get<uint32_t>(host, lane, reg, i);
      default: return 0;
    }
  }
  void xlu_size(const Host &host) {
    if (xlu_in_.size() == host.lanes()) return;
    xlu_in_.assign(host.lanes(), {});
    xlu_before_.assign(host.lanes(), {});
    xlu_after_.assign(host.lanes(), {});
    xlu_out_.assign(host.lanes(), {});
  }
  void xlu_push(const Host &host, const Insn &insn) {
    xlu_size(host);
    const uint32_t operation = vcix_accel::rs1(insn);
    if (operation >> 3 == LOAD) {
      for (uint32_t lane = 0; lane < host.lanes(); lane++)
        for (uint32_t i = 0; i < insn.vl; i++)
          xlu_after_[lane].push_back(get<uint32_t>(host, lane, vcix_accel::rs2(insn), i));
      return;
    }
    xlu_operation_ = operation;
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++)
        xlu_in_[lane].push_back(xlu_element(host, insn, lane, vcix_accel::rs2(insn), i));
    xlu_depth_ += insn.vl;
  }
  // The pattern of the shuffle before is in the register the rd field names: a lane number of 32 bits per element.
  void xlu_push_pattern(const Host &host, const Insn &insn) {
    xlu_size(host);
    xlu_operation_ = vcix_accel::rs1(insn);
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++) {
        xlu_in_[lane].push_back(xlu_element(host, insn, lane, vcix_accel::rs2(insn), i));
        xlu_before_[lane].push_back(get<uint32_t>(host, lane, vcix_accel::rd(insn), i));
      }
    xlu_depth_ += insn.vl;
  }
  // Every lane reads some lane's row: the lane its pattern names for ARBITRARY, which needs a lane number per
  // column, and lane 0 otherwise. What is missing reads as 0.
  Tile xlu_shuffle(uint32_t what, const Tile &tile, const Tile &pattern, const char *stage) const {
    if (what == BYPASS) return tile;
    const size_t lanes = tile.size();
    size_t width = 0;
    for (const std::vector<uint32_t> &row : tile) width = row.size() > width ? row.size() : width;
    if (what == ARBITRARY)
      for (size_t lane = 0; lane < lanes; lane++)
        if (pattern[lane].size() != width) {
          std::fprintf(stderr, "XLU ERROR: the %s stage walks %zu columns but lane %zu carries %zu pattern entries (SIMM5 %u)\n",
                       stage, width, lane, pattern[lane].size(), xlu_operation_);
          std::exit(INVALID_XLU_PATTERN);
        }
    Tile got(lanes);
    for (size_t lane = 0; lane < lanes; lane++)
      for (size_t k = 0; k < width; k++) {
        const size_t from = what == ARBITRARY ? pattern[lane][k] : 0;
        got[lane].push_back(from < lanes && k < tile[from].size() ? tile[from][k] : 0u);
      }
    return got;
  }
  // Column k becomes lane k's row; only `depth` lanes receive anything.
  static Tile xlu_cross(const Tile &tile, uint64_t depth) {
    const size_t lanes = tile.size();
    Tile got(lanes);
    for (uint64_t k = 0; k < depth && k < lanes; k++)
      for (size_t lane = 0; lane < lanes; lane++) got[k].push_back(k < tile[lane].size() ? tile[lane][k] : 0u);
    return got;
  }
  // Both patterns are taken whether or not the operation reads them.
  void xlu_run() {
    const size_t lanes = xlu_in_.size();
    Tile tile(lanes), before(lanes), after(lanes);
    for (size_t lane = 0; lane < lanes; lane++) {
      for (uint64_t k = 0; k < xlu_depth_ && !xlu_in_[lane].empty(); k++) {
        tile[lane].push_back(xlu_in_[lane].front());
        xlu_in_[lane].pop_front();
      }
      before[lane].assign(xlu_before_[lane].begin(), xlu_before_[lane].end());
      xlu_before_[lane].clear();
      after[lane].assign(xlu_after_[lane].begin(), xlu_after_[lane].end());
      xlu_after_[lane].clear();
    }
    Tile got = xlu_shuffle(xlu_operation_ >> 3, tile, before, "pre");
    if (xlu_operation_ & 4) got = xlu_cross(got, xlu_depth_);
    got = xlu_shuffle(xlu_operation_ & 3, got, after, "post");
    for (size_t lane = 0; lane < lanes; lane++) xlu_out_[lane].assign(got[lane].begin(), got[lane].end());
    xlu_depth_ = 0;
  }
  // A lane the operation gave nothing keeps what vd held.
  void xlu_pop(const Host &host, const Insn &insn) {
    xlu_size(host);
    if (xlu_depth_ > 0) xlu_run();
    const uint32_t vd = vcix_accel::rd(insn);
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < insn.vl; i++) {
        if (xlu_out_[lane].empty()) break;
        const uint32_t value = xlu_out_[lane].front();
        xlu_out_[lane].pop_front();
        switch (insn.sew_bits) {
          case 8: put<uint8_t>(host, lane, vd, i, static_cast<uint8_t>(value)); break;
          case 16: put<uint16_t>(host, lane, vd, i, static_cast<uint16_t>(value)); break;
          default: put<uint32_t>(host, lane, vd, i, value); break;
        }
      }
  }

 private:
  std::vector<std::deque<uint32_t>> xlu_in_, xlu_before_, xlu_after_, xlu_out_;
  uint64_t xlu_depth_ = 0;
  uint32_t xlu_operation_ = 0;
};

// The values are a register, not the pipeline's entries: a push of the pattern ([4:3] 3) enters the stages but no
// tile, and the first pop computes the whole tile, whose rows outlive the pops that take part of them.
class Xlu {
  using Host = vcix_uarch::Host;
  using Route = vcix_uarch::Route;

 public:
  // `name` is the unit as its statistics report it; the model that holds the unit says it.
  explicit Xlu(const char *name)
      : entry_{name, "input", "elements", 1, vcix_uarch::PRIMARY}, issue_{name, "issue", "instructions", 1} {}
  void configure(const vcix_uarch::Config &config) {
    const uint64_t latency = config.uint(LATENCY_KEY, DEFAULT_LATENCY);
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    if (latency < 1 || latency > UINT32_MAX)
      throw vcix_accel::ConfigError(LATENCY_KEY, config.get(LATENCY_KEY), "is not a latency the unit can have");
    if (entries < 1 || entries > UINT32_MAX)
      throw vcix_accel::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    stages_.configure(latency, entries, vcix_uarch::Stall::WHEN_OUTPUT_FULL);
  }
  template <class H>
  void hardware(H &&h) {
    h.port(entry_), h.port(issue_);
    h.pipeline(stages_, entry_);
    h.reg(values_), h.reg(popped_);
  }

  // vl elements into the stages, after forgetting the tile before when a pop came last. arg() 1: xlu_push_pattern,
  // whose rd names the shuffle's pattern.
  void push(Route &r) {
    r.use(issue_).at_issue([&] {
      if (popped_.get()) stages_.flush(), popped_.write() = false;
    });
    r.send(stages_, r.vl());
    r.compute([&](const Host &host) {
      if (r.arg()) values_.write().xlu_push_pattern(host, r.insn());
      else values_.write().xlu_push(host, r.insn());
    });
  }
  // Up to vl elements of the output queue, once vl are out or none is on its way.
  void pop(Route &r) {
    r.use(issue_).receive_at_most(stages_, r.vl()).at_issue([&] {
      if (!popped_.get()) popped_.write() = true;
    });
    r.compute([&](const Host &host) { values_.write().xlu_pop(host, r.insn()); });
  }

  static constexpr uint32_t FORM = 0xFE00707F;
  static constexpr vcix_uarch::Instruction<Xlu> INSTRUCTIONS[] = {{0x2E00305B, FORM, "xlu_push", &Xlu::push, 0},
                                                                  {0xAE00305B, FORM, "xlu_push_pattern", &Xlu::push, 1},
                                                                  {0x0400305B, FORM, "xlu_pop", &Xlu::pop}};

 private:
  // TODO: the unit's latency is not known; 10 is a placeholder.
  static constexpr uint32_t DEFAULT_LATENCY = 10;
  static constexpr uint32_t DEFAULT_ENTRIES = 256;
  static constexpr const char *LATENCY_KEY = "tpu_xlu_latency_cycles";
  static constexpr const char *QUEUE_KEY = "tpu_xlu_queue_entries";

  vcix_uarch::Port entry_, issue_;
  vcix_uarch::Pipeline<> stages_;
  vcix_uarch::Register<XluValues> values_;
  vcix_uarch::Register<bool> popped_;
};

}  // namespace tpu

#endif
