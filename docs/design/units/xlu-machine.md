# Units xlu + machine: tpu::Xlu, the [tpu] trace, tpu.cc on the cycle()-function framework

Labels: **FACT** = read in the repo (`file:line`, branch `cp-component`) or produced by my throwaway check;
**DERIVED** = worked out by hand from the rules in `ENGINE.md`; **OPINION** = judgement or proposal.

Files written (nothing in the repo was edited):
- `units/examples/tpu/xlu.hpp` (new Xlu, 429 lines; today 379)
- `units/examples/tpu/machine.hpp` (new trace, 85 lines; today 95)
- `units/examples/tpu/tpu.cc` (unchanged byte for byte: same members, same constructors)
- `units/check-xlu/xlu_check.cc`: a throwaway check, rebuilt in `verify/xlu/` with g++ `-std=c++17 -Wall -Wextra
  -Werror` against the revised design headers (which now have `Queue::room` and the per-cycle limit). It contains:
  - a stand-in for an in-order gem5 CPU that offers the next instruction while `can_accept` holds and commits in
    order on `ready()`;
  - the `tests/tpu/ports.cc` drivers, changed to poll `ready()`;
  - a `cp::Instance` stats dump and a squash run;
  - Spike `execute` with a fake 4-lane host.

## 1. Component structure

All seven members are copyable plain data (deques and vectors of values, no pointers). FACT (check): a wrong-path
push issued at 18 and squashed at 22 leaves every statistic the same as a run without it.

```
            gem5 issue / Spike execute
                     |
                     v
   +--------------------------------------+
   | insns() Queue<Insn> (2*issue, issue) | in order; only the head works;
   +--------------------------------------+ a start spends 1 of issue_ (tpu_xlu_issue_per_cycle)
        |  push / push_reduce        | xlu_pop              set_pattern / load push:
        |  (vl rows, when available(vl))  |                      plain members only, done at once
        v                            |
   +---------------------------+     |
   | queue_  Queue<Element>    |     |  capacity tpu_xlu_queue_entries (elements)
   +---------------------------+     |
        | head group of gcd(vl, width) elements, 1 group a cycle, spends entry_ ("input", PRIMARY)
        v                            |
   +---------------------------+  +---------------------------+
   | stages_ Pipeline<Group>   |  | moves_  Pipeline<Group>   |  a fold's / exchange's tile: stages_
   | latency tpu_xlu_latency_  |  | latency tpu_xlu_move_     |  a move's tile: moves_
   | cycles, interval 1        |  | latency_cycles, interval 1|  (the tile's first push decides)
   +---------------------------+  +---------------------------+
        | taken as soon as done (never stalls)
        v                            |
   +---------------------------+     |
   | buffer_ Queue<Elements>   | <---+ the first pop takes the whole tile (box_.elements rows) at once,
   | capacity 65535            |       transform() (Spike: values) -> one Result of `out` slots
   +---------------------------+
        |
        v
   +---------------------------+
   | line_  Pipeline<Result>   |  latency 1: "its slots made"
   +---------------------------+
        | taken as soon as done: `out` rows
        v
   +---------------------------+
   | out_   Queue<Column>      |  capacity 65535; each pop takes exactly vl rows -> vd (Spike)
   +---------------------------+

   plain members: box_ (kind, fold, width, operation, element and pattern-entry counts, Spike's patterns),
                  left_, keep_, pop_ (the started head pop), pattern_ / pattern_width_, staged_ (elements in stages)
```

The order inside `cycle()` (FACT `xlu.hpp`, `Xlu::cycle`):
1. take `line_`'s finished result into `out_`;
2. take each line's finished group into `buffer_`;
3. feed one group from `queue_` into its line;
4. step the head of `insns()` until it is not done;
5. `entry_.hold(staged_)`.

Because every finished head is taken in the same cycle it is visible, neither pipeline ever stalls. That matches
today's unlimited buffers (`Stall::WHEN_OUTPUT_FULL` with depth `UNLIMITED`, FACT `examples/tpu/xlu.hpp:94-96`).

## 2. Each instruction's path through the blocks

- **xlu_push** (field [4:3] not 3) and **xlu_push_reduce**:
  - Accept: always (`valid()` is not overridden). Fold fields above 4 and bf16 folds other than max/min are a
    `fault()` as the instruction reaches the head (`fields()`).
  - At the head it waits while `queue_.available() < vl`. A push `fits()` refuses, on a tile of another kind or width
    or on more than 65535 elements held, is a `fault()`: no pop can come before it to empty the tile.
  - Then it spends 1 `issue_`. If a fold's or an exchange's result is left over (`left_ && !keep_`), it drops it by
    popping every `out_` row.
  - It updates `box_`, takes the set pattern (Spike), and pushes vl `Element` rows of `gcd(vl, width)` groups into
    `queue_`, filled from rs2 on Spike.
  - `complete()` comes in the same cycle, so the commit is 1 cycle after the issue when it does not wait.
  - The rows then pass `queue_ -> stages_|moves_ -> buffer_` as in the diagram.
- **xlu_push with field [4:3] = 3** (load the "after" pattern): no wait beyond order. It spends 1 `issue_`, adds vl
  to `box_.after_entries`, loads the pattern from rs2 on Spike, and completes.
- **xlu_set_pattern**: elements other than 32 bits are a `fault()` at the head. Else it spends 1 `issue_`, sets
  `pattern_width_` and loads `pattern_` on Spike, and completes.
- **xlu_pop**:
  - Start (needs 1 `issue_`): it is a first pop when `left_ == 0`, with `pushed = box_.elements` and
    `out = pushed`, or `vpu_num_lanes` for an exchange.
  - A first pop with an ARBITRARY shuffle and too few pattern entries is a `fault(in, why, 201)`: `patterned()`
    returns the why (the entry counts `box_.before_entries` / `after_entries` are plain members, counted on both
    hosts), so Spike exits 201 as the pop's execute returns and gem5 as the pop commits, where it waited for good
    before. Same text as the old direct `std::exit(201)`.
  - At the start it updates `left_`, `keep_` and `box_` exactly as today's `on_issue` did (FACT
    `examples/tpu/xlu.hpp:192-200`).
  - A first pop with `pushed > 0` then waits until `buffer_.length() >= pushed`. It takes them, computes
    `transform()` on Spike, and issues one `Result{out}` into `line_`.
  - Every pop then waits for `line_.idle()` and `out_.length() >= vl`, takes vl rows (writes vd on Spike) and
    completes. Once the line is idle and `out_` holds fewer than vl slots, nothing can add any (a push behind it
    waits): a `fault()`.

Faults and refusals (FACT: check run on Spike with the fake host, exit codes as shown; a `fault()` exits with its
exit code, 1 unless given, on Spike as the execute returns, on gem5 as the instruction commits):

| case | where | message (verbatim) | exit |
|---|---|---|---|
| fold > 4 | head `fields()` -> `fault()` | `tpu: xlu push_reduce: fold %u, which is none of max (0), sum (1), min (2), argmax (3), argmin (4)` | 1 |
| bf16 sum/argmax/argmin | head `fields()` -> `fault()` | `tpu: xlu push_reduce: fold 1 of 16-bit elements; every fold takes f32, max and min also bf16` (check: exit 1) | 1 |
| set_pattern not e32 | head `fields()` -> `fault()` | `tpu: xlu set_pattern: lane numbers are words of 32 bits, not of 16` (check: exit 1) | 1 |
| mixed tile kind | head `fits()` -> `fault()` | `tpu: xlu push: a tile of one kind takes a push of another (fold %u, or another width)` (check: exit 1) | 1 |
| 65535 held | head `fits()` -> `fault()` | `tpu: xlu push: %llu elements held, and a result holds at most 65535; a pop must take them first` (DERIVED, not run) | 1 |
| pattern missing | head `patterned()` -> `fault(in, why, 201)` | `XLU ERROR: the pre stage walks 1 columns but lane 0 carries 0 pattern entries (SIMM5 16)` (check: exit 201 on Spike; gem5 stand-in: ready, commit exits 201) | 201 |
| pop of more than left | started pop, line idle -> `fault()` | `tpu: xlu pop: 8 elements asked, 3 left` (check: exit 1). "left" = `out_.size()` | 1 |
| anything else stuck | framework | `vcix_cp: xlu_... waits, and nothing in its unit moves any more: ...` | 1 |

All configuration keys, defaults and `ConfigError` texts are kept verbatim (FACT: compare `configure` with
`examples/tpu/xlu.hpp:71-99`). `tpu_xlu_issue_per_cycle` W is again the accepts a cycle (`insns().configure(2W, W)`)
and the width of `issue_` (starts a cycle at the head).

Values on Spike (FACT, check, 4 lanes, tile lane L = 10L + k):

| field | result (lanes 0..3) |
|---|---|
| 0 | the tile unchanged |
| 4 | `0 10 20 30 / 1 11 21 31 / ...` |
| 8 | `0 1 2 3` in every lane |
| 16, with the pattern 3-L | `30.. / 20.. / 10.. / 0..` |
| fold sum | `60 64 68 72` |

These are the README's tables (`examples/tpu/README.md` "What it computes" / "Folding"). `transform`, `shuffle`,
`cross`, `element`, `maximum` and `minimum` are today's code, moved but not changed.

## 3. machine.hpp (trace) and tpu.cc

machine.hpp changes, FACT against `examples/tpu/machine.hpp`:
- `using Insn = vcix_insn` (was `cp::Insn`, now `vcix_insn` plus an id).
- `tpu::Component::trace_queues(const vcix_insn &)` (was `const cp::Insn &`). **Every other tpu::Component
  (sfu, mxu, and systolic/msa if they become one) must change its override signature.**
- The owner is found with `dynamic_cast<const Component *>(owner(insn))`, the framework's protected `owner()`, in
  place of `with_owner`.
- The one-pipeline fallback (`machine.hpp:51-60`, `each_pipeline`) is deleted. A plain `cp::Component` adds nothing
  to the issue line.
- The commit line, `replaying()`, `in_flight_` and `reset()` are unchanged.
- Spike never prints, because the framework's execute calls the non-virtual base methods.

Xlu's `trace_queues` prints `, input queue <queue_.size()>, output queue <buffer_.size()>`. At gem5's issue(t),
that is the state after `tick(t)`.

tpu.cc: no change is needed (FACT, the file in `units/` is a byte-for-byte copy).

## 4. Statistics (no ABI change)

FACT, `verify/xlu/xlu_check` `Instance::dump_stats`, xlu.S on xlu.yml, 420 ticks (gem5 prefixes each line with
`system.cpu.executeFuncUnits.funcUnits<N>.`, FACT `README.md:837`):

```
vcix.XLU_insns.admitted    13  # instructions admitted at port insns     capacity 1 (per cycle), occupancy 40
vcix.XLU.admitted          20  # elements admitted at port input         capacity 1, occupancy 60 (20 x 3)
vcix.XLU_queue.admitted    20  # elements admitted at port queue         capacity 8
vcix.XLU_stages.admitted   20  # groups admitted at port stages          capacity 1, occupancy 60
vcix.XLU_moves.admitted     0  # groups admitted at port moves           capacity 1
vcix.XLU_buffer.admitted   20  # elements admitted at port buffer        capacity 65535
vcix.XLU_line.admitted      2  # results admitted at port line           capacity 1
vcix.XLU_out.admitted     146  # slots admitted at port out              capacity 65535 (128 + 18)
```

Each unit also prints `.cycles`, `.occupancy` and `.utilized_cycles`.
- `vcix.XLU.*` keeps today's meaning (FACT check: `sfu_and_xlu` gives admitted 5, occupancy 15). It is the PRIMARY
  `cp::Port` `input`: elements entering the stages, with occupancy = elements in the stages per cycle (via
  `entry_.hold`).
- `issue` is a REPORTING port: listed in `ports()`, not a statistic.
- Today's REPORTING `output` port (`UNLIMITED`, never limited anything) is deleted.
- ports() order is `XLU.insns, XLU, (issue), XLU.queue, XLU.stages, XLU.moves, XLU.buffer, XLU.line, XLU.out`.

## 5. Re-baselined tests

DERIVED numbers: produced by the throwaway in-order CPU stand-in in `verify/xlu/xlu_check` (offers the next
accelerator instruction while `can_accept` holds, commits in order on `ready()`); not run on gem5. The stand-in
assumes (OPINION) that MinorCPU offers the next instruction in the same cycle while `can_accept` holds (FACT: the old
`xlu_issue.yml` run issued push and pop in one cycle), and that xlu.S's 128 nops still take 351 cycles.

With the per-cycle limit, `tpu_xlu_issue_per_cycle: 1` again keeps a push and its pop in different cycles, and `: 2`
lets them share one. Most deltas come from the rows entering `queue_` in `tick(t+1)`, not at issue.

| test | old | new (derived) | why |
|---|---|---|---|
| `tests/tpu/run.sh:114-118` xlu-fold | lines 2, commits 1 (`commit xlu_pop: 9`) | **lines 1, commits 0**: `issue xlu_pop: 1 cycles after the last issue, 0 in flight, input queue 2, output queue 0`; `commit xlu_pop: 8 cycles after its issue` | the push's 2 rows enter `queue_` at t+1, so both are still there at the pop's issue; the pop is done 9 cycles after the push's issue (old: 10) |
| `run.sh:119-122` xlu-wide | lines 1, commits 1 (8) | **lines 0, commits 0**: pop line `1 cycles …, 0 in flight, input queue 2, output queue 0`; commit 7 | one group of 2 enters at t+2 |
| `run.sh:123-125` xlu-issue (W=2) | lines 1 (`0 cycles …, 1 in flight, input queue 2, output queue 0`) | **lines 0**: `issue xlu_pop: 0 cycles after the last issue, 1 in flight, input queue 0, output queue 0`; pop commit 9 | the pop issues in the push's cycle (the key's meaning holds), but the rows are not in `queue_` yet |
| `run.sh:127-131` xlu: first push and pop, `commit xlu_pop: 9` ×2 | lines 2, commits 2 | **lines 1, commits 0** (both first pops commit 8) | as xlu-fold |
| `run.sh:133-135` xlu-pattern | sets 3, commits 2 | **sets 3, commits 2** | unchanged |
| `run.sh:137-139` `commit xlu_pop: 9` (pattern), `12` (move), `9` (move-exchange) | 2 and 2, 2 | **0 and 0, 0**: pattern 8 and 8, move 11 and 11, move-exchange 8 and 8 | the move latency still adds 3 |
| `run.sh:141-147` eight pushes | lines 8 | **lines 5**: `8 cycles …, 0 in flight, input queue 1, output queue 0`, then `1 cycles …` with queues `2 0`, `3 0`, `4 1`, `5 2`, `6 3`, `7 4` | the push after the pop waits for it at the head; the queue counts include the previous push's rows |
| `run.sh:149-154` pops after the 18-element tile | lines 3 of 13 | **lines 0 of 13**: `issue xlu_pop: 1 cycles …, 0 in flight, input queue 2, output queue 16`, `issue xlu_pop: 1 cycles …, 1 in flight, input queue 1, output queue 16`; commits 8 and 7 | the second pop waits in `insns()` behind the first and completes from what is left |
| `tests/tpu/ports.cc:192-199` sfu_and_xlu, XLU part | admitted 5, occupancy 15 | **5, 15** | 3 cycles per element in the stages |
| `ports.cc:291-302` fewer_pushed(XLU) | 1, 100 | **1, 1**; the first pop faults, and its commit exits 1 (`tpu: xlu pop: 2 elements asked, 1 left`) | a second pop fits behind the faulted first |
| `ports.cc:325-336` xlu_shallower, per tile (pop, rest, push) | (1, 11, 1) ×100, first 1 | **(1, 1, 10) ×100, first 1** | both pops queue in `insns()` |
| `ports.cc:108-123` list(), `tests/tpuv3/ports.cc:37-49`, `tests/tpuv6e/ports.cc:37-49` | XLU = 1 unit | **XLU = 8 units**: `XLU.insns, XLU, XLU.queue, XLU.stages, XLU.moves, XLU.buffer, XLU.line, XLU.out` (FACT `verify/mm/liststats`) | every Queue and Pipeline is a unit; rewrite the expected lists |
| `tests/tpuv3/run.sh:62`, `tests/tpuv6e/run.sh:63` `units …` | `… TPUV3_XLU` | adds `TPUV3_XLU_buffer _insns _line _moves _out _queue _stages` (TPUV6E_ likewise) | same |
| `tests/stats/run.sh` consistent / beyond_units | 5 of 5, 0 | **5 of 5, 0** | `vcix.XLU.*` is still the PRIMARY port |
| `tests/tpu/functional/run.sh:141-146,162-164` | pass; exit 201 / 1 / 1 | **unchanged** (FACT `verify/xlu`: values of fields 0/4/8/16 and fold sum, exits 201, 1, 1) | same values and messages |

The `ports.cc` drivers must poll `ready()` (FACT `tests/tpu/ports.cc:63` commits at `now + latency`). The README's
xlu timing section (`examples/tpu/README.md:626-696`) must be regenerated.

## 6. What the framework header lacked (all now in it)

1. `Queue::available(now)`: free slots (a slot freed now still taken) and the per-cycle limit; a push enters vl rows all
   or nothing (today's `stages_.input_rows() + moves_.input_rows() + vl <= entries_`, FACT `examples/tpu/xlu.hpp:122`).
2. The per-cycle accept limit: `insns().configure(2 * issue, issue)` restores `tpu_xlu_issue_per_cycle` at accept.
3. Nothing else: a fold's left-over result is flushed by popping its visible rows; pipelines are never flushed.

## 7. Behaviour that differs beyond cycle counts (FACT where cited, else OPINION)

- **Refusals move from accept to the head**: wrong fields, mixed kind, 65535 held, a pop of more than is left. Today
  they were `require`s at `can_accept` (FACT `xlu.hpp:118,126,320`). Now the head of `insns()` faults them: Spike
  prints the same message, and gem5 prints it and exits 1 as the instruction commits instead of hanging. The missing
  pattern goes the same way with its own code: `fault(in, why, 201)`, exit 201 on both hosts (gem5 no longer hangs).
- **Strict program order inside the unit.**
  - Today on gem5, a `set_pattern` or a load push could be accepted while a pop waited in `pops_` (FACT
    `xlu.hpp:114,122`: only non-load pushes required `pops_.idle()`). The load could even change that pop's
    "after" pattern.
  - Now everything waits behind the head, which is what Spike (one at a time) always did. So Spike's values are
    unchanged.
- Today `transform` reset `box.fold = 0` on Spike only (FACT `xlu.hpp:296`). That line is dropped: every push sets
  `box.fold`, and `out` is 0 whenever `pushed` is 0, so nothing read it. OPINION: no observable effect.
- **Copy cost (OPINION)**: on gem5 `buffer_`/`out_` hold one empty vector per element or slot, at most 65535 each,
  and gem5 copies them at every issue. This matters only near the 65535 limits. If a profile shows it, a row-count
  run-length row would cure it.
