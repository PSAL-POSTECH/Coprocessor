# rtl

Models whose unit is RTL: a SystemVerilog module, Verilated, behind one shared
wrapper (`rtl_model.hpp`, `rtl::Model<V, T>`). The wrapper knows nothing about
the unit; a model is the module, a table of the encodings it owns, and the
ports the module has. Both simulators clock the same Verilated unit: Spike for
the values, gem5 for the timing. A model of this kind writes none of the
methods of "Writing your hardware model": the wrapper is them.

| Model | Library | Module | Instructions |
|---|---|---|---|
| `rtl_systolic` | `librtl_systolic.so` | `systolic/systolic.sv`, `L=4`, `WEIGHT_WAITS=1` | the tpu example's systolic array: input push (`sf.vc.iv` 0), weight push (`sf.vc.iv` 1), pop (`sf.vc.v.i` 2), int8, format 0 |
| `rtl_systolic_nowait` | `librtl_systolic_nowait.so` | the same, `WEIGHT_WAITS=0` | the same; a weight push does not wait for older inputs, so it gives other values (the variant the checks catch) |
| `rtl_systolic_badtag` | `librtl_systolic_badtag.so` | the same, `BAD_TAG=1` | the same; every `out_tag` is one off, which the gem5 face's check catches |
| `rtl_copier` | `librtl_copier.so` | `copier/copier.sv` | `sf.vc.x` 1 sets the source from `x[rs1]`, 2 copies the next 256 bytes of the source to `x[rs1]` and returns at once, 3 waits until every earlier copy is in memory |

## The unit's ports

A module has `clk` and `rst`, and:

| Port | Signals | Present |
|---|---|---|
| command | `cmd_valid`/`cmd_ready`, `cmd_op`; `cmd_vl`, `cmd_tag` with beats; `cmd_x` with a scalar | always |
| in beats | `in_valid`/`in_ready`, `in_data`: one element of every lane a beat, lane-major | `BEATS` |
| out beats | `out_valid`/`out_ready`, `out_data`, `out_tag`, `out_last` | `BEATS` |
| memory | `mem_valid`/`mem_ready`, `mem_we`, `mem_addr`, `mem_wdata`, `mem_rdata`: 8 bytes a request | `MEM` |
| busy | `busy`: the unit's state can still change without new input | always |

Commands are taken in order. A command's in beats follow it; its out beats
carry its tag, and the last of them `out_last`.

## The table

The unit's code (`systolic/systolic.cc`, `copier/copier.cc`) is a table and
nothing else:

| Member | Says |
|---|---|
| `NAME` | the model's name |
| `LANES` | elements a beat |
| `TAGS` | command tags the unit tells apart; 0 without tag ports |
| `BEATS`, `SCALAR`, `MEM` | which optional ports the module has |
| `check(config)` | refuses a machine description the module was not built for |
| `OPS[]` | per encoding an `rtl::Op`: `{match, mask}`, its name, the `cmd_op` it is, whether `vl` beats come in from `vs2`, whether `vl` beats go out to `vd`, and whether it `waits` |

An `Op` that `waits` has no beats; the wrapper refuses the table at compile
time otherwise. An instruction whose `Op` has beats and whose `vl` is 0 is not
sent to the unit: it does nothing and is done at once, on both simulators.

## Spike: a core infinitely faster than the unit

`execute` clocks the unit only while the instruction itself needs it: until
its command is taken, its beats are in and its results are out; `vd` is
written last, after any clocks the schedule adds. Between
instructions the unit does not move. So a command that returns at once (a
copy) has done nothing yet when the next instruction runs, and a program sees
its effect only after an instruction that waits for it.

An `Op` with `waits` is that instruction: it clocks the unit until `busy` falls
-- a drain. Nothing drains at the end of the program.

Two rules keep a program's values the same however the unit is clocked:

1. **Synchronize only by an instruction that waits by handshake**: a pop, or an
   `Op` with `waits`. Never poll memory with scalar loads: under the default
   schedule the unit does not move while the core polls.
2. **Every state change that affects results goes through the ordered command
   and in streams.** The array's weight push is such a change; it waits in
   hardware until the inputs before it have left the array
   (`WEIGHT_WAITS=1`). `rtl_systolic_nowait` breaks this rule, and its values
   differ under the default schedule.

`RTL_SCHEDULE` clocks the unit more. It is optional: a check that a program
keeps to the two rules, not a mode to run in.

| `RTL_SCHEDULE` | After each instruction, the unit is clocked |
|---|---|
| unset | not at all |
| `slow` | until it is not busy |
| `random:<seed>:<gap>` | 0 to `<gap>` cycles, drawn from `<seed>` (not 0) |

A trap from memory (a page `pk` has not mapped yet) passes through `execute`.
For a unit with `MEM`, `execute` saves the unit at its start and restores it
before the trap leaves, so the unit has taken no half of the instruction;
Spike runs it again after the trap. Writes that reached memory before the trap
stay, so such a unit must leave the same memory when an execute is repeated
from its start (the copier writes the same bytes again; a unit that
accumulates into memory would not). A copy moves the copier's source past the
bytes it copied, so a copy taken twice would copy the source's second half:
the slow fault check sees a missing restore.

## gem5: one clock a cycle, no data

Every instruction is issued as `Unknown`. `tick` presents the oldest command
not yet taken, feeds in beats, drains out beats, and clocks the unit once;
gem5 moves no data, so `cmd_x` is 0, in beats carry none of the program's
values, and memory requests are answered at once with zeros.

`can_accept` is true when nothing was issued this cycle, no command waits to
be taken, `cmd_ready` is high and a tag is free. A tag is taken at issue and
given back when the instruction is ready; an out beat whose `out_tag` is not
the oldest draining command's ends the run with a message.

An instruction is ready at its last out beat if it has out beats, at its last
in beat if it has only in beats, and otherwise when its command is taken. One
that waits is ready when `busy` is low and no older command has a beat left.
Its line is printed at commit, so an instruction gem5 squashes prints nothing.
If commands are left and for 2^22 cycles nothing fires while `busy` is low
(a pop with nothing to pop), the run ends with a deadlock message; Spike says
the same at once.

At exit each simulator prints, on stderr, how many cycles it clocked units
(gem5's replays after a squash included), how many copies it made of one
(gem5 before each issue, Spike before each execute of a unit with memory) and
how many executes it put back after a trap, when any of these is not 0.

## Build and run

The models are built only when CMake finds Verilator (5.x; `VERILATOR_ROOT`,
or `verilator` on `PATH`), and skipped otherwise; `-DVCIX_RTL=OFF` skips them
as well.

```
PATH=<verilator>/bin:$PATH scripts/build.sh     # or VERILATOR_ROOT=<verilator>/share/verilator
examples/rtl/run.sh                              # both models on both simulators
tests/rtl/run.sh                                 # the checks; SKIP when not built
```

`tests/run.sh` runs `tests/rtl` with the rest; without the libraries (no
Verilator, or `-DVCIX_RTL=OFF`) it prints `SKIP` and passes. The image has
no Verilator, so CI takes the `SKIP`.

## What the checks show

`tests/rtl/run.sh` (Verilator 5.046, image `env-2b76031ccf17`), 22 checks:

| Check | Result |
|---|---|
| `libtpu.so` runs `values.c` | exit 0, 1536 bytes |
| `rtl_systolic`, default, `slow`, `random:1:12` to `random:10:12` | exit 0, the same bytes as `libtpu.so` (12 checks) |
| `rtl_systolic_nowait`, default | exit 0, differs |
| copier, no wait, default | exit 1, 0 of 256 bytes copied |
| copier, wait, default and `slow` | exit 0, 256 of 256, 0 restored |
| page not touched, wait, default | exit 0, 256 of 256, 1 restored |
| page not touched, no wait, `slow` | exit 0, 256 of 256, 1 restored; without the restore, exit 1 and 0 of 256 |
| gem5, `timing.S` | exit 0, 6 timing lines |
| gem5, `copy.c -DWAIT -DTIMING` | exit 0, 3 timing lines |
| gem5, `timing.S` on `rtl_systolic_badtag` | the out-tag message, no exit line |

A timing line reads
`[rtl] systolic weight push vl=4: issued @9597, ready @9602 (5 cycles)`.

## Limits

- **gem5 sees no data.** Scalars, in beats and memory replies carry none of
  the program's values, so a unit whose timing depends on values (early
  exit, sparsity, a data-dependent address) is timed wrong without an error.
  As for every model, data-dependent timing is out of scope.
- **Values come only from Spike.** gem5's run of `copy.c` reads 0 bytes
  copied; `-DTIMING` makes it exit 0 regardless.
- **The array matches `libtpu.so` where `values.c` looks.** Its inputs and
  weights are in [-3, 3], so no sum leaves int8: the RTL keeps a sum's low
  byte, while `libtpu.so` converts a float to `int8_t`, which C++ leaves
  undefined out of range. Elsewhere they part: an input pushed before any
  weight is computed with zero weights (`libtpu.so` fails), more than about
  `2*Q + 2*L - 1` elements pushed without a pop deadlock the RTL (`libtpu.so`
  has no bound), and `L` must be 2 or more (`systolic.cc` asserts it).
- **A unit with `MEM` must be repeatable** from the start of an execute, as
  above: the restore after a trap puts back the unit, not memory.
- **RTL parameters are fixed at build time.** `L` and `WEIGHT_WAITS` are
  Verilator `-G` flags in `CMakeLists.txt`; `check(config)` refuses a machine
  description that disagrees, it does not resize the unit. Another size is
  another library.
- **Every copy is a whole save of the unit.** gem5 makes one before each
  issue (for squash), restored into a new unit; Spike saves a unit with `MEM`
  before each execute and restores only after a trap. It grows with the
  unit's state; a large array pays it per instruction.
- **Verilator's flags.** `L=4` builds with the defaults; a larger array may
  need flags such as `--unroll-count`, passed as extra arguments to
  `vcix_accel_add_rtl_model`. Not tried here.
- **Squash puts the copy back.** gem5 restores the unit saved before the
  squashed issue and replays `tick` and `commit`, so a module is built
  `--savable` and its whole state must be what Verilator saves.

## Files

```
rtl_model.hpp           the wrapper: the unit, its copies, the two faces
systolic/systolic.sv    the array, L lanes, queues of Q
systolic/systolic.cc    its table
systolic/values.c       int8 through tests/tpu/functional's kernels; compared with libtpu.so
systolic/timing.S       its timing on gem5
copier/copier.sv        the copier, DEPTH commands queued
copier/copier.cc        its table
copier/copy.c           a copy, then loads; -DWAIT, -DUNTOUCHED, -DTIMING
run.sh                  both models on both simulators
```
