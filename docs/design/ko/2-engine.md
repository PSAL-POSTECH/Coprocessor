# 엔진

## 1. 층 구조

시뮬레이터는 C ABI의 함수 표만 알며, C ABI(`vcix_accel.h`)는 이 재설계에서 바뀌지 않습니다. 수치는 일회용 드라이버 결과이며 gem5/Spike 실행값이 아닙니다.

```
   gem5 MinorCPU (timing)                              Spike (functional)
   tick  ready  commit  squash  can_accept  issue        execute
      |                                                    |
      v                                                    v
 +--------------------------------------------------------------------------+
 | C ABI  coprocessor.h : struct vcix_model (function table, counters)     |
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
 |   | Counter: 통계                      |   | Counter                   | |
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
| 2 | A가 보임 (`length(2)=1`). `eup_.push(A의 레지스터 0)`, `insns().pop()`, `instructions_.add(1)` | A 나이 1 (단계 k=0) | `ready(A,2)` 거짓. `can_accept(B,2)`: `available(2)=min(2−0−1, 1−0)=1` → issue B |
| 3 | B가 보임. `available(3)`: 3−2 ≥ 1 → `eup_.push(B의 레지스터 0)`, pop, add | A 2, B 1 | |
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
| `port.utilize(n)` at t | 사용자 | t의 나머지 동안 `available()`이 줄고, `tick(t+1)`부터 다시 width. `available()`보다 많으면 아무것도 안 가져가고 `false` |
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

블록(Port, Queue, Pipeline)은 동작만 하고 통계를 갖지 않습니다. 통계는 `cp::Counter(component, name, capacity = 0)`뿐이며 `add(n)`으로만 늘어납니다. C 표의 항목은 `{group = 컴포넌트 이름, name, capacity}`이고, 그 뒤에 인코딩마다 `{"committed", 이름, 0}`이 옵니다. capacity가 0보다 큰 카운터는 사이클당 최대 capacity만큼 일하는 단위로, TOGSim이 `<COMPONENT>.<name>`으로 시간을 잽니다. capacity 0은 그냥 개수입니다. gem5는 항목마다 스칼라 하나 `system.cpu.executeFuncUnits.funcUnits<N>.vcix.<group>.<name>`를 내며 `[A-Za-z0-9_]` 밖의 글자는 `_`로 바꿉니다. 카운터는 모델 안에 있으므로 squash의 복사본이 squash된 명령이 센 것을 되돌리고, reset은 값을 그대로 둡니다.

```
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.rows                    12
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.rows_in_flight          84
```

## 8. 엔진 코드

엔진은 [`include/vcix_cp.hpp`](../../../include/vcix_cp.hpp)(블록 `Port`, `Queue<T>`, `Pipeline<T>`, `Component`, `Coprocessor`, `Counter`의 컴포넌트 생성자)와 [`include/vcix_cp_base.hpp`](../../../include/vcix_cp_base.hpp)(`Model`, `Config`, `Host`, `Counter`, `Instance`, `export_model`)입니다. 사용자가 보는 것은 `Component`의 public/protected, 블록 세 개의 public 메서드, `Counter`이고, 나머지는 프레임워크 내부입니다.
