# rtl

Models whose unit is RTL: a SystemVerilog module, Verilated, behind one shared
wrapper (`rtl_model.hpp`, `rtl::Model<V, T>`). A model is the module, a table
of the encodings it owns, and the ports the module has; the wrapper is every
method of "Writing your hardware model". Both simulators clock the same unit:
Spike for the values, gem5 for the timing.

| Model | Module | Instructions |
|---|---|---|
| `rtl_systolic` | `systolic/systolic.sv`, `L=4` | the tpu example's systolic array: input push (`sf.vc.iv` 0), weight push (`sf.vc.iv` 1), pop (`sf.vc.v.i` 2), int8 |
| `rtl_systolic_nowait` | the same, `WEIGHT_WAITS=0` | a weight push does not wait for older inputs: breaks rule 2, caught by the checks |
| `rtl_systolic_badtag` | the same, `BAD_TAG=1` | every `out_tag` one off, caught by the gem5 face |
| `rtl_copier` | `copier/copier.sv` | `sf.vc.x` 1 sets the source from `x[rs1]`, 2 copies 256 bytes to `x[rs1]` and returns at once, 3 waits for every earlier copy |

## Ports

| Port | Signals | Present |
|---|---|---|
| command | `cmd_valid`/`cmd_ready`, `cmd_op`; `cmd_vl`, `cmd_tag` with beats; `cmd_x` with a scalar | always |
| in beats | `in_valid`/`in_ready`, `in_data`: element `i` of every lane, lane `j` at `[8j +: 8]` | `BEATS` |
| out beats | `out_valid`/`out_ready`, `out_data`, `out_tag`, `out_last` | `BEATS` |
| memory | `mem_valid`/`mem_ready`, `mem_we`, `mem_addr`, `mem_wdata`, `mem_rdata`: 8 bytes a request | `MEM` |
| busy | the state can still change without new input | always |

Commands are taken in order; an instruction is one command, its `vl` in beats,
and its `vl` out beats (the last with `out_last`). Each clock the wrapper sets
the inputs, evaluates with `clk` low (serving a memory request), takes what
fires (`valid && ready`), then raises `clk`. Ready, valid and `busy` come from
registers. A unit stalled on a full output queue is not busy. Build the module
`--savable` and keep no state outside what Verilator saves.

## The table

`systolic/systolic.cc` and `copier/copier.cc` are a struct each: `NAME`,
`LANES`, `TAGS` (0 without tag ports), the `BEATS`/`SCALAR`/`MEM` flags,
`check(config)`, and `OPS[]`, one `rtl::Op` per encoding: `{match, mask}`,
name, `cmd_op`, beats in from `vs2`, beats out to `vd`, `waits`. An `Op` that
waits has no beats; one with beats and `vl` 0 is done at once without the unit.

## Spike and gem5

- **Spike is a core infinitely faster than the unit.** `execute` clocks the
  unit only until its own command, beats and results are through, then writes
  `vd`. Between instructions the unit does not move: a push returns before it
  is computed, and a later pop clocks the array until its results come.
- **gem5 clocks the unit once a cycle with no data.** Every instruction issues
  as `Unknown` and is ready at its last out beat, its last in beat, when
  `busy` falls (`waits`), or when its command is taken. `can_accept` needs
  `cmd_ready`, a free tag and nothing else issued that cycle. A wrong
  `out_tag` or a deadlock ends the run. A squash restores the copy made before
  the issue (a Verilator save).

`timing.S` on gem5: `[rtl] systolic weight push vl=4: issued @9602, ready @9618
(16 cycles)` -- the module holds `in_ready` low until older inputs leave the
array.

## Two rules

1. **Synchronize only by an instruction that waits by handshake** (a pop, or an
   `Op` with `waits`). Scalar polling never ends: the unit does not move.
2. **Every state change that affects results goes through the ordered command
   and in streams.** The array's weight push waits in hardware for older inputs.

Under them a result depends on the order of inputs, not their timing, so Spike
gives the values gem5 and the hardware would. An `Op` with `waits` clocks the
unit until `busy` falls (a drain); nothing drains at exit. `copy.c` reads 0 of
256 bytes copied without the wait, 256 with it.

`RTL_SCHEDULE=slow` (settle after each instruction) or `random:<seed>:<gap>`
clocks the unit more, as a check of the rules: values must not change.
`rtl_systolic_nowait` passes `slow` and fails the default and some seeds.

A trap from a host memory access restores a `MEM` unit to the start of
`execute` before Spike re-runs it; memory already written stays, so such a
unit must be repeatable.

## Writing a unit

1. The module, with the ports it needs and the two rules.
2. The table `.cc`, then `using M = rtl::Model<Vtop, Unit>; VCIX_ACCEL_REGISTER(M)`.
3. In `CMakeLists.txt`: `vcix_accel_add_rtl_model(name unit.cc unit.sv top -GPARAM=value)`.
4. Check values on Spike under the default, `slow` and random schedules
   against a reference, and read the `[rtl]` lines on gem5.

## Build and run

Built when CMake finds Verilator 5.x (`PATH` or `VERILATOR_ROOT`; the image
has it, and `scripts/build.sh` finds it); `-DVCIX_RTL=OFF` skips them.

```
scripts/build.sh
examples/rtl/run.sh     # both models on both simulators
tests/rtl/run.sh        # 27 checks, also run by tests/run.sh; SKIP when not built
```

The checks: `rtl_systolic` gives `libtpu.so`'s bytes under the default,
`slow` and ten random schedules; `rtl_systolic_nowait` differs, and
`libtpu.so` under `tpu_systolic_weight_waits: 0` matches it; `rtl_stall`, with
no simulator, gives each array instruction the cycle the RTL gives it where the
stall modes part; the copier copies 0 of 256 without a wait and 256 with one,
also across a trap; `libcopier.so` (`examples/copier`) gives gem5's calls the
answers `rtl_copier` gives, cycle by cycle, in 50 seeded programs
(`copier_lockstep`, no simulator), but on Spike copies as the copy issues, so
it copies 256 without a wait too; gem5 prints one line per instruction, and
`rtl_systolic_badtag` stops.

## Limits

- gem5 sees no data: data-dependent timing is out of scope, and values come
  only from Spike.
- Spike's `execute` is atomic, so it never shows instructions overlapping.
- The array matches `libtpu.so` only where sums stay in int8, weights are
  pushed first, and no more than about `2Q + 2L - 1` elements wait for a pop.
- RTL parameters are fixed at build time; another size is another library.
- Every copy is a whole save of the unit, one per gem5 issue.
- A large array may need Verilator flags such as `--unroll-count`.

## Files

```
rtl_model.hpp           the wrapper
systolic/systolic.sv    the array; systolic.cc its table
systolic/values.c       values, compared with libtpu.so; timing.S its gem5 program
copier/copier.sv        the copier; copier.cc its table; copy.c the test
run.sh                  both models on both simulators
```
