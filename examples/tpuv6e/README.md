# tpuv6e

A TPU v6e on the two custom opcodes: one model (`libtpuv6e.so`, name `tpuv6e`).
It is the tpu example's machine with another array: the special-function unit,
the one-cycle instructions and the cross-lane unit of `examples/tpu`, and the
matrix unit `tpu::Mxu` (`examples/tpu/mxu.hpp`), in the v6e's shape, in place
of the systolic array and the multi-precision array. It is another machine, so
another library, and the matrix unit has the encodings of the two arrays it
stands in for: a program for one model is not a program for the other.

## Units

`tpuv6e.cc` is a `tpu::Machine` (`examples/tpu/machine.hpp`) of four units,
each built with the name its statistics report:

| Unit | File | Reported as |
|---|---|---|
| `tpu::Sfu` | `examples/tpu/sfu.hpp` | `TPUV6E_SFU` |
| `tpu::Misc` | `examples/tpu/misc.hpp` | `TPUV6E_MISC` |
| `tpu::Xlu` | `examples/tpu/xlu.hpp` | `TPUV6E_XLU` |
| `tpu::Mxu<Shape>` | `examples/tpu/mxu.hpp` | `TPUV6E_MXU` |

All four are the tpu example's units, included as `tpu/<unit>.hpp` (the build
adds `examples` to the include path); the first three are timed as its README
says, the matrix unit as below, and only the name they report is this
model's. `Shape`, in `tpuv6e.cc`, is the matrix unit's: an operand of two
registers (`REGISTERS`), two MXU slots a cycle (`SLOTS`) and a word a cycle
into an array (`WORD_A_CYCLE`). The ports, and so the statistics,
are in this order: gem5 reports `vcix.TPUV6E_MXU.admitted` and so on (before,
as `libtpu_mxu.so`, `vcix.Mxu.*`, `vcix.Sfu.*`, `vcix.Misc.*`, `vcix.Xlu.*`).
The encodings' names, and so `vcix.committed.<encoding>`, did not change.

## The matrix unit

The unit follows what the final VLIW bundles of a TPU v6e show of its MXU
(XLA's LLO dump of a bf16 `dot`, 1024 x 1024 x 1024): an array of 256 x 256
fed by registers of 128 lanes, so that an operand is two registers and two
instructions. The machine has `tpu_mxu_units` such arrays (the v6e's bundles
name two, `mxu0` and `mxu1`), each with its own held halves, staged and
active weights, queues and delay line. An instruction names its array in a
field the encodings leave free: bits 11:7 of `sf.vc.iv` (`rd`, which names
no register) for a `vmatprep`, `vmatpush` or `vmatmul`, bits 24:20 of
`sf.vc.v.i` (`rs2`, which names none) for a `vpop`; with one array both are
0, and a program for one array runs unchanged on a machine of more. One for
an array the machine does not have ends the run, by name, when Spike runs
it; gem5 never accepts it.

| Instruction | Encoding | Field at 19:15 | What it does |
|---|---|---|---|
| `vmatprep` | `sf.vc.iv` 2 | [1:0] format, [2] 0 for weights and 1 for inputs | holds the columns from `vpu_num_lanes` up of that operand, until the next `vmatpush` (weights) or `vmatmul` (inputs) brings the other half |
| `vmatpush` | `sf.vc.iv` 1 | [1:0] format, [4:3] 1 for the first half of the depth and 2 for the second | with what a `vmatprep` of weights held, one weight per row for each of the 2 x `vpu_num_lanes` columns, staged in that half of the depth |
| `vmatmul` | `sf.vc.iv` 0 | [1:0] format, [2] 1 to latch | with what a `vmatprep` of inputs held, the rows of an input: a row is 2 x `vpu_num_lanes` numbers, and every column computes its active weights times it; one that latches first makes the staged weights the active |
| `vpop` | `sf.vc.v.i` 2 | [2] 0 the lower half of the columns, 1 the upper | `vl` rows of one half of the columns, as singles |

An element is a word of 32 bits. Format 0 is a single; 1 is two halves and 3
two bfloat16, the earlier row in the low half of the word, so a register of
8 words is 16 rows.

The unit holds an upper half of the weights and one of the inputs, apart: a
`vmatprep` of the weights is taken by the next `vmatpush`, one of the inputs
by the next `vmatmul`, and what comes between, the other operand's
instructions included, leaves it. So the v6e's orders run as its bundles
issue them (bundle: op): `43 vmatprep.subr / 44 vmatprep.mubr / 45 vmatpush2`,
the inputs' half waiting under seven more pairs of `vmatprep.subr` and
`vmatpush2` until `64 vmatmul.mubr.vlgmr`; and `63 vmatprep.subr / 64
vmatmul.vlgmr / 65 vmatpush1`, the weights' half waiting under the
`vmatmul`. A `vmatpush` or a `vmatmul` with no half of its operand held, or
one of another number of rows, ends the run; so does a `vmatprep` of an
operand whose held half nothing has taken yet, rather than drop it; and so
does a `vpop` of more rows than its half holds.

Weights. A column has a weight at each depth k, 0 to 2 x `vpu_num_lanes` - 1,
and element k of an input row is multiplied by it: the depths below
`vpu_num_lanes` meet the half of the row a `vmatmul` brings, the rest the
half its `vmatprep` held. The unit holds two such matrices, the staged and
the active. A `vmatpush` writes the staged only, in the half of the depth its
[4:3] names, 1 the first and 2 the second, the v6e's `vmatpush1` and
`vmatpush2`: what that half held moves down as many depths as the push has
rows, what passes the half's last depth is gone, and the rows take the top of
the half in their order, row r at its depth r. The other half is untouched.
So the last push of a half is at its lowest depths, and a tile pushed as the
v6e pushes it, 16 rows at a time with the deepest first (k 112 to 127 first,
k 0 to 15 last, in each half), is staged as the matrix it is. A `vmatmul`
computes with the active weights; with [2] of its field set it first copies
the staged whole into the active, as the v6e's `vmatmul` marked `.vlgmr`
does, and the staged stay as they were. So the next tile may be pushed
between the `vmatmul`s of the one in use, as the v6e's bundles do, and a tile
of 128 deep pushes only the first half and brings zeros under the
`vmatprep` of its inputs, as XLA does for K = 128, while the second half
keeps what it held. The staged weights are zeros at the first push. A
`vmatmul` that latches with nothing pushed ends the run, as does one that
computes before any latch; so does a `vmatpush` whose [4:3] is 0, or 3: the
v6e's `vmatpush3`, which a dot of N = 128 uses with the two halves of the
depth apart, is not modelled.

Timing: a cycle takes two of `vmatprep`, `vmatpush` and `vmatmul` over all
the arrays and, of each array, one of the weights' (a `vmatprep` of weights or
a `vmatpush`), one of the inputs' (a `vmatprep` of inputs or a `vmatmul`) and
one `vpop`; a `vmatprep` and a `vmatpush` enter nothing. These are the v6e's
slots: its static per-bundle utilization gives a bundle `MXU 2` and `VPOP 2`
for its two MXUs, and in the final bundles of five dumps (256^3 and M = 1024,
2048, 4096, 8192 of M x 384 x 1152; `.subr` and `vmatpush` the weights',
`.mubr` the inputs') no MXU has two of the weights', two of the inputs' or two
`vpop`s in a bundle and no bundle more than two of the first three over both
MXUs, while one MXU takes a `vpop` with its `vmatmul` (1024 x 384 x 1152,
bundle `0x90`: `vpop.mxu1`, `vmatmul.mxu1`, `vmatprep.mubr.mxu0`) and all three
kinds at once (`0x167`: `vpop.mxu0`, `vmatmul.mxu0`, `vmatprep.subr.mxu0`).
gem5 asks the model of each instruction whether it is accepted, as many a
cycle as its CPU issues, through the one accelerator unit of
`examples/tpuv6e/gem5/script_systolic.py`, so these slots, not gem5's units,
bound the issue. The rows of a `vmatmul` wait in its array's input
queue, and one word a cycle enters the array's delay line of
`tpu_mxu_delay_cycles` slots, as a word enters a SIMD unit -- a word into
each array a cycle, so the unit's input port, `TPUV6E_MXU.input`, takes 2 x
`tpu_mxu_units` rows a cycle: a word brings two rows of 16 bits (format 1 or
3), both computed in the cycle it enters, so rows of 16 bits enter two a
cycle and singles (format 0) one. The rows of each `vmatmul` enter at its own
format's rate, whatever the others in the queue hold. A row that comes out is
in both halves' output queues of its array, a `vpop` takes it from its own,
and the line stops while either is full. A latch costs nothing more: the bundles do not say what it costs. Nor
do they give the unit's latency: how far a `vmatmul` is from its first
`vpop` in them is where a schedule placed the two (15 bundles in the 256^3
dump, 103 in the 1024 x 384 x 1152 one), not what the unit takes. A machine
description may give it as `tpu_mxu_delay_cycles`; the default is the
array's width + depth - 1.

## The functional face

Each unit computes its own instructions' values (`examples/tpu/README.md`);
`tpu::Mxu<Shape>` (`examples/tpu/mxu.hpp`) those of `vmatprep`, `vmatpush`,
`vmatmul` and `vpop`, per array: the staged and the active weights and the
held upper halves in a register, the results riding the array's pipeline. A
run it cannot carry out ends as the other units' do, with a line on standard error that starts `tpu: ` and
exit 1.

## The gem5 machine

`gem5/` is the machine PyTorchSim measures `libtpuv6e.so` on: the tpu
example's machine (`examples/tpu/gem5`, its README says what it is and what
its port from PyTorchSim changed) with the v6e's scratchpad ports and vector
ALUs, and two compensations for the CPU not being a VLIW machine.
`script_systolic.py` builds it with the functions of
`examples/tpu/gem5/script_systolic.py`, and `vpu_config.py` takes its units
from `examples/tpu/gem5/vpu_config.py`: only what differs is here. The options
are that script's; the scratchpad needs a gem5 with `MultiPortScratchpad`,
release `vcix-v0.3.3` or later (`setup/versions.env`).

```
gem5.opt -d m5out examples/tpuv6e/gem5/script_systolic.py -c <binary> --model libtpuv6e.so \
    [--machine-config machine.yml] [--vlane N] [--vlen N]
```

Each count of the v6e is its slots a bundle in libtpu's static bundle profile
of a v6e kernel, an 8192 x 4096 x 4096 bf16 matmul of 13475 bundles, which
gives each kind as used/slots over the kernel:

| | PyTorchSim's original | This machine | From |
|---|---|---|---|
| scratchpad | one bank of 32 GB/s behind the bus | `MultiPortScratchpad`: up to 3 loads, 2 stores and 4 accesses in all a cycle, at any address; each answered 1 cycle later | `VECTOR_LOAD_AND_MISC (19181/40425)`, `VECTOR_STORE_AND_MISC (8872/26950)`, `VECTOR_LOAD_AND_STORE_AND_MISC (28041/53900)`: 3, 2 and 4 a bundle. The latency of 1 is a choice: the bundles do not give it |
| the CPU's data port | to the bus | to the scratchpad | behind the bus every access would share its one layer to the scratchpad, which refuses a second packet in the same bus cycle |
| vector ALUs | 4 each of adder, multiplier, divider and reduction, and 4 of misc, each its own op classes | 4 `MinorVALU`s, each every one of those op classes | `VECTOR_ALU (16027/53900)`: 4 a bundle |
| `executeLSQRequestsQueueSize` | 1 | 2 | a compensation, below |
| integer units (`IntAlu`) | 4 | 6 | a compensation, below |

The instruction cache and the system port still reach the scratchpad through
the bus, on its second port, and their reads take a load port. Every unit
before the fifth integer unit keeps its index in the statistics
(`funcUnits16` is the accelerator); the VALUs are `funcUnits21`, `22`, `27`
and `28`, the fifth and sixth integer units `29` and `30`.

### Four VALUs

A v6e bundle has 4 VALU slots and any vector ALU op takes any of them; the
original's pool gives each kind its own units, so a stream of adds and
multiplies issued 8 a cycle. Here the four VALUs take every vector ALU op
class: a mixed stream issues 4 a cycle, whatever the mix. On a loop of 64
independent vector ops (cycles an iteration, ops a cycle):

| The loop | The original's pool | Four VALUs |
|---|---|---|
| vfadd only, vfmul only, vfmacc only | 16 (4.0) | 16 (4.0) |
| vfadd and vfmul in turn | 11 (5.8) | 16 (4.0) |
| add, mul, vmv, vslide, cvt, div, sqrt, sgnj | 11 (5.8) | 16 (4.0) |
| vfredusum only | 16 (4.0) | 32 (2.0) |

A reduction costs two VALU slots: this gem5 issues each as two micro-ops, a
`VPinVdMicroInst` (op class `SimdMisc`) and the reduction itself, and both are
VALU ops. The first is how gem5 decodes a reduction, not an op of the v6e;
it shares its op class with `vmv`, so the pool cannot leave it out.

### Two LSQ requests and two more integer units

Both are compensations, not counts of the v6e. Minor sends the loads and
stores in its LSQ's request (address translation) queue, one by default; two
let two accesses go together, a step toward the v6e's 4 a cycle, not its
count.

The v6e computes a vector load's address inside the load, as a base register
plus an immediate, and spends no scalar slot on it: in the final bundles of
the fusion of a 1024 x 384 x 1152 matmul (`fusion-76`), a load is
`vld [vmem:[%s27831_s1 + $0xec] ...]`; of its 1802 vector loads, 215 are
`[%s + imm]`, 941 `[#allocation3 + imm]`, 640 a spill slot and 6 a bare base.
An RVV load has no immediate offset, so a compiled loop steps a pointer with
an `addi` for each access. In gem5's Minor CPU an `addi` finishes in a cycle
but holds its unit until it commits, and commit is in order: behind an older
vector load that has not committed, finished `addi`s hold all the units, and
the next one waits, and the in-order issue with it. Six units are the fewest
at which a steady matrix-unit loop of `libtpuv6e.so`, pipelined two blocks
deep (a vmatmul of 16 rows of bfloat16 a block, its 6 vector loads, 4 vector
stores and 4 vfadds, 6 `addi`s), runs at its own pace, 6.49 cycles a block
against the matrix unit's 8; with four 7.98, with five 7.98 (measured on the
tpu machine with two LSQ requests and a scratchpad of two banks; cycles per
vmatmul, the difference of 96 and 192 blocks over 96, against a matrix unit
that does not hold the loop back).

### What the scratchpad changes

On a 4096 x 4096 x 4096 bf16 matmul, PyTorchSim's tile of it (`a_w512c1024`)
takes 7.72 cycles an input in its steady loop on this machine, where the
matrix unit allows 8, and where the same CPU with a scratchpad of two banks
of 32 GB/s behind the bus took 10.35: loads no longer stop the loop, the
matrix unit does. The four VALUs do not change it.

## Keys read from the machine description

| Key | Default | Meaning |
|---|---|---|
| `vpu_num_lanes` | 128 | the matrix unit is 2 x this wide and deep (this, bound to the lanes); 1 to 2^19 |
| `tpu_mxu_bound_to_lanes` | 0 | 1: the matrix unit is `vpu_num_lanes` x `vpu_num_lanes`, an operand one register, as in `examples/tpuv3`, so the lanes size it (8 lanes, 8 x 8); the slots and a word a cycle stay this model's. 0: the model's own, 2 x the lanes |
| `tpu_mxu_queue_entries` | 256 | rows in each array's input queue and in each half's output queue; at least 2: a word of two rows enters whole |
| `tpu_mxu_delay_cycles` | the array's width + depth - 1: 4 x `vpu_num_lanes` - 1 (2 x, bound) | slots in each array's delay line, one cycle each; at least 1 |
| `tpu_mxu_units` | 1 | how many arrays the matrix unit has, each with its own queues, delay line and weights; 1 to 32, what an instruction's field of 5 bits names |
| `tpu_sfu_latency_cycles` | 10 | as in `examples/tpu` |
| `tpu_misc_issue_width` | 2 | as in `examples/tpu` |
| `tpu_xlu_latency_cycles`, `tpu_xlu_queue_entries` | 10, 256 | as in `examples/tpu` |
| `tpu_trace` | 0 | as in `examples/tpu`; an issue of the matrix unit also reports its array, when there is more than one, and that array's input queue and the two halves' output queues |
| `vpu_spad_size_kb_per_lane`, `vpu_spad_base_vaddr`, `run_base_path` | | the DMA's, as in `examples/tpu` |

A key is `tpu_<unit>_<what>` whichever TPU model reads it: the shared units
read the same keys in both libraries, and the matrix unit reads its own.
`tpu_mxu_queue_entries` was `tpu_systolic_queue_entries` as `libtpu_mxu.so`;
that key is the systolic array's and the multi-precision array's, which this
model does not hold, and it is no longer read here. A description that still
gives it gets the default of 256.

With `tpu_trace`:

```
[tpu] issue vmatmul: 1 cycles after the last issue, 0 in flight, input queue 0, output queues 0 and 0
[tpu] issue vpop: 17 cycles after the last issue, 0 in flight, input queue 0, output queues 4 and 4
```

## Tests

`tests/tpuv6e/run.sh` checks the matrix unit's timing on gem5 (`mxu.S`,
`mxu_rate.S`, `mxu_arrays.S` on two arrays, and `mxu_slots.S`, a `vpop` and a
`vmatmul` in one cycle, also on the gem5 machine), the units' names and order in the statistics (`ports.cc`, with
no simulator, and gem5's dump), the gem5 machine (its configuration; three
loads, two stores and four accesses a cycle at the scratchpad, `scratchpad.S`;
`tests/tpu`'s `loads.S` alike from any lines and `addresses.S`; 64
independent vfadds and vfmuls 4 a cycle at the VALUs, `valu.S`), and a refused
machine description;
`functional/run.sh` checks what it computes on Spike against a formula
(`mxu.c`, on one array and on two), and what it refuses. It runs on the machine of
`tests/tpu/functional` (`machine.yml`, `common.h`).
