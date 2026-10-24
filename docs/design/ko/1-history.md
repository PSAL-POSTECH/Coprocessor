# 무엇이 왜 바뀌었나

**코프로세서 모델을 쓰는 방법이 "명령마다 경로를 선언하면 프레임워크가 그것을 해석한다"에서 "컴포넌트마다 한 사이클을 직접 쓴다"로 바뀝니다.**

수치는 일회용 드라이버 결과이며 gem5/Spike 실행값이 아닙니다.

## 세 단계의 흐름

main과 PR #59는 같은 모델이고, 새 설계(구현 전)만 `cycle()`로 넘어갑니다.

```
 stage 1: main (vcix_uarch)          stage 2: PR #59 (vcix_cp)            stage 3: new design (cycle())
 -------------------------           -------------------------           ------------------------------
 Machine<Sfu, Misc, ...>              cp::Coprocessor                      cp::Coprocessor
   units as a template pack             components as members               components as members
   std::apply over units                handle(Encoding, &fn)               register_insn(Encoding)
 per insn: route(Route &r)            per insn: handler(cp::Instr &in)     per component: cycle()
   r.use().send().done_after()          in.use(); in.send(); ...            pop insns(), push/pop,
 engine reads route twice:            engine reads steps twice:             complete(insn)
   CHECK -> can_accept                  CHECK -> can_accept               engine: push into insns(),
   ISSUE -> issue + command             ISSUE -> issue + CmdQueue           tick every cycle(), then blocks
          |                                    |                                  ^
          +------------ same model ------------+                                  |
                                               +-- v1 (Rate/Queue/Pipe) ----------+
```

## 세 설계 비교

1·2단계는 "기록한 단계를 엔진이 해석"하고, 3단계는 "컴포넌트가 사이클을 직접 씁니다".

| 항목 | main `vcix_uarch` | PR #59 `vcix_cp` | 새 설계 (구현 전) |
|---|---|---|---|
| 사용자가 쓰는 것 | 명령마다 `void f(Route &)` | 명령마다 `void f(cp::Instr &)` | `register_insn`, `configure`, `reset`, `cycle()`, `complete`/`fault`; 흐름 제어가 필요하면 `valid` |
| 모델 조립 | `Machine<U...>` 템플릿 팩, `std::apply` | 멤버 컴포넌트, 오프셋 등록 | PR #59와 같음 |
| 명령의 흐름 | CHECK로 `can_accept`, ISSUE로 issue | 같음: `Instr::CHECK`/`Instr::ISSUE` | `issue`는 `insns()`에 push만 |
| 기다림/멈춤 | `require(holds)`, 포트 room, `when_stuck` | 같은 단계 + `Done::WhenIdle`, `wait_empty`, `on_stuck` | `head()`가 false. 영원히 받지 않을 것만 `valid()`가 false |
| 지연 모델링 | `done_after(latency_)` 또는 `Pipeline` + `Stall` | 같음 | `cp::Pipeline<T>`(latency, interval) |
| 보임(visibility) | "Moves are seen next cycle" | `Slots`, `arriving()` | 타임스탬프: t에 push한 행은 t+1부터 보임 |
| 프레임워크 줄 수 | `include/vcix_uarch.hpp` 1091 | `include/vcix_cp.hpp` 1093 | `cpdesign/vcix_cp.hpp.design` 460 |

## 같은 예제: SFU `vexp`

PR #59은 단계를 기록하고, 새 설계는 `cycle()`에서 EUP 파이프라인 `eup_`를 직접 진행시킵니다(특수함수와 push가 공유).

```cpp
  void special_function(cp::Instr &in) {
    const uint32_t f = in.arg(), sew = in.sew_bits();
    const char *name = in.name();
    in.require(!in.vl() || sew == 16 || sew == 32 || (sew == 64 && WIDE[f]),
               [name, sew] { fail("%s: an element of %u bits is not supported", name, sew); });
    in.use(entry_);
    in.done_after(latency_);
    in.on_done([&in](const Host &host) {
```

```cpp
  void cycle() override {
    if (eup_.length()) {
      const Row &out = eup_.front();
      if (out.special) {
        const Row row = eup_.pop();
        if (row.j + 1 == cp::group_size(row.insn)) {
          values([&](const Host &host) { special_function(host, row.insn); });
          complete(row.insn);
        }
      } else if (!eup_output_.full()) {
        eup_output_.push(eup_.pop().pushed);
      }
    }
    if (eup_.available() && eup_input_.length()) eup_.push({Insn{}, 0, false, eup_input_.pop()});
    if (insns().length() && head(insns().front())) {
      insns().pop();
      entry_.utilize(1);
      moved_ = 0;
    }
  }
```

`insns()`에서 꺼내는 데 한 사이클이 들어 `vexp`의 commit은 10에서 11로 재기준됩니다.

## 왜 이렇게 바뀌었나

각 행은 "문제 → 사용자의 결정"입니다.

| 문제 | 사용자의 결정 |
|---|---|
| 명령마다 단계를 기록하는 방식이 직관적이지 않았습니다. | `virtual void cycle(Cycle now)`를 사용자가 직접 씁니다. |
| 코루틴 프레임은 복사할 수 없어 squash와 맞지 않습니다. | 코루틴 없이 모든 상태를 복사 가능한 값으로 둡니다. |
| 받아들임 조건이 경로의 단계에 흩어져 있었습니다. | 내장 입력 큐 `insns()`: `can_accept = valid && insns()에 자리`. |
| 고정 지연을 `done_after`와 `Pipeline`+`Stall` 두 가지로 표현했습니다. | 고정 지연은 `Pipeline`으로 모델합니다. |
| 블록 종류가 많았습니다(`CmdQueue`, `Register<S>` 등). | `cp::Port`, `cp::Pipeline<T>`, `cp::Queue<T>` 셋. |
| 블록별 통계를 늘리면 ABI가 바뀔 수 있습니다. | 4 kind(ADMITTED, CAPACITY, CYCLES, OCCUPANCY)만, ABI 변경 없음. |
| 보임이 엔진 내부 목록에 있었습니다. | 타임스탬프: t에 push한 행은 t+1부터 보입니다. |
