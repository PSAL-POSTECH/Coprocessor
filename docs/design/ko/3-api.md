# API 레퍼런스

모델은 `cp::Coprocessor` 하나이고, 그 멤버가 `cp::Component`, 컴포넌트의 멤버가 블록(`cp::Port`, `cp::Queue<T>`, `cp::Pipeline<T>`)입니다. 이 설계는 아직 저장소에 구현되지 않았습니다.

## 한 화면에 보는 API

사용자가 반드시 쓰는 것은 `Coprocessor` 하위 클래스, `cycle()`, `register_insn`, `complete`이고 블록은 선택입니다.

| 타입 | 역할 |
|---|---|
| `cp::Coprocessor` | 모델 전체. 인코딩으로 라우팅하고, 매 사이클 `cycle()`과 블록을 진행시키고, `complete()`를 `ready()`로 답합니다 |
| `cp::Component` | 모델의 한 부분. 인코딩, 블록, plain 멤버, `cycle()`를 가집니다 |
| `cp::Encoding` | C ABI의 `vcix_encoding {match, mask, name}`. `(bits & mask) == match`로 소유자를 정하고, `name`은 통계 `committed.<name>`이 됩니다 |
| `cp::Insn` | issue된 명령: `vcix_insn {bits, vl, sew_bits, lmul_log2}` + `id` |
| `cp::Port` | 사이클당 대역폭. PRIMARY 포트가 컴포넌트의 통계 단위입니다 |
| `cp::Queue<T>` | 깊이와 사이클당 push 한도를 가진 FIFO. t에 넣은 행은 t+1부터 보입니다 |
| `cp::Pipeline<T>` | gem5 MinorFU 모양의 파이프라인(latency, interval) |
| `cp::Config`, `cp::Host`, `cp::Cycle`, `cp::Id`, `cp::UNLIMITED`, `cp::intern` | 공용 층(`vcix_cp_base.hpp`)의 타입, 한도 없음 상수(`UINT64_MAX`), 프로그램 끝까지 사는 이름 |

```
 MyModel : cp::Coprocessor                    exported by VCIX_CP_REGISTER(MyModel)
  +-- Component "SFU"
        insns()    : Queue<Insn>     (2 deep, 1 push a cycle)  -> no statistic
        entry_     : Port  PRIMARY                             -> stats unit SFU
        eup_       : Pipeline<Row>                             -> no statistic
```

## cp::Coprocessor

`Coprocessor`의 override는 라우팅과 진행만 하고, 시간 규칙은 모두 컴포넌트의 `cycle()`에 있습니다.

| 메서드 | 효과 |
|---|---|
| `owns()` / `configure(config)` | 모든 컴포넌트의 인코딩을 생성 순서로 이어 붙임 / 각 컴포넌트 `configure` 후 `reset()` |
| `can_accept(insn, t)` | 소유자가 있고 accept hook이 참이고 `insns().full(t)`가 거짓 |
| `issue(insn, id, t)` | 소유자의 `insns()`에 push. `tick(t+1)`부터 보임 |
| `ready(id, t)` / `commit(insn, id, t)` | `cycle()`에서 `complete()`된 id면 같은 t에 참 / 완료 목록에서 지움 |
| `tick(t)` | 모든 `cycle()`, 그 다음 모든 블록의 `cycle()` |
| `execute(host, insn)` | Spike: `can_accept`까지 tick, push, `ready`까지 tick, commit |

`configure`, `reset`, `issue`, `commit`을 override하면 `cp::Coprocessor::`의 같은 메서드를 불러야 합니다. 인코딩이 겹치면 먼저 등록한 컴포넌트가 소유자입니다.

## cp::Component

컴포넌트는 생성자에서 인코딩을 등록하고, `configure`에서 크기를 정하고, `cycle()`에서 움직여 끝난 명령을 `complete()`합니다.

| 메서드 | 효과 |
|---|---|
| `configure(config)` / `reset()` | 키를 읽고 블록 크기를 정함 / plain 멤버만 되돌림 |
| `cycle()` | 매 tick, 블록의 `cycle()`보다 먼저. 순수 가상 |
| `valid(insn)` (선택) | 흐름 제어 전용: 지금은 못 받지만 곧 받을 때 false. copier와 systolic RTL 모드만 씁니다 |
| `Component(cp, name)`, `register_insn(e)` | 모델에 등록하고 `insns().configure(2, 1)`, 생성자에서 인코딩 등록 |
| `insns()` | 이 컴포넌트의 명령 큐 |
| `complete(insn)` | `cycle()`에서 부르면 `ready(id, t)`가 같은 t에 참 |
| `now()` | 지금 cycle. 블록이 읽는 시계이며, 사용자 코드가 직접 쓸 일은 드뭅니다 |
| `fault(insn, why, exit_code = 1)` | 틀린 프로그램(필드 오류, 순서 오류)을 실패로 표시하고 complete합니다. gem5는 commit에서, Spike는 execute에서 `why`를 내고 `exit_code`로 종료합니다. `why_stuck`은 없앱니다 |
| `values(f)` | Spike에서 `f(host())`, gem5에서는 아무것도 안 함. 값만, 타이밍은 정하지 않음 |
| `functional_model()`, `host()` | Spike(기능 모델) 여부 / Spike 레지스터와 메모리 |

```cpp
  void cycle() override {
    while (insns().length() && issue_.available()) {
      const Insn in = insns().pop();
      issue_.utilize(1);
      values([&](const Host &host) { apply(host, in); });
      complete(in);
    }
  }
```

블록은 모델 객체 안에 있어야 하므로 `std::vector`에 담으면 create가 거절합니다. 기다림은 plain 카운트다운이 아니라 블록에 두어야 Spike가 멈춤으로 보지 않습니다.

## cp::Port

`available()`는 매 사이클 width로 다시 차고 `utilize(n)`가 그만큼 쓰며, `cp::Port`를 가진 컴포넌트는 그 중 정확히 하나가 PRIMARY여야 합니다.

| 메서드 | 효과 |
|---|---|
| `Port(c, name, unit_of_work, width, role)` | role은 `REPORTING`(기본) 또는 `PRIMARY` |
| `available()` / `utilize(n)` | width − 이번 cycle에 쓴 양 / `n > available()`이면 abort |
| `set_width(w)`, `occupy(n)` | configure에서만 폭 설정, OCCUPANCY에 n을 더함 |

## `cp::Queue<T>`

t에 push한 행은 t+1부터 보이고, t에 pop으로 비운 칸은 t+1부터 빕니다.

| 메서드 | 효과 |
|---|---|
| `configure(capacity, per_cycle)` | configure에서. 큐를 비움 |
| `size()` / `length()` / `available()` / `full()` | 든 행 모두 / 이번 cycle에 보이는 행 / 더 넣을 수 있는 수 / `available() == 0` |
| `push(row)` | `tick(t+1)`부터 보임. `full()`이면 abort |
| `front()` / `pop()` | 보이는 머리 / 머리를 꺼냄. `length() == 0`이면 abort |

```cpp
    if (eup_.length() && !eup_output_.full()) eup_output_.push(eup_.pop());
    if (eup_.available() && eup_input_.length()) eup_.push(eup_input_.pop());
```

## `cp::Pipeline<T>`

t에 push한 행은 정지가 없으면 t+latency에 끝나고, 끝난 머리를 가져가지 않으면 파이프라인 전체가 멈춥니다.

| 메서드 | 효과 |
|---|---|
| `configure(latency, interval)` | 각 1 .. `UINT32_MAX`. 단계를 비움 |
| `available()` / `push(row)` | 머리가 안 막혔고 interval이 지남 / 단계 k = 0에 넣음 |
| `length()` / `front()` / `pop()` | 머리가 끝남 / 끝난 머리 / 꺼냄, `available()`가 즉시 참이 될 수 있음 |
| `on_stage(&D::fn)` | Spike에서만, 행이 나아갈 때마다 `fn(row, k)` |

## 통계와 블록 없는 모델

컴포넌트마다 통계 단위가 하나(PRIMARY `cp::Port`) 생기고, 큐·파이프라인·REPORTING 포트·표준 컨테이너·plain 멤버는 통계에 나오지 않습니다.

| 단위 | 조건 | ADMITTED | CAPACITY | OCCUPANCY |
|---|---|---|---|---|
| `<C>` | PRIMARY `cp::Port` | `utilize` 합 | width | `occupy` 합 |

Spike 진행으로 세는 것은 Queue `push`/`pop`, Pipeline `push`/`pop`/나이 듦/interval, Port `utilize(n > 0)`, `complete()`뿐입니다.
