// C++ face of coprocessor.h: subclass Model, then VCIX_CP_REGISTER(YourModel). The cp engine (vcix_cp.hpp) is built on it.
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

#include "coprocessor.h"

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

class Component;

// A statistic `group`.`name` whose value add() only grows. capacity > 0: a unit doing at most capacity a cycle, which
// TOGSim times; 0: a plain count. Names must outlive the model.
class Counter {
 public:
  Counter(const char *group, const char *name, uint64_t capacity = 0) : group_(group), name_(name), capacity_(capacity) {
    if (!group || !name) throw std::invalid_argument("a counter's group and name are named");
  }
  // Of component c, its group c.name(): vcix_cp.hpp.
  Counter(Component &c, const char *name, uint64_t capacity = 0);
  // In configure only.
  void set_capacity(uint64_t capacity) { capacity_ = capacity; }
  void add(uint64_t n) { value_ += n; }

  const char *group() const { return group_; }
  const char *name() const { return name_; }
  uint64_t capacity() const { return capacity_; }
  uint64_t value() const { return value_; }

 private:
  const char *group_, *name_;
  uint64_t capacity_;
  uint64_t value_ = 0;
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

  // Its counters, the same list each time. Asked anew: keep no pointer.
  virtual std::vector<const Counter *> counters() const { return {}; }

 protected:
  // True while commit and tick are run again after a squash: skip what must happen once.
  bool replaying() const { return replaying_; }
  // Throws if the model's state is not all inside its bytes (sizeof the exported class), before configure.
  virtual void check_layout(size_t) const {}

 private:
  template <class M>
  friend class Instance;
  bool replaying_ = false;
};

// Copies the model before each issue; a squash puts the copy back and runs again the ticks and commits since.
template <class M>
class Instance {
  static_assert(std::is_copy_constructible<M>::value && std::is_copy_assignable<M>::value,
                "a model must be copyable: a squashed instruction is undone from a copy");

 public:
  M model;

  Instance() : encodings_(model.owns()), committed_(encodings_.size(), 0) {}

  // Configures the model, then fixes its list of statistics; a counter list or a layout that breaks the rules throws.
  // The layout is checked before configure (blocks the constructors made) and after it (blocks configure made).
  void configure(const Config &config) {
    static_cast<const Model &>(model).check_layout(sizeof(M));
    model.configure(config);
    static_cast<const Model &>(model).check_layout(sizeof(M));
    describe();
  }
  void tick(Cycle now) { model.tick(now); }
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
  // Each counter's value in the order of counters(), then each encoding's commits.
  void read_stats(uint64_t *values) const {
    const std::vector<const Counter *> counters = model.counters();
    if (counters.size() != counted_) broken("counters() changed its length since create");
    for (size_t i = 0; i < counters.size(); i++) {
      if (std::strcmp(counters[i]->group(), stats_[i].group) || std::strcmp(counters[i]->name(), stats_[i].name) ||
          counters[i]->capacity() != stats_[i].capacity)
        broken("counters() changed since create");
      values[i] = counters[i]->value();
    }
    for (size_t i = 0; i < committed_.size(); i++) values[counted_ + i] = committed_[i];
  }
  // The values as gem5 names them, vcix.<group>.<name>, each with its description.
  void dump_stats(FILE *out) const {
    std::vector<uint64_t> values(stats_.size());
    read_stats(values.data());
    for (size_t i = 0; i < stats_.size(); i++) {
      const vcix_stat &s = stats_[i];
      const std::string at = "vcix." + stat_name(s.group) + "." + stat_name(s.name);
      if (s.capacity)
        std::fprintf(out, "%-48s %" PRIu64 "  # %s, at most %" PRIu64 " a cycle\n", at.c_str(), values[i], s.name, s.capacity);
      else
        std::fprintf(out, "%-48s %" PRIu64 "  # %s\n", at.c_str(), values[i], s.name);
    }
  }
  // A name as a gem5 statistic: each character outside [A-Za-z0-9_] becomes '_'.
  static std::string stat_name(const char *name) {
    std::string out(name);
    for (char &c : out)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
    return out;
  }

 private:
  [[noreturn]] static void broken(const char *why) {
    std::fprintf(stderr, "vcix_cp: %s\n", why);
    std::abort();
  }
  const char *keep(const char *text) { return strings_.emplace_back(text).c_str(); }
  // Fixes the stat list: the counters, then the commits; names are checked as stat_name() prints them.
  void describe() {
    stats_.clear();
    strings_.clear();
    const std::vector<const Counter *> counters = model.counters();
    for (size_t i = 0; i < counters.size(); i++) {
      const Counter &c = *counters[i];
      const std::string group = stat_name(c.group()), name = stat_name(c.name());
      if (group == "committed") throw std::invalid_argument("a counter's group is named 'committed', the name of the commit counts");
      for (size_t j = 0; j < i; j++) {
        const Counter &other = *counters[j];
        if (stat_name(other.group()) != group) continue;
        if (std::strcmp(other.group(), c.group()))
          throw std::invalid_argument(std::string("groups ") + other.group() + " and " + c.group() + " are both " + group +
                                      " as statistics");
        if (stat_name(other.name()) == name)
          throw std::invalid_argument(std::string("counters ") + c.group() + "." + other.name() + " and " + c.group() + "." +
                                      c.name() + " are both vcix." + group + "." + name + " as statistics");
      }
      stats_.push_back({keep(c.group()), keep(c.name()), c.capacity()});
    }
    counted_ = counters.size();
    for (size_t i = 0; i < encodings_.size(); i++)
      for (size_t j = 0; j < i; j++)
        if (stat_name(encodings_[j].name) == stat_name(encodings_[i].name))
          throw std::invalid_argument(std::string("encodings '") + encodings_[j].name + "' and '" + encodings_[i].name +
                                      "' are both committed." + stat_name(encodings_[i].name) + " as statistics");
    const char *committed = keep("committed");
    for (const Encoding &e : encodings_) stats_.push_back({committed, keep(e.name), 0});
  }

  std::vector<Encoding> encodings_;
  std::vector<uint64_t> committed_;
  size_t counted_ = 0;
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
      COPROCESSOR_ABI_VERSION,
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
  extern "C" __attribute__((visibility("default"))) const vcix_model *coprocessor_model(void) { \
    return cp::export_model_or_null<ModelClass>();                                 \
  }

