# 사용자 가이드

## 내 하드웨어를 모델링하기: 개요

하드웨어 블록 하나를 `cp::` 컴포넌트로 옮기고, 빌드하고, Spike와 gem5에서 돌리고, `stats.txt`를 읽기까지를 보입니다. 이 `cp::` API는 설계(`cpdesign/vcix_cp.hpp.design`)이며, 수치는 일회용 드라이버 결과이며 gem5/Spike 실행값이 아닙니다.

- 컴포넌트(`cp::Component`)는 내 하드웨어 블록 하나입니다.
- `insns()`는 그 블록의 명령 입력 큐입니다. 프레임워크가 명령을 여기에 넣어 줍니다.
- `cycle()`는 그 블록이 매 클럭 하는 일입니다. 타이밍은 모두 여기서 정합니다.
- 블록(`cp::Port`, `cp::Pipeline<T>`, `cp::Queue<T>`)은 컴포넌트 안에 배치하는 부품이며 동작만 합니다. 통계는 `cp::Counter`뿐입니다.
- 값(레지스터·메모리)은 `values()` 안에서만 씁니다. 이 코드는 Spike에서만 돕니다. 명령이 끝나면 `complete(insn)`을 부릅니다.

## 1. 예제 A: `vexp`만 하는 SFU

머리 명령이 지연 L·간격 1의 EUP에 들어가고, 끝나면 `vd = exp(vs2)`를 쓰고 완료합니다.

```
 insns()  Queue (2, 1)       in order, one accepted per cycle
            | head, when eup.push() takes it
 instructions  Counter (1)   -> statistic SFU.instructions
            v
 eup      Pipeline (L, 1)    stage 0 -> stage 1 -> ... -> stage L-1
            | length() -> pop()
 values(): vd = exp(vs2)     complete(insn)
```

파일 하나가 모델 전체입니다: 컴포넌트, 코프로세서, export. 설계 헤더로 경고 없이 빌드됩니다(실행 결과는 아래).

```cpp
// libexp_sfu.so: an SFU that computes only vexp (sf.vc.v.iv 0x3, vd, vs2, 0) on a pipelined EUP.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "vcix_cp.hpp"

namespace {

class ExpSfu : public cp::Component {
 public:
  static constexpr cp::Encoding VEXP{0x2C00305B, 0xFE00707F, "vexp"};

  ExpSfu(cp::Coprocessor &cp, const char *name) : Component(cp, name) { register_insn(VEXP); }

  void configure(const cp::Config &config) override {
    const uint64_t latency = config.uint("exp_sfu_latency_cycles", 10);
    if (latency == 0) throw cp::ConfigError("exp_sfu_latency_cycles", "0", "is not a pipeline depth: at least 1");
    eup_.configure(latency, 1);  // latency L, a new row every cycle
  }
  void cycle() override {
    // 1. Drain first: a finished vexp writes vd and completes.
    if (eup_.length()) {
      const cp::Insn in = eup_.pop();
      values([&](const cp::Host &host) { exp_of(host, in); });
      complete(in);
    }
    // 2. Then fill: the head instruction enters the EUP when it takes a row; a wrong one is a fault there instead.
    if (insns().length()) {
      const cp::Insn &head = insns().front();
      const bool wrong = !supported(head);
      if (wrong ? eup_.available() : eup_.push(head)) {
        if (wrong) fault(head, "exp_sfu: vexp: only 32-bit elements in one register (LMUL <= 1) are supported");
        instructions_.add(1);
        insns().pop();
      }
    }
  }

 private:
  static bool supported(const vcix_insn &in) { return in.sew_bits == 32 && in.lmul_log2 <= 0; }
  static void exp_of(const cp::Host &host, const cp::Insn &in) {
    for (uint32_t lane = 0; lane < host.lanes(); lane++) {
      const float *vs2 = host.vreg<float>(lane, cp::rs2(in));
      float *vd = host.vreg_mut<float>(lane, cp::rd(in));
      for (uint32_t i = 0; i < in.vl; i++) vd[i] = std::exp(vs2[i]);
    }
  }

  cp::Counter instructions_{*this, "instructions", 1};
  cp::Pipeline<cp::Insn> eup_{*this, "eup"};
};

class ExpSfuCP final : public cp::Coprocessor {
 public:
  const char *name() const override { return "exp_sfu"; }

 private:
  ExpSfu sfu_{*this, "SFU"};
};

}  // namespace

VCIX_CP_REGISTER(ExpSfuCP)
```

설정(`machine.yml`)과, 이 모델에 vexp 두 개를 연속으로 넣은 gem5식 드라이버의 출력입니다(일회용 드라이버, gem5 실행은 아님).

```yaml
vpu_num_lanes: 4
vpu_spad_size_kb_per_lane: 128
vpu_vector_length_bits: 256
exp_sfu_latency_cycles: 3
```

```text
cycle 0: can_accept=1
cycle 1: can_accept=1
cycle 4: vexp 0 ready (issued 0)
cycle 5: vexp 1 ready (issued 1)
vcix.SFU.instructions                            2  # instructions, at most 1 a cycle
vcix.committed.vexp                              2  # vexp
```

틀린 명령(64비트 원소)은 받아들여진 뒤 `fault`로 끝납니다. gem5식에서는 commit 때, Spike식에서는 `execute`가 돌아올 때 같은 문구를 내고 exit 1입니다.

```text
$ ./drive_fault                      # gem5-style
cycle 1: can_accept=1
cycle 2: ready, committing
exp_sfu: vexp: only 32-bit elements in one register (LMUL <= 1) are supported
exit=1
$ ./spike_a bad                      # Spike-style: the good vexp, then the bad one
v8 = 1 2.71828 7.38906
exp_sfu: vexp: only 32-bit elements in one register (LMUL <= 1) are supported
exit=1
```

L = 3, A를 사이클 0에, B를 사이클 1에 제시하면 issue에서 ready까지 **1 + L** 사이클입니다.

| t | `cycle()` 안에서 | `cycle()` 뒤 EUP | 같은 t의 gem5 |
|---|---|---|---|
| 0 | 할 일 없음 | 빈 상태 | A accept·issue. 사이클당 한도 1로 B 거절 |
| 1 | `eup.push(A)`, A를 세고 pop | A 나이 1 | `available(1) = 2 − 0 − 1 = 1` → B accept·issue |
| 2 | `eup.push(B)`, B를 세고 pop | A 2, B 1 | |
| 3 | 할 일 없음 | A 3 = L → A 끝남, B 2 | |
| 4 | A pop, `values`, `complete(A)` | B 3 → B 끝남 | `ready(A, 4)` 참 |
| 5 | B pop, `complete(B)` | 빈 상태 | `ready(B, 5)` 참 |

### 1.1 실제 유닛 전체: tpu `Sfu` (`sfu.hpp`)

명령 9개(특수함수 7개, `sfu_push`, `sfu_pop`)를 EUP 하나로 처리하는 실제 유닛입니다. 위 예제와 같은 뼈대에 큐 두 개와 머리 처리 함수 `head()`가 더해져 있습니다. `tpu::Component`, `message`, `get`/`put`, `fail`은 tpu 예제의 공용 헬퍼(`machine.hpp`, `values.hpp`)입니다.

```cpp
// A TPU's special-function unit around one EUP, a pipeline of tpu_sfu_latency_cycles taking one register a cycle. A special
// function sends its group's registers through it and writes vd = f(vs2) when the last leaves; sfu_push queues its
// registers for it, and sfu_pop takes them back in push order once they are out. Queued push registers enter first.
#ifndef TPU_SFU_HPP
#define TPU_SFU_HPP

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "machine.hpp"

#include "values.hpp"

namespace tpu {

class Sfu : public Component {
  using Cycle = cp::Cycle;
  using Host = cp::Host;
  using Insn = cp::Insn;
  using Register = std::vector<std::vector<float>>;
  // One register in the EUP: of a special function (j of its group), or one sfu_push queued.
  struct Row {
    Insn insn;
    uint32_t j = 0;
    bool special = false;
    Register pushed;
  };

 public:
  // sf.vc.v.iv on custom-2: opcode, funct3, funct6 and vm; FUNCTION_FIELD adds the field at 19:15. sfu_push is
  // sf.vc.vv 3 (reads vs2, writes nothing), sfu_pop sf.vc.v.x 3 (writes vd; rs1 unread).
  static constexpr uint32_t FUNCTION = 0xFE00707F, FUNCTION_FIELD = 0xFE0FF07F;
  static constexpr cp::Encoding VERF{0x2000305B, FUNCTION, "verf"};
  static constexpr cp::Encoding VTANH{0x2400305B, FUNCTION, "vtanh"};
  static constexpr cp::Encoding VSIN{0x2800305B, FUNCTION_FIELD, "vsin"};
  static constexpr cp::Encoding VCOS{0x2800B05B, FUNCTION_FIELD, "vcos"};
  static constexpr cp::Encoding VLOG{0x2801305B, FUNCTION_FIELD, "vlog"};
  static constexpr cp::Encoding VATAN{0x2801B05B, FUNCTION_FIELD, "vatan"};
  static constexpr cp::Encoding VEXP{0x2C00305B, FUNCTION, "vexp"};
  static constexpr cp::Encoding SFU_PUSH{0x2E00005B, FUNCTION, "sfu_push"};
  static constexpr cp::Encoding SFU_POP{0x0C00405B, FUNCTION, "sfu_pop"};

  // `name` is the component as its statistics report it; the model that holds the component says it.
  Sfu(cp::Coprocessor &cp, const char *name) : Component(cp, name) {
    for (const cp::Encoding *e : ALL) register_insn(*e);
  }
  void configure(const cp::Config &config) override {
    const uint64_t latency = config.uint(LATENCY_KEY, 10);
    if (latency == 0) throw cp::ConfigError(LATENCY_KEY, "0", "is not a pipeline depth: at least 1");
    const uint64_t entries = config.uint(QUEUE_KEY, DEFAULT_ENTRIES);
    if (entries < 1 || entries > UINT32_MAX)
      throw cp::ConfigError(QUEUE_KEY, config.get(QUEUE_KEY), "is not a queue size");
    eup_.configure(latency, 1);
    eup_input_.configure(entries);
    eup_output_.configure(entries);
  }
  void reset() override { moved_ = 0; }
  // The EUP's finished register leaves (a special function's last completes it); a queued push register enters, else
  // the head's next register; then the head instruction.
  void cycle() override {
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
  }
  // What the [tpu] trace adds to an issue: the EUP's queues for a push or a pop, nothing for a special function.
  void trace_queues(const vcix_insn &insn) const override {
    if (form(insn) == SFU_PUSH.match || form(insn) == SFU_POP.match)
      std::printf(", input queue %u, output queue %u", static_cast<uint32_t>(eup_input_.size()),
                  static_cast<uint32_t>(eup_output_.size()));
  }

 private:
  // One cycle of the head: a special function's next register enters the EUP; a push queues, a pop takes, what fits,
  // and both complete with their last register. An element width it does not compute, or a pop with nothing left
  // anywhere to take, is a fault. True: the head leaves insns().
  bool head(const Insn &in) {
    const uint32_t registers = cp::group_size(in);
    if (!supported(in)) {
      fault(in, message("%s: an element of %u bits is not supported", ALL[index(in)]->name, in.sew_bits));
      return true;
    }
    if (form(in) == SFU_PUSH.match) {
      for (; moved_ < registers && !eup_input_.full(); moved_++) {
        Register row;
        values([&](const Host &host) { row = pushed(host, in, moved_); });
        eup_input_.push(std::move(row));
      }
      if (moved_ == registers) complete(in);
      return moved_ == registers;
    }
    if (form(in) == SFU_POP.match) {
      for (; moved_ < registers && eup_output_.length(); moved_++) {
        const Register row = eup_output_.pop();
        values([&](const Host &host) { popped(host, in, moved_, row); });
      }
      if (moved_ == registers) complete(in);
      else if (!eup_input_.size() && eup_.idle() && !eup_output_.size()) {
        fault(in, message("sfu_pop: lane %u has no pushed result left", 0u));
        return true;
      }
      return moved_ == registers;
    }
    if (!eup_.available()) return false;
    eup_.push({in, moved_++, true, {}});
    return moved_ == registers;
  }
  // sfu_push's register j of vs2's group: 2^x of its elements per lane.
  static Register pushed(const Host &host, const Insn &in, uint32_t j) {
    const uint32_t per = host.vlen_bits() / 32, from = j * per, to = std::min(in.vl, from + per);
    Register out(host.lanes());
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = from; i < to; i++) out[lane].push_back(::exp2f(get<float>(host, lane, cp::rs2(in), i)));
    return out;
  }
  // sfu_pop's register j of vd's group, up to vl.
  static void popped(const Host &host, const Insn &in, uint32_t j, const Register &got) {
    const uint32_t per = host.vlen_bits() / 32, from = j * per;
    for (uint32_t lane = 0; lane < host.lanes() && lane < got.size(); lane++)
      for (uint32_t k = 0; k < got[lane].size() && from + k < in.vl; k++)
        put<float>(host, lane, cp::rd(in), from + k, got[lane][k]);
  }
  // Function f of the seven, element by element, on elements of 16, 32 or (vlog, vatan) 64 bits.
  static void special_function(const Host &host, const Insn &in) {
    const uint32_t f = index(in), vs = cp::rs2(in), vd = cp::rd(in);
    for (uint32_t lane = 0; lane < host.lanes(); lane++)
      for (uint32_t i = 0; i < in.vl; i++)
        if (in.sew_bits == 16) put<uint16_t>(host, lane, vd, i, double_to_half(SINGLE[f](half_to_float(get<uint16_t>(host, lane, vs, i)))));
        else if (in.sew_bits == 32) put<float>(host, lane, vd, i, SINGLE[f](get<float>(host, lane, vs, i)));
        else put<double>(host, lane, vd, i, WIDE[f](get<double>(host, lane, vs, i)));
  }
  // The element widths each instruction computes: push and pop 32 bits; a special function 16, 32, or 64 if WIDE.
  static bool supported(const vcix_insn &in) {
    if (form(in) == SFU_PUSH.match || form(in) == SFU_POP.match) return in.sew_bits == 32;
    const uint32_t sew = in.sew_bits;
    return !in.vl || sew == 16 || sew == 32 || (sew == 64 && WIDE[index(in)]);
  }
  static uint32_t form(const vcix_insn &in) { return in.bits & FUNCTION; }
  // Its place in ALL, which for a special function is its place in SINGLE and WIDE.
  static uint32_t index(const vcix_insn &in) {
    uint32_t i = 0;
    while ((in.bits & ALL[i]->mask) != ALL[i]->match) i++;
    return i;
  }

  static inline const cp::Encoding *const ALL[] = {&VERF, &VTANH, &VSIN, &VCOS, &VLOG, &VATAN, &VEXP, &SFU_PUSH, &SFU_POP};
  static inline float (*const SINGLE[])(float) = {::erff, ::tanhf, ::sinf, ::cosf, ::logf, ::atanf, ::expf};
  static inline double (*const WIDE[])(double) = {nullptr, nullptr, nullptr, nullptr, ::log, ::atan, nullptr};
  static constexpr const char *LATENCY_KEY = "tpu_sfu_latency_cycles";
  static constexpr uint64_t DEFAULT_ENTRIES = 16;
  static constexpr const char *QUEUE_KEY = "tpu_sfu_queue_registers";

  cp::Counter instructions_{*this, "instructions", 1};
  cp::Queue<Register> eup_input_{*this, "eup_input"};
  cp::Pipeline<Row> eup_{*this, "eup"};
  cp::Queue<Register> eup_output_{*this, "eup_output"};
  uint32_t moved_ = 0;
};

}  // namespace tpu

#endif
```

tpu 모델의 공용 부분 `machine.hpp`: trace와 `message`를 더한 `Coprocessor`/`Component`입니다.

```cpp
// A TPU as one model: a cp::Coprocessor of its components, plus tpu_trace, a line at every issue and every commit. A
// model derives from it and holds its components as members, as tpu.cc does. An issue's line ends with what the
// owner's trace_queues(insn) prints when it is a tpu::Component; any other component adds nothing.
#ifndef TPU_MACHINE_HPP
#define TPU_MACHINE_HPP

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>

#include "vcix_cp.hpp"

namespace tpu {

// A fault's why, as fail() would print it: "tpu: " and the formatted text.
inline std::string message(const char *format, ...) {
  char text[512];
  va_list arguments;
  va_start(arguments, format);
  std::vsnprintf(text, sizeof text, format, arguments);
  va_end(arguments);
  return std::string("tpu: ") + text;
}

// A component that says itself what the [tpu] trace adds to an issue of its instructions.
class Component : public cp::Component {
 public:
  using cp::Component::Component;
  virtual void trace_queues(const vcix_insn &insn) const = 0;
};

class Coprocessor : public cp::Coprocessor {
  using Cycle = cp::Cycle;
  using Id = cp::Id;
  using Insn = vcix_insn;

 public:
  void configure(const cp::Config &config) override {
    trace_ = config.uint("tpu_trace", 0) != 0;
    cp::Coprocessor::configure(config);
  }
  Cycle issue(const Insn &insn, Id id) override {
    if (trace_) trace_issue(insn);
    return cp::Coprocessor::issue(insn, id, now);
  }
  void commit(const Insn &insn, Id id) override {
    if (trace_) trace_commit(insn);
    cp::Coprocessor::commit(insn, id, now);
  }
  void reset() override {
    cp::Coprocessor::reset();
    issued_any_ = false;
    last_issue_ = 0;
    in_flight_.clear();
  }

 protected:
  Coprocessor() = default;

 private:
  const char *name_of(const Insn &insn) const {
    for (const cp::Encoding &e : owns())
      if ((insn.bits & e.mask) == e.match) return e.name;
    return "?";
  }
  void trace_issue(const Insn &insn) {
    std::printf("[tpu] issue %s: ", name_of(insn));
    if (issued_any_)
      std::printf("%" PRIu64 " cycles after the last issue, %zu in flight", now - last_issue_, in_flight_.size());
    else
      std::printf("the first");
    if (const auto *own = dynamic_cast<const Component *>(owner(insn))) own->trace_queues(insn);
    std::printf("\n");
    std::fflush(stdout);
    issued_any_ = true;
    last_issue_ = now;
    in_flight_.push_back();
  }
  void trace_commit(const Insn &insn) {
    const Cycle issued = in_flight_.front();
    in_flight_.pop_front();
    if (replaying()) return;
    std::printf("[tpu] commit %s: %" PRIu64 " cycles after its issue\n", name_of(insn), now - issued);
    std::fflush(stdout);
  }

  bool trace_ = false;
  bool issued_any_ = false;
  Cycle last_issue_ = 0;
  std::deque<Cycle> in_flight_;
};

}

#endif
```

## 2. 예제 B: backpressure가 있는 유한 버퍼

출력 큐가 차면 배열이 서고, 입력 큐가 차면 push 명령이 `insns()`의 머리에서 기다립니다.

```
 push: n = min(vl - done, input.available())           pop: n = min(vl - done, output.length())
   v                                                   ^
 input_queue (N) --1/cycle, when enable--> array (L, 1) --pop, when enable--> output_queue (N)
```

```cpp
    const bool enable = !output_.full();
    if (insns().length() && work(insns().front())) insns().pop();
    if (enable && array_.length() && output_.push(std::move(array_.front()))) array_.pop();
    if (enable && input_.length() && array_.push(std::move(input_.front()))) { input_.pop(); rows_.add(1); }
```

## 3. 빌드하고 실행하기

`CMakeLists.txt`에 `vcix_accel_add_model(exp_sfu examples/exp_sfu/exp_sfu.cc)` 한 줄을 더하면 `build/libexp_sfu.so`가 나옵니다.

```
scripts/build.sh
source scripts/sim.sh
rv_program examples/exp_sfu/vexp2.S build/vexp2
spike_run build/libexp_sfu.so examples/exp_sfu/machine.yml build/vexp2                  # values
gem5_run build/m5out-exp build/libexp_sfu.so examples/exp_sfu/machine.yml build/vexp2   # time
```

gem5의 `stats.txt` 줄은 카운터마다 `system.cpu.executeFuncUnits.funcUnits<N>.vcix.<component>.<counter>` 모양입니다. §1의 두 `vexp`라면 다음이 나와야 합니다.

```
system.cpu.executeFuncUnits.funcUnits8.vcix.SFU.instructions           2
system.cpu.executeFuncUnits.funcUnits8.vcix.committed.vexp             2
```

## 4. 점검표

| 규칙 | 이유 |
|---|---|
| 블록과 카운터는 모델 객체 안에 둡니다(`std::vector` 금지, `std::optional`은 가능) | 바이트 오프셋으로 찾으므로 복사본(squash)이 자기 블록을 찾아야 합니다 |
| 카운터 이름은 컴포넌트 안에서 유일하게(`_`로 바꾼 뒤에도) | 아니면 `counters C.a and C.b are both vcix.C.x as statistics`로 거절됩니다 |
| 멤버에 포인터를 두지 않습니다 | 복사본이 원본을 가리킵니다 |
| 시간만 흐르는 기다림은 `Pipeline(L, 1)`에 둡니다 | plain 카운트다운은 Spike에서 첫 tick에 멈춤으로 끝납니다 |
| 틀린 명령(필드 오류, 순서 오류)은 `cycle()`에서 `fault(insn, why, exit_code = 1)`; "나중에 될" 명령은 머리에서 기다리게 | accept hook은 RTL의 ready처럼 잠깐 못 받을 때만 씁니다 |
| ISA 명령 하나 = `Insn` 하나 = `complete()` 한 번 | commit은 id를 하나만 지웁니다 |
