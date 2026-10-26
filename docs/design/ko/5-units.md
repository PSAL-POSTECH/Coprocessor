# 유닛 이식과 cycle 재기준

수치는 일회용 드라이버 결과이며 gem5/Spike 실행값이 아닙니다("(run)"은 RTL 바이너리나 드라이버로 실제 실행한 값입니다).

## 요약과 유닛 목록

일곱 유닛 모두 `handle()` 핸들러를 버리고 `cycle()` 하나로 다시 썼고, Spike 값·거절 메시지·설정 키는 같고 gem5 사이클만 바뀝니다. issue 시점에 아무 일도 일어나지 않으므로 대부분의 지연이 1 늘고, pop 같은 대기는 `insns()`의 머리에서 합니다.

| 유닛 | 블록 흐름 | 줄 수 | 바뀐 동작 |
|---|---|---|---|
| Sfu | `insns -> entry -> eup -> ready`, `eup_input -> eup -> eup_output` (EUP 하나를 공유) | 127 → 166 | 지연 L → L+1. sfu_pop은 바로 accept되고 머리에서 기다립니다. |
| Misc | `insns(2W, W) -> issue(W) -> values() -> complete` | 140 → 143 | DMA 값 쓰기가 issue 시점에서 t+1의 `values()`로 옮겨 갑니다. |
| Dma | 없음(값만) | 271 → 271 | `reset()` 추가, `dma_index_key` 삭제. |
| Xlu | `insns -> queue -> stages/moves -> buffer -> line -> out` | 379 → 429 | push의 행이 t+1에 큐에 들어가고, 상태 거절이 머리로 옮겨 갑니다. |
| Systolic | `insns -> commands -> input_queue -> array -> output_queue` | 202 → 285 | golden은 머리에서 명령 전체를 옮기거나 기다리고, RTL 키는 RTL과 wrapper를 옮겨 적었습니다. |
| Msa | `insns -> input_queue -> array -> output_queue` | 145 → 199 | 상태 검사가 머리로 옮겨 가고, 배열은 끝난 머리만 붙잡습니다. |
| Mxu | `insns(2W, W) -> array<i>_input -> array<i>_line -> array<i>_lower/_upper` | 294 → 362 | vpop·vmatmul이 머리에서 기다리고, accept가 W = SLOTS + units로 묶입니다. |
| Copier | `insns(2, 1) -> commands(4) -> left_ x memory -> waits_` | 68 → 87 | copier.sv와 wrapper를 사이클 단위로 옮겨 적었고, 사이클 값은 같습니다. |

## 유닛별 cycle()

모든 `cycle()`은 블록의 출구를 비우고(`pop`), 입구를 채우고(`push`), 마지막으로 `insns()`의 머리를 처리합니다.

**Sfu** — 특수함수(vexp 등)와 sfu_push가 EUP 하나를 공유합니다. 사이클당 레지스터 하나가 들어가고, 큐에 있는 push 레지스터가 먼저 들어갑니다. 특수함수는 그룹의 레지스터마다 한 칸을 쓰고, 마지막 레지스터가 나올 때 값을 쓰고 끝납니다. EUP는 끝난 push 레지스터가 꽉 찬 `eup_output`에 못 들어갈 때만 멈춥니다(MinorFU 규칙).
```cpp
    if (eup_.length()) {
      const Row &out = eup_.front();
      if (out.special) {
        const Row row = eup_.pop();
        if (row.j + 1 == cp::group_size(row.insn)) {
          values([&](const Host &host) { special_function(host, row.insn); });
          complete(row.insn);
        }
      } else if (eup_output_.push(std::move(eup_.front().pushed))) {
        eup_.pop();
      }
    }
    if (eup_input_.length() && eup_.push(Row{Insn{}, 0, false, eup_input_.front()})) eup_input_.pop();
    if (insns().length() && head(insns().front())) {
      insns().pop();
      instructions_.add(1);
      moved_ = 0;
    }
```

**Misc** — 11개 명령 모두 t+1에 끝나며, 사이클당 W개 accept는 `insns().configure(2 * width, width)`가 맡습니다.
```cpp
    while (insns().length() && issue_.utilize(1)) {
      const Insn in = insns().pop();
      instructions_.add(1);
      values([&](const Host &host) { apply(host, in); });
      complete(in);
    }
```

**Xlu** — 유닛 안은 엄격한 프로그램 순서이고, 머리가 못 나가면 뒤 명령도 기다립니다.
```cpp
    while (insns().length() && step(insns().front())) complete(insns().pop());
    elements_in_flight_.add(staged_);
```

**Systolic** — golden 배열은 push의 첫 행을 t+1이 아니라 t+2에 받습니다.
```cpp
    if (whole_) {
      if (insns().length() && work(insns().front())) weigh(insns().pop());
    } else {
      if (commands_.length() && work(commands_.front())) commands_.pop();
      if (insns().length() && !commands_.full()) {
        const Insn in = insns().pop();
        if (!wrong(in)) enter(in);
      }
    }
```

**Msa** — 출력 큐가 찼을 때 배열 전체가 아니라 끝난 머리만 붙잡습니다.
```cpp
    if (array_.length() && output_.push(std::move(array_.front()))) array_.pop();
    if (input_.length() && array_.push(std::move(input_.front()))) {
      rows_.add(1);
      input_.pop();
    }
    if (insns().length() && carry_out(insns().front())) insns().pop();
    rows_in_flight_.add(array_.size());
```

**Mxu** — 필드 검사만 `valid()`에 남고, 상태 검사는 `carried(VALID|HEAD|WHY)`로 머리에서 합니다.
```cpp
    uint64_t in_line = 0;
    for (size_t i = 0; i < units_; i++) in_line += arrays_[i]->flow(entry_, rows_);
    while (insns().length() && carry_out(insns().front())) insns().pop();
    rows_in_flight_.add(in_line);
    active_cycles_.add(in_line != 0);
```

**Copier** — copier.sv의 한 클럭 에지가 `cycle()` 하나이며, `copier_lockstep`이 PASS입니다.
```cpp
    if (left_) {
      left_ -= memory_.utilize(1);
    } else if (commands_.length() && is(COPY, commands_.pop())) {
      left_ = 2 * BYTES / 8;
    }
```

## cycle 재기준

바뀌는 값만 모았고, 첫 gem5 실행이 이 값들의 사실상 검증입니다(구현 순서 10단계).

| 유닛 | test | old → new |
|---|---|---|
| 프레임워크 | `tests/contract/route_once.cc:59,63` | 6 → **5** |
| 프레임워크 | `tests/tpu/ports.cc` 외 드라이버, `tpuv3/run.sh:62`·`tpuv6e/run.sh:63` units | `now + latency` commit → `ready()` poll; 유닛 목록 **FAIL**, 블록 단위 추가 |
| Sfu/Misc | `tests/tpu/run.sh:52`, `:64`, `:66`, `:164`, `:186` | 10 → **11**, 10/10 → **11/11**, 4/4 → **5/5**, 10 → **11**, 10 → **11** |
| Sfu | `ports.cc:180-190` 1000 vexp, `tests/tpuv6e/run.sh:126-128` push → pop ready | `SFU.instructions` 1000 → **999**; L=10: 19 → **20**, L=4: 13 → **14** |
| Sfu (EUP 공유, 고정 테스트 없음) | push m8 다음 vexp m1 / vexp m2 / vexp m8 (L=10) | vexp ready−issue 11 → **19** / 11 → **12** / 11 → **18** |
| Xlu | `tests/tpu/run.sh:114-131` fold / wide / issue / xlu | lines 2 / 1 / 1 / 2 → **1 / 0 / 0 / 1** |
| Xlu | `run.sh:137-154` commits / eight pushes / 18원소 타일 | 2 and 2, 2 → **0 and 0, 0**; lines 8 → **5**; 3 of 13 → **0 of 13** |
| Xlu | `ports.cc` fewer_pushed(XLU) / xlu_shallower | 1, 100 → **1, 1**; (1, 11, 1) → **(1, 1, 10)** ×100 |
| Systolic | `tests/tpu/run.sh:73-95` full / stream / output full | lines 3 → **2**; 10 of 11 → **6 of 11**; 3 of 11 → **2 of 11** |
| Systolic | `ports.cc` continuous / fewer_pushed / more_pushed / interleaved | 3999 → **728** FAIL; 100, 100 → **1, 1** FAIL; 10 → **11** PASS; 56 → **51** FAIL (run) |
| Msa | `tests/tpu/run.sh:103`, `ports.cc:380,382,385` | lines 3 of 3 → **2 of 3**; 100, 100 → **1, 1**; 10 → **11**; 56 → **51** |
| Mxu | `tests/tpuv3/run.sh:54`·`tests/tpuv6e/run.sh:44` ports; tpuv3 `:59`, `:75`, `:82`, `:86` vpop commit | 1 passed → **FAIL as written**; **13, 17** / **261, 265** / **10, 12** / **258, 260** |
| Mxu | tpuv6e `:54`, `:60`, `:88`, `:98`, `:109`, `:118` | lines 4 → **1**; 4 of 8 → **0 of 8**; 8 of 12 → **0 of 12**; 3 of 12 → **0 of 12**; 2 of 14 → **0 of 14**; 2 of 10 → **0 of 10** |

## 확정된 결정 (다섯 가지 모두 사용자가 정함)

| 결정 | 정한 내용 | 영향 |
|---|---|---|
| 1. `Pipeline::hold()` | 넣지 않고 MinorFU 규칙만 둡니다. | systolic.sv의 배열 전체 정지와 다르지만 건드리는 테스트가 없습니다. 필요하면 `if (!enable) array_.hold();` 한 줄로 추가합니다. |
| 2. 행이 아직 없는 pop | 미리 받아 머리에서 기다립니다. | continuous·interleaved는 드라이버를 프로그램 순서로 고치고, fewer_pushed는 기대값을 바꿉니다. |
| 3. Spike 진행 규칙 | 유지하고 `moved()` 같은 탈출구는 만들지 않습니다. | 지금 유닛 중 걸리는 것은 없습니다. |
| 4. `vcix_cp_base.hpp`의 두 변경 | trap 시 모델 복원과 `check_layout`을 승인합니다. | ABI는 그대로이고, 비용은 Spike 명령마다 모델 복사 한 번입니다. |
| 5. 틀린 프로그램 | 순서 오류와 필드 오류 모두 `fault(insn, why, exit_code = 1)`로 완료합니다. gem5는 commit에서, Spike는 `execute`에서 `why`를 내고 끝납니다. `why_stuck`은 없앱니다. | 순서 오류는 XLU·MXU, 필드 오류는 sfu·xlu·msa·mxu·misc·systolic이 대상입니다. gem5도 이유를 출력합니다(지금은 받지 않고 멈춤). squash된 명령은 commit되지 않으므로 오류가 나지 않습니다. |
| 6. `valid()` | 흐름 제어 전용으로 좁힙니다. 이름은 `valid`로 둡니다. | copier와 systolic RTL 모드만 씁니다. `copier_lockstep`이 이 accept 시점에 기댑니다. |
| 7. 종료 코드 201·200 | xlu pattern 오류(201)와 MVIN/MVOUT 주소 overflow(200)도 `fault(insn, why, 201|200)`로 통일합니다. `std::exit` 직접 호출은 없습니다. | Spike 거절 테스트 12개의 문구와 종료 코드는 그대로입니다. gem5는 pattern 누락 pop을 영구 대기 대신 commit에서 exit 201로 끝냅니다. "gem5는 받지 않는다" 테스트와 README 문장은 "commit에서 오류로 끝난다"로 바꿉니다. |
| 8. 이름 | 공용 층과 엔진을 한 네임스페이스 `cp`로 합칩니다. `vcix_accel.hpp` → `vcix_cp_base.hpp`, `Block::end_cycle` → `cycle`, `spike()`/`on_spike()` → `functional_model()`, `VCIX_ACCEL_REGISTER` → `VCIX_CP_REGISTER`. | C ABI(`vcix_accel.h`, 심볼 `vcix_accel_model`)는 그대로입니다. RTL 모델과 contract 테스트 14곳의 include와 네임스페이스를 함께 고칩니다. 공용 층의 `Insn` 별칭은 없애고 `vcix_insn`을 씁니다(`cp::Insn`은 엔진의 id 달린 명령).  설계 파일 전체에 적용했고, 일회용 드라이버 출력 42개가 변경 전과 바이트 단위로 같습니다. |
| 9. 블록 메서드 이름 | `room(now)` → `available()`, `spend(n, now)` → `utilize(n)`. | `Port`와 `Queue`에 같이 적용. 드라이버 출력 42개 동일. |
| 10. 통계 (ABI 13) | 블록은 동작만 합니다. 통계는 `cp::Counter(component, name, capacity = 0)`뿐이고, C 표 항목은 `{group, name, capacity}`입니다. `utilize`와 `push`는 `[[nodiscard]] bool`: 자리가 없으면 아무것도 안 가져가고 `false`. | capacity > 0인 카운터가 TOGSim의 단위 `<COMPONENT>.<name>`입니다. 예: `rows`/`instructions`/`elements`(사이클당 최대 capacity)와 `*_in_flight`(사이클마다 든 양의 합). |

## 구현 순서

| 순서 | 파일 | 내용 | 검증 |
|---|---|---|---|
| 1 | `include/vcix_cp_base.hpp` | `vcix_cp_base.hpp.diff` 적용 | `tests/contract`, `tests/rtl` PASS |
| 2 | `include/vcix_cp.hpp` | `vcix_cp.hpp.design`으로 교체 | `verify/probe2`, `verify/eng/check`를 `tests/contract`로 |
| 3 | `examples/copier/copier.cc` | 새 파일 | `copier_lockstep`, `timing_probe`, `tests/rtl/run.sh` |
| 4–8 | `machine.hpp`, `{sfu,misc,dma}`, `{systolic,msa}`, `xlu`, `mxu` | trace 변경, 새 파일 | 빌드, `rtl_stall`, 각 functional |
| 9 | `tests/*/ports.cc` | `ready()` poll, 블록 단위 목록, 프로그램 순서 | 각 ports 테스트 |
| 10 | `tests/*/run.sh`, `route_once.cc` | 새 값으로 다시 고정 | gem5 실행 후 드라이버 값과 비교 |
| 11 | `README.md`, `examples/tpu*/README.md` | 핸들러 설명과 사이클 예시 재생성 | `units/*.md`의 README 목록 |
