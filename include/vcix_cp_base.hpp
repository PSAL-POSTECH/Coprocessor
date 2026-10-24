// C++ face of vcix_accel.h: subclass Model, then VCIX_CP_REGISTER(YourModel). The cp engine (vcix_cp.hpp) is built on it.
#pragma once

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "vcix_accel.h"

namespace cp {

using Cycle = vcix_cycle_t;
using Encoding = vcix_encoding;
using Id = vcix_id_t;
// Returned by issue when the latency is not known yet: ready() says when the result is ready.
constexpr Cycle Unknown = VCIX_LATENCY_UNKNOWN;

inline uint32_t rd(const vcix_insn &insn) { return (insn.bits >> 7) & 0x1f; }
inline uint32_t rs1(const vcix_insn &insn) { return (insn.bits >> 15) & 0x1f; }
inline uint32_t rs2(const vcix_insn &insn) { return (insn.bits >> 20) & 0x1f; }
inline uint32_t funct3(const vcix_insn &insn) { return (insn.bits >> 12) & 0x7; }
// Registers in one vector operand's group.
inline uint32_t group_size(const vcix_insn &insn) { return insn.lmul_log2 > 0 ? 1u << insn.lmul_log2 : 1u; }

// Thrown from configure, it stops the run with this message.
class ConfigError : public std::runtime_error {
 public:
  ConfigError(const std::string &key, const std::string &value, const std::string &why)
      : std::runtime_error("machine description: " + key + ": '" + value + "' " + why) {}
};

class Config {
 public:
  explicit Config(const vcix_config *c) : c_(c) {}
  // nullptr when the key is absent. Valid until configure returns.
  const char *get(const std::string &key) const { return c_->get(c_->ctx, key.c_str()); }
  // Unsigned decimal; `fallback` only for an absent key.
  uint64_t uint(const std::string &key, uint64_t fallback) const {
    const char *value = get(key);
    if (!value) return fallback;
    const char *end = value + std::strlen(value);
    uint64_t number = 0;
    const std::from_chars_result parsed = std::from_chars(value, end, number, 10);
    if (parsed.ec == std::errc::invalid_argument || parsed.ptr != end)
      throw ConfigError(key, value, "is not an unsigned decimal number");
    if (parsed.ec == std::errc::result_out_of_range) throw ConfigError(key, value, "does not fit in 64 bits");
    if (value[0] == '0' && end - value > 1)
      throw ConfigError(key, value, "has a leading zero, which reads as octal or as decimal depending on the reader");
    return number;
  }
  // Written 0x...; `fallback` only for an absent key.
  uint64_t hex(const std::string &key, uint64_t fallback) const {
    const char *value = get(key);
    if (!value) return fallback;
    if (std::strncmp(value, "0x", 2) != 0) throw ConfigError(key, value, "is not a hexadecimal number written 0x...");
    const char *end = value + std::strlen(value);
    uint64_t number = 0;
    const std::from_chars_result parsed = std::from_chars(value + 2, end, number, 16);
    if (parsed.ec == std::errc::invalid_argument || parsed.ptr != end)
      throw ConfigError(key, value, "is not a hexadecimal number written 0x...");
    if (parsed.ec == std::errc::result_out_of_range) throw ConfigError(key, value, "does not fit in 64 bits");
    return number;
  }

 private:
  const vcix_config *c_;
};

class Host {
 public:
  explicit Host(const vcix_host *h) : h_(h) {}
  uint32_t lanes() const { return h_->lanes(h_->ctx); }
  uint32_t vlen_bits() const { return h_->vlen_bits(h_->ctx); }
  template <class T>
  const T *vreg(uint32_t lane, uint32_t reg) const {
    return static_cast<const T *>(h_->vreg(h_->ctx, lane, reg, 0));
  }
  template <class T>
  T *vreg_mut(uint32_t lane, uint32_t reg) const {
    return static_cast<T *>(h_->vreg(h_->ctx, lane, reg, 1));
  }
  uint64_t xreg(uint32_t reg) const { return h_->xreg_read(h_->ctx, reg); }
  void set_xreg(uint32_t reg, uint64_t v) const { h_->xreg_write(h_->ctx, reg, v); }
  uint64_t freg_bits(uint32_t reg) const { return h_->freg_bits(h_->ctx, reg); }
  uint64_t csr(uint32_t number) const { return h_->csr_read(h_->ctx, number); }
  void mem_read(uint64_t addr, void *dst, size_t n) const { h_->mem_read(h_->ctx, addr, dst, n); }
  void mem_write(uint64_t addr, const void *src, size_t n) const { h_->mem_write(h_->ctx, addr, src, n); }

 private:
  const vcix_host *h_;
};

// Where a unit decides how much more enters this cycle; it counts what it let in. Names must outlive the model.
class Stats {
 public:
  enum Role : uint32_t { REPORTING = 0, PRIMARY = 1 };
  Stats(const char *unit, const char *name, const char *unit_of_work, uint64_t capacity_per_cycle, Role role = REPORTING)
      : unit_(unit), name_(name), unit_of_work_(unit_of_work), capacity_(capacity_per_cycle), role_(role) {
    if (!unit || !name || !unit_of_work) throw std::invalid_argument("a port's unit, name and unit of work are named");
    set_capacity(capacity_per_cycle);
  }
  // In configure only: utilized cycles read one capacity for the whole run.
  void set_capacity(uint64_t capacity_per_cycle) {
    if (capacity_per_cycle == 0) throw std::invalid_argument(std::string("port ") + unit_ + "." + name_ + ": capacity 0");
    capacity_ = capacity_per_cycle;
  }

  uint64_t available(Cycle now) const { return now == cycle_ ? capacity_ - used_ : capacity_; }
  // More than available(now) is a broken model: the run stops.
  void admit(uint64_t amount, Cycle now) {
    const uint64_t left = available(now);
    if (amount > left) {
      std::fprintf(stderr, "vcix_cp: port %s.%s admitted %" PRIu64 " %s in cycle %" PRIu64 " with room for %" PRIu64 "\n",
                   unit_, name_, amount, unit_of_work_, now, left);
      std::abort();
    }
    if (amount) {
      reopened_ += now == reset_in_;
      reset_in_ = Unknown;
    }
    if (now != cycle_) used_ = 0;
    cycle_ = now;
    used_ += amount;
    admitted_ += amount;
  }
  // Adds `amount` to occupancy. vcix_cp adds a pipeline's rows in its stages, for each pipeline this port is the
  // entry of, in each cycle that pipeline is not drained.
  void hold(uint64_t amount) { occupancy_ += amount; }
  // Forgets what this cycle admitted, as a model's reset must; the counts stay. A room it used is opened again,
  // and the next admission in that same cycle counts in reopened(), so admitted <= capacity * (cycles + reopened).
  void reset() {
    if (used_) reset_in_ = cycle_;
    cycle_ = Unknown;
    used_ = 0;
  }

  const char *unit() const { return unit_; }
  const char *name() const { return name_; }
  const char *unit_of_work() const { return unit_of_work_; }
  uint64_t capacity() const { return capacity_; }
  bool primary() const { return role_ == PRIMARY; }
  uint64_t admitted() const { return admitted_; }
  uint64_t occupancy() const { return occupancy_; }
  // Rooms a reset opened a second time within one cycle and that were then used.
  uint64_t reopened() const { return reopened_; }

 private:
  const char *unit_, *name_, *unit_of_work_;
  uint64_t capacity_;
  Role role_;
  Cycle cycle_ = Unknown;
  uint64_t used_ = 0;
  uint64_t admitted_ = 0;
  uint64_t occupancy_ = 0;
  Cycle reset_in_ = Unknown;
  uint64_t reopened_ = 0;
};

// Must be copyable with its state in its members. name() and owns() are asked of an object never configured.
class Model {
 public:
  virtual ~Model() = default;
  virtual const char *name() const = 0;
  virtual std::vector<Encoding> owns() const = 0;
  virtual void configure(const Config &) {}

  virtual void execute(const Host &host, const vcix_insn &insn) = 0;

  // In a cycle: tick, then ready, then commit, then can_accept and issue. A squashed instruction was never issued.
  virtual bool can_accept(const vcix_insn &insn, Cycle now) const = 0;
  virtual Cycle issue(const vcix_insn &insn, Id id, Cycle now) = 0;
  virtual void commit(const vcix_insn &, Id, Cycle) {}
  virtual void tick(Cycle) {}
  virtual bool ready(Id, Cycle) const { return true; }

  virtual void reset() {}

  // The ports that decide its timing, one PRIMARY per unit, the same list each time. Asked anew: keep no pointer.
  // Only each unit's PRIMARY port is a statistic; the others only time.
  virtual std::vector<const Stats *> ports() const { return {}; }
  // Cycles ticked since create, the ticks run again after a squash included.
  uint64_t cycles() const { return cycles_; }

 protected:
  // True while commit and tick are run again after a squash: skip what must happen once.
  bool replaying() const { return replaying_; }
  // Throws if the model's state is not all inside its bytes (sizeof the exported class), before configure.
  virtual void check_layout(size_t) const {}

 private:
  template <class M>
  friend class Instance;
  bool replaying_ = false;
  uint64_t cycles_ = 0;
};

// Copies the model before each issue; a squash puts the copy back and runs again the ticks and commits since.
template <class M>
class Instance {
  static_assert(std::is_copy_constructible<M>::value && std::is_copy_assignable<M>::value,
                "a model must be copyable: a squashed instruction is undone from a copy");

 public:
  M model;

  Instance() : encodings_(model.owns()), committed_(encodings_.size(), 0) {}

  // Configures the model, then fixes its list of statistics; a port list or a layout that breaks the rules throws.
  // The layout is checked before configure (blocks the constructors made) and after it (blocks configure made).
  void configure(const Config &config) {
    static_cast<const Model &>(model).check_layout(sizeof(M));
    model.configure(config);
    static_cast<const Model &>(model).check_layout(sizeof(M));
    describe();
  }
  void tick(Cycle now) {
    model.cycles_++;
    model.tick(now);
  }
  Cycle issue(const vcix_insn &insn, Id id, Cycle now) {
    issued_.push_back({id, now, model});
    return model.issue(insn, id, now);
  }
  void commit(const vcix_insn &insn, Id id, Cycle now) {
    for (size_t i = 0; i < encodings_.size(); i++)
      if ((insn.bits & encodings_[i].mask) == encodings_[i].match) {
        committed_[i]++;
        break;
      }
    model.commit(insn, id, now);
    if (!issued_.empty()) issued_.pop_front();
    while (!commits_.empty() && (issued_.empty() || commits_.front().cycle <= issued_.front().cycle))
      commits_.pop_front();
    if (!issued_.empty()) commits_.push_back({insn, id, now});
  }
  void squash(Id first, Cycle now) {
    auto from = issued_.begin();
    while (from != issued_.end() && from->id < first) ++from;
    if (from == issued_.end()) return;
    const Cycle issued = from->cycle;
    model = std::move(from->before);
    issued_.erase(from, issued_.end());
    auto commit = commits_.begin();
    while (commit != commits_.end() && commit->cycle <= issued) ++commit;
    model.replaying_ = true;
    for (Cycle cycle = issued + 1; cycle <= now; cycle++) {
      tick(cycle);
      for (; commit != commits_.end() && commit->cycle == cycle; ++commit)
        model.commit(commit->insn, commit->id, cycle);
    }
    model.replaying_ = false;
  }
  void reset() {
    issued_.clear();
    commits_.clear();
    model.reset();
  }

  const std::vector<vcix_stat> &stats() const { return stats_; }
  // Each unit's ADMITTED, CAPACITY, CYCLES and OCCUPANCY at its primary port, units in the order of their primary
  // ports in ports(), then each encoding's commits. CYCLES is the ticks plus the port's reopened() rooms.
  void read_stats(uint64_t *values) const {
    const std::vector<const Stats *> ports = model.ports();
    if (ports.size() != listed_.size()) broken("ports() changed its length since create");
    for (size_t p = 0; p < ports.size(); p++)
      if (std::strcmp(ports[p]->unit(), listed_[p].first) || std::strcmp(ports[p]->name(), listed_[p].second))
        broken("ports() changed its order since create");
    for (size_t u = 0; u < primary_.size(); u++) {
      const Stats *port = ports[primary_[u]];
      values[PER_UNIT * u] = port->admitted();
      values[PER_UNIT * u + 1] = port->capacity();
      values[PER_UNIT * u + 2] = model.cycles() + port->reopened();
      values[PER_UNIT * u + 3] = port->occupancy();
    }
    for (size_t i = 0; i < committed_.size(); i++) values[PER_UNIT * primary_.size() + i] = committed_[i];
  }
  // The values as gem5 names them: vcix.<unit>.<stat>, vcix.<unit>.utilized_cycles, vcix.committed.<encoding>.
  void dump_stats(FILE *out) const {
    std::vector<uint64_t> values(stats_.size());
    read_stats(values.data());
    for (size_t u = 0; u < primary_.size(); u++) {
      const vcix_stat *unit = &stats_[PER_UNIT * u];
      const uint64_t *v = &values[PER_UNIT * u];
      const std::string at = "vcix." + stat_name(unit->unit) + ".";
      std::fprintf(out, "%-48s %" PRIu64 "  # %s admitted at port %s\n", (at + "admitted").c_str(), v[0],
                   unit->unit_of_work, unit->name);
      std::fprintf(out, "%-48s %" PRIu64 "  # %s per cycle\n", (at + "capacity").c_str(), v[1], unit->unit_of_work);
      std::fprintf(out, "%-48s %" PRIu64 "\n", (at + "cycles").c_str(), v[2]);
      std::fprintf(out, "%-48s %" PRIu64 "  # %s held, summed over cycles\n", (at + "occupancy").c_str(), v[3],
                   unit->unit_of_work);
      std::fprintf(out, "%-48s %f  # admitted / capacity\n", (at + "utilized_cycles").c_str(),
                   double(v[0]) / double(v[1]));
    }
    for (size_t i = 0; i < committed_.size(); i++)
      std::fprintf(out, "%-48s %" PRIu64 "\n", ("vcix.committed." + stat_name(stats_[PER_UNIT * primary_.size() + i].name)).c_str(),
                   values[PER_UNIT * primary_.size() + i]);
  }
  // A name as a gem5 statistic: each character outside [A-Za-z0-9_] becomes '_'.
  static std::string stat_name(const char *name) {
    std::string out(name);
    for (char &c : out)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
    return out;
  }

 private:
  static constexpr size_t PER_UNIT = 4;

  [[noreturn]] static void broken(const char *why) {
    std::fprintf(stderr, "vcix_cp: %s\n", why);
    std::abort();
  }
  const char *keep(const char *text) { return strings_.emplace_back(text).c_str(); }
  // Fixes the stat list from each unit's one primary port; units and encodings are checked as stat_name() prints them.
  void describe() {
    stats_.clear();
    strings_.clear();
    listed_.clear();
    primary_.clear();
    const std::vector<const Stats *> ports = model.ports();
    for (size_t i = 0; i < ports.size(); i++) {
      const Stats *port = ports[i];
      const std::string unit = stat_name(port->unit());
      if (unit == "committed")
        throw std::invalid_argument("a port's unit is named 'committed', the name of the commit counts");
      size_t primaries = 0;
      for (const Stats *other : ports) {
        if (stat_name(other->unit()) != unit) continue;
        if (std::strcmp(other->unit(), port->unit()))
          throw std::invalid_argument(std::string("units ") + port->unit() + " and " + other->unit() + " are both " +
                                      unit + " as statistics");
        primaries += other->primary();
      }
      if (primaries != 1)
        throw std::invalid_argument(std::string("unit ") + port->unit() + " has " + std::to_string(primaries) +
                                    " primary ports, not one");
      const char *unit_kept = keep(port->unit()), *name_kept = keep(port->name());
      listed_.push_back({unit_kept, name_kept});
      if (!port->primary()) continue;
      primary_.push_back(i);
      const char *work = keep(port->unit_of_work());
      for (uint32_t kind : {VCIX_STAT_ADMITTED, VCIX_STAT_CAPACITY, VCIX_STAT_CYCLES, VCIX_STAT_OCCUPANCY})
        stats_.push_back({unit_kept, name_kept, work, kind});
    }
    for (size_t i = 0; i < encodings_.size(); i++)
      for (size_t j = 0; j < i; j++)
        if (stat_name(encodings_[j].name) == stat_name(encodings_[i].name))
          throw std::invalid_argument(std::string("encodings '") + encodings_[j].name + "' and '" + encodings_[i].name +
                                      "' are both committed." + stat_name(encodings_[i].name) + " as statistics");
    const char *committed = keep("committed"), *instructions = keep("instructions");
    for (const Encoding &e : encodings_) stats_.push_back({committed, keep(e.name), instructions, VCIX_STAT_COUNT});
  }

  std::vector<Encoding> encodings_;
  std::vector<uint64_t> committed_;
  std::vector<std::pair<const char *, const char *>> listed_;
  std::vector<size_t> primary_;
  std::vector<vcix_stat> stats_;
  std::deque<std::string> strings_;

  struct Issued {
    Id id;
    Cycle cycle;
    M before;
  };
  struct Committed {
    vcix_insn insn;
    Id id;
    Cycle cycle;
  };
  std::deque<Issued> issued_;
  std::deque<Committed> commits_;
};

// The table of model M. No exception from M crosses the C boundary but Spike's traps out of execute, which leave the
// model as it was before that execute, so Spike's retry of the instruction runs it once.
template <class M>
__attribute__((visibility("hidden"))) const vcix_model *export_model() {
  struct Description {
    const char *name;
    std::vector<Encoding> encodings;
  };
  static const Description description = [] {
    M asked;
    return Description{asked.name(), asked.owns()};
  }();
  static const vcix_model table = {
      VCIX_ACCEL_ABI_VERSION,
      description.name,
      description.encodings.data(),
      description.encodings.size(),
      [](const vcix_config *c, char *error, size_t error_size) -> void * {
        try {
          std::unique_ptr<Instance<M>> instance(new Instance<M>);
          instance->configure(Config(c));
          return instance.release();
        } catch (const std::exception &e) {
          std::snprintf(error, error_size, "%s", e.what());
        } catch (...) {
          std::snprintf(error, error_size, "an exception that is not a std::exception");
        }
        return nullptr;
      },
      [](void *s) { delete static_cast<Instance<M> *>(s); },
      [](void *s, const vcix_host *h, const vcix_insn *i) {
        M &model = static_cast<Instance<M> *>(s)->model;
        M saved = model;
        try {
          model.execute(Host(h), *i);
        } catch (...) {
          model = std::move(saved);
          throw;
        }
      },
      [](void *s, const vcix_insn *i, Cycle n) -> int {
        return static_cast<const Instance<M> *>(s)->model.can_accept(*i, n);
      },
      [](void *s, const vcix_insn *i, Id id, Cycle n) -> Cycle {
        return static_cast<Instance<M> *>(s)->issue(*i, id, n);
      },
      [](void *s, Id first, Cycle n) { static_cast<Instance<M> *>(s)->squash(first, n); },
      [](void *s, const vcix_insn *i, Id id, Cycle n) { static_cast<Instance<M> *>(s)->commit(*i, id, n); },
      [](void *s, Cycle n) { static_cast<Instance<M> *>(s)->tick(n); },
      [](void *s, Id id, Cycle n) -> int { return static_cast<const Instance<M> *>(s)->model.ready(id, n); },
      [](void *s) { static_cast<Instance<M> *>(s)->reset(); },
      [](void *s) -> size_t { return static_cast<const Instance<M> *>(s)->stats().size(); },
      [](void *s, size_t i) -> const vcix_stat * {
        const std::vector<vcix_stat> &stats = static_cast<const Instance<M> *>(s)->stats();
        return i < stats.size() ? &stats[i] : nullptr;
      },
      [](void *s, uint64_t *values) { static_cast<const Instance<M> *>(s)->read_stats(values); },
  };
  return &table;
}

template <class M>
__attribute__((visibility("hidden"))) const vcix_model *export_model_or_null() {
  try {
    return export_model<M>();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "vcix_cp: the model cannot be made: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "vcix_cp: the model cannot be made: an exception that is not a std::exception\n");
  }
  return nullptr;
}

}  // namespace cp

#define VCIX_CP_REGISTER(ModelClass)                                                    \
  extern "C" __attribute__((visibility("default"))) const vcix_model *vcix_accel_model(void) { \
    return cp::export_model_or_null<ModelClass>();                                 \
  }

