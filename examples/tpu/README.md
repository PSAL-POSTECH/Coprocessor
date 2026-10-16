# tpu

A TPU's units on the two custom opcodes: one model (`libtpu.so`, name `tpu`).
It is five `vcix_uarch` units (`include/vcix_uarch.hpp`); each lists its own
encodings, its blocks, and per instruction the route it takes through them and
what it computes. `tpu.cc` holds them in a `tpu::Machine` (`machine.hpp`), a
`vcix_uarch::Machine` that adds `tpu_trace`; one route gives both faces, the
timing on gem5 and the values on Spike.

## Units

| Unit | File | Reported as | Instructions | Timing |
|---|---|---|---|---|
| `tpu::Sfu` | `sfu.hpp` | `SFU` | `verf`, `vtanh`, `vsin`, `vcos`, `vlog`, `vatan`, `vexp` (`sf.vc.v.iv`, custom-2) | a pipeline: one instruction enters per cycle, the result is ready `tpu_sfu_latency_cycles` after it entered, and any number are inside at once |
| `tpu::Misc` | `misc.hpp` | `MISC` | `vlane_idx`, `compute`, `pack`, `unpack_lo`, `unpack_hi` (custom-2); the DMA's `dma_config_desc`, `mvin`, `mvin2`, `mvin3`, `mvout` (custom-1) | one cycle each, any number in a cycle |
| `tpu::Systolic` | `systolic.hpp` | `SYSTOLIC` | the systolic array's input push (`sf.vc.iv` 0), weight push (`sf.vc.iv` 1) and pop (`sf.vc.v.i` 2) (custom-2) | one instruction per cycle. A push of `vl` elements waits for room in the input queue; one element a cycle enters a delay line of 2 x `vpu_num_lanes` - 1 slots and comes out into the output queue; a pop of `vl` waits until the output queue holds `vl`, or until nothing more is on its way. A weight push changes nothing. The array stops while the output queue is full |
| `tpu::Xlu` | `xlu.hpp` | `XLU` | the cross-lane unit's `xlu_push` (`sf.vc.iv` 3), `xlu_push_pattern` (`sf.vc.ivv` 3) and `xlu_pop` (`sf.vc.v.i` 1) (custom-2) | one instruction per cycle, one cycle each. A push of `vl` elements, of either kind, waits for room in the input queue; one element a cycle enters a delay line of `tpu_xlu_latency_cycles` slots and comes out into the output queue; a pop of `vl` waits until the output queue holds `vl`, or until nothing more is on its way. The first push after a pop starts a tile, and what the unit still held is forgotten. The unit stops while the output queue is full |
| `tpu::Msa` | `msa.hpp` | `MSA` | the multi-precision array's `msa push` (`sf.vc.iv` 2) and `msa pop` (`sf.vc.v.i` 3) (custom-2) | the systolic array's, with queues and a delay line of its own, from the same two keys. One push serves both operands: with [4] of its field at 19:15 set it is a weight push and changes nothing, otherwise an input push of `vl` elements. The delay line is 2 x `vpu_num_lanes` - 1 slots whatever the field's active-width shift ([3:2]) and format ([1:0]) are |

The systolic array's `compute` has no timing effect and is owned by
`tpu::Misc`. The DMA has no model of its own either: `tpu::Misc` owns its
instructions at one cycle each. Nothing on custom-3 is owned.

The array and the cross-lane unit are the same structure with a delay line of
a different length: a `vcix_uarch::Pipeline` (input queue, stages, output
queue, `Stall::WHEN_OUTPUT_FULL`), and each unit has one. Its latency is not
known: the 10 of `tpu_xlu_latency_cycles` is a placeholder. The
multi-precision array is a third pipeline, as long as the systolic array's. That an array working on a reduced width takes the full
delay is a decision, not a measurement: no hardware says what it takes.

What a stream is asked to pop need not be what it was pushed, and the rule
is the stream's, the same for the three units. A pop of `vl` is issued when
the output queue holds `vl`, or when the input queue and the delay line are
empty: nothing more is on its way, so it takes what is out, `vl` at most. A
stream whose pops count the rows its pushes counted never meets the second
case, and its timing is what it was.

What a pop leaves stays in the output queue. The two arrays compute as
queues, so it is popped next: an array program that pushes more than it pops
fills the output queue and stops, and a push then waits for good. The
cross-lane unit computes a tile, the pushes between two pops, and its first
pop replaces what the last tile left. So its timing forgets
the old tile at the first push after a pop, the queues and the line, and a pop
is never served from the tile before. The tile's shape is not read, nor the
field at 19:15: a tile 2 lanes by 1 deep that comes out 1 lane by 2 deep is a
push of 1 and a pop of 2.

A unit is a plain copyable class: a constructor taking its name,
`configure(Config)`, `hardware(h)` listing its ports, pipelines, command queue
and registers, and `INSTRUCTIONS`, each encoding with the function that
declares its route (`r.use(port)`, `r.send(pipeline, n)`,
`r.receive_at_most(pipeline, n).through(port)`, `r.done_after(n)`, ...) and its
values (`r.compute(...)`). `vcix_uarch::Machine` derives `can_accept`, `issue`,
`tick`, `reset`, the ports and `execute` from them. Under `tpu_trace` an issue
reports the unit's pipeline queues, or what its `trace_queues(insn)` prints
(the matrix unit's).

A unit has no name of its own: it is built with the name its statistics
report, and the model that holds it says which. `tpu.cc` gives the five the
names of the table's "Reported as", so gem5 reports `vcix.SYSTOLIC.admitted`
and so on (before, `vcix.Systolic.*`, `vcix.Sfu.*`, `vcix.Misc.*`,
`vcix.Xlu.*`, `vcix.Msa.*`). The encodings' names, and so
`vcix.committed.<encoding>`, are the units' and did not change.
`tpu::Machine<Unit...>` holds the units in the order it is given
them: configure, tick, reset and the ports go to them in that order, a call
about an instruction to the first that owns it. The order of the ports is the
order of the units in the statistics a simulator lists.

## The values

The units' values do what the Spike these units came from did
(riscv-isa-sim branch `spike-multi-precision-sa`, ea9ec10: `spike-fp8` and the
multi-precision array, the units built in; the DMA as of 9f555b4, without the
element type `spike-fp8` added to the descriptor), in every lane:

| Instructions | What they do |
|---|---|
| `verf`, `vtanh`, `vsin`, `vcos`, `vlog`, `vatan`, `vexp` | `vd[i] = f(vs2[i])` on halves and singles, and on doubles for `vlog` and `vatan` |
| `vlane_idx` | every element of `vd` is its lane's number, written as 64 bits whatever the element width |
| `pack` (`sf.vc.v.vv`) | `vd`'s `vl` 32-bit words are `vs2`'s `vl` f32 elements then `vs1`'s, rounded to bf16 (nearest, ties to even), two to a word, element `2w` in the low half -- the v6e's `vpack.c.bf16` |
| `unpack_lo`, `unpack_hi` (`sf.vc.v.iv` field 8, 9) | `vs2`'s `vl` words hold `2 vl` bf16, element `2w` in the low half of word `w`; `vd`'s `vl` f32 are elements `0 .. vl-1` (lo) or `vl .. 2vl-1` (hi), widened exactly -- the v6e's `vunpack.c.l/h.bf16` |
| systolic weight push, input push, pop; `compute` | a weight push adds a column to the matrix, of which a lane keeps its last `lanes` weights; an input push multiplies at once, element `i` of every lane being one input vector; a pop takes the results; `compute` does nothing. Elements are singles, 16 or 8 bits wide; what an element is rides the instruction in its rs1 field, one table at every width: 0 the width's own (an integer of 8 bits, a half, a single), 1 an E4M3 float, 2 an E5M2 float, 3 a bfloat16. A push or pop whose code its width cannot hold (3 on 8 bits, 1 or 2 on 16, anything but 0 on 32, any code above 3) ends the run. A pop reads the same table and ends the run on the same codes, and under 3 too: a bfloat16 result is not modeled; a pop of 64 bits writes singles. A pop to halves or to 8-bit floats rounds by `frm`, which the model reads through the host |
| `xlu_push`, `xlu_push_pattern`, `xlu_pop` | a tile of raw 32-bit values is pushed; the first pop after a push moves it across the lanes as the field at 19:15 of the push says ([4:3] the lanes shuffled before, [2] depth and lane exchanged, [1:0] the lanes shuffled after). A shuffle is none (0), every lane reading lane 0 (1), or every lane reading the lane a pattern names (2). The pattern of the shuffle before comes with `xlu_push_pattern`; the one of the shuffle after is loaded by an `xlu_push` whose [4:3] is 3 |
| `msa push`, `msa pop` | the multi-precision array: the systolic array on words of 32 bits that hold 1 single, 2 halves or 4 floats of 8 bits, so a column keeps `lanes` times that many weights. The field at 19:15 of a push says [4] weights or inputs, [3:2] how many columns compute (all of them, shifted right by it; the others give 0), [1:0] what a word holds (single, half, E4M3, E5M2). A weight push of another format or width empties the matrix. A pop takes what is there, as singles, halves, or 8-bit floats (E5M2 when its field's [1:0] is 3, E4M3 otherwise) |
| `dma_config_desc`, `mvin`, `mvin2`, `mvin3`, `mvout` | a tensor of up to four dimensions between memory and the scratchpad, as the descriptor says: a mask, skip axes, a fill value, a bound on memory, an accumulating `mvout`, indices |

Each unit keeps its values in its own blocks. The arrays' results ride their
pipeline (`Pipeline<Row>`: `put` at the push, `take` at the pop); the weights,
the cross-lane unit's tile and the DMA's descriptor are `vcix_uarch::Register`s.
The cross-lane unit's tile is a register, not the pipeline's entries: a push
of its pattern enters the stages and no tile, and its first pop computes the
whole tile. `values.hpp` holds the numbers (halves, bfloat16, the floats of 8
bits and how a single is narrowed to them, an operation's NaN as golden's x86
build gave it) and `dma.hpp` the DMA, `tpu::Misc`'s untimed side path.

Where the old Spike trapped, asserted or read past a buffer, the model ends
the run with a line on standard error (`tpu: ...`, exit 1): a special function
on a width it has no form for, a pop of more than was computed, an input push
before any weight, a push to the multi-precision array of anything but words
or of inputs packed otherwise than its weights, a descriptor with a dimension
of 0 or one that reaches past its tensor. As before, a transfer past the end
of a lane's scratchpad ends the run with exit 200, and a cross-lane shuffle
whose pattern is not as wide as its tile with exit 201.

Not carried over: what the old Spike printed under `SPIKE_DEBUG` and
`SPIKE_XLU_DEBUG`, and the list of all-zero tiles it wrote under
`SPIKE_DUMP_SPARSE_TILE`. A special function, and a pop of the
multi-precision array, round to nearest even; the old Spike used the rounding
mode the last floating-point instruction left.

`tests/tpu/functional/run.sh` checks it on Spike: each program prints what its
instructions left, and the checksums in `from-old-spike.sha256` are of what the
old Spike printed for the same program. Given an old Spike as a fourth
argument it also runs it and compares the outputs byte by byte.

## Keys read from the machine description

| Key | Default | Meaning |
|---|---|---|
| `tpu_sfu_latency_cycles` | 10 | cycles from the issue of a special function to its result; at least 1 |
| `tpu_trace` | 0 | not 0: the model prints a line at every issue and every commit |
| `vpu_num_lanes` | 128 | width and height of the systolic array and of the multi-precision array; 1 to 2^20 |
| `tpu_systolic_queue_entries` | 256 | entries in the input queue and in the output queue of each of the two arrays; at least 1 |
| `tpu_systolic_command_queue` | 0 | 0: every step of a systolic instruction happens at issue (the timing above). N: instructions wait in an in-order queue of N, the head working, as `examples/rtl/systolic/systolic.sv` does |
| `tpu_systolic_in_rows_per_cycle` | no limit | rows a cycle of an input or weight push at the queue head; needs the queue |
| `tpu_systolic_pop_rows_per_cycle` | `tpu_systolic_queue_entries` | rows a cycle of a pop; given, needs the queue |
| `tpu_systolic_weight_waits` | 0 | not 0: a weight push moves only once the array is empty; needs the queue |
| `tpu_xlu_latency_cycles` | 10 | slots in the cross-lane unit's delay line, one cycle each; at least 1. A placeholder: the unit's latency is not known |
| `tpu_xlu_queue_entries` | 256 | entries in the cross-lane unit's input queue and in its output queue; at least 1 |
| `vpu_spad_size_kb_per_lane` | none | the scratchpad of one lane, which the DMA lays a tensor out over; a transfer without it ends the run |
| `vpu_spad_base_vaddr` | 0xD0000000 | where the scratchpad starts, written `0x...` |
| `run_base_path` | none | not in the file: Spike's `--base-path`. An indirect transfer writes the indices it read to `<path>/indirect_access/indirect_index<n>.raw` |

With `tpu_trace` the model reports, on gem5:

```
[tpu] issue vexp: the first
[tpu] issue vtanh: 10 cycles after the last issue, 0 in flight
[tpu] commit vtanh: 10 cycles after its issue
[tpu] issue systolic pop: 11 cycles after the last issue, 0 in flight, input queue 0, output queue 4
[tpu] issue xlu_pop: 5 cycles after the last issue, 0 in flight, input queue 0, output queue 2
```

"In flight" counts the instructions issued and not committed. The issue of a
systolic, cross-lane or multi-precision instruction also reports the entries
it found in its unit's input queue and output queue. The reports do not change
what the model answers. `tests/tpu/run.sh` checks the timing of the five units
with them, all on `libtpu.so`.

## The gem5 machine

`gem5/` is the machine these units sit in: the CPU and memory configuration
PyTorchSim measures its cycle binaries on, moved here from
`gem5_script/vpu_config.py` (the functional unit pool and the CPU `RiscvVPU`)
and `gem5_script/script_systolic.py` (the syscall-emulation script) of
`PSAL-POSTECH/PyTorchSim` 4b2555a, and ported to the gem5 of branch `vcix`.
The CPU's widths and limits, the pool's unit counts and latencies, the clock
domains, the instruction cache and the bus are the original's; its loads and
stores have two ports where the original's have one, and its pool has two more
integer units (below).

```
gem5.opt -d m5out examples/tpu/gem5/script_systolic.py -c <binary> --model libtpu.so \
    [--machine-config machine.yml] [--vlane N] [--vlen N]
```

| Option | Meaning |
|---|---|
| `-c`, `--cmd` | the program; `-o`, `--options` its arguments |
| `--model` | the model library, `libtpu.so`; required |
| `--machine-config` | the machine description (the PyTorchSim config file, as for Spike): every key of it goes to the model, read as `examples/gem5_se.py` reads it |
| `--vlane` | `vpu_num_lanes` for the model. Given, it replaces the key of the machine description; not given, the description's value holds, and without either the model's default, 128 |
| `--vlen` | VLEN of the CPU, 256 by default; the description's `vpu_vector_length_bits` is not read |
| `--cpu`, `--mem`, `--sparse` | accepted as before; `--cpu` is `RiscvVPU` by default, the other two were and are read by nothing |

PyTorchSim reads `system.cpu.numCycles` of every statistics dump in
`m5out/stats.txt` but the last. `examples/tpuv6e/gem5` is the v6e's machine,
built with this script's functions (`machine`, `multiport_scratchpad`,
`simulate`) and its own CPU, whose pool is `vliw_pool` of `vpu_config.py`: a
VLIW TPU's VALU slots as `MinorVALU`s, each of which takes every vector ALU op
class. `machine` takes the system clock, the original's 1GHz by default;
`multiport_scratchpad` a gem5 `MultiPortScratchpad`'s load, store and access
ports and latency.

This is the generic machine, a model's stand-in, not a model's own. Run as
gem5's script, it says so on stderr before it builds anything:

```
warning: examples/tpu/gem5 is the generic machine; time a model with its own machine, examples/<model>/gem5 (e.g. examples/tpuv6e/gem5)
```

It warns only when it is the script gem5 runs, not when
`examples/tpuv6e/gem5/script_systolic.py` imports its functions, and nothing in
`m5out` (`config.ini`, `stats.txt`) changes with it.

What the port changed:

1. **The accelerator units are one `MinorVcixAccelFU`.** `SystolicArray`
   (`CustomMatMul*`) and `SpecialFunctionUnit` (`CustomV*`, 10 cycles) are
   gone, and `CustomVlaneIdx` left `MinorVecMisc`: this gem5 has none of those
   op classes, and the one unit, in the place `SystolicArray` had in the pool,
   hands their instructions to the model. So do the cross-lane units the fork
   `student-Jungmin/PyTorchSim` (`develop-npu`, 84fec6e) has in this pool
   (`TransposeUnit`, `CrossbarUnit` and their pops, or `CrossLaneUnit`): what a
   cross-lane instruction costs is the model's to say. `SparseAccelerator`,
   which no pool used, is gone with its op classes.
2. **The model is configured from the machine description**, by `--model`,
   `--machine-config` and `--vlane` above, in place of
   `SystolicArray.systolicArrayWidth` and `systolicArrayHeight`.
3. **Op classes gem5 25.1 added have a unit of their kind:**

   | Unit | Added |
   |---|---|
   | `MinorFPUnit` | `Bf16Cvt` |
   | `MinorVecAdder` | `SimdBf16Add`, `SimdBf16Cmp` |
   | `MinorVecMultiplier` | `SimdDotProd`, `SimdBf16Mult`, `SimdBf16MultAcc`, `SimdBf16MatMultAcc`, `SimdBf16DotProd` |
   | `MinorVecMisc` | `SimdBf16Cvt` |
   | `MinorVecLdStore` | `SimdUnitStrideSegmentedFaultOnlyFirstLoad`, `SimdStrideSegmentedLoad`, `SimdStrideSegmentedStore` |
   | `MinorCustomMiscFU` | `System`, which gem5's `MinorDefaultMiscFU` now lists where it listed `IprAccess` |

   Not placed: `SimdSha3`, `SimdSm4e` and `SimdCrc`. The pool has no unit of
   their kind: the original gives none to `SimdAes`, `SimdAesMix`, the six
   `SimdSha*` classes before them, or to `Matrix`, `MatrixMov` and `MatrixOP`,
   and that is kept. gem5 warns of each of the fourteen when the CPU is built;
   no RISC-V instruction of this gem5 has one of them.
4. **What this gem5 does not take as written:** `vpu_config` is imported from
   beside the script, not from `$TORCHSIM_DIR/gem5_script`, and the parameters
   `unitType`, `systolicArrayWidth` and `systolicArrayHeight` of a unit do not
   exist. Everything else is accepted unchanged, `SpmXBar` included. The
   branch predictor is gem5's default, as before; that default is now a
   `BranchPredictor` around the same `TournamentBP`.

### Two load/store ports

The original moves one vector register a cycle between the CPU and the
scratchpad: `RiscvVPU` leaves `executeLSQRequestsQueueSize` at gem5's default,
so one load or store at a time enters the LSQ, and the scratchpad is one bank
of 32 GB/s, the 32 bytes of a 256-bit register a cycle at 1 GHz. This machine
has two of each:

| | The original | This machine |
|---|---|---|
| `RiscvVPU.executeLSQRequestsQueueSize` (`vpu_config.py`) | 1 | 2 |
| scratchpad banks (`spad_num_bank`, `script_systolic.py`) | 1 | 2, interleaved by the 64-byte line |
| scratchpad bandwidth, in all | 32 GB/s | 64 GB/s, 32 GB/s each bank |

The banks interleave by the line, not finer: the bus carries whole 64-byte
lines (`cache_line_size`), and a line split over two banks has no destination:
at a 32-byte interleave gem5 stops before the program's first instruction with
`fatal: Unable to find destination for [0:0x40] on system.membus`.

Why two: the TPU v6e moves more than one register a cycle. XLA's static
per-bundle utilization of a v6e kernel (the fusion of a 1024 x 384 x 1152
matmul) gives the slots of a bundle as

| MXU | XLU | VALU | VPOP | EUP | VLOAD | VLOAD:FILL | VSTORE | VSTORE:SPILL | SALU |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 2 | 4 | 2 | 1 | 3 | 3 | 2 | 2 | 2 |

that is, up to three loads and two stores of a vector register a cycle, each in
a slot of its own. Two ports is a step toward that, not the v6e: here loads and
stores share the two requests and the two banks, a third load waits, and two
accesses to the same bank take turns.

What it changes, on a steady loop of `libtpuv6e.so`'s matrix unit (a vmatmul of
16 rows a block, its 6 vector loads and 4 vector stores; cycles per vmatmul,
the difference of 96 and 192 blocks over 96; measured with four integer units):

| The loop | One port | Two ports |
|---|---|---|
| its loads of C first, each kind of instruction together | 12.79 | 10.78 |
| in the order the compiler emits | 20.02 | 20.01 |

The two loops are the same instructions. In the compiler's order each vfadd
waits on the load just before it, and two ports change nothing there; they pay
where two independent accesses are there to go together.

`tests/tpu/run.sh` runs `sfu.S` on this machine as well, and `loads.S`: a
window of 64 independent vector loads from lines all in one bank, then one from
lines in both banks, 85 and 56 cycles (92 and 58 with four integer units, 89
and 57 with five, below); in the second the loads commit two a cycle.

### Two more integer units

The original's pool has four integer units (`MinorCustomIntFU`, `IntAlu`, one
cycle; `funcUnits01`, `02`, `09` and `10`); this machine has six, the fifth and
the sixth last in the pool (`funcUnits45` and `46`), so that every unit before
them keeps its index in the statistics.

This is a compensation for the CPU not being a VLIW machine, not a fact of the
v6e. The v6e computes a vector load's address inside the load, as a base
register plus an immediate, and spends no scalar slot on it: in the final
bundles of the fusion of a 1024 x 384 x 1152 matmul (`fusion-76`), a load is
`vld [vmem:[%s27831_s1 + $0xec] ...]`; of its 1802 vector loads, 215 are
`[%s + imm]`, 941 `[#allocation3 + imm]`, 640 a spill slot and 6 a bare base,
the offset riding the load. An RVV load
has no immediate offset, so a compiled loop steps a pointer with an `addi` for
each access. In gem5's Minor CPU an `addi` finishes in a cycle but holds its
unit until it commits, and commit is in order: behind an older vector load
that has not committed, finished `addi`s hold all the units, and the next one
waits, and the in-order issue with it.

What the count changes, on a steady loop of `libtpuv6e.so`'s matrix unit: a
vmatmul of 16 rows of bfloat16 a block, its 6 vector loads (4 of C, 2 of A), 4
vector stores and 4 vfadds, 64 vmatmuls ahead of their pops; cycles per
vmatmul, the difference of 96 and 192 blocks over 96, all the loops in one
binary. The matrix unit allows 8 a block (a word of two rows a cycle, 8 words);
how far under 8 the CPU's own time is, is measured against a matrix unit that
does not hold it back: a build of `libtpuv6e.so` whose arrays take four words a
cycle, with `tpu_mxu_queue_entries: 65536` and `tpu_mxu_delay_cycles: 32`, not
in the tree.

| Integer units | 4 | 5 | 6 | 7 | 8 | 10 |
|---|---|---|---|---|---|---|
| the CPU alone: |
| every access its own pointer, an `addi` each (32 instructions a block) | 9.05 | 8.05 | 8.05 | 8.05 | 8.05 | 8.05 |
| C a block ahead too, two sets of registers in turn (31) | 8.39 | 7.87 | 7.73 | 7.72 | 7.22 | 6.89 |
| C two blocks ahead of its vfadd, four sets in turn (26) | 7.98 | 7.98 | 6.49 | 6.49 | 6.49 | 6.49 |
| with the matrix unit: |
| every access its own pointer | 9.05 | 8.12 | 8.12 | 8.12 | 8.12 | 8.12 |
| C a block ahead too | 8.39 | 7.93 | 7.79 | 7.78 | 7.87 | 8.02 |
| C two blocks ahead | 8.04 | 8.04 | 8.07 | 8.07 | 8.07 | 8.07 |

The third loop is the second pipelined a block deeper: a block's C is loaded a
step before its pops, added a step after them and stored the step after that,
and its load and its store share one pointer, stepped once a block after the
store, so a block spends 6 `addi`s, not 10 (A's two halves, one block apart,
the other two). Its stores go before the vcix and its loads before its
stores, as a load waits behind an older store until the store commits. Six
units are the fewest at which it runs at its own pace; with five, its 6
`addi`s, behind the loads, hold all five units for 3 cycles a block, and the
issue with them. At 6.49 the CPU has 1.5 cycles a block to spare under the
matrix unit; what is left of its time is the four vpops at one a cycle, the
stores waiting for a vector memory unit behind loads that the LSQ sends in
order, fetch, and the vmatprep and vmatmul sharing one input slot. Against the
matrix unit every loop runs at about 8 from five units on (7.8 to 8.1 in this
measure).

`tests/tpu/run.sh` runs `addresses.S` on this machine: a window of 64 vector
loads, each followed by the `addi` that steps its pointer, 93 cycles (101 with
four units, 98 with five).
