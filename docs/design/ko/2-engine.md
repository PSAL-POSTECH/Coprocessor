# 엔진

## 1. 층 구조

시뮬레이터는 C ABI의 함수 표만 알며, C ABI(`vcix_accel.h`)는 이 재설계에서 바뀌지 않습니다. 수치는 일회용 드라이버 결과이며 gem5/Spike 실행값이 아닙니다.

```
   gem5 MinorCPU (timing)                              Spike (functional)
   tick  ready  commit  squash  can_accept  issue        execute
      |                                                    |
      v                                                    v
 +--------------------------------------------------------------------------+
 | C ABI  vcix_accel.h : struct vcix_model (function table, stats kinds)    |
 +--------------------------------------------------------------------------+
      |                                                    |
 +--------------------------------------------------------------------------+
 | cp::export_model<M>()  fills the table                           |
 |   issue/commit/squash/tick -> Instance<M>    can_accept/ready -> model   |
 |   execute -> saved = model; model.execute(); on throw: model = saved     |
 | cp::Instance<M>                                                  |
 |   copy model before each issue, restore + re-run ticks on squash         |
 |   configure: check_layout -> model.configure -> check_layout -> stats    |
 +--------------------------------------------------------------------------+
      |
 +--------------------------------------------------------------------------+
 | cp::Coprocessor  (is a cp::Model)                                |
 |   route insn by encoding -> owner.insns().push()                         |
 |   tick(t): every Component::cycle(), then every Block::cycle()     |
 |   done_ (ids completed, not yet committed)   Spike: until(...) loop      |
 |   +--- cp::Component "SFU" ------------+   +--- cp::Component "MISC" --+ |
 |   | insns(): Queue<Insn> (2, 1)        |   | insns()                   | |
 |   | cycle()   complete(insn)        |   | cycle()                | |
 |   | blocks: Port  Queue<T>  Pipeline<T>|   | Port                      | |
 |   | plain members (copyable values)    |   | plain members             | |
 |   +------------------------------------+   +---------------------------+ |
 +--------------------------------------------------------------------------+
```

## 2. 한 사이클의 순서 (gem5)

issue는 `insns()`에 넣기만 하고, 그 밖의 모든 움직임은 사용자의 `cycle()`에 있습니다.

1. `tick(t)`: 생성 순서대로 모든 컴포넌트의 `cycle()`, 그다음 모든 블록의 `cycle()` (Pipeline은 여기서만 나이를 먹음).
2. `ready(id, t)` / `commit(insn, id, t)`: id가 `done_`에 있는지 / 지움. `cycle()`에서 `complete()`했으면 같은 t에 참입니다.
3. `squash(first, t)`: `Instance`가 복사본을 복원합니다 (§5).
4. `can_accept(insn, t)`: `owner && valid(insn) && !insns().full(t)`, const입니다.
5. `issue(insn, id, t)`: `Instance`가 모델을 복사한 뒤 `insns().push(insn, t)`, `Unknown`을 돌려줍니다.

예: `insns()` `(2, 1)`, 지연 L=10, interval 1에서 연달아 오는 독립 vexp A, B를 t=1부터 제시하면 둘 다 issue에서 ready까지 11 사이클(= L+1)입니다.

| t | `tick(t)`: `SFU.cycle()` | `cycle()` | gem5 쪽 (`tick` 뒤) |
|---|---|---|---|
| 1 | `insns()` 비어 있음 (`length(1)=0`) | 없음 | `can_accept(A,1)`: `available(1)=min(2−0−0, 1−0)=1` → issue A. `can_accept(B,1)`: `available(1)=min(2−1, 1−1)=0` → 거절 |
| 2 | A가 보임 (`length(2)=1`). `eup_.push(A의 레지스터 0, 2)`, `insns().pop(2)`, `entry_.utilize(1,2)` | A 나이 1 (단계 k=0) | `ready(A,2)` 거짓. `can_accept(B,2)`: `available(2)=min(2−0−1, 1−0)=1` → issue B |
| 3 | B가 보임. `available(3)`: 3−2 ≥ 1 → `eup_.push(B의 레지스터 0, 3)`, pop, utilize | A 2, B 1 | |
| 4–10 | 할 일 없음 | 매 사이클 모두 한 단계 | |
| 11 | `length(11)` 거짓 (A의 나이 9) | A 나이 10 = L → `done_at=11`. B 9 | |
| 12 | `length(12)`: 막힘 && 11 < 12 → `pop(A)`(그룹의 마지막 레지스터), `values(...)`(gem5에서는 실행 안 됨), `complete(A)` | B 나이 10 → `done_at=12` | `ready(A,12)` 참 → `commit(A,12)` |
| 13 | `pop(B)`, `complete(B)` | 비어 있음 | `ready(B,13)` 참 → `commit(B,13)` |

드라이버의 vexp 5개 연속 결과: `vexp burst issues: 1 2 3 4 5  ready-issue: 11 11 11 11 11`.

## 3. 보임 규칙

블록에 들어간 것은 다음 사이클에 보이고 비운 칸은 다음 사이클에 비므로, 컴포넌트들의 `cycle()` 호출 순서는 결과를 바꾸지 않습니다. 같은 컴포넌트 안의 plain 멤버만 즉시 보입니다.

| t에 일어난 일 | 누가 | 처음 보이는 곳 |
|---|---|---|
| `issue(insn, t)`가 `insns()`에 넣음 | gem5 / Spike | `tick(t+1)` |
| `q.push(row)` at t | 사용자 | `tick(t+1)`. `length()`, `front()`, `pop()` of t에는 안 보임 |
| `q.pop()` at t | 사용자 | 빈 칸은 `tick(t+1)`부터 `full()`/`available()`에 보임. `insns()`라면 `can_accept(t+1)` |
| t에 `per_cycle`개 push | 사용자 / gem5 | t의 나머지 동안 `full()` 참. `tick(t+1)`부터 `available()`을 다시 셈 |
| `p.push(row)` at t | 사용자 | `cycle()`에서 단계 k = 0 |
| 행이 `cycle()`에서 나이 latency에 도달 | 파이프라인 | `length()`, `front()`, `pop()` from `tick(t+1)` |
| `p.pop()` at t | 사용자 | `available()` 즉시 1 (MinorFU의 단계 넘김) |
| 끝난 머리를 `cycle()`에서 안 가져감 | 사용자 | `cycle()`에서 파이프라인 전체 정지 |
| `port.utilize(n)` at t | 사용자 | t의 나머지 동안 `available()`이 줄고, `tick(t+1)`부터 다시 width |
| `complete(insn)` in `cycle()` | 사용자 | `ready(id, t)` 참. Spike의 `execute`는 `tick(t)` 뒤에 끝남 |
| plain 멤버를 `cycle()`에서 씀 | 사용자 | 즉시, 그 컴포넌트 안에서: 뒤의 코드와 `can_accept(t)`의 `valid()` |
| `values(f)` in `cycle()` | 사용자 | Spike의 레지스터와 메모리에 즉시 |

## 4. `insns()`의 처리량

비운 칸이 t+1에야 비므로 `p_t = min(W, d - p_{t-1})`이고, d ≥ 2W이면 매 사이클 W개가 유지됩니다.
그래서 기본값 `(2, 1)`은 사이클당 1개를 받으며(probe: `1 1 1 1 1 1 1 1`), `(3, 2)`는 `2 1 2 1 2 1 2 1`이 됩니다.

## 5. squash와 복사 안전성

squash는 issue 직전의 모델 복사본으로 되돌린 뒤 그 사이의 tick과 commit을 다시 실행합니다.

- 모델은 복사 가능해야 하며 `static_assert`로 검사합니다(참조 멤버는 컴파일 오류). 모델 안의 연결은 포인터가 아니라 바이트 오프셋(`to_cp_`, `to_owner_`)이고, `check_layout(sizeof(M))`이 configure 앞뒤로 블록이 모델 밖에 있으면 거절합니다.
- 형제 컴포넌트를 가리키는 raw 포인터는 검사할 수 없어 규칙으로만 금지합니다. 유일한 raw 포인터 `Coprocessor::host_`는 scope guard가 지웁니다.
- 한 번만 일어날 일(출력)은 다시 실행하는 동안 건너뜁니다. probe: `PASS  squash: a wrong-path issue at 3, squashed at 6, leaves every statistic as without it`.

## 6. Spike 경로

`execute`는 gem5와 같은 `tick`을 자체 시계 `now_`로 돌리며, 받아들여질 때까지, 그리고 `complete`될 때까지 기다립니다.

- `values(f)`와 `on_stage`는 Spike에서만 실행되므로, 같은 `cycle()` 코드가 gem5에서는 타이밍만, Spike에서는 값까지 냅니다.
- 멈춤 판정: tick 한 번 뒤 소유 컴포넌트의 `motion_`이 그대로면 (사용자가 `fault`를 부르지 않은 경우) `vcix_cp: op waits, and nothing in its unit moves any more: …`와 `exit 1`로 끝납니다. interval만 도는 동안은 멈춤이 아닙니다 (probe: `PASS  spike: latency 1, interval 4: the interval is waited out, not judged stuck`).
- trap 복원: `export_model`의 execute 칸이 `M saved = model;`을 두고 throw 때 `model = std::move(saved);` 후 다시 던집니다. probe: `P2 effects after trap + one re-execute: 1 (1 expected)`.

## 7. 통계

컴포넌트 하나가 통계 단위 하나이며(그 PRIMARY 포트에서 셈; 큐와 파이프라인은 통계에 오르지 않음), gem5는 단위 이름의 `.`을 `_`로 바꿔 `system.cpu.executeFuncUnits.funcUnits<N>.vcix.<unit>.<stat>` 꼴로 냅니다.

```
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.admitted                12   # rows admitted at port input
```

## 8. 엔진 전체 코드 (`vcix_cp.hpp`)

설계 헤더 전체를 역할별로 나눠 실었습니다. 합치면 파일 하나입니다. 사용자가 보는 것은 `Component`의 public/protected와 블록 세 개의 public 메서드이고, 나머지는 프레임워크 내부입니다.

### 8.1 공통: 이름, `Insn`, `Block`

블록은 자기 컴포넌트까지의 **바이트 오프셋**만 저장합니다. 그래서 모델을 통째로 복사해도(squash) 복사본의 블록이 복사본의 컴포넌트를 가리킵니다.

```cpp
// vcix_cp: a model is a cp::Coprocessor whose members are cp::Components. Each component writes cycle(), its timing
// and (through values()) Spike's values, over optional blocks the framework advances: Port, Queue<T>, Pipeline<T>.
// Blocks and components are found by byte offset, so a copy of the model (gem5's squash) reaches its own.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "vcix_cp_base.hpp"

namespace cp {

constexpr uint64_t UNLIMITED = UINT64_MAX;

// A name kept for the life of the program (never destroyed): gem5 and Stats hold it as a const char *.
inline const char *intern(const std::string &name) {
  static auto *names = new std::set<std::string>;
  return names->insert(name).first->c_str();
}

// An issued instruction: the simulator's fields and the id ready() and commit name it by (0 in valid()).
struct Insn : vcix_insn {
  Id id = 0;
};

class Coprocessor;
class Component;

// A component's part, a member of it, kept as an offset to its component; label_ "<component>.<name>" names it in
// messages. Its port counts under the component's unit; only a PRIMARY Port is a statistic. The framework calls
// cycle() after every cycle(), clear() at reset, and stat() for the statistics.
class Block {
 public:
  virtual ~Block() = default;

 protected:
  Block(Component &c, const char *name);
  Component &owner() const;
  bool functional_model() const;
  void moved() const;
  [[noreturn]] void broken(const char *what) const {
    std::fprintf(stderr, "vcix_cp: %s %s in cycle %llu\n", unit_, what, static_cast<unsigned long long>());
    std::abort();
  }
  const char *label_, *name_;

 private:
  friend class Coprocessor;
  virtual void cycle() {}
  virtual void clear() = 0;
  virtual const Stats *stat() const = 0;
  ptrdiff_t to_owner_;
};
```

### 8.2 `Port`: 사이클당 대역폭

```cpp
// Per-cycle bandwidth: utilize(n) takes available(), which is width() again in every new cycle. The component's one
// PRIMARY port is the component's unit in gem5's stats; a REPORTING port only times.
class Port : public Block {
 public:
  using Role = Stats::Role;
  static constexpr Role REPORTING = Stats::REPORTING, PRIMARY = Stats::PRIMARY;
  Port(Component &c, const char *name, const char *unit_of_work, uint64_t width, Role role = REPORTING);
  // In configure only.
  void set_width(uint64_t width) { stats_.set_capacity(width); }
  uint64_t width() const { return stats_.capacity(); }
  uint64_t available(Cycle now) const { return stats_.available(); }
  // More than available() stops the run (Stats::admit).
  void utilize(uint64_t n) {
    if (!n) return;
    stats_.admit(n);
    moved();
  }
  // Adds n to its OCCUPANCY statistic.
  void occupy(uint64_t n) { stats_.hold(n); }

 private:
  void clear() override { stats_.reset(); }
  const Stats *stat() const override { return &stats_; }
  Stats stats_;
};
```

### 8.3 `Queue<T>`: 깊이와 사이클당 한도가 있는 FIFO

t에 넣은 행은 t+1부터 보이고, t에 비운 칸은 t+1부터 빕니다. 이 규칙이 `cycle()` 호출 순서를 무관하게 만듭니다.

```cpp
// A FIFO of at most capacity() rows, at most per_cycle pushes a cycle. A row pushed in cycle t is visible (length,
// front, pop) from t + 1; a slot a pop frees in t is free from t + 1. A push when full(), or a read past the visible
// rows, stops the run. Depth 2W with W a cycle sustains W pushes a cycle under one pop of each visible row a cycle.
template <class T>
class Queue : public Block {
 public:
  Queue(Component &c, const char *name, const char *unit_of_work = "rows")
      : Block(c, name), stats_(c.name(), name_, unit_of_work, 1) {}
  // In configure: the capacity, at least 1, and the pushes a cycle (UNLIMITED: no bound); empties the queue.
  void configure(uint64_t capacity, uint64_t per_cycle = UNLIMITED) {
    if (!capacity || !per_cycle) throw std::invalid_argument(std::string("vcix_cp: queue ") + unit_ + ": capacity or per-cycle limit 0");
    stats_.set_capacity(std::min(capacity, per_cycle));
    capacity_ = capacity, per_cycle_ = per_cycle;
    clear();
  }
  uint64_t capacity() const { return capacity_; }
  // Every row held, visible or not.
  uint64_t size() const { return rows_.size(); }
  uint64_t length(Cycle now) const { return rows_.size() - pushed(); }
  // The pushes still allowed in this cycle: free slots (a slot freed now still counts as taken) and per_cycle.
  uint64_t available(Cycle now) const {
    const uint64_t slots = capacity_ == UNLIMITED ? UNLIMITED : capacity_ - rows_.size() - freed();
    return std::min(slots, per_cycle_ == UNLIMITED ? UNLIMITED : per_cycle_ - pushed());
  }
  bool full(Cycle now) const { return !available(); }
  void push(T row) {
    if (full()) broken("pushed to while full");
    if (pushed_at_ != now) pushed_at_ = now, pushed_ = 0;
    rows_.push_back(std::move(row));
    pushed_++;
    stats_.admit(1);
    moved();
  }
  const T &front(Cycle now) const { return visible(), rows_.front(); }
  T &front(Cycle now) { return visible(), rows_.front(); }
  T pop(Cycle now) {
    visible();
    T row = std::move(rows_.front());
    rows_.pop_front();
    if (freed_at_ != now) freed_at_ = now, freed_ = 0;
    freed_++;
    moved();
    return row;
  }

 private:
  uint64_t pushed(Cycle now) const { return pushed_at_ == now ? pushed_ : 0; }
  uint64_t freed(Cycle now) const { return freed_at_ == now ? freed_ : 0; }
  void visible(Cycle now) const {
    if (!length()) broken("read with no visible row");
  }
  // OCCUPANCY: the rows held as cycle() began.
  void cycle() override { stats_.hold(length() + freed()); }
  void clear() override {
    rows_.clear();
    pushed_ = freed_ = 0;
    pushed_at_ = freed_at_ = Unknown;
    stats_.reset();
  }
  const Stats *stat() const override { return &stats_; }
  std::deque<T> rows_;
  uint64_t capacity_ = 1, per_cycle_ = UNLIMITED;
  Cycle pushed_at_ = Unknown, freed_at_ = Unknown;
  uint64_t pushed_ = 0, freed_ = 0;
  Stats stats_;
};
```

### 8.4 `Component`: 사용자가 상속하는 것

`configure`, `valid`(흐름 제어 전용), `cycle`을 override하고 `register_insn`, `insns()`, `complete`, `fault`, `values`를 씁니다.

```cpp
// A part of the model: encodings, blocks, plain copyable members (all state copied by value: no pointer to state, no
// reading another component), and cycle(), which moves only when it has work. An instruction it owns is accepted
// when valid() and insns() has room, and pushed there; complete() ends it, fault() ends it as a wrong program. Time
// spent waiting lives in a Pipeline.
class Component {
 public:
  Component(Coprocessor &cp, const char *name);
  virtual ~Component() = default;
  const char *name() const { return name_; }

  // Reads its keys; sizes insns() (default: depth 2, one a cycle) and its blocks. Before any other call.
  virtual void configure(const Config &) {}
  // Back to what configure left, for the plain members; the framework empties the blocks.
  virtual void reset() {}
  // Flow control only, as an RTL ready: false while the component cannot take the instruction this cycle. A wrong
  // instruction is accepted and ends in fault().
  virtual bool valid(const Insn &) const { return true; }
  virtual void cycle() = 0;

 protected:
  // In the constructor: instructions with (bits & mask) == match go to this component.
  void register_insn(const Encoding &encoding) { layout().encodings.push_back(encoding); }
  Queue<Insn> &insns() { return insns_; }
  const Queue<Insn> &insns() const { return insns_; }
  // ready(insn.id) is true from this cycle on (gem5); Spike's execute of it returns after this cycle.
  void complete(const Insn &insn);
  // The instruction can never end (a wrong field, an order the program got wrong): completes it as failed. gem5
  // prints why and exits with exit_code when it commits (a squashed one never does); Spike as its execute() returns.
  void fault(const Insn &insn, const std::string &why, int exit_code = 1);
  bool functional_model() const;
  // Spike only: the run stops when asked on gem5.
  const Host &host() const;
  // f(host()) on Spike, nothing on gem5: values only, never timing. Not const, so valid() cannot.
  template <class F>
  void values(F &&f) {
    if (functional_model()) f(host());
  }

 private:
  friend class Block;
  friend class Coprocessor;
  struct Layout {
    std::vector<Encoding> encodings;
    std::vector<ptrdiff_t> blocks;
  };
  Layout &layout() {
    if (layout_.use_count() > 1) layout_ = std::make_shared<Layout>(*layout_);
    return *layout_;
  }
  template <class F>
  void each_block(F &&f) const {
    for (ptrdiff_t at : layout_->blocks) f(*reinterpret_cast<Block *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + at));
  }
  const Encoding *encoding(const vcix_insn &insn) const {
    for (const Encoding &e : layout_->encodings)
      if ((insn.bits & e.mask) == e.match) return &e;
    return nullptr;
  }
  Coprocessor &cp() const {
    return *reinterpret_cast<Coprocessor *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + to_cp_);
  }

  const char *name_;
  ptrdiff_t to_cp_;
  std::shared_ptr<Layout> layout_ = std::make_shared<Layout>();
  uint64_t motion_ = 0;
  Queue<Insn> insns_{*this, "insns", "instructions"};
};
```

### 8.5 `Pipeline<T>`: gem5 MinorFU 모양의 파이프라인

`push`한 행은 latency 뒤 `length`가 1이 되고, 끝난 머리를 `pop`하지 않으면 전체가 멈춥니다. 전진은 프레임워크가 `cycle`에서 합니다.

```cpp
// A pipelined unit, as gem5's MinorFU: push(row) when available() (interval cycles since the last push, and the
// finished head popped); a row pushed in cycle t is at the head, length(t + latency) == 1, unless the pipeline stalled.
// After cycle() it advances one stage, or holds still entirely while its finished head is not popped.
template <class T>
class Pipeline : public Block {
 public:
  using Stage = void (Component::*)(T &, uint32_t);
  Pipeline(Component &c, const char *name, const char *unit_of_work = "rows")
      : Block(c, name), stats_(c.name(), name_, unit_of_work, 1) {}
  // In D's constructor: on Spike, fn(row, k) for each row each cycle it advances, k = 0 the stage it was pushed to.
  template <class D>
  void on_stage(void (D::*fn)(T &, uint32_t)) {
    if (!dynamic_cast<D *>(&owner())) throw std::logic_error(std::string("vcix_cp: ") + unit_ + ": on_stage of another class");
    stage_ = static_cast<Stage>(fn);
  }
  // In configure: latency and interval from 1 to UINT32_MAX; empties the stages.
  void configure(uint64_t latency, uint64_t interval) {
    if (!latency || latency > UINT32_MAX || !interval || interval > UINT32_MAX)
      throw std::invalid_argument(std::string("vcix_cp: pipeline ") + unit_ + ": latency " + std::to_string(latency) +
                                  ", interval " + std::to_string(interval));
    latency_ = static_cast<uint32_t>(latency), interval_ = interval;
    clear();
  }
  bool available(Cycle now) const { return !blocked() && (!pushed_ || now - last_ >= interval_); }
  void push(T row) {
    if (!available()) broken("pushed to while it cannot take a row");
    rows_.push_back({std::move(row), 0, Unknown});
    pushed_ = true, last_ = now;
    stats_.admit(1);
    moved();
  }
  uint64_t length(Cycle now) const { return blocked() && rows_.front().done_at < now; }
  const T &front(Cycle now) const { return finished(), rows_.front().row; }
  T pop(Cycle now) {
    finished();
    T row = std::move(rows_.front().row);
    rows_.pop_front();
    moved();
    return row;
  }
  bool idle() const { return rows_.empty(); }
  uint64_t size() const { return rows_.size(); }

 private:
  struct Slot {
    T row;
    uint32_t age;
    Cycle done_at;
  };
  bool blocked() const { return !rows_.empty() && rows_.front().age == latency_; }
  void finished(Cycle now) const {
    if (!length()) broken("has no finished row");
  }
  // A stage on every row, or none while the head waits; an interval still running is motion too.
  void cycle() override {
    if (!blocked() && !rows_.empty()) {
      for (Slot &s : rows_) {
        if (++s.age == latency_) s.done_at = now;
        if (stage_ && functional_model()) (owner().*stage_)(s.row, s.age - 1);
      }
      moved();
    }
    if (pushed_ && now - last_ < interval_) moved();
    stats_.hold(rows_.size());
  }
  void clear() override {
    rows_.clear();
    pushed_ = false;
    stats_.reset();
  }
  const Stats *stat() const override { return &stats_; }
  std::deque<Slot> rows_;
  uint32_t latency_ = 1;
  uint64_t interval_ = 1;
  bool pushed_ = false;
  Cycle last_ = 0;
  Stage stage_ = nullptr;
  Stats stats_;
};
```

### 8.6 `Coprocessor`: gem5와 Spike 양쪽 face

`tick`은 모든 `cycle()` 뒤에 모든 블록의 `cycle()`을 부릅니다. `execute`는 같은 `tick`을 자체 시계로 돌려 Spike를 흉내 냅니다. `commit`에서 `fault`된 명령을 만나면 이유를 내고 끝냅니다.

```cpp
// The model: routes each instruction by encoding to its component's insns(), ticks every cycle() then every block,
// answers ready() from complete(), and runs Spike's execute on the same cycles. Squash: Instance's copies;
// a Spike trap: export_model's copy. check_layout refuses a block or component outside the model object.
class Coprocessor : public Model {
 public:
  std::vector<Encoding> owns() const override {
    std::vector<Encoding> all;
    each([&](const Component &c) { all.insert(all.end(), c.layout_->encodings.begin(), c.layout_->encodings.end()); });
    return all;
  }
  void configure(const Config &config) override {
    each([&](Component &c) { c.configure(config); });
    reset();
  }
  bool can_accept(const vcix_insn &insn) const override {
    const Component *c = owner(insn);
    return c && c->valid(Insn{insn}) && !c->insns_.full();
  }
  Cycle issue(const vcix_insn &insn, Id id) override {
    unit(insn).insns_.push(Insn{insn, id});
    return Unknown;
  }
  void commit(const vcix_insn &, Id id, Cycle) override {
    failed(id);
    const auto it = std::find(done_.begin(), done_.end(), id);
    if (it != done_.end()) done_.erase(it);
  }
  bool ready(Id id, Cycle) const override { return std::find(done_.begin(), done_.end(), id) != done_.end(); }
  // Every component's cycle(), then every block's cycle(): pipelines advance, queues count.
  void tick() override {
    each([&](Component &c) { c.cycle(); });
    each([&](Component &c) { c.each_block([&](Block &b) { b.cycle(); }); });
  }
  // Spike: tick until can_accept, push into insns(), tick until complete(); a tick that moves nothing in the
  // instruction's component ends the run.
  void execute(const Host &host, const vcix_insn &insn) override {
    struct Leave {
      const Host *&host;
      ~Leave() { host = nullptr; }
    } leave{host_ = &host};
    Component &c = unit(insn);
    executing_ = Insn{insn, ++spike_id_};
    until(c, [&] { return Coprocessor::can_accept(executing_, now_); });
    c.insns_.push(executing_, now_);
    until(c, [&] { return Coprocessor::ready(executing_.id, now_); });
    Coprocessor::commit(insn, executing_.id, now_);
  }
  void reset() override {
    each([](Component &c) {
      c.each_block([](Block &b) { b.clear(); });
      c.reset();
    });
    done_.clear();
    faults_.clear();
    now_ = spike_id_ = 0;
  }
  std::vector<const Stats *> ports() const override {
    std::vector<const Stats *> all;
    each([&](const Component &c) { c.each_block([&](const Block &b) { all.push_back(b.stat()); }); });
    return all;
  }

 protected:
  Coprocessor() = default;
  // The component that owns the instruction, nullptr if none (for a trace).
  const Component *owner(const vcix_insn &insn) const {
    const Component *found = nullptr;
    each([&](const Component &c) {
      if (!found && c.encoding(insn)) found = &c;
    });
    return found;
  }
  // Every component and block lies in [this, this + bytes): a copy of the model then reaches its own.
  void check_layout(size_t bytes) const override {
    const auto outside = [&](ptrdiff_t at) { return at < 0 || static_cast<size_t>(at) >= bytes; };
    for (ptrdiff_t at : *components_) {
      const Component &c = *reinterpret_cast<const Component *>(reinterpret_cast<const char *>(this) + at);
      if (outside(at)) throw std::invalid_argument(std::string("vcix_cp: component ") + c.name_ + " is not a member of the model");
      for (ptrdiff_t b : c.layout_->blocks)
        if (outside(at + b))
          throw std::invalid_argument(std::string("vcix_cp: a block of ") + c.name_ + " lies outside the model object");
    }
  }

 private:
  friend class Block;
  friend class Component;
  template <class F>
  void each(F &&f) const {
    for (ptrdiff_t at : *components_)
      f(*reinterpret_cast<Component *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + at));
  }
  Component &unit(const vcix_insn &insn) const {
    if (const Component *c = owner(insn)) return const_cast<Component &>(*c);
    std::fprintf(stderr, "vcix_cp: instruction %08x has no unit\n", insn.bits);
    std::abort();
  }
  // Ticks Spike's clock until done(); a tick after which done() is false and c moved nothing repeats forever.
  template <class Done>
  void until(const Component &c, Done &&done) {
    while (!done()) {
      const uint64_t before = c.motion_;
      tick(++now_);
      if (done() || c.motion_ != before) continue;
      std::fprintf(stderr, "vcix_cp: %s waits, and nothing in its unit moves any more: on Spike, which runs each "
                   "instruction to its end before the next, it never ends\n", c.encoding(executing_)->name);
      std::exit(1);
    }
  }

  // A faulted instruction, as it commits: its why, then the run ends with its exit code.
  void failed(Id id) {
    for (const Fault &f : faults_)
      if (f.id == id) {
        std::fprintf(stderr, "%s\n", f.why.c_str());
        std::exit(f.code);
      }
  }

  struct Fault {
    Id id;
    std::string why;
    int code;
  };
  std::shared_ptr<std::vector<ptrdiff_t>> components_ = std::make_shared<std::vector<ptrdiff_t>>();
  std::vector<Id> done_;
  std::vector<Fault> faults_;
  const Host *host_ = nullptr;
  Insn executing_{};
  Cycle now_ = 0;
  Id spike_id_ = 0;
};
```

### 8.7 inline 정의

```cpp
inline Block::Block(Component &c, const char *name)
    : label_(intern(std::string(c.name()) + "." + name)), name_(intern(name)),
      to_owner_(reinterpret_cast<char *>(&c) - reinterpret_cast<char *>(this)) {
  c.layout().blocks.push_back(-to_owner_);
}
inline Component &Block::owner() const {
  return *reinterpret_cast<Component *>(const_cast<char *>(reinterpret_cast<const char *>(this)) + to_owner_);
}
inline bool Block::functional_model() const { return owner().functional_model(); }
inline void Block::moved() const { owner().motion_++; }

inline Port::Port(Component &c, const char *name, const char *unit_of_work, uint64_t width, Role role)
    : Block(c, name), stats_(c.name(), Block::name_, unit_of_work, width, role) {}

inline Component::Component(Coprocessor &cp, const char *name)
    : name_(intern(name)), to_cp_(reinterpret_cast<char *>(&cp) - reinterpret_cast<char *>(this)) {
  cp.components_->push_back(-to_cp_);
  insns_.configure(2, 1);
}
inline void Component::complete(const Insn &insn) {
  cp().done_.push_back(insn.id);
  motion_++;
}
inline void Component::fault(const Insn &insn, const std::string &why, int exit_code) {
  cp().faults_.push_back({insn.id, why, exit_code});
  complete(insn);
}
inline bool Component::functional_model() const { return cp().host_ != nullptr; }
inline const Host &Component::host() const {
  if (!functional_model()) {
    std::fprintf(stderr, "vcix_cp: %s asked for Spike's host on gem5\n", name_);
    std::abort();
  }
  return *cp().host_;
}

}  // namespace cp
```

### 8.8 공용 층 전체 (`vcix_cp_base.hpp`, 옛 `vcix_accel.hpp`)

C ABI(`vcix_accel.h`) 위의 C++ 층입니다. `Model`(gem5/Spike face), `Instance`(issue 전 복사·squash 복원, 통계 수집), `Stats`(통계 카운터 4종), `export_model`(함수표)입니다. RTL 모델처럼 `cp::` 엔진을 쓰지 않는 모델은 이 층에 바로 올립니다.

```cpp
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
  // More than available() is a broken model: the run stops.
  void admit(uint64_t amount) {
    const uint64_t left = available();
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
  virtual bool can_accept(const vcix_insn &insn) const = 0;
  virtual Cycle issue(const vcix_insn &insn, Id id) = 0;
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
  Cycle issue(const vcix_insn &insn, Id id) {
    issued_.push_back({id, now, model});
    return model.issue(insn, id, now);
  }
  void commit(const vcix_insn &insn, Id id) {
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
  void squash(Id first) {
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
```
