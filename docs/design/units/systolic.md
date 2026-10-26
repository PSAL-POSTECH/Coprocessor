# Unit systolic on the new cp:: framework

**Summary.** The systolic array (examples/tpu/systolic.hpp) is rewritten as a `cycle()` function over the design
header. Under the RTL keys, the component is a cycle-by-cycle transcription of the RTL plus its `rtl_model.hpp`
wrapper. As a result, the RTL-equality tests pass unchanged when run against the real Verilated libraries. The default (golden) systolic timing keeps its values. Its gem5 cycles move, because nothing
happens at issue any more.

Labels: **FACT** = read in the repo (`file:line`) or measured in my throwaway build. **DERIVED** = reasoned, not run.
**OPINION** = a judgement or a proposal.

## Files

- New unit code, complete:
  - `cpdesign/units/examples/tpu/systolic.hpp` (285 lines; old: 202)
- Throwaway checks in `cpdesign/units/check-cs/`, rebuilt against the revised design headers in `cpdesign/verify/cs/`
  (every result below was re-run there, and equals the first run). Nothing in the repo was edited. All of this is FACT:
  - **Build.** `libcs_systolic.so` (a `tpu::Coprocessor` holding only `Systolic "SYSTOLIC"`). It
    is built with `g++ -std=c++17 -Wall -Wextra -Werror` against the two design headers (`vcix_cp.hpp.design`,
    `vcix_cp_base.hpp.design`) and `check-cs/machine.hpp`, a stub of ENGINE.md §7.
  - **Run against the repo's built binaries.** `build/rtl_stall` and `build/timing_probe`, with
    `build/librtl_systolic*.so` and the old `build/libtpu.so`. These need
    `LD_LIBRARY_PATH=/home/wonhyuks/miniforge3/lib` (GLIBCXX_3.4.30).
  - **`fakespike.cc`.** Spike's `execute()` loop over a fake host: 4 lanes, VLEN 256, an xreg/memory/frm map. It runs
    `examples/rtl/systolic/values.c`'s instruction sequence, a sweep of every format with frm 0/2/4, and the five refusal
    cases. It is not Spike, and no functional test program ran (no Spike or gem5 on this
    host).
  - **`timing.cc`.** `tests/tpu/ports.cc`'s systolic cases, with commits polling `ready()`. It also has a gem5-like
    in-order driver for `systolic_full.S` and `systolic_stream.S`. That driver offers one instruction a cycle, with the
    CPU gaps the old gem5 log shows (357 and 361 cycles for the nop blocks). Against the old libtpu.so it reproduces the
    old gem5 trace lines exactly (FACT, `build/tpu-systolic_*.gem5.log`).

---

## 1. Systolic (`tpu::Systolic`, unit SYSTOLIC)

The component has two modes, selected by `tpu_systolic_command_queue`. Both are FACT for the old semantics:

- **0, the golden default.** Old: every step happened at issue (`systolic.hpp:59-60`, the ConfigError text).
  - New: the head of `insns()` (default `(2, 1)`) does the whole instruction in one cycle, or waits.
- **N > 0, systolic.sv's mode.** New: `insns()` is the wrapper's pending skid, gated by `valid()`.
  - `commands_` is systolic.sv's command queue of N (`c_count != 2`).
  - Its head works one command a cycle, in order.

### `cycle()`, in this order (each step is pre-edge, as in systolic.sv)

1. **Spike only.** The weight beats taken in t−1 land in `weights_` (`land()`). systolic.sv's weight registers change
   at the clock edge, and old vcix_cp landed the beats after the stages (`vcix_cp.hpp:948`). The stages run in the
   blocks' `cycle()`, after the components', so the landing has to wait a cycle.
2. `enable = !output_.full(t)`: systolic.sv's `enable = out_count != Q` (`systolic.sv:67`).
3. **Head work.**
   - Golden: `insns()` head; when it is done, the head is popped.
   - RTL: first `commands_` head, then the hop `insns()` → `commands_`.
   - A vl-0 instruction completes at the hop and never enters, as in the wrapper's `issue` (`rtl_model.hpp:263-267`).
4. **Array step.** If `enable && array_.length(t)`, the finished row goes to `output_`. If `enable && input_.length(t) &&
   array_.available(t)`, one row enters the array and `rows_.add(1)`; then `rows_in_flight_.add(array_.size())`.

### Per instruction (head work)

- **input push.** As it reaches the head of `insns()` (`wrong()`), it is a `fault()` if a vl>0 format names no
  element, and before any weight push of a beat or more (`weighted_`).
  - It moves `n = min(vl − done, in_.available(), input_.available())` rows into `input_queue`. Each row's values
    are made on Spike with `values()`.
  - Golden: all vl rows or none.
  - Golden rows carry `weights_` as a snapshot (a shared_ptr, copy-on-write). RTL rows read the current weights.
  - It is `complete()`d when vl rows have moved.
- **weight push.** It sets `weighted_` as it leaves `insns()`.
  - Under `tpu_systolic_weight_waits` it waits while `input_queue` or the array holds a row. That is `weight_open`
    (`systolic.sv:56`).
  - It moves `n = min(vl − done, in_.available())` beats. Spike stores them in `landing_`, and they land next cycle.
  - It is `complete()`d at vl.
- **pop.** At the head (`wrong()`), a format that names no element (vl 0 included) and a pop to bf16 are a `fault()`.
  - It takes `n = min(vl − done, pop_.available(), output_.length(t))` rows. On Spike each row is written to element i
    of vd.
  - Golden: all vl rows or none.
  - It is `complete()`d at vl. Asking more rows than the unit holds anywhere (input queue empty, array idle, fewer
    than `vl − done` in the output queue: nothing behind it can add rows) is a `fault()`,
    `systolic pop: %u elements asked, %llu computed` with `done_ + output_queue.size()`, old `got`.
- **Row path.** input_queue → array (Pipeline, latency 2·lanes−1, interval 1, `on_stage(&Systolic::stage)`, k=0 on the
  issue cycle) → output_queue.
  - A row that enters at t is taken at t+7 on 4 lanes and is visible to a pop at t+8. That is systolic.sv's
    `valid[SLOTS-1]` exit at E0+7.

```
                       golden (command queue 0): the head moves the whole instruction
 push(t) -> +---------------+ ---------------------------------------------------------+
             | insns() 2     |                                                           v
             | valid: rules  | RTL keys: hop (valid: insns empty, commands_ room)  +-----------+
             | [+ skid]      | ----------------------------------------------->   | commands_ |
             +---------------+                                                     | N, Queue  |
                                                                                   +-----------+
                                                                                        | head works, one a cycle
   input push --in_ (rows/cycle)--> +-------------+  entry_ "input"  +-------------------------+
                                    | input_queue |---1 a cycle----->| array  Pipeline<Row>    |
                                    | entries     |  when enable     | 2L-1 stages, interval 1 |
                                    +-------------+                  | stage(row, k) on Spike  |
   weight push --in_--> landing_ --next cycle--> weights_ (plain)    +-------------------------+
                        (Spike)                                                  | take when enable
   pop <--pop_ (rows/cycle)-- +--------------+ <---------------------------------+
                              | output_queue |   enable = output_queue not full as the cycle began
                              | entries      |
                              +--------------+
```

### Preserved

- **Config keys, defaults and ConfigError texts**, verbatim: `vpu_num_lanes`, `tpu_systolic_queue_entries`,
  `_command_queue`, `_in_rows_per_cycle` (default UNLIMITED), `_pop_rows_per_cycle` (default entries) and
  `_weight_waits`. Also the "is no queue: …" rule. FACT: same code.
- **Values.** FACT in fakespike, byte for byte:
  - new golden equals old golden on the values.c sequence, and on all formats × frm 0/2/4;
  - new under `rtl-nowait.yml`'s keys equals `librtl_systolic_nowait.so` and the old libtpu's nowait run;
  - new under the RTL keys with waits equals `librtl_systolic.so`, which equals golden.
- **Refusal and stuck messages.** FACT in fakespike: exit 1 and the same text from old and new for
  - `systolic pop: 1 elements asked, 0 computed` (also under the RTL keys),
  - `systolic input push: no weight was pushed before it`,
  - `systolic: format 3 names no element of 8 bits`,
  - `systolic pop: a pop to bfloat16 is not modeled`,
  - `systolic: format 1 names no element of 32 bits`,
  - the framework's `vcix_cp: systolic input push waits, and nothing in its unit moves any more…` when the queues are
    full.

  These are `tests/tpu/functional/run.sh:157-161`. They are `fault(insn, why, exit_code = 1)`s at the head (`wrong()`,
  in the old order, and the pop's rows check in `work()`); `valid()` keeps only the RTL mode's skid rule (flow control
  only). The full-queue case has no fault, so the framework's generic message stays.
- **Spike motion.** Every wait is a pipeline aging, a queue push or pop, or a port utilize. So no rule-of-ENGINE §5
  escape hatch is needed (DERIVED; decision 3 keeps the rule).
- **Trace.** `trace_queues` prints `, input queue N, output queue M` from `input_.size()` and `output_.size()`. Old: all
  rows of both, since every arrival is SEEN by issue time (`vcix_cp.hpp:407-411`). It derives from `tpu::Component`,
  with ENGINE §7's signature `const vcix_insn &`, as the sfu design does.
- **Stats.** `SYSTOLIC.rows` (capacity 1): rows into the array; `SYSTOLIC.rows_in_flight`: rows in the stages per
  cycle (FACT: ports.cc one_row gives rows 1, capacity 1, rows_in_flight 7, committed 1).
  - No `issue` or `weight_push` port: the one-head-a-cycle structure is their limit of one a cycle.

### Statistics as gem5 prints them

After markers.S's window 1 (`tests/stats/run.sh`); gem5 prefixes each line with
`system.cpu.executeFuncUnits.funcUnits8.`.

```
vcix.SYSTOLIC.rows                   12   # rows, at most 1 a cycle      <- tests/stats pins this counter
vcix.SYSTOLIC.rows_in_flight         84   # 12 rows x 7 stages
```

---

## 2. Test table

"run" means measured in the throwaway check (FACT). "derived" means reasoned, not run.

| test | old | new | why |
|---|---|---|---|
| `tests/rtl/run.sh:71-74` rtl_stall, ready cycles | 6, 70, 79, 82, 85, 94, 158 | **6, 70, 79, 82, 85, 94, 158 (run)**, 7 PASS | the RTL keys path is the RTL plus the wrapper, and no bubble freeze occurs in that program (see R2) |
| `tests/rtl/run.sh:49-68` values vs rtl_systolic / nowait | same, differs, same | **same, differs, same (derived; fakespike byte-equal run)** | golden snapshots weights; RTL mode lands beats at the edge |
| `tests/tpu/functional` systolic sha and refusals `:157-161` | match | **match (derived; fakespike run)** | values code unchanged; refusals via `fault()` at the head |
| `tests/tpu/run.sh:73-77` systolic_full: pop delay | `pop: 11 cycles after the last issue, 0 in flight, input queue 0, output queue 4`; lines 3 | **`pop: 1 cycles after the last issue, 0 in flight, input queue 4, output queue 0`, then `commit systolic pop: 12 cycles after its issue` (derived, driver run)**; lines 2 | a pop is accepted into `insns()` and waits at its head (decision 3); rows enter one cycle later (head at t+1) |
| `tests/tpu/run.sh:79-88` systolic_stream | iq 0, iq 1..6, then `2 cycles…0 in flight, input queue 6, output queue 1/3/5`; 10 of 11 | **iq 0; iq 2..7 (`1 cycles…0 in flight`); `1 cycles…1 in flight, input queue 6, output queue 0`; `2 cycles…1 in flight, input queue 6, output queue 1`; `…output queue 3`** (derived, driver run); the old list matches 6 of 11, the new list 10 of 11 | the queue counts are read after the head moved the previous push's rows; a waiting push is in flight |
| `tests/tpu/run.sh:90-95` systolic_full, output full | 3 of 11; third `weight push … input queue 0, output queue 5` | **2 of 11 with the old text**; the new third line is `weight push: 1 cycles … input queue 1, output queue 4` (derived, driver run) | the pop of 4 takes its rows at the head one cycle after issue; the array refills next cycle |
| `tests/tpu/run.sh:163-168` one_cycle: `weight push … input queue 0, output queue 0` | 1 | queue fields unchanged (derived); the gap of 10 is MISC's design | systolic is empty there |
| `tests/tpu/ports.cc` list | PASS | PASS: SYSTOLIC stays one unit, its `input` port | queues and pipelines are no units |
| ports.cc one_row | 1, 1, 20, 7, 1 | **1, 1, 20, 7, 1 (run)** | 7 cycles in the stages, either way |
| ports.cc continuous (>0.99) | 3999 of 4000 | **728 of 4000 (run)**, FAIL | the driver offers a pop first; it is now accepted before its rows exist and blocks the pushes behind it in order |
| ports.cc squashed | same | **same (run, systolic-only)** | the whole model is plain or copy-on-write data |
| ports.cc fewer_pushed systolic | 100, 100 | **1, 1; the third pop's commit exits 1 (run)**, FAIL | an unsatisfiable pop is accepted and faults; `insns()` holds 2 |
| ports.cc more_pushed systolic | stops after 10, refused 1000 | **11, 1000 (run)**, PASS | |
| ports.cc interleaved (48+7+1) | 56 | **51 (run)**, FAIL | pops are issued ahead and wait at the head; they no longer wait for acceptance |
| `tests/stats/run.sh` counts | 12 3 3 / 0 0 0 / 4 1 1 / 8 2 2 / 0 0 0 | **unchanged (derived)** | an m5op commits after the pops before it, and a pop commits only after its rows entered the array |
| `tests/stats` listed | the eight counters | **the same** | blocks are no statistics |

The ports.cc drivers commit at `now + latency`, which wraps with Unknown (ENGINE §8). Their numbers above are from my
copy, which polls `ready()`.

---

## 3. Behaviour that moves (besides cycle counts)

All of these are consequences of decisions 3 and 5, now final (DESIGN.md §8).

- **A pop is no longer refused for missing rows.** gem5 issues it at once, and it waits at the head of `insns()`,
  blocking later SYSTOLIC instructions in order. The old `can_accept` acted as a scoreboard.
  - A pop that can never fill is a `fault()` with the same message: Spike exits 1 as its execute returns, gem5 as it
    commits (`verify/cs/timing` fewer_pushed: `1 1`, then the third pop's commit exits 1).
  - This is the cause of the continuous, fewer_pushed and interleaved changes.
- **Golden accepts one SYSTOLIC instruction a cycle**, as the old `issue` port did, now through `insns()`'s per-cycle
  limit.
- **The golden array takes a push's first row at t+2.** Before, it was t+1, because the head works at t+1.

---

## 4. Requests to the framework (status)

- **R1. Spike trap safety: done** (ENGINE §5). `export_model`'s execute restores the model when a `values()` throws,
  so a retried instruction runs once from the saved model (FACT `verify/probe2`).
- **R2. `Pipeline::hold()`: closed, not added** (DESIGN.md §8 decision 1). systolic.sv freezes the whole array while
  `output_queue` is full, bubbles included (FACT `systolic.sv:67,122-134`). A MinorFU pipeline stops only on a finished
  head not taken. No current test hits the difference: values.c pops at most 40 of 64, and rtl_stall passes (FACT
  `verify/cs`: 7 PASS). The bubble difference is accepted.
- **R3. `Queue::available()`: done.** The push uses `input_.available()`; no ordering dependence remains.
- **R4.** A block a mode does not use costs nothing in the statistics: queues and pipelines are no units.
- **R5.** `tests/tpu/ports.cc` `list()` keeps one unit per component.

## 5. Decisions (final, DESIGN.md §8)

1. A pop is accepted before its rows exist and waits at the head (the re-pins above stand); `valid()` stays flow
   control only, no scoreboard. The `ports.cc` drivers are to offer instructions in program order.
2. No `Pipeline::hold`: the bubble difference is accepted (R2).
