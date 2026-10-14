# tpuv6

A TPU v6e on the two custom opcodes: one model (`libtpuv6.so`, name `tpuv6`).
It is the tpu example's machine with another array: the special-function
unit, the one-cycle instructions and the cross-lane unit of `examples/tpu`,
and the matrix unit `tpuv6::Mxu` (`mxu.hpp`) in place of the systolic array
and the multi-precision array. It is another machine, so another library, and
the matrix unit has the encodings of the two arrays it stands in for: a
program for one model is not a program for the other.

## Units

`tpuv6.cc` is a `tpu::Machine` (`examples/tpu/machine.hpp`) of four units,
each built with the name its statistics report:

| Unit | File | Reported as |
|---|---|---|
| `tpu::Sfu` | `examples/tpu/sfu.hpp` | `TPUV6_SFU` |
| `tpu::Misc` | `examples/tpu/misc.hpp` | `TPUV6_MISC` |
| `tpu::Xlu` | `examples/tpu/xlu.hpp` | `TPUV6_XLU` |
| `tpuv6::Mxu` | `mxu.hpp` | `TPUV6_MXU` |

The first three are the tpu example's units, included as `tpu/<unit>.hpp`
(the build adds `examples` to the include path) and timed as its README says;
only the name they report is this model's. The ports, and so the statistics,
are in this order: gem5 reports `vcix.TPUV6_MXU.admitted` and so on (before,
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

Timing: one instruction per cycle, for all the arrays; a `vmatprep` and a
`vmatpush` enter nothing. The rows of a `vmatmul` wait in its array's input
queue, and one word a cycle enters the array's delay line of
`tpu_mxu_delay_cycles` slots, as a word enters a SIMD unit -- a word into
each array a cycle, so the unit's input port, `TPUV6_MXU.input`, takes 2 x
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

`tpuv6::Functional` (`functional.hpp`) derives from the tpu example's
`tpu::FunctionalCore` (`examples/tpu/functional_core.hpp`), which carries out
the special functions, `vlane_idx` and `compute`, the cross-lane unit and the
DMA as `examples/tpu/README.md` says, and adds the matrix unit: the handlers
of `vmatprep`, `vmatpush`, `vmatmul` and `vpop`, the unit whose encodings
those are, and its state, per array (the staged and the active weights, the
held upper halves, the results). A run it cannot carry out ends as the core's do, with a
line on standard error that starts `tpu: ` and exit 1.

## Keys read from the machine description

| Key | Default | Meaning |
|---|---|---|
| `vpu_num_lanes` | 128 | the matrix unit is 2 x this wide and deep; 1 to 2^19 |
| `tpu_mxu_queue_entries` | 256 | rows in each array's input queue and in each half's output queue; at least 2: a word of two rows enters whole |
| `tpu_mxu_delay_cycles` | 4 x `vpu_num_lanes` - 1 | slots in each array's delay line, one cycle each; at least 1 |
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

`tests/tpuv6/run.sh` checks the matrix unit's timing on gem5 (`mxu.S`,
`mxu_rate.S`, and `mxu_arrays.S` on two arrays), the units' names and order in the statistics (`ports.cc`, with
no simulator, and gem5's dump), and a refused machine description;
`functional/run.sh` checks what it computes on Spike against a formula
(`mxu.c`, on one array and on two), and what it refuses. It runs on the machine of
`tests/tpu/functional` (`machine.yml`, `common.h`).
