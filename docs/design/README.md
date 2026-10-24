# vcix_cp 새 엔진 설계: cycle() 함수로 쓰는 컴포넌트

표기:
- **[사실]**: 저장소 `file:line`(브랜치 `cp-component`)에서 읽은 것, 또는 `cpdesign/verify/`의 일회용 빌드(g++ 11.5
  `-std=c++17 -Wall -Wextra -Werror`, 시뮬레이터 없음)로 측정한 것입니다.
- **[도출]**: 규칙에서 손으로 계산했거나 일회용 in-order 드라이버가 낸 숫자입니다. gem5나 Spike에서 돌린 값은 아닙니다.
- **[의견]**: 판단이나 제안입니다.

저장소는 고치지 않았습니다. 설계 파일은 모두 `cpdesign/` 아래에 있습니다(§7 끝의 목록).

---

## 1. 한 줄 요약과 구조

**한 줄 요약.** 모델은 컴포넌트를 멤버로 가진 `cp::Coprocessor`입니다. 각 컴포넌트는 사용자가 쓰는 `cycle()` 하나로
타이밍을 정하고 `values()` 안에서 Spike의 값을 씁니다. 프레임워크는 명령을 `insns()`에 넣어 주고, 블록(Port, Queue,
Pipeline)을 매 사이클 진행시키고, `complete()`를 gem5의 `ready()`로 알려 줍니다.

```
              gem5 MinorCPU                                  Spike
   can_accept / issue / ready / commit / squash          execute(insn)
                     |                                         |
                     v                                         v
 +---------------------------------------------------------------------------------+
 | cp::Coprocessor  (cp::Model; copied whole by cp::Instance before each issue)      |
 |   route by encoding -> owner.insns().push()        Spike: tick until complete()   |
 |                                                                                 |
 |   +---------------- cp::Component "SFU" ----------------+   +-- "MISC" --+  ...  |
 |   | insns(): Queue<Insn> (2, 1)                          |   | insns()    |        |
 |   | user cycle(): pop / issue / take / push / complete|   | cycle() |        |
 |   | blocks: Port  Queue<T>  Pipeline<T>  (self-registered|   | Port       |        |
 |   |         by byte offset; each block = one stats unit) |   +------------+        |
 |   | plain members: copyable values only                  |                         |
 |   +------------------------------------------------------+                         |
 |                                                                                 |
 |   tick(t):  every component cycle()  ->  every block cycle()                   |
 +---------------------------------------------------------------------------------+
```

전체 흐름은 다음 세 가지입니다.
- **gem5:** `tick(t)`에서 모든 `cycle()`가 돌고, 블록이 한 칸 나아갑니다. 그 다음 같은 사이클에 gem5가 `ready`,
  `commit`, `can_accept`, `issue`를 부릅니다. t에 issue된 명령은 `tick(t+1)`부터 보입니다.
- **squash:** `cp::Instance`가 issue마다 모델 전체를 복사하고 squash에서 되돌립니다. 그래서 모델의 상태는 모두
  복사 가능한 값이어야 하고, 블록과 컴포넌트는 포인터가 아니라 바이트 오프셋으로 서로를 찾습니다.
- **Spike:** `execute(insn)`는 issue한 뒤, `complete(insn)`가 올 때까지 같은 `tick`을 반복합니다. 한 사이클 동안 그
  컴포넌트에서 아무것도 움직이지 않으면 멈춘 것으로 보고 일반 메시지를 찍은 뒤 종료합니다. 틀린 프로그램(잘못된 필드,
  틀린 순서)은 accept된 뒤 `fault(insn, why, exit_code = 1)`로 끝납니다: Spike는 `execute`가 돌아올 때, gem5는 그
  명령이 commit될 때 `why`를 찍고 종료합니다.

---

## 2. 프레임워크 API 전체

이 절은 `vcix_cp.hpp.design`(약 470줄)과 `vcix_cp_base.hpp.design`(오늘의 `include/vcix_accel.hpp`를 이름 바꾸고
namespace `cp`로 옮긴 것, `vcix_cp_base.hpp.diff`)의 공개 면을 보입니다. 둘 다 namespace `cp`입니다. 오늘의
`include/vcix_cp.hpp`는 1093줄입니다 [사실]. C ABI(`vcix_accel.h`)와 심볼 `vcix_accel_model`은 바뀌지 않습니다 [사실];
등록 매크로는 `VCIX_CP_REGISTER`입니다. 아래의 `vcix_accel.hpp:N` 인용은 이름을 바꾸기 전 오늘 파일의 줄입니다.

### 2.1 `cp::Component` (사용자가 상속하는 것)

```cpp
class Component {
 public:
  Component(Coprocessor &cp, const char *name);        // name is interned; insns() starts as (2, 1)
  virtual void configure(const Config &) {}            // read keys, size insns() and blocks
  virtual void reset() {}                              // plain members back to what configure left
  virtual bool valid(const Insn &) const { return true; }  // flow control only: false = cannot take it this cycle
  virtual void cycle() = 0;                   // everything that moves

 protected:
  void register_insn(const Encoding &encoding);        // in the constructor
  Queue<Insn> &insns();                                // the built-in input queue
  void complete(const Insn &insn);                     // gem5 ready(id) from this cycle; ends Spike's execute
  void fault(const Insn &insn, const std::string &why, int exit_code = 1);  // completes as failed: why printed, exit at commit (gem5) / execute return (Spike)
  bool functional_model() const;                       // true inside Spike's execute
  const Host &host() const;                            // Spike only
  template <class F> void values(F &&f);               // f(host()) on Spike only; not const
};
struct Insn : vcix_insn { Id id = 0; };                // gem5's id; 0 inside valid()
```

`can_accept(insn, t)`는 `valid(insn) && !insns().full(t)`이고, `issue`는 `insns()`에 넣고 `Unknown`을 돌려줍니다
[사실] `vcix_cp.hpp.design:321-328`. 기다림, 순서, 해저드는 모두 사용자의 `cycle()`이 정합니다. `valid()`는 흐름 제어만
합니다(RTL의 ready). 틀린 명령은 accept된 뒤 머리에서 `fault()`로 끝납니다.

### 2.2 블록 (선택 사항, 통계가 따라옴)

```cpp
class Port {        // per-cycle bandwidth; holds a cp::Stats member, unit = the component's name
  Port(Component &c, const char *name, const char *unit_of_work, uint64_t width, Role role = REPORTING);
  void set_width(uint64_t width);  uint64_t width() const;           // configure only
  uint64_t available(Cycle now) const;                               // width - utilized at now
  void utilize(uint64_t n);                               // more than available() aborts
  void occupy(uint64_t n);                                           // adds to OCCUPANCY
};
template <class T> class Queue {   // FIFO; a push at t is visible from t+1, a slot freed at t is free from t+1
  Queue(Component &c, const char *name, const char *unit_of_work = "rows");
  void configure(uint64_t capacity, uint64_t per_cycle = UNLIMITED); // depth and pushes a cycle
  uint64_t capacity() const;  uint64_t size() const;  uint64_t length(Cycle now) const;
  uint64_t available(Cycle now) const;  bool full(Cycle now) const;  // full == !available
  void push(T row);                                       // when full: the run stops
  const T &front(Cycle now) const;  T &front(Cycle now);  T pop(Cycle now);
};
template <class T> class Pipeline { // gem5 MinorFU: opLat = latency, issueLat = interval
  Pipeline(Component &c, const char *name, const char *unit_of_work = "rows");
  template <class D> void on_stage(void (D::*fn)(T &, uint32_t));    // Spike: per-stage values; D must be the owner
  void configure(uint64_t latency, uint64_t interval);               // each 1 .. UINT32_MAX
  bool available(Cycle now) const;  void issue(T row);
  uint64_t length(Cycle now) const;  const T &front(Cycle now) const;  T pop(Cycle now);
  bool idle() const;  uint64_t size() const;
};
```

핵심 구현 발췌입니다 [사실] `vcix_cp.hpp.design:100-115, 250`.

```cpp
void configure(uint64_t capacity, uint64_t per_cycle = UNLIMITED) {
  if (!capacity || !per_cycle) throw std::invalid_argument(...);
  stats_.set_capacity(std::min(capacity, per_cycle));
  capacity_ = capacity, per_cycle_ = per_cycle;
  clear();
}
uint64_t available(Cycle now) const {
  const uint64_t slots = capacity_ == UNLIMITED ? UNLIMITED : capacity_ - rows_.size() - freed();
  return std::min(slots, per_cycle_ == UNLIMITED ? UNLIMITED : per_cycle_ - pushed());
}
bool full(Cycle now) const { return !available(); }
bool available(Cycle now) const { return !blocked() && (!pushed_ || now - last_ >= interval_); }
```

### 2.3 `cp::Coprocessor`와 기반 층(`vcix_cp_base.hpp`)의 두 변경

```cpp
void tick() override {                       // vcix_cp.hpp.design:336-339
  each([&](Component &c) { c.cycle(); });
  each([&](Component &c) { c.each_block([&](Block &b) { b.cycle(); }); });
}
void check_layout(size_t bytes) const override;       // every component and block inside [this, this + bytes)
```

```cpp
// vcix_cp_base.hpp.design:221-226: Instance::configure
static_cast<const Model &>(model).check_layout(sizeof(M));   // before configure ...
model.configure(config);
static_cast<const Model &>(model).check_layout(sizeof(M));   // ... and after (blocks made in configure)
// vcix_cp_base.hpp.design:416-425, export_model: execute
M &model = static_cast<Instance<M> *>(s)->model;
M saved = model;
try { model.execute(Host(h), *i); } catch (...) { model = std::move(saved); throw; }
```

결정 2의 목록에 없는 추가 항목과 그 이유는 다음과 같습니다 [의견, 각 항목의 필요는 사실].
- `cp::Insn`(id 포함): `complete()`가 gem5의 id를 알아야 합니다.
- `virtual reset()`: `Model::reset`이 plain 멤버를 되돌려야 합니다 [사실] `tests/rtl/copier.cc:84`.
- `fault(insn, why, exit_code = 1)`: 틀린 프로그램을 accept한 뒤 끝내는 길입니다. `why_stuck()`과 `executing()`은 없습니다
  (멈춤을 설명하는 대신 머리에서 fault합니다).
- protected `owner(insn)`: tpu trace가 소유 컴포넌트를 찾습니다.
- `Queue::configure(capacity, per_cycle)`, `available()`: 리뷰가 찾은 처리량 결함(§3.3)과 통째 push(MXU, MSA, XLU,
  systolic)에 필요합니다.
- `check_layout`, Spike trap 복원: 리뷰의 HIGH/MEDIUM 결함(§3.4, §3.5)을 막습니다.

---

## 3. 엔진 규칙

### 3.1 gem5 한 사이클의 순서

한 사이클 t의 순서는 다음과 같습니다 [사실] `include/vcix_accel.h:99`, gem5 `execute.cc:1735-1736`(tick),
`:1767-1787`(commit), `:1807`(issue).

```
 tick(t):  C1.cycle()  C2.cycle() ...   ->   every block cycle()
              |                                  Pipeline: age one stage, or hold (head not taken)
              |                                  Queue: add occupancy;  Port: nothing (available is per cycle number)
           ready(id, t)  ->  commit  ->  squash  ->  can_accept(insn, t)  ->  issue(insn, t)
                                                         valid() && !insns().full()     insns().push()
```

- `complete()`를 `cycle()`에서 부르면 `ready(id, t)`가 같은 사이클에 참이 됩니다. Minor는 issue 때
  `markupInstDests(now+0, unpredictable)`, 해제 때 `markInstDestsPredictable`을 하므로 [사실] gem5
  `execute.cc:858-859, 630-631`, 의존 명령은 t에 issue할 수 있습니다.
- 블록들은 자기 `cycle()`에서 서로를 건드리지 않습니다. 컴포넌트끼리도 서로 읽지 않으므로 순서가 결과를 바꾸지 않습니다
  [도출].

### 3.2 보임 규칙 표

| t에 일어난 일 | 누가 | 처음 보이는 곳 |
|---|---|---|
| `issue(insn, t)`가 `insns()`에 넣음 | gem5 / Spike | `tick(t+1)` |
| `q.push(row)` at t | 사용자 | `tick(t+1)`. `length()`, `front()`, `pop()` of t에는 안 보임 |
| `q.pop()` at t | 사용자 | 빈 칸은 `tick(t+1)`부터 `full()`/`available()`에 보임. `insns()`라면 `can_accept(t+1)` |
| t에 `per_cycle`개 push | 사용자 / gem5 | t의 나머지 동안 `full()` 참. `tick(t+1)`부터 `available()`을 다시 셈 |
| `p.push(row)` at t | 사용자 | 블록의 `cycle()`에서 단계 k = 0 |
| 행이 블록의 `cycle()`에서 나이 latency에 도달 | 파이프라인 | `length()`, `front()`, `pop()` from `tick(t+1)` |
| `p.pop()` at t | 사용자 | `available()` 즉시 1 (MinorFU의 단계 넘김) |
| 끝난 머리를 컴포넌트의 `cycle()`에서 안 가져감 | 사용자 | 블록의 `cycle()`에서 파이프라인 전체 정지 (MinorFU stall) |
| `port.utilize(n)` at t | 사용자 | t의 나머지 동안 `available()`이 줄고, `tick(t+1)`부터 다시 width |
| `complete(insn)` in `cycle()` | 사용자 | `ready(id, t)` 참. Spike의 `execute`는 `tick(t)` 뒤에 끝남 |
| plain 멤버를 `cycle()`에서 씀 | 사용자 | 즉시, 그 컴포넌트 안에서: 뒤의 코드와 `can_accept(t)`의 `valid()` |
| `values(f)` in `cycle()` | 사용자 | Spike의 레지스터와 메모리에 즉시 |

경로별 지연은 다음과 같습니다 [도출; L=10은 사실 `verify/sm/units`].

| 경로 | t에 issue → ready |
|---|---|
| `cycle()`이 pop하고 바로 complete | t+1 |
| pop해서 지연 L의 Pipeline에 넣고 `take`에서 complete | t+1+L (L=10이면 11) |
| m개 레지스터 그룹을 한 사이클에 하나씩 그 Pipeline에 넣고 마지막이 나올 때 complete | t+m+L (L=10: m2는 12, m8은 18) [사실] `verify/renamed2/sm_mix.out` |
| 그 뒤 사용자 Queue를 거쳐 다음 단계가 complete | t+2+L |
| 폭 1 Port로 n beat | t+n |

### 3.3 `insns()`의 처리량: 깊이와 사이클당 한도

개요: 빈 칸은 t+1부터 비므로, 깊이만으로는 처리량을 정확히 정할 수 없습니다. 그래서 Queue에 사이클당 push 한도를
넣었습니다.

- 매 사이클 보이는 행을 모두 꺼내는 유닛에서, 깊이 d와 한도 W의 accept 수는 `p_t = min(W, d − p_{t−1})`입니다 [도출].
  깊이 2W, 한도 W이면 정확히 W가 유지되고 버스트도 없습니다.
- 측정값입니다 [사실] `verify/probe2`, gem5가 사이클당 4개를 제시한 경우.

| `insns().configure(d, W)` | 사이클별 accept |
|---|---|
| 기본값 `(2, 1)` | 1 1 1 1 1 1 1 1 |
| `(4, 2)` | 2 2 2 2 2 2 2 2 |
| `(6, 3)` | 3 3 3 3 3 3 3 3 |
| `(3, 2)` | 2 1 2 1 2 1 2 1 |

- 한도는 실제로 필요합니다. Minor의 `executeIssueLimit` 기본값은 2이고 [사실] gem5 `BaseMinorCPU.py:409-410`, vcix
  명령은 MinorFU 슬롯을 쓰지 않습니다 [사실] `execute.cc:855,868`. 한도를 고정하는 테스트도 있습니다 [사실]
  `tests/tpu/run.sh:47-51, 209, 223`.
- 유닛별 설정: Sfu, Msa, systolic, copier는 기본값 `(2, 1)`, Misc는 `(2W, W)`, Xlu는 `(2·issue, issue)`, Mxu는
  `(2(SLOTS+units), SLOTS+units)`입니다.
- Queue의 통계 CAPACITY는 `min(capacity, per_cycle)`, 곧 한 사이클에 받을 수 있는 양입니다.

### 3.4 squash와 복사 안전성

개요: 복사본이 자기 블록을 찾도록 모든 연결은 바이트 오프셋이고, 오프셋이 모델 밖을 가리키면 create에서 거절합니다.

- `Component`는 `to_cp_ = &coprocessor − this`를, `Block`은 `to_owner_`를 저장합니다. 컴포넌트 목록과 Layout(블록과
  인코딩)은 copy-on-write로 복사본끼리 공유합니다. 암시적 복사는 Block 생성자를 부르지 않으므로 다시 등록하지 않습니다
  [사실] `verify/eng/check`: 복사본, 대입본, 원본이 각자 tick해서 모두 5에 끝납니다.
- `check_layout`은 오프셋이 `[this, this + sizeof(M))` 밖이면 throw합니다. `std::vector`에 Queue를 담은 모델은
  `create refused: vcix_cp: a block of VEC lies outside the model object`로 거절됩니다 [사실] `verify/probe2`. 고치기
  전에는 복사본의 `ports()`가 segfault였습니다(리뷰 P1).
- 참조 멤버는 컴파일 오류입니다 [사실] `vcix_accel.hpp:211`. 형제 컴포넌트를 가리키는 raw 포인터는 컴파일되고 복사본에서
  원본을 가리키므로(리뷰 P3), 다음 규칙으로 금지합니다 [의견]. "모든 가변 상태는 대입할 때 값으로 복사된다. 모델 상태를
  가리키는 포인터도, 모델 밖 상태를 가리키는 포인터도 두지 않는다. 컴포넌트는 서로를 읽지 않는다."
- `on_stage(&D::fn)`은 소유자가 D가 아니면 throw합니다 [사실] `verify/probe2`: `vcix_cp: W.p: on_stage of another class`.
- 이름은 한 번 할당하고 해제하지 않는 집합에 intern합니다. 그래서 stats Port와 gem5가 가진 `const char *`는 복사본에서도,
  프로그램 종료 시에도 유효합니다.
- squash 동등성 [사실]:
  - `verify/eng/check`: wrong-path issue 3, squash 6 → 모든 통계가 같습니다.
  - `verify/mm/squash`: 두 배열 MXU에 wrong-path vmatmul 6, squash 9 → 통계와 commit 사이클이 같습니다.
  - `verify/xlu`: wrong-path push 18, squash 22 → 모든 통계가 같습니다.

### 3.5 Spike 실행과 멈춤 판정

```
export_model execute:  M saved = model;  try { model.execute(host, insn) } catch (...) { model = saved; throw }
Coprocessor::execute:  host_ = &host (scope guard clears it, also on a throw)
                       until(c, can_accept)  ->  c.insns().push  ->  until(c, ready)  ->  commit
                       commit: a faulted id prints its why and exits with its exit_code
until(c, done):        before = c.motion_; tick(++now_);
                       if !done() && c.motion_ == before:  print generic message;  exit(1)
```

`fault(insn, why, exit_code = 1)`는 명령을 실패로 complete합니다. Spike는 `execute`가 돌아올 때, gem5는 그 명령이
commit될 때(squash된 것은 commit되지 않습니다) `why`를 찍고 종료합니다. fault를 부르지 않았으면 위의 일반 메시지가 남습니다.

**진행 판정(구체적, O(1)).** 컴포넌트마다 `uint64_t motion_` 하나를 둡니다. 다음이 그 값을 올립니다.

| 블록 | motion을 올리는 것 |
|---|---|
| Queue | `push`, `pop` |
| Pipeline | `push`, `pop`, 행을 나이 들게 한 `cycle`, interval이 아직 도는 `cycle` |
| Port | `utilize(n > 0)` |
| 컴포넌트 | `complete()` |

- 왜 맞는지 [도출]: 컴포넌트 c에서 tick t+1 동안 아무것도 안 움직였다면, t+2에 c의 새 행도 빈 칸도 끝난 interval도 없고
  Port의 available도 같습니다. 컴포넌트는 서로를 읽지 않으므로 `tick(t+2)`의 `c.cycle()`는 `tick(t+1)`의 것과 같은 것을 봅니다. 단 "시간만
  흐르는 기다림은 블록에 둔다"는 규칙이 전제입니다 [의견].
- 컴포넌트별로 세므로, 다른 컴포넌트가 끝없이 움직여도 멈춘 소유자를 가리지 못합니다 [사실] `verify/probe2`: 매 사이클
  Port를 쓰는 컴포넌트 옆에서 멈춘 명령이 일반 메시지와 exit 1로 끝납니다.
- 이 규칙은 `Component` 머리 주석에 적습니다 [의견]: `cycle()`은 할 일이 있을 때만 움직이고, 기다림은 Pipeline에 둡니다.

**Spike trap.** `values()` 안의 `mem_write`가 throw하면(pk demand paging) 모델은 반쯤 tick된 상태가 됩니다. 이제
`export_model`이 실행 전 복사본으로 되돌리므로, Spike가 다시 실행하면 처음부터 한 번만 돕니다 [사실] `verify/probe2`:
`effects after trap + one re-execute: 1 (1 expected)`(고치기 전 2). RTL wrapper도 같은 일을 합니다 [사실]
`examples/rtl/rtl_model.hpp:226-238`. 비용은 Spike 명령마다 모델 복사 한 번입니다 [의견].

**테스트가 grep하는 Spike 메시지.** 모두 그대로 나옵니다. 아래에서 "verify"라고 적은 것은 일회용 가짜 Spike로 확인했습니다
[사실].

| 메시지 | grep 위치 | 만드는 곳 |
|---|---|---|
| `vcix_cp: vmatprep waits, and nothing in its unit moves any more` | `tests/tpuv6e/functional/run.sh:58-59` | 프레임워크 `until`, fault 없음 (verify/mm) |
| `tpu: vexp: an element of 64 bits is not supported` | `tests/tpu/functional/run.sh:156` | Sfu `fault` (verify/sm) |
| `tpu: systolic pop: 1 elements asked, 0 computed`, `… input push: no weight was pushed before it`, `… format 3 names no element of 8 bits`, `… a pop to bfloat16 is not modeled`, `… format 1 names no element of 32 bits` | `:157-161` | Systolic `fault` (verify/cs) |
| `XLU ERROR: the pre stage walks 1 columns but lane 0 carries 0 pattern entries (SIMM5 16)` (exit 201) | `:162` | Xlu `fault`, exit_code 201 (verify/xlu) |
| `tpu: xlu pop: 8 elements asked, 3 left`; `tpu: xlu push_reduce: fold 1 of 16-bit elements; every fold takes f32, max and min also bf16` | `:163-164` | Xlu `fault` (verify/xlu) |
| `tpu: msa pop: 8 elements asked, 3 computed`; `tpu: msa push: its elements are words of 32 bits, not of 16` | `:165-166` | Msa `fault` (verify/mm) |
| `MVIN ERROR: Scratchpad address overflow: 0xd0080000` (exit 200) | `:167` | `Dma::mvin`, `values()` 안 |
| MXU 12개 문장 (`tpu: vpop: 1 rows asked of the upper half, 0 computed` … `tpu: vmatmul: array 2, and the machine has 2 (tpu_mxu_units)`) | `tests/tpuv6e/functional/run.sh:50-69` | Mxu `carried` → `fault` (verify/mm: stderr 22개 프로그램이 오늘 코드와 바이트 단위로 같음) |
| lanes x lanes 4개 문장 | `tests/tpuv3/functional/run.sh:71-76` | Mxu `carried` → `fault` |
| `'0' is not a pipeline depth: at least 1`, `'0' is not an issue width: at least 1`, `tpu_mxu_queue_entries: '1' …`, `tpu_mxu_bound_to_lanes: '2' …`, `reports: … latency: '8 cycles' …` | `tests/tpu/run.sh:172,225`, `tests/tpuv3/run.sh:103,145`, `tests/tpuv6e/run.sh:163`, `tests/contract/run.sh:74` | `configure()`의 `ConfigError`, 그대로 |

### 3.6 통계와 trace

**통계(ABI 변경 없음).** 쓰는 kind는 ADMITTED, CAPACITY, CYCLES, OCCUPANCY뿐입니다 [사실] `vcix_accel.h:60-63`.
`cp::Port`는 `cp::Stats`를 멤버로 가집니다(상속 아님). Queue와 Pipeline은 단위 `"<component>.<block>"`의 PRIMARY
`Stats`를 하나씩 가집니다. gem5는 `.`을 `_`로 바꿉니다 [사실] gem5 `vcix_accel_model.cc:22-33`.

| 단위 | ADMITTED | CAPACITY | OCCUPANCY |
|---|---|---|---|
| `<C>` (PRIMARY `cp::Port` 하나) | `utilize` 합 | width | 사용자 `occupy` |
| `<C>_insns` | issue된 명령 | 사이클당 한도 (기본 1) | 각 `cycle()` 시작 때 든 명령 |
| `<C>_<queue>` | push된 행 | `min(capacity, per_cycle)` | 각 `cycle()` 시작 때 든 행 |
| `<C>_<pipeline>` | push된 행 | 1 | 각 사이클 뒤 단계에 든 행 |

gem5 `stats.txt`에는 다음처럼 나옵니다 [사실] 접두어 `README.md:837`, 값 `verify/cs/dump`(markers 창 1).

```
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.admitted                12   # rows admitted at port input
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.capacity                 1
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.occupancy               84
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC_insns.admitted           7   # instructions admitted at port insns
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC_insns.capacity           1   # instructions per cycle
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC_input_queue.admitted    12
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC_array.occupancy         84
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC_output_queue.admitted   12
system.cpu.executeFuncUnits.funcUnits8.vcix.COPIER_commands.capacity          4   # commands per cycle
```

- 각 유닛의 PRIMARY 포트는 그대로입니다: SFU `entry`, MISC `issue`, SYSTOLIC `input`, XLU `input`, MSA `input`,
  MXU `input`, COPIER `issue`. 그래서 `tests/stats`의 `consistent`(5 of 5)와 `beyond_units`(0)는 유지됩니다 [도출].
- 새 단위 목록은 [사실] `verify/mm/liststats`, 4개 `.so`에서 확인했습니다. 예: `MSA.insns MSA MSA.input_queue
  MSA.array MSA.output_queue`, `XLU.insns XLU XLU.queue XLU.stages XLU.moves XLU.buffer XLU.line XLU.out`.

**trace.** 다음은 그대로 유지합니다.
- `tpu::Coprocessor`가 `issue`와 `commit`을 override해서 줄을 찍습니다 [사실] `examples/tpu/machine.hpp:32-38`.
- commit 줄은 `replaying()`이면 찍지 않습니다 [사실] `machine.hpp:79-85`.

바뀌는 것은 다음입니다.
- `using Insn = vcix_insn` (기반 층의 `Insn` 별칭은 없어졌습니다. `cp::Insn`은 엔진의 id 달린 명령입니다).
- 소유자는 `dynamic_cast<const tpu::Component *>(owner(insn))`로 찾습니다.
- `trace_queues(const vcix_insn &)`.
- 파이프라인 하나짜리 fallback(`machine.hpp:51-60`)은 지웁니다.
- issue 줄의 큐 숫자는 `tick(t)` 뒤, 이 명령을 넣기 전의 상태입니다.

---

## 4. 유닛별 설계

개요: 모든 유닛은 같은 틀을 따릅니다. `cycle()`은 먼저 블록의 출구를 비우고(take, pop), 그 다음 입구를 채우고(issue,
push), 마지막으로 `insns()`의 머리를 처리합니다. 그래서 가득 찬 파이프라인도 매 사이클 한 행이 나가고 한 행이 들어옵니다.
값과 거절 메시지는 모두 오늘과 같습니다 [사실, 각 절의 verify].

### 4.1 Sfu (`units/examples/tpu/sfu.hpp`, 178줄)

```
 insns (2,1) --head--> entry Port(1) PRIMARY "SFU"
      | verf..vexp: one register a cycle          | sfu_push                     ^ sfu_pop
      v                                           v                              |
 eup Pipeline(L,1), the ONE EUP  <--1/cycle-- eup_input Queue   eup --pop if room--> eup_output Queue
      | take: a special function's last register -> values, complete
```

```cpp
void cycle() override {                                   // sfu.hpp:62
  if (eup_.length()) {
    if (eup_.front().special) { const Row row = eup_.pop();
      if (row.j + 1 == cp::group_size(row.insn)) { values(...special_function...); complete(row.insn); } }
    else if (!eup_output_.full()) eup_output_.push(eup_.pop().pushed);
  }
  if (eup_.available() && eup_input_.length()) eup_.push({Insn{}, 0, false, eup_input_.pop()});
  if (insns().length() && head(insns().front())) { insns().pop(); entry_.utilize(1); moved_ = 0; }
}
```

특수 함수와 sfu_push는 EUP 파이프라인 하나(`eup`)를 함께 씁니다(별도 function 파이프라인 없음). 특수 함수는 그룹의
레지스터마다 EUP 슬롯 하나를 쓰고 마지막이 나올 때 complete합니다. 큐에 든 push 레지스터가 먼저 들어갑니다.
결과(일회용 드라이버, L=10) [사실] `verify/renamed2/sm_mix.out`, `sm_units.out`: vexp m1은 그대로(ready-issue 11), m2는 12,
m8은 18, push m8 바로 뒤의 vexp m1은 19입니다. gem5 타이밍 테스트는 모두 m1이므로 고정값은 바뀌지 않습니다.

| 명령 | 경로 |
|---|---|
| verf, vtanh, vsin, vcos, vlog, vatan, vexp | 지원하지 않는 sew는 머리에서 `fault`합니다. 머리에서 레지스터를 한 사이클에 하나씩 `eup`에 넣고(큐에 든 push 레지스터 뒤), 마지막이 나올 때 값 쓰기, complete합니다. m1: t+L+1. |
| sfu_push | 머리에서 그룹의 레지스터를 들어가는 만큼 `eup_input`에 넣습니다. 마지막 레지스터를 넣으면 complete합니다. |
| sfu_pop | 머리에서 `eup_output`의 보이는 행을 가져갑니다. 마지막 레지스터에서 complete합니다. 어디에도 남은 것이 없으면 `sfu_pop: lane 0 has no pushed result left`로 `fault`합니다. |

### 4.2 Misc와 Dma (`misc.hpp`, `dma.hpp`)

```
 insns (2W, W) --up to W a cycle--> issue Port(W) PRIMARY "MISC" --> values(): lane idx | pack | unpack | DMA --> complete
```

```cpp
void cycle() override {                                   // misc.hpp:57
  while (insns().length() && issue_.available()) {
    const Insn in = insns().pop();
    issue_.utilize(1);
    if (transfer(in) && !dma_.scratchpad()) { fault(in, message("%s: the machine description has no %s", ...)); continue; }
    values([&](const Host &host) { apply(host, in); });
    complete(in);
  }
}
```

- 11개 명령은 모두 t+1에 끝납니다.
- 스크래치패드 없는 기계의 전송 명령은 accept된 뒤 머리에서 `fault`합니다: `tpu: mvin: the machine description has no
  vpu_spad_size_kb_per_lane`.
- `configure`는 `dma_ = Dma{}`로 새로 만듭니다. 그래서 키가 없는 두 번째 configure가 옛 크기를 남기지 않습니다.
- `dma_index_key`는 호출하는 곳이 없어서 지웁니다.

### 4.3 Xlu (`xlu.hpp`)

```
 insns (2W, W)  in order, head only; a start spends issue Port(W)
   | push / push_reduce (vl rows, when queue_.available() >= vl)  | xlu_pop
   v                                                                 |
 queue_ Queue<Element> --1 group/cycle, entry Port PRIMARY "XLU"--> stages_ | moves_ Pipeline<Group>
                                                                     | taken when done (never stalls)
                                                                     v
                                buffer_ Queue <-- first pop takes the tile --> line_ Pipeline<Result>(1) --> out_ Queue
```

```cpp
void cycle() override {                                   // xlu.hpp:115
  if (line_.length()) { Result r = line_.pop(); /* r.count rows into out_ */ }
  for (auto *line : {&stages_, &moves_}) if (line->length()) { /* group into buffer_ */ }
  if (queue_.length() && (head moves ? moves_ : stages_).available()) { /* one group in, entry_.utilize */ }
  while (insns().length() && step(insns().front())) complete(insns().pop());
  entry_.occupy(staged_);
}
```

| 명령 | 경로 |
|---|---|
| xlu_push, xlu_push_reduce | 머리에서 필드를 검사해 틀리면 `fault`합니다. `fits()`와 `queue_.available() >= vl`이면 vl 행을 넣고 complete합니다. |
| load push ([4:3]=3), xlu_set_pattern | 머리에서 issue 한 칸을 쓰고, 패턴(Spike 값)을 바꾸고 complete합니다. |
| xlu_pop | 첫 pop은 타일 전체가 `buffer_`에 오면 `transform()`해서 `line_`에 넣습니다. 그 뒤 `out_`에 vl 행이 보이면 가져가고 complete합니다. |

`tpu_xlu_issue_per_cycle` W는 다시 "사이클당 accept 수"를 뜻합니다 [사실] `verify/xlu`: W=1이면 pop이 push 다음
사이클에, W=2이면 같은 사이클에 issue됩니다.

### 4.4 Systolic (`systolic.hpp`, 280줄)

```
 golden (command queue 0): head of insns (2,1) moves the whole instruction, or waits
 RTL keys: insns = wrapper's pending skid (valid: empty, command slot free) -> commands_ Queue(N), head 1/cycle

 input push --in Port--> input_queue --1/cycle, entry Port PRIMARY "SYSTOLIC", when enable--> array Pipeline(2L-1,1)
 weight push --in Port--> landing_ --next cycle--> weights_          on_stage(stage) on Spike  | take when enable
 pop <--pop Port-- output_queue <-----------------------------------------------------------+
 enable = !output_queue.full() as the cycle began
```

```cpp
void cycle() override {                                   // systolic.hpp:88
  values([&](const Host &) { land(); });
  const bool enable = !output_.full();
  if (whole_) { if (insns().length() && wrong(insns().front())) insns().pop();            // wrong(): fault
                else if (insns().length() && work(insns().front())) weigh(insns().pop()); }
  else { if (commands_.length() && work(commands_.front())) commands_.pop();
         if (insns().length() && !commands_.full()) { const Insn in = insns().pop(); if (!wrong(in)) enter(in); } }
  if (enable && array_.length()) output_.push(array_.pop());
  if (enable && input_.length() && array_.available()) { array_.push(input_.pop()); entry_.utilize(1); }
  entry_.occupy(array_.size());
}
```

- input push는 `min(vl − done, in.available(), input_.available())` 행을 넣습니다. golden에서는 전부 아니면 0입니다.
- weight push는 `weight_waits`이면 배열이 빌 때까지 기다립니다. beat는 다음 사이클에 PE에 닿습니다.
- pop은 `min(vl − done, pop.available(), output_.length())`를 가져갑니다. 채울 수 없는 pop은 `fault`합니다.
- `valid`는 RTL 모드의 skid 규칙만 봅니다. 필드와 순서 검사(`wrong()`)는 머리에서 `fault`합니다.
- RTL 동등성 [사실] `verify/cs`, 저장소의 빌드된 바이너리로 확인했습니다.
  - `rtl_stall`은 7 PASS이고 ready 사이클은 6, 70, 79, 82, 85, 94, 158입니다.
  - values 순서의 출력이 golden = 오늘 golden, nowait = 오늘 nowait = RTL nowait, wait = RTL = golden입니다.

### 4.5 Msa (`msa.hpp`, 190줄)

```
 insns (2,1) --head, issue Port(1)--> weight push: weights_, format_ (plain)
                                  --> input push: vl rows at once into input_queue (available() >= vl)
                                  --> pop: vl rows at once from output_queue (length() >= vl)
 input_queue --1/cycle, entry Port PRIMARY "MSA"--> array Pipeline(2L-1,1) --pop if output_queue not full--> output_queue
```

```cpp
void cycle() override {                                   // msa.hpp:53
  if (array_.length() && !output_.full()) output_.push(array_.pop());
  if (input_.length() && array_.available()) { entry_.utilize(1); array_.push(input_.pop()); }
  if (insns().length() && carry_out(insns().front())) insns().pop();   // carry_out: complete or fault
  entry_.occupy(array_.size());
}
```

- 검사는 `refusal(insn)` 한 곳에 오늘 순서대로 있습니다(필드, 그 다음 상태). 처음 실패한 것이 머리에서 `fault`의
  `why`가 됩니다. `valid`는 override하지 않습니다.
- 큐 이름은 `input_queue`/`output_queue`입니다. Port `input`과 abort 메시지에서 헷갈리지 않게 하려는 것입니다.

### 4.6 Mxu (`mxu.hpp`, 357줄)

```
 insns (2W, W), W = SLOTS + units; heads in order; issue Port(SLOTS); per array: weight_issue, input_issue, pop_issue (1)
   vmatprep / vmatpush: array state and values only
   vmatmul: vl*rows_in_word rows at once into array<i>_input (available() >= rows)
 array<i>_input --1 word/cycle, entry Port PRIMARY (PER_CYCLE*units)--> array<i>_line Pipeline<Word>("words", delay)
   --pop when every output has available() >= rows--> array<i>_lower (+ array<i>_upper on the v6e)
 vpop: vl rows at once from one output queue, shared pop Port
```

```cpp
void cycle() override {                                   // mxu.hpp:76
  uint64_t in_line = 0;
  for (size_t i = 0; i < units_; i++) in_line += arrays_[i]->flow(entry_);
  while (insns().length() && carry_out(insns().front())) insns().pop();   // carry_out: complete or fault
  entry_.occupy(in_line);
}
```

- 배열은 `std::optional<Array>` 안의 블록입니다. `configure`에서 만들어지므로 configure 뒤의 `check_layout`이
  검사합니다(통과, [사실] `verify/out/libtpuv{3,6e}.so` create 성공).
- Spike 값 해시 36개와 거절 stderr 22개가 오늘 코드와 같습니다 [사실] `verify/mm`.

### 4.7 Copier (`examples/copier/copier.cc`, 87줄)

```
 insns (2,1) [valid: nothing pending, commands_ < 4] --tick(t+1)--> commands_ Queue(4), issue Port PRIMARY "COPIER"
   source / copy: values() at entry, complete() at entry     wait: waits_ (plain) until !left_ && commands_ empty
 head popped while left_ == 0; a copy then makes 64 requests, memory Port(1) one a cycle
```

```cpp
void cycle() override {                               // copier.cc:35
  if (left_) { memory_.utilize(1); left_--; }
  else if (commands_.length() && is(COPY, commands_.pop())) left_ = 2 * BYTES / 8;
  if (insns().length() && !commands_.full()) enter(insns().pop());
  if (left_ || commands_.size()) return;
  for (const cp::Insn &wait : waits_) complete(wait);
  waits_.clear();
}
```

- `copier_lockstep`(50 seeds × 20000 cycles, 109928 명령)이 PASS입니다 [사실] `verify`.
- `timing_probe` 출력이 `README.md:812-822`와 같습니다 [사실].
- trap 복원 덕분에 처음 건드리는 페이지로의 copy도 한 번만 복사됩니다 [도출].

---

## 5. cycle 재기준 표 (전체)

개요: 새 규칙에서는 issue에서 아무 일도 일어나지 않습니다. 그래서 대부분의 지연이 1 늘어납니다. 또 상태를 기다리는
명령(pop 등)이 accept 단계가 아니라 `insns()`의 머리에서 기다립니다. 그래서 issue 줄의 "cycles after the last issue"가
줄고 commit 줄이 늘어납니다.

"new" 열은 [도출]입니다. 일회용 드라이버가 낸 값이고, 그 드라이버는 오늘 코드에 대해 "old" 열을 그대로 재현합니다 [사실]
`verify/pins.py`, `verify/xpins.py`. gem5에서 돌린 값은 아닙니다.

**구현 후 결과 (2026-10-25, PR #61):** 아래 표의 "new" 값으로 `tests/tpu`, `tests/tpuv3`, `tests/tpuv6e`의 고정값을 다시 정했고, gem5 실측은 모든
줄에서 도출값과 같았습니다. 다른 곳은 둘인데, 둘 다 드라이버 쪽 가정의 차이였습니다: tpuv6e `mxu_pops`는 `.balign 64` 뒤의 fetch 간격 때문에 두
vmatmul이 앞선 명령 없이 함께 실행되어 vpop commit이 21·22가 아니라 20·21이고, tpuv6e `mxu_slots-script`(폭 12)는 "비운 칸은 t+1에 빈다"
규칙으로 세 번째 사이클에 vmatmul 하나만 들어가지만 모든 명령의 실행 사이클은 gem5_se와 같습니다. `ports.cc`의 `continuous`는 push 6개를 앞서
보내는 프로그램 순서로 3998/4000(0.9995)입니다. 전체 테스트 243개가 통과합니다. 설계 코드 사본(`examples/`, 헤더)은 구현이 들어온 뒤 이 디렉터리에서
지웠습니다. 실제 코드는 `include/vcix_cp.hpp`, `include/vcix_cp_base.hpp`, `examples/`에 있습니다.

### 5.1 프레임워크

| test | old | new (derived) | why |
|---|---|---|---|
| `tests/contract/route_once.cc:59,63` | 6에 끝남 | **5** (복사본, 대입본, 원본 모두) [사실] `verify/eng/check` | 도착 스탬프가 없습니다. "handler ran once"는 "a copy's cycle() ticks its own blocks"로 다시 씁니다(`run.sh:103` 문장도). |
| `tests/tpu/ports.cc:63` 외 드라이버 | `now + latency`에 commit | `ready()`를 poll해야 합니다 | `Unknown`이면 wrap됩니다 |
| `tests/tpu/ports.cc:108-123`, `tests/tpuv3/ports.cc:37`, `tests/tpuv6e/ports.cc:37` list | 유닛 4–5개 | **FAIL**, 목록을 다시 씀 (블록마다 단위) | 결정 4 |
| `tests/tpuv3/run.sh:62`, `tests/tpuv6e/run.sh:63` units | `…_MISC …_MXU …_SFU …_XLU` | 각 `_insns`와 블록 단위 추가 | 같은 이유 |
| `tests/stats` counts/consistent/beyond_units | 12 3 3 …, 5 of 5, 0 | **그대로** | PRIMARY 포트 불변 |

### 5.2 Sfu / Misc (드라이버 [사실] `verify/sm/units`)

| test | old | new (derived) | why |
|---|---|---|---|
| `tests/tpu/run.sh:47-51` vexp 버스트 | 1 cycles after ×4 | **같음** (issue 1,2,3,4,5) | 기본 `(2, 1)` |
| `run.sh:52` commit vexp | 10 | **11** | t+1에 pop, L, take |
| `run.sh:64` chain (L=10) | readers 10 / commits 10, 11 lines | **11 / 11**, 11 lines | 생산자가 t+11에 ready |
| `run.sh:66` chain (L=4) | 4 / 4 | **5 / 5** | 같음 |
| `run.sh:186` chain on script_systolic.py | 10 | **11** | 같음 |
| `run.sh:161` Misc "1 cycles after its issue" | 1 (8 of 10) | **1** | t+1 |
| `run.sh:164` "issue systolic weight push: 10 cycles after the last issue" | 10 | **11** | vexp의 v4를 읽으므로 |
| `run.sh:209` misc_per_cycle W=2 | at most 2, all 8 | **2, 8** | `(4, 2)` |
| `run.sh:211` MISC capacity/admitted | 2 / 8 of 8 | **2 / 8 of 8** | |
| `run.sh:223` W=3 | 3, 3, 8 | **3, 3, 8** | `(6, 3)` |
| `ports.cc:180-190` 1000 vexp | admitted 1000 | **admitted 999**, cycles 1000 | 1000번째는 tick되지 않음 |
| `ports.cc:216-237` misc 10 | 2/cycle ×5 | **같음** | |
| `tests/tpuv6e/run.sh:126-128` push m8 → pop (L=10) | `issue sfu_pop: 18 cycles …, input queue 0, output queue 8` | `issue sfu_pop: 1 cycles …, 0 or 1 in flight, input queue 8, output queue 0`, pop commit **19**, push→pop ready **20** (was 19) | pop이 머리에서 기다림 |
| 같은 것, L=4 | `issue sfu_pop: 12 …` | `1 …`, pop commit **13**, push→pop **14** (was 13) | 같음 |
| (고정된 테스트 없음) vexp m2 / m8 / push m8 뒤 vexp m1, L=10 | — | ready-issue **12 / 18 / 19** [사실] `verify/renamed2/sm_mix.out` | 특수 함수가 push와 EUP 하나를 공유 (§4.1) |

### 5.3 Xlu (드라이버 [사실] `verify/xlu/xlu_check`)

| test | old | new (derived) | why |
|---|---|---|---|
| `tests/tpu/run.sh:114-118` xlu-fold | lines 2, commits 1 (`commit xlu_pop: 9`) | **lines 1, commits 0**. 새 pop 줄은 `1 cycles …, 0 in flight, input queue 2, output queue 0`이고 commit은 8 | push의 행이 t+1에 `queue_`에 들어갑니다. pop은 push issue 뒤 9에 끝납니다(old 10). |
| `run.sh:119-122` xlu-wide | lines 1, commits 1 (8) | **lines 0, commits 0**. pop 줄은 `input queue 2`이고 commit은 7 | |
| `run.sh:123-125` xlu-issue (W=2) | lines 1 | **lines 0**: `0 cycles …, 1 in flight, input queue 0, output queue 0` | 같은 사이클에 issue됩니다(키의 의미는 유지). 행은 아직 큐에 없습니다. |
| `run.sh:127-131` xlu | lines 2, commits 2 (9) | **lines 1, commits 0** (8과 8) | |
| `run.sh:133-135` xlu-pattern | sets 3, commits 2 | **같음** | |
| `run.sh:137-139` commits 9 / 12 / 9 | 2 and 2, 2 | **0 and 0, 0** (8,8 / 11,11 / 8,8) | move 지연은 여전히 +3 |
| `run.sh:141-147` eight pushes | lines 8 | **lines 5** | 큐 숫자에 앞 push의 행이 포함됩니다 |
| `run.sh:149-154` 18원소 타일 뒤 pop | lines 3 of 13 | **lines 0 of 13** (commit 8, 7) | 두 번째 pop이 `insns()`에서 기다림 |
| `ports.cc:192-199` sfu_and_xlu | 5, 15 | **5, 15** | |
| `ports.cc:291-302` fewer_pushed(XLU) | 1, 100 | **1, 1** (세 번째 pop: 100) | |
| `ports.cc:325-336` xlu_shallower | (1, 11, 1) ×100 | **(1, 1, 10) ×100** | |

### 5.4 Systolic / Copier (드라이버와 RTL [사실] `verify/cs`)

| test | old | new | why |
|---|---|---|---|
| `tests/rtl/run.sh:94-97` copier_lockstep | PASS | **PASS (run)** | wrapper의 skid를 그대로 옮겼습니다 |
| `README.md:812-822` timing_probe 6 copies / 2 waits | 1,2,3,4,5, 67→68 / 2, 4 | **같음 (run)** | |
| `tests/rtl/run.sh:88-91` libcopier on Spike | 256 of 256 | **같음** (가짜 Spike: wait 전에 512 of 512) | copy 값은 들어갈 때 |
| `tests/rtl/run.sh:66-69` rtl_stall | 6, 70, 79, 82, 85, 94, 158 | **같음 (run), 7 PASS** | |
| `tests/rtl/run.sh:49-63` values | same, differs, same | **same, differs, same** (가짜 Spike 바이트 동일) | |
| `tests/tpu/run.sh:73-77` systolic_full pop | `pop: 11 cycles …, input queue 0, output queue 4`; lines 3 | `pop: 1 cycles …, 0 in flight, input queue 4, output queue 0`, commit **12**; **lines 2** | pop이 머리에서 기다림 |
| `run.sh:79-88` systolic_stream | 10 of 11 | **6 of 11** (옛 문장 기준). 새 줄: input queue 0, 2..7 (`1 cycles…0 in flight`), `1 cycles…1 in flight, input queue 6, output queue 0`, `2 cycles…1 in flight, input queue 6, output queue 1/3` | 앞 push의 행이 머리에서 옮겨진 뒤 큐를 셉니다 |
| `run.sh:90-95` output full | 3 of 11 | **2 of 11** (세 번째 줄은 `weight push: 1 cycles … input queue 1, output queue 4`) | |
| `run.sh:163-168` one_cycle weight push 큐 | `input queue 0, output queue 0` | **같음** | |
| `ports.cc` one_row | 1, 1, 20, 7, 1 | **같음 (run)** | |
| `ports.cc` continuous | 3999 of 4000 | **728 of 4000 (run), FAIL** | 드라이버가 pop을 먼저 제시합니다. 프로그램 순서로 제시하도록 고칩니다 |
| `ports.cc` fewer_pushed(systolic) | 100, 100 | **1, 1 (run), FAIL** | 채울 수 없는 pop이 accept됩니다 |
| `ports.cc` more_pushed(systolic) | 10, 1000 | **11, 1000 (run), PASS** | |
| `ports.cc` interleaved | 56 | **51 (run), FAIL** | |
| `ports.cc` squashed | same | **same (run)** | |

### 5.5 Msa / Mxu (드라이버 [사실] `verify/mm/harness`, CPU 폭 2 또는 12)

| test | old | new (derived) | why |
|---|---|---|---|
| `tests/tpuv3/run.sh:54`, `tests/tpuv6e/run.sh:44` ports | 1 passed | **FAIL as written** | 블록 단위 |
| tpuv3 `:59` mxu | `vmatmul:1:0/0 vpop:11:0/4 vpop:4:0/4` | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0` | vpop commit 13, 17 |
| tpuv3 `:62` admitted/capacity/occupancy | 8, 1, 56 | **8, 1, 56** | |
| tpuv3 `:66` mxu_rate | `vmatmul:1:0/0 vmatmul:4:0/0 vpop:7:1/4 vpop:4:0/4 vpop:4:0/4` | `vmatmul:0:0/0 vmatmul:1:0/0 vpop:0:0/0 vpop:1:4/0 vpop:1:3/0` | 두 번째 vmatmul이 머리에서 기다림 (commit 6) |
| tpuv3 `:70` mxu_rate-16 | `vmatmul:1:0/0 vmatmul:1:3/0` | `vmatmul:0:0/0 vmatmul:1:0/0` | |
| tpuv3 `:75` lanes-128 | `vmatmul:1:0/0 vpop:259:0/4 vpop:4:0/4` | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0` | vpop commit 261, 265 |
| tpuv3 `:82` bound-4 | `vmatmul:0:0/0 vpop:9:0/4 vpop:2:0/4`; 2, 56 | `vmatmul:0:0/0 vpop:1:8/0 vpop:0:8/0`; **2, 56** | commit 10, 12 |
| tpuv3 `:86` bound-128 | `vmatmul:0:0/0 vpop:257:0/4 vpop:2:0/4` | `vmatmul:0:0/0 vpop:1:8/0 vpop:0:8/0` | commit 258, 260 |
| tpuv3 `:93` lanes-8 / bound-8 | `…vpop:19:0/4 vpop:4:0/4`, same | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0`, **same** | |
| tpuv3 `:99` tpuv6e unbound | 8 issues, same | **8 issues, same** | |
| tpuv3 `:137` machine-mxu (폭 12) | gem5_se와 같음 | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0`, **gem5_se와 같음** | 한도 W=2가 CPU 폭보다 먼저 묶습니다 |
| tpuv6e `:54` mxu pairs | lines 4 | **lines 1** | vmatpush가 vmatprep 옆에서 accept |
| tpuv6e `:60` mxu pops | lines 4 of 8 | **lines 0 of 8** | commit 19, 20, 20, 21 |
| tpuv6e `:63` admitted | 8, 2, 120 | **8, 2, 120** | |
| tpuv6e `:69` mxu_rate 입력 큐 | `0 0 0 0 3 2 10 9 8 6 0 0 0 0 0 0` | `0 0 0 0 0 0 4 4 3 3 10 9 9 8 6 4` | |
| tpuv6e `:73` mxu_rate pops | `14:4/4 1:2/6 1:4/4 1:2/6 1:4/4 1:0/4` | `1:0/0 1:0/0 0:0/0 1:0/0 1:0/0 1:0/0` | commit 18, 18, 19, 19, 19, 19 |
| tpuv6e `:88` mxu_arrays | 8 of 12; capacity 4 | **0 of 12; capacity 4** | 배열 0 commit 18, 배열 1 commit 19 |
| tpuv6e `:98` mxu_arrays-script (폭 12) | 3 of 12 | **0 of 12** | W=4 |
| tpuv6e `:109` mxu_slots (se, machine) | 2 of 14, 2 of 14 | **0 of 14, 0 of 14** | vmatmul과 vpop은 여전히 한 사이클에 issue |
| tpuv6e `:118` mxu_pops | 2 of 10 | **0 of 10** | commit 21, 22 (공유 pop 포트) |
| `tests/tpu/run.sh:103` msa | lines 3 of 3 | **lines 2 of 3** (pop: `1 cycles …, 0 in flight, input queue 4, output queue 0`) | pop commit 12, input push issue 뒤 13에 끝남 (old 12) |
| `ports.cc:210` msa | 3, 2, 1 | **같음** | |
| `ports.cc:380` fewer_pushed(MSA) | 100, 100 | **1, 1** | |
| `ports.cc:382` more_pushed(MSA) | 10 (<100), 1000 | **11, 1000 (PASS)** | MinorFU take 규칙 |
| `ports.cc:385` interleaved(MSA) | 56 | **51** (마지막 commit 58) | |
| `tests/*/functional` | PASS | **PASS** (값과 stderr 동일, [사실]) | |

---

## 6. 지금의 `include/vcix_cp.hpp`에서 다시 쓰는 것과 지우는 것

개요: 복사 안전성의 뼈대(오프셋 등록)와 결과 표면(stats Port, 멈춤 메시지)은 남깁니다. 명령마다 단계를 기록하던 기계는
모두 지웁니다.

| 오늘 | 처리 |
|---|---|
| 오프셋 등록, `components_` shared_ptr, `Layout` COW (`:46-104, 1079-1089`) | **재사용**. Layout은 인코딩과 블록을 담고, `check_layout`을 추가합니다. |
| `vcix_accel::Port`를 컴포넌트 이름으로 쓰는 `cp::Port` (`:106-113`) | **재사용**. `Block`이 되어 `cp::Stats`를 멤버로 갖고 `available()/utilize(n)/occupy(n)`를 씁니다. |
| `ready_` 목록, commit 삭제, ready 조회 (`:868-872`) | **재사용** (`done_`) |
| 멈춤 메시지와 `exit(1)` (`:1034-1036`), "has no unit" (`:855`) | **재사용**, 문장 그대로 |
| 단계 함수, 들어온 사이클 k = 0, Spike 전용 (`:434-438`) | **재사용** (`Pipeline::cycle`) |
| `Slots` (`:149-348`) | **삭제**. `deque<T>`와 스탬프 두 개로 대신합니다. |
| `AnyPipeline` (`:350-417`), `Stall` | **삭제**. MinorFU stall 하나로 대신합니다. |
| `CmdQueue`, `Free`, `Done`, `arrival_cycles`, waiters (`:443-497`) | **삭제**. `insns()`, `cycle()`, `complete()`로 대신합니다. |
| `Instr`, `handle()`, `Handler`, `Step`, `Move`, `Hooks`, `MAX_STEPS`, `Through*`, `Output`, `require/use/send/receive/transfer/on_*` (`:499-833`) | **삭제** |
| `Register<S>`, `AnyRegister` (`:115-147`) | **삭제**. plain 멤버와 `Component::reset()`으로 대신합니다. |
| `motion()` 벡터, `arriving()`, `drained_receive()`, `queued()`, `Beats` (`:912-917, 1038-1077`) | **삭제**. 컴포넌트당 `motion_` 하나로 대신합니다. |
| `with_owner`, `each_pipeline` (`:897-908`) | **대체** (`owner(insn)`) |

유닛 쪽에서 지우는 것은 다음입니다 [사실, 새 파일 기준].
- REPORTING 포트 중 타이밍만 주던 것: SFU `eup`, systolic `issue`/`weight_push`, XLU `output`.
- `cp::Register<Dma>`, `dma_index_key`, Sfu와 Misc의 `held_`.

---

## 7. 구현 순서(파일 단위)와 검증

개요: 아래 순서는 각 단계가 끝날 때마다 빌드와 일부 테스트가 돌도록 짰습니다. 설계 파일을 저장소로 옮기는 일이 대부분입니다.

| 순서 | 파일 | 내용 | 그 단계의 검증 |
|---|---|---|---|
| 1 | `include/vcix_accel.hpp` → `include/vcix_cp_base.hpp` | `vcix_cp_base.hpp.diff` 적용: namespace `cp`, `vcix_accel::Port` → `cp::Stats`, `VCIX_CP_REGISTER`, `Model::check_layout`, `Instance::configure`의 두 번 검사, `export_model` trap 복원. `vcix_accel.h`와 `vcix_accel_model`은 그대로 | 기존 `tests/contract`, `tests/rtl` 그대로 PASS. RTL 모델이 영향을 받지 않는지 확인합니다. |
| 2 | `include/vcix_cp.hpp` | `vcix_cp.hpp.design`으로 교체 | `verify/probe2`와 `verify/eng/check`를 `tests/contract`에 옮깁니다(trap 1회, vector 거절, 처리량 표, 컴포넌트별 멈춤, interval 거절, on_stage 거절). |
| 3 | `examples/copier/copier.cc` | `units/examples/copier/copier.cc` | `copier_lockstep`, `timing_probe`, `tests/rtl/run.sh` 전체 |
| 4 | `examples/tpu/machine.hpp` | trace 변경 (§3.6) | 빌드 |
| 5 | `examples/tpu/{sfu,misc,dma}.hpp` | 새 파일 | `tests/tpu/functional`의 sfu/dma 부분, `tpu_ports`의 SFU/MISC |
| 6 | `examples/tpu/{systolic,msa}.hpp` | 새 파일 | `rtl_stall`, `tests/rtl/run.sh:49-63` values, functional systolic/msa |
| 7 | `examples/tpu/xlu.hpp` | 새 파일 | functional xlu (값 표, exit 201/1/1) |
| 8 | `examples/tpu/mxu.hpp` | 새 파일 (`tpu.cc`, `tpuv3.cc`, `tpuv6e.cc`는 그대로 [사실] `cmp`) | tpuv3/tpuv6e functional |
| 9 | `tests/tpu/ports.cc`, `tests/tpuv3/ports.cc`, `tests/tpuv6e/ports.cc` | `ready()` poll, 블록 단위 목록, 드라이버가 프로그램 순서로 제시 | 각 ports 테스트 |
| 10 | `tests/tpu/run.sh`, `tests/tpuv3/run.sh`, `tests/tpuv6e/run.sh`, `tests/contract/route_once.cc`, `run.sh:103` | §5의 새 값으로 다시 고정 | gem5 실행. 결과를 §5의 [도출] 값과 비교하고, 다르면 원인을 기록합니다. |
| 11 | `README.md`, `examples/tpu*/README.md` | 핸들러 설명과 사이클 예시를 다시 생성 | `units/*.md`의 README 목록 |

검증 원칙은 다음 네 가지입니다 [의견].
- 값은 Spike functional 테스트의 sha와 바이트로 비교합니다.
- 타이밍은 gem5 run.sh의 새 고정값으로 확인합니다. 다만 그 값은 이 문서의 드라이버가 만든 [도출] 값이므로, 첫 gem5 실행이
  사실상의 검증입니다.
- RTL 동등성(lockstep, rtl_stall)은 바뀌지 않아야 합니다.
- squash 동등성은 `ports.cc squashed`와 contract로 확인합니다.

설계 파일(`cpdesign/`)은 다음과 같습니다.
- `vcix_cp.hpp.design`, `vcix_cp_base.hpp.design`, `vcix_cp_base.hpp.diff`
- `ENGINE.md` (영문 엔진 명세)
- `units/examples/tpu/*.hpp`, `units/examples/copier/copier.cc`, `units/*.md` (유닛별 영문 설계)
- `verify/` (일회용 검증: `inc/`, `out/`, `sm/`, `mm/`, `xlu/`, `cs/`, `eng/`, `probe2.cc`, `pins.py`, `xpins.py`)

---

## 8. 확정된 결정

다섯 결정은 모두 확정되었습니다. 코드(`vcix_cp.hpp.design`, `units/examples/`)가 기준입니다.

1. **`Pipeline::hold()`는 넣지 않습니다.**
   - 배경: systolic.sv는 출력 큐가 차면 거품까지 포함해 배열 전체를 멈춥니다 [사실] `systolic.sv:67,122-134`. 오늘의 MSA도
     같습니다 [사실] `msa.hpp:38`. MinorFU 규칙은 끝난 머리를 안 가져갈 때만 멈춥니다.
   - 영향: 이 차이를 건드리는 테스트는 없습니다(`rtl_stall` 7 PASS, [사실]). `weight_waits: 0`의 Spike 값만 달라질 수
     있습니다 [도출]. 결정 4가 Pipeline을 MinorFU로 정했으므로 그대로 둡니다.
2. **행이 아직 없는 pop은 accept되고 머리에서 기다립니다.**
   - 그 결과 `ports.cc`의 continuous, fewer_pushed, interleaved가 FAIL합니다(§5.4, §5.5). 드라이버를 프로그램 순서로 제시하게
     고칩니다. `valid()`에 scoreboard를 넣지 않습니다(결정 3).
3. **Spike 진행 규칙 "시간만 흐르는 기다림은 블록에 둔다"는 유지합니다.** `moved()` 같은 탈출구는 없습니다. 지금 유닛 중에
   이 규칙에 걸리는 것은 없습니다 [사실, 각 verify].
4. **기반 층(`vcix_cp_base.hpp`)의 두 변경(trap 복원, `check_layout`)은 승인되었습니다.** ABI는 바뀌지 않습니다. 비용은 Spike
   명령마다 모델 복사 한 번입니다. 이것이 없으면 처음 건드리는 페이지로의 copy나 mvin이 두 번 실행되거나 거짓 멈춤이 됩니다
   (리뷰 P2).
5. **틀린 프로그램은 accept된 뒤 `fault(insn, why, exit_code = 1)`로 끝납니다.** `valid()`는 흐름 제어만 합니다. 필드 오류와
   순서 오류(XLU의 다른 종류 타일, 65535 한도, 패턴 없음; MXU의 held/pushed/latched; systolic과 msa의 채울 수 없는 pop)는
   머리에서 fault하고, gem5는 그 명령이 commit될 때, Spike는 `execute`가 돌아올 때 `why`를 찍고 종료합니다. fault를 부르지
   않은 기다림(머리가 아직 held인 vmatprep, 가득 찬 큐)에는 프레임워크의 일반 메시지 "waits, and nothing in its unit
   moves"가 그대로 남습니다. README의 "gem5 never accepts" 문장은 고칩니다.
