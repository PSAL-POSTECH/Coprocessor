# msa-mxu: the multi-precision array and the matrix unit on the cycle()-function framework

Labels:
- **FACT**: read in the repo (`file:line`, branch `cp-component`), or measured with the throwaway build described in
  "Verification".
- **DERIVED**: a cycle number computed by a throwaway in-order driver of the new design. It is not a gem5 run. The
  same driver reproduces every cycle value the current code pins in the tests (see "Verification").
- **OPINION**: a design choice or a judgement.

## 1. Files

| file | state |
|---|---|
| `units/examples/tpu/mxu.hpp` | rewritten: 362 lines (294 today) |
| `units/examples/tpu/msa.hpp` | rewritten: 199 lines (145 today) |
| `units/examples/tpuv3/tpuv3.cc`, `units/examples/tpuv6e/tpuv6e.cc` | **byte-for-byte unchanged** (FACT, `cmp`): they only hold components and a `Shape`, and use no engine API |
| `examples/tpuv3/gem5/*.py`, `examples/tpuv6e/gem5/*.py` | unchanged: they use no engine API |
| `examples/tpuv3/README.md`, `examples/tpuv6e/README.md` | **not rewritten here**. §9 lists the sections that change. |
| `check-msa-mxu/` (next to `units/`) | the throwaway checks: `harness.cc`, `squash.cc`, `ports_msa.cc`, `liststats.cc`; rebuilt against the revised design headers in `verify/mm/` (all numbers below are from that rebuild) |

Both headers include `machine.hpp`. They use the new `tpu::Component`, whose
`trace_queues(const vcix_insn &)` is declared in `units/examples/tpu/machine.hpp`, written by the
machine design. The repo's Msa is a plain `cp::Component` (FACT `examples/tpu/msa.hpp:17`). In this design it
becomes a `tpu::Component`, so the machine's one-pipeline fallback can go (ENGINE §7).

## 2. Design choices (OPINION, each needed to keep a behaviour)

1. **Instructions are carried out at the head of `insns()`, in program order, each one whole in one cycle.** This
   is today's "every step at issue", moved from `push(t)` to `tick(t+1)`:
   - a `vmatmul` or an msa input push puts all its rows in the input queue at once;
   - a `vpop` or an msa pop takes all its `vl` rows at once.

   The head waits, and blocks everything behind it, until its slots, its rows and its room allow it.
   - Today such an instruction was refused at `can_accept`, which blocked the in-order CPU (FACT
     `vcix_cp.hpp:726-741`). So the net order is the same.
   - Several heads can go in one cycle, as far as the ports allow. For example, on the v6e a vmatpush and the
     inputs' vmatprep go in one cycle.
   - A later head sees the state an earlier head wrote in the same cycle: a vmatpush and a latching vmatmul can
     go in one cycle.
2. **Every check runs at the head.** One function per unit (`Mxu::carried(insn, why)`, `Msa::refusal(insn)`) holds
   every check in today's order: fields and configuration (sew, format, array index, `[4:3]`, `[2]`, registers, the
   msa column shift), then the array's state (held halves, held rows, pushed, latched, the msa's packing against
   `format_`). The first that fails is a `fault(insn, message(...))` (signature `fault(insn, why, exit_code = 1)`) with
   today's text: Spike exits 1 as the instruction's execute returns, gem5 as it commits. `valid()` is not overridden
   (flow control only).

   Why: a state check in `valid()` would read the state before the instructions ahead of it in `insns()` have
   changed it, and a wrong program is not refused but faulted. A vmatprep whose operand's half is still held is not
   a fault: it waits, and the framework's generic message follows (as today).
3. **`insns()` depth and per-cycle limit** (`Queue::configure(2W, W)` sustains exactly W a cycle, ENGINE §3).
   - MSA: the default `(2, 1)`, what its `issue` port (1 a cycle) allows.
   - MXU: `(2 (SLOTS + units), SLOTS + units)`, W the most it can carry out in a cycle (SLOTS MXU slots plus one
     vpop per array).
   - Consequence: acceptance no longer depends on the CPU's issue width beyond W: FACT (driver) the tpuv3 `mxu`
     issue lines are the same at CPU width 2 and 12 (§7, `machine-mxu`).
4. **Lines.**
   - MXU: `take` the finished word only when every output queue of its array has `available(now) >= its rows`. This is
     exactly today's `Stall::WHEN_HEAD_BLOCKED` (FACT `vcix_cp.hpp:392`), so the MinorFU rule matches the MXU.
   - MSA: `take` only when `!output_.full()`. Today it is `WHEN_OUTPUT_FULL` (FACT `msa.hpp:38`,
     `vcix_cp.hpp:392`): the array stops whenever the output queue is full, even if its head is not finished.
     DESIGN.md §8 decision 1 (final) adds no `Pipeline::hold`: the MSA takes the MinorFU rule. No test pins MSA's
     full-queue timing exactly. `more_pushed(MSA)` still passes, stopping after 11 rounds instead of 10 (DERIVED).
5. **Values** are computed where today computes them. An entering row's outputs are made when the row is pushed into
   the input queue (FACT `mxu.hpp:154-155`, `msa.hpp:56`), now inside `values()`. A latch is applied before the
   vmatmul's rows (FACT: `on_issue` runs inside the handler, before `Instr::issue` moves the rows,
   `vcix_cp.hpp:630-637, 750-753`).

## 3. Block diagrams

### MSA (`tpu::Msa`, unit `MSA`)

```
 gem5 issue / Spike execute
            |
            v
   +------------------+
   | insns() (2, 1)   |  Queue<Insn>                                   no statistic
   +------------------+
            | head, at most 1 a cycle (port issue, 1)
   +--------+-------------------------+---------------------------+
   | weight push                      | input push                | pop
   v                                  v                           ^
 weights_, format_            +----------------+                  |
 (plain members;              | input_         |  Queue<Row>      |   stats unit MSA_input_queue
  port weight_push)           | queue_entries  |  vl rows at once |
                              +----------------+                  |
                                      | 1 row a cycle, port input (PRIMARY, 1)   stats unit MSA
                                      v                           |
                              +----------------+                  |
                              | array_         |  Pipeline<Row>   |   stats unit MSA_array
                              | 2*lanes-1      |  interval 1      |
                              +----------------+                  |
                                      | taken while output_ not full
                                      v                           |
                              +----------------+                  |
                              | output_        |  Queue<Row> -----+   vl rows at once, port pop (entries a cycle)
                              | queue_entries  |                      stats unit MSA_output_queue
                              +----------------+
```

### MXU (`tpu::Mxu<Shape>`, unit `TPUV3_MXU` / `TPUV6E_MXU`), arrays i = 0 .. tpu_mxu_units-1

```
 gem5 issue / Spike execute
            |
            v
   +--------------------------------+
   | insns() (2W, W), W=SLOTS+units |  Queue<Insn>, program order          no statistic
   +--------------------------------+
            | heads in order; shared port issue = SLOTS (vmatprep, vmatpush, vmatmul)
            | per array: port weight_issue (prep W, push), input_issue (prep I, matmul), pop_issue (vpop), 1 each
            |
   vmatprep / vmatpush: array i's State and Values only (nothing enters the array)
   vmatmul: vl*rows_in_word rows at once into array i's input queue
            |
   +--------v---------------------------------- array i ----------------------------------+
   |  +--------------------+   1 word a cycle    +------------------+  word taken when   |
   |  | array<i>_input     |-------------------->| array<i>_line    |  every output has  |
   |  | Queue<Input>, rows |   shared port input | Pipeline<Word>   |  room for its rows |
   |  | queue_entries      |   (PRIMARY,         | delay_cycles,    |----------+         |
   |  +--------------------+   PER_CYCLE*units)  | interval 1       |          |         |
   |                                             +------------------+          v         |
   |                     +---------------------------+   +---------------------------+   |
   |                     | array<i>_lower (or _output)|  | array<i>_upper (2 regs)   |   |
   |                     | Queue<Row>, queue_entries |   | Queue<Row>, queue_entries |   |
   |                     +---------------------------+   +---------------------------+   |
   +----------------------------------|-------------------------------|------------------+
                                      +---------- vpop [2] picks -----+
                                                   | vl rows at once, shared port pop (queue_entries a cycle)
                                                   v
                                                vd (Spike)
```

On a bound or v3 array (one register an operand) there is one output queue, named `array<i>_output`.

## 4. Per instruction: its path through the blocks

All instructions enter `insns()` at gem5's issue and are at most seen by `tick(t+1)`.

| instruction | at the head of `insns()` (in `cycle`) | completes |
|---|---|---|
| **msa push, weights** (`[4]`) | needs `issue` room. Spends `issue` and `weight_push`. Spike: the columns shift in, emptied when the format or shift changes. `format_ = [1:0]`. | the same cycle |
| **msa push, inputs** | needs `issue` room, the HEAD check (inputs pack as the weights do), and `input_.available() >= vl`. Pushes `vl` rows; Spike: each row's outputs made now. | the same cycle. The rows then go input_ → array_ (1 a cycle, `MSA.input` port) → output_ |
| **msa pop** | needs `issue` room, `output_.length() >= vl` and `pop` room `>= vl`. Pops `vl` rows; Spike: narrowed into vd. | the same cycle |
| **vmatprep** | needs an MXU slot, its operand's slot, and the HEAD check `!held[operand]`. Sets held and rows. Spike: keeps the upper half. | the same cycle |
| **vmatpush** | needs an MXU slot, `weight_issue`, and HEAD: a held half of equal rows (2 registers). Spike: stages the weights. Clears held, sets pushed. | the same cycle |
| **vmatmul** | needs an MXU slot, `input_issue`, HEAD (held half, rows, pushed or latched), and `input.available() >= vl*rows_in_word`. Spike: latch first. Pushes its rows, a word's first row tagged with its row count; Spike: outputs made now. Clears held, sets latched on a latch. | the same cycle. Then each array's `flow()`: one word a cycle into the line (spending its rows at `<C>.input`), and after `tpu_mxu_delay_cycles` into both outputs |
| **vpop** | needs `pop_issue`, `out(half).length() >= vl` and `pop` room `>= vl`. Pops `vl` rows; Spike: columns `[half*lanes, +lanes)` to vd. | the same cycle |

Latency from issue at t, with nothing ahead (DERIVED):
- a one-step instruction completes at t+1, as the framework's Misc does;
- an entering row is first poppable at (cycle the vmatmul is carried out) + 1 + (its word's place in line) + delay + 1.
  The first +1 is the input queue's stamp, the last +1 the output queue's.

**Faults (Spike as execute returns, gem5 as the instruction commits).**
1. The first failing field or state check, exactly today's text and order. A vmatprep whose operand's half is still
   held is no fault, as today (FACT `mxu.hpp:97`, `require` without a reason): it waits, and the framework's generic
   message follows.
2. A vpop or msa pop asking more rows than its output queue holds while its array is idle (input queue empty and
   line idle: nothing behind it can add rows) faults with `… rows asked … computed` and the rows its output queue
   holds. That is today's "receive from a drained pipeline" (FACT `vcix_cp.hpp:1002, 1058-1069`). A pop whose rows
   are still in flight waits.
3. Otherwise nothing faults, and the generic `vcix_cp: X waits, …` message follows. Example: a vpop of more than
   the queue holds, whose line stalls; a vmatmul of more rows than the input queue has entries.

## 5. Kept behaviours

- **Config keys and their meaning:** `vpu_num_lanes`, `tpu_mxu_queue_entries`, `tpu_mxu_delay_cycles`,
  `tpu_mxu_units`, `tpu_mxu_bound_to_lanes`, `tpu_systolic_queue_entries`. Each has the same check and the same
  `ConfigError` text (FACT: copied from `mxu.hpp:45-59`, `msa.hpp:33-37`). The queue key sizes the input queue
  and each output queue in rows, as today's slots did.
- **Encodings and names:** unchanged.
- **Spike values:** identical. FACT, by register-file hash, old model vs new on the same programs: tpuv3 mxu.S and
  mxu_rate.S; tpuv6e mxu.S and mxu_rate.S; mxu_arrays.S on two arrays; the bound v6e on mxu.S; and an msa program
  of all four formats, every pop width and a shift. 7 of 7 are the same.
- **Refusal messages:** every message below, with exit 1. The stderr of old and new is byte-identical over 22
  refusal programs (FACT, harness). The programs follow `tests/tpuv6e/functional/mxu.c:120-160`,
  `tests/tpuv3/functional/run.sh:142-147` and `tests/tpu/functional/refused.c:52-56`.

| message | grepped at | new producer |
|---|---|---|
| `tpu: vpop: 1 rows asked of the upper half, 0 computed` | tpuv6e/functional/run.sh:50 | `fault`, idle array |
| `tpu: vmatmul: no vmatprep of its upper half came before it` | :51 | `carried`, `fault` |
| `tpu: vmatpush: no vmatprep of its upper half came before it` | :52, tpuv3/functional/run.sh:145 | `carried`, `fault` |
| `tpu: vmatpush: 1 rows, and the vmatprep before it held 2` | :53 | `carried`, `fault` |
| `tpu: vmatmul: no weight was pushed before it` | :54 | `carried`, `fault` |
| `tpu: vmatmul: no weights were latched before it, and it does not latch` | :55 | `carried`, `fault` |
| `tpu: vmatpush: field 3 names no half of the depth: [4:3] is 1 or 2` | :56 | `carried`, `fault` |
| `tpu: vmatpush: [4:3] 3 is the v6e's vmatpush3, which is not modelled` | :57 | `carried`, `fault` |
| `vcix_cp: vmatprep waits, and nothing in its unit moves any more` | :58, :59 | framework: the held vmatprep waits, no fault |
| `tpu: vmatprep: format 2 names nothing a word holds` | :60 | `carried`, `fault` |
| `tpu: vmatpush: its elements are words of 32 bits, not of 16` | :61 | `carried`, `fault` |
| `tpu: vpop: array 1, and the machine has 1 (tpu_mxu_units)` | :62 | `carried`, `fault` |
| `tpu: vmatmul: array 2, and the machine has 2 (tpu_mxu_units)` | :69 | `carried`, `fault` |
| `tpu: vmatprep: an operand of an array of lanes x lanes is one register, with no upper half` | tpuv3/functional/run.sh:142,146 | `carried`, `fault` |
| `tpu: vmatpush: [4:3] 2 names a second half of the depth, which an array of lanes x lanes has not` | :143 | `carried`, `fault` |
| `tpu: vpop: [2] names the upper half of the columns, which an array of lanes x lanes has not` | :144 | `carried`, `fault` |
| `tpu: msa pop: 8 elements asked, 3 computed` | tpu/functional/run.sh:165 | `fault`, idle array |
| `tpu: msa push: its elements are words of 32 bits, not of 16` | :166 | `carried`, `fault` |
| `tpuv3:`/`tpuv6e: machine description: tpu_mxu_queue_entries: '1' is not a queue size: a word of two rows needs 2` | tpuv3/run.sh:145, tpuv6e/run.sh:163 | `configure`, unchanged |
| `tpuv6e: machine description: tpu_mxu_bound_to_lanes: '2' is not 0 …` | tpuv3/run.sh:103 | `configure`, unchanged |

- **Trace:** `trace_queues` prints the same fields: `, array i` (several arrays only), then `, input queue N, output
  queue M` or `, output queues L and U`. N, L and U are now `Queue::size()`: the rows held after `cycle()`. An
  issue line therefore shows the queues when the instruction entered `insns()`, not when it is carried out.
- **Squash:** FACT (`check-msa-mxu/squash.cc`). The MXU of two arrays (blocks inside `std::optional<Array>`)
  takes a wrong-path vmatmul issued at 6 and squashed at 9. It ends with every statistic and every commit cycle
  of the run without it. The timing harness also issues through `cp::Instance`, which copies the model at every
  issue.

## 6. Statistics

**Component units are unchanged** (FACT, harness):

| run | old | new |
|---|---|---|
| tpuv3 `mxu.S`, `TPUV3_MXU` admitted/capacity/occupancy | 8 / 1 / 56 | 8 / 1 / 56 |
| tpuv6e `mxu.S`, `TPUV6E_MXU` | 8 / 2 / 120 | 8 / 2 / 120 |
| bound v6e on tpuv3 `mxu.S` | 8 / 2 / 56 | 8 / 2 / 56 |
| `mxu_arrays.S`, 2 units | 16 / 4 / 240 | 16 / 4 / 240 |
| tpu `msa.S`, `MSA` | 4 / 1 / 28 | 4 / 1 / 28 |

OCCUPANCY at the primary `input` port is still the rows in the lines, summed per cycle. Each component calls
`entry_.occupy(rows in its lines)` after the cycle's pop and push, which counts a row on its `delay` cycles, as
`vcix_cp.hpp:945` did.

**No new units** (framework rule: only a component's PRIMARY port is a statistic). The blocks' ports() order:

- `TPUV3_MXU`: `insns`, `input` (PRIMARY), `array0_input`, `array0_line`, `array0_output`.
- `TPUV6E_MXU`: the same, with `array0_lower` and `array0_upper` in place of `array0_output`, and per array
  `array<i>_input`, `array<i>_line`, `array<i>_lower`, `array<i>_upper`.
- `MSA`: `insns`, `input` (PRIMARY), `input_queue`, `array`, `output_queue`. (The queues are named
  `input_queue`/`output_queue` so that an abort message never confuses the queue with the Port `input`.)

As gem5 prints them in `stats.txt` (values DERIVED by the driver for tpuv3 `mxu.S` on 4 lanes, queues of 8; gem5
prefixes each line with `system.cpu.executeFuncUnits.funcUnits<N>.`, FACT `README.md:837`):

```
vcix.TPUV3_MXU.admitted                      8   # rows admitted at port input
vcix.TPUV3_MXU.capacity                      1   # rows per cycle
vcix.TPUV3_MXU.occupancy                    56   # rows held, summed over cycles
```

(`cycles` and `utilized_cycles` follow as in ENGINE §6.) The arrays' queues and lines are no units. On the v6e
`TPUV6E_MXU.admitted` counts rows: 8, with occupancy 120.

`tests/stats` (DERIVED): `consistent` still finds `vcix.MSA.*`, and `beyond_units` stays 0.

## 7. Tests: old cycles, new cycles

The driver (`verify/mm/harness`) issues in order, up to `executeIssueLimit` a cycle: 2 on gem5_se.py (MinorCPU's
default, FACT gem5 `BaseMinorCPU.py:409-410`) and 12 on the tpuv3/tpuv6e machines (FACT
`examples/tpu/gem5/vpu_config.py:294`), and commits in order in the cycle `ready()` is true. Built against today's
code it reproduces every "old" value below (FACT: `check-msa-mxu/old.out` through the tests' own `issues`/`said`
rules, `verify/pins.py`). The "new" values are DERIVED by the same driver on the new code, not run on gem5.

`issues` is `tests/tpuv3/run.sh:38-41`'s format `name:cycles-after-last:input/output` (lines that are not "the
first"). "lines k of n" is the tests' `said` count.

| test | old | new (derived) | why |
|---|---|---|---|
| tpuv3/run.sh:54 `tpuv3_ports`, tpuv6e/run.sh:44 | exit 0, 1 passed | exit 0, 1 passed | one unit per component (§6); the `expected` table stays |
| tpuv3/run.sh:59 `mxu` | `vmatmul:1:0/0 vpop:11:0/4 vpop:4:0/4` | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0` | the vpops are accepted at once and wait at the head; they commit 13 and 17 after their issues |
| tpuv3/run.sh:62 units, admitted | `TPUV3_MISC TPUV3_MXU TPUV3_SFU TPUV3_XLU`; 8, 1, 56 | unchanged; **8, 1, 56** | §6 |
| tpuv3/run.sh:66 `mxu_rate` | `vmatmul:1:0/0 vmatmul:4:0/0 vpop:7:1/4 vpop:4:0/4 vpop:4:0/4` | `vmatmul:0:0/0 vmatmul:1:0/0 vpop:0:0/0 vpop:1:4/0 vpop:1:3/0` | the second vmatmul (8 rows) waits at the head for the singles to leave the queue of 8 (commits 6 after its issue); vpops commit 13, 17, 20 |
| tpuv3/run.sh:70 `mxu_rate-16` (first two) | `vmatmul:1:0/0 vmatmul:1:3/0` | `vmatmul:0:0/0 vmatmul:1:0/0` | queues of 16 take both, one MXU slot a cycle |
| tpuv3/run.sh:75 `lanes-128` | `vmatmul:1:0/0 vpop:259:0/4 vpop:4:0/4` | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0` | as `mxu`; vpops commit 261 and 265 after their issues |
| tpuv3/run.sh:82 `tpuv6e-bound-4` | `vmatmul:0:0/0 vpop:9:0/4 vpop:2:0/4`; capacity 2, occupancy 56 | `vmatmul:0:0/0 vpop:1:8/0 vpop:0:8/0`; **capacity 2, occupancy 56** | 2 MXU slots; vpops commit 10 and 12 |
| tpuv3/run.sh:86 `tpuv6e-bound-128` | `vmatmul:0:0/0 vpop:257:0/4 vpop:2:0/4` | `vmatmul:0:0/0 vpop:1:8/0 vpop:0:8/0` | vpops commit 258 and 260 |
| tpuv3/run.sh:93 `lanes-8` / `bound-8` | `vmatmul:1:0/0 vpop:19:0/4 vpop:4:0/4, bound: trace same` | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0, bound: trace same` | FACT (driver): the two traces are identical |
| tpuv3/run.sh:99 tpuv6e unbound | 8 issues, trace same | 8 issues, trace same | key 0 is the default |
| tpuv3/run.sh:137 `machine-mxu` (12 wide) | `vmatmul:1:0/0 vpop:11:0/4 vpop:4:0/4`, equal to gem5_se's | `vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0`, **equal to gem5_se's** | the per-cycle limit (W = 2) binds before the CPU width: "the matrix unit's timing is the model's" still holds |
| tpuv6e/run.sh:54 `mxu` pairs | lines 4 | **lines 1**: `vmatprep: the first …` / `vmatpush: 0 cycles …, 1 in flight, …` / `vmatprep: 1 cycles …, 1 in flight, …` / `vmatmul: 0 cycles …, 2 in flight, …` (all `input queue 0, output queues 0 and 0`) | the vmatpush is accepted beside its vmatprep and waits for the weights slot at the head |
| tpuv6e/run.sh:60 `mxu` pops | lines 4 of 8 | **lines 0 of 8**: `vpop: 1 cycles …, 1 in flight, input queue 0` / `0 cycles …, 2 in flight, input queue 0` / `1 cycles …, 2 in flight, input queue 8` / `0 cycles …, 3 in flight, input queue 8` (`output queues 0 and 0`) | pops wait at the head; they commit 19, 20, 20, 21 after their issues |
| tpuv6e/run.sh:63 units, admitted | 8, 2, 120 | unchanged; **8, 2, 120** | §6 |
| tpuv6e/run.sh:69 `mxu_rate` input queues | `0 0 0 0 3 2 10 9 8 6 0 0 0 0 0 0` | `0 0 0 0 0 0 4 4 3 3 10 9 9 8 6 4` | rows enter the queue a cycle after the vmatmul's issue; vpops issue before their rows |
| tpuv6e/run.sh:73 `mxu_rate` pops | `14:4/4 1:2/6 1:4/4 1:2/6 1:4/4 1:0/4` | `1:0/0 1:0/0 0:0/0 1:0/0 1:0/0 1:0/0` | all six vpops are accepted early (W = 3); they commit 18, 18, 19, 19, 19, 19 |
| tpuv6e/run.sh:88 `mxu_arrays` | lines 8 of 12; capacity 4 | **lines 0 of 12; capacity 4** | both vmatmuls are accepted in one cycle (`1 cycles …, 1 in flight, array 0` / `0 cycles …, 2 in flight, array 1`); array 0's vpops commit 18, array 1's 19 |
| tpuv6e/run.sh:98 `mxu_arrays-script` (12 wide) | lines 3 of 12 | **lines 0 of 12** | W = 4: the first cycle takes both arrays' vmatprep and vmatpush; two a cycle are still carried out over both arrays |
| tpuv6e/run.sh:109 `mxu_slots`, gem5_se and machine | lines 2 of 14, both | **lines 0 of 14, both** | a vmatmul and a vpop still issue in one cycle (`vmatmul: 1 cycles …, 2 in flight, input queue 8` + `vpop: 0 cycles …, 3 in flight, input queue 8`); the vmatmul waits behind the vpop at the head (commit 18) |
| tpuv6e/run.sh:118 `mxu_pops` | lines 2 of 10 | **lines 0 of 10** | both vpops issue in one cycle (`1 cycles …, 2 in flight, array 0` / `0 cycles …, 3 in flight, array 1`); they commit 21 and 22: array 1's a cycle behind, by the shared pop port |
| tpu/run.sh:103 `msa` | lines 3 of 3 | **lines 2 of 3**: the pop line is `issue msa pop: 1 cycles after the last issue, 0 in flight, input queue 4, output queue 0` | the pop is accepted at once and waits at the head; it commits 12 after its issue, 13 after the input push's issue (old 12) |
| tpu/ports.cc:210 msa stats | admitted 3, commits 2, 1 | **admitted 3, commits 2, 1** | driver polling `ready()` (ENGINE §8) |
| tpu/ports.cc:380 `fewer_pushed(MSA)` | 100, 100 | **1, 1** | a pop is accepted and waits at the head; a second fits behind it |
| tpu/ports.cc:382 `more_pushed(MSA)` | stops after 10, refused 1000 of 1000 | **stops after 11, refused 1000 of 1000** (PASS) | MinorFU take rule (choice 4) |
| tpu/ports.cc:385 `interleaved(MSA, 56)` | 56 | **51** (last commit at 58) | waits moved from accept to the head |
| tpu/functional, tpuv3/functional, tpuv6e/functional | PASS | PASS (FACT, driver on a fake Spike: the 36 value hashes and the stderr of 22 refusal programs equal today's code's) | values unchanged |

## 8. What the framework header lacked (all now in it)

1. `Queue::available(now)` (needed: a v6e word of two rows enters both output queues whole; a vmatmul or msa input push
   fits whole in its input queue; FACT today's `vcix_cp.hpp:392, 735`).
2. Interned block names in `Block`; run-time names such as `array<i>_input` are safe without care.
3. The per-cycle accept limit (§2 choice 3).
4. Nothing for the MXU line: its old stall rule is the MinorFU rule. The MSA's old "stop while the output queue is
   full" is dropped by DESIGN.md §8 decision 1 (final: no `Pipeline::hold`); no test pins it.

## 9. README sections that change (not rewritten here)

- `examples/tpuv3/README.md`:
  - `## The model's source` (54-102): the ports output and the unit list, per §6.
  - `## A walk through tests/tpuv3/mxu.S` (271-347), `### Room in the input queue` (348-386) and
    `### Lanes set the delay` (387-419): all cycle numbers, per §7.
  - `:205` "the unit never takes it" stays true: the refusal is a field check.
- `examples/tpuv6e/README.md`:
  - `:18-19`: handler steps → `cycle()`.
  - `### Timing` (437-654): issue slots act at the head; the stall quote `vcix_cp.hpp:391-393` → `Array::flow`;
    every cycle walk per §7.
  - `### What it refuses` (655-702): "gem5 never accepts the instruction" no longer holds: every refusal is accepted
    and faults; gem5 prints it and exits 1 as it commits. The gem5 excerpt (8 issues, the fifth vpop never issues)
    changes: the fifth vpop issues, and its commit ends the run with `tpu: vpop: 4 rows asked of the lower half, 0
    computed` (FACT `verify/mm/extra`, `EXTRA=1`); regenerate the excerpt with `verify/mm/harness`.
  - `### The code, for vmatmul` (703-738): replace the handler excerpt with `carried`/`carry_out`/`matmul`.

## 10. Verification (FACT)

Every check below runs with `-std=c++17 -Wall -Wextra -Werror`, against the revised design headers
(`verify/inc/`). Nothing in the repo was edited.

- `units/examples/tpu/{msa,mxu}.hpp` compile.
- With the other units' new headers, `tpuv3.cc`, `tpuv6e.cc` and `tpu.cc` build as `.so`s, and `create` accepts
  their stats lists.
- **Driver calibration.** `check-msa-mxu/harness.cc`, compiled against today's `include/vcix_cp.hpp` and
  `examples/tpu/{msa,mxu,machine}.hpp`, reproduces these old pins at width 2 or 12:
  - tpuv3/run.sh:59, 66, 70, 75, 82, 86, 93 and 137;
  - tpuv6e/run.sh:50-53, 56-59, 69, 73, 79-86, 95-97, 107-108 and 116-117;
  - tpu/run.sh:100-102.

  `ports_msa.cc` reproduces tpu/ports.cc's MSA values: 100, 100; 10 < 100 and 1000; 56. The same sources compiled
  against the new design give §7's "new" column.
- **Values and refusals:** §5. **Squash:** §5.
