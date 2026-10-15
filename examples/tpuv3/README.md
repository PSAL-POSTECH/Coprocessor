# tpuv3

A TPU v3 TensorCore as one model (`libtpuv3.so`, name `tpuv3`): the tpu
example's units (`examples/tpu/`), with its matrix unit `tpu::Mxu` in the
v3's shape, 128 x 128 on 128 lanes.

## Units

`tpuv3.cc` is a `tpu::Machine` (`examples/tpu/machine.hpp`) of four units,
each built with the name its statistics report:

| Unit | File | Reported as |
|---|---|---|
| `tpu::Sfu` | `examples/tpu/sfu.hpp` | `TPUV3_SFU` |
| `tpu::Misc` | `examples/tpu/misc.hpp` | `TPUV3_MISC` |
| `tpu::Xlu` | `examples/tpu/xlu.hpp` | `TPUV3_XLU` |
| `tpu::Mxu<Shape>` | `examples/tpu/mxu.hpp` | `TPUV3_MXU` |

## The matrix unit

The unit's encodings, fields, weights, queues and delay line are described in
`examples/tpuv6e/README.md` ("The matrix unit"). The `Shape` in `tpuv3.cc`
sets what differs between models:

| | tpuv3 | tpuv6e | The v3's value |
|---|---|---|---|
| `REGISTERS`: an operand's registers; the array is this x `vpu_num_lanes` each way | 1 | 2 | 128 x 128 on 128 lanes |
| `SLOTS`: `vmatprep`, `vmatpush` and `vmatmul` a cycle, all arrays | 1 | 2 | a placeholder: the TPUv2 bundle has one push slot, for its one MXU and the transpose unit ([Norrie][norrie], p.4; [CACM 2020][cacm], p.70); the v3's, for two MXUs, no source states |
| `WORD_A_CYCLE`: a word of two 16-bit rows a cycle, or one row | no | yes | a row a cycle gives 123 TFLOPS per chip at 940 MHz |

An operand is one register, so there is no `vmatprep` and one half of the
depth and of the columns: a `vmatprep`, a `vmatpush` with [4:3] = 2 and a
`vpop` with [2] = 1 end the run. Rows enter one a cycle; a queue still needs
two entries, as a `vmatmul` of bfloat16 queues a word of two rows. The delay
line is 2 x lanes - 1 slots, 255 at 128 lanes.

A v3 TensorCore has two MXUs; the model has one (`tpu_mxu_units` 1), and
TOGSim's array split models the second.

Not modelled: a fixed array larger than the lanes (the field at 19:15 cannot
number an operand's registers), whether the two MXUs share slots, the cost of
a latch, the RHS transpose, int8, and the rounding of inputs to bfloat16.

## The gem5 machine

`gem5/` is the machine PyTorchSim measures `libtpuv3.so` on: the tpu
example's machine (`examples/tpu/gem5`) with the v3's scratchpad ports, vector
ALUs and clock, built as the v6e's is (`examples/tpuv6e/README.md`, "The gem5
machine"): `script_systolic.py` calls `machine` with
`multiport_scratchpad(1, 1, 2, 1)` and `clock="940MHz"`, and `vpu_config.py`'s
CPU takes `vliw_pool(2)`, both from `examples/tpu/gem5`. It needs a gem5 with
`MultiPortScratchpad`, release `vcix-v0.3.3` or later.

```
gem5.opt -d m5out examples/tpuv3/gem5/script_systolic.py -c <binary> --model libtpuv3.so \
    [--machine-config machine.yml] [--vlane N] [--vlen N]
```

The sources describe the TPUv2 core's bundle: 322 bits, two scalar slots, four
vector slots of which two are vector load and store, two matrix slots (a push
and a pop), a misc slot and six immediates ([Norrie et al., IEEE Micro
2021][norrie], p.4, "TPUv2 Core"); "two scalar, two vector ALU, vector load
and store, and a pair of slots that queue data to and from the matrix multiply
and transpose units" ([Jouppi et al., CACM 2020][cacm], p.70, the TensorCore
of both). No source lists the v3's slots; this machine takes them to be the
v2's, an inference: the v3 kept the bundle ("TPUv2 and TPUv3 shared a 322-bit
VLIW instruction bundle length", [Jouppi et al., ISCA 2021][isca], p.6), the
v3's enhancements do not include the vector unit ([Norrie][norrie], p.7: two
MXUs, the clock, HBM, the links), and its 32-bit TFLOPS grow by its clock
alone, 3 to 4 as 700 to 940 MHz ([CACM 2020][cacm], Table 3). Each kind of
slot is one resource here:

| | The v6e machine | This machine | From |
|---|---|---|---|
| scratchpad loads, stores, accesses a cycle | 3, 2, 4 | 1, 1, 2 | the bundle's two load/store slots read as one load and one store slot, as CACM's "vector load and store"; Norrie's "two used for vector load/store" also reads as two slots that each load or store, 2 loads a cycle, which would change `loads.S` and `addresses.S`; no source bounds their sum, so access is the sum and no further limit |
| scratchpad latency | 1 | 1 | a placeholder: no source gives VMEM's latency |
| `MinorVALU`s | 4 | 2 | the bundle's two vector ALU slots. 128 lanes x 8 sublanes x 2 VALUs x 2 cores x 0.94 GHz = 3.85 T, the v3's "4 (32b)" TFLOPS of [CACM 2020][cacm], Table 3 |
| system clock | 1GHz (the original's) | 940MHz | "Increasing clock frequency from 700 to 940 MHz" ([Norrie][norrie], p.7); [CACM 2020][cacm], Table 3 |
| `executeLSQRequestsQueueSize` | 2 | 2 | a compensation, as on the v6e; that it equals the v3's 2 accesses a cycle is not why |
| integer units (`IntAlu`) | 6 | 6 | a compensation, as on the v6e (#33, #34): chosen there as the fewest units at which `libtpuv6e.so`'s matrix-unit loop runs at its own pace (`examples/tpuv6e/README.md`); not measured on the v3 |
| matrix push and pop slots | | | the model's: `SLOTS` 1 in `tpuv3.cc`, a pop port per array (`examples/tpu/mxu.hpp`) |

The VALUs are `funcUnits21` and `26`, the fifth and sixth integer units `27`
and `28`; the accelerator is `funcUnits16`. Everything else, the CPU's widths,
the bus, the instruction cache, VLEN 256, is the tpu machine's.

The bundle is not a limit of its own. Minor issues in order, up to 12 a
cycle; the slots above bound each kind, so independent ops in program order
issue as a bundle would. A cap of 8 ops a cycle would make the instructions an
RVV program adds and a v3 bundle does not have (an `addi` for each access, as
an RVV load has no immediate offset, `vsetvli`, branches, scalar loads and
stores) take the v3's slots, a cost the v3 does not pay; the misc slot and the
six immediates are not modelled for the same reason.

gem5 rounds the clock to a tick of a picosecond: 940 MHz is a cycle of 1064
ticks, 939.85 MHz. Cycles do not depend on it: `scratchpad.S`, `valu.S`,
`loads.S`, `addresses.S` and `mxu.S` take the same `system.cpu.numCycles` in
every dump at 940MHz and at 1GHz, and the matrix unit's trace is the same;
only `simTicks` and `simSeconds` scale, by 1064/1000.

Not modelled: VMEM's banks and their conflicts (the ports take any address),
its capacity (the sources disagree: 16 MiB a core in [CACM 2020][cacm], p.71's
text, 8 MiB a core in its Figure 3, of the TPUv2), Smem apart from VMEM (a
scalar load or store and an instruction fetch take a vector load or store
port: with one load port, a cold instruction cache shows, below), the bundle's
static schedule, the forwarding of at most three scalar values to the vector
unit a bundle ([Norrie][norrie], p.4), each slot's pipeline latency (VALU
`opLat` 1 is the original's), the transcendental unit (its inverse square
root, [CACM 2020][cacm], pp.70-71: a vector divide or square root takes a VALU
slot with `opLat` 1, as on the tpu machine, a placeholder), the second MXU
(TOGSim's array split), and the v3's 4 KiB vector registers (8 x 128 x 32
bits, [CACM 2020][cacm], p.71; VLEN stays PyTorchSim's 256). A reduction takes
two VALU slots, as on the v6e machine: gem5 decodes it as two micro-ops.

On this machine (`tests/tpuv3/run.sh`; cycles of the window):

| Program | v6e machine | v3 machine |
|---|---|---|
| `tests/tpuv6e/valu.S`, 64 vfadds and vfmuls | VALUs busy 16 of 34 | 32 of 50 |
| `tests/tpu/loads.S`, 64 loads, 128 and 64 bytes apart | 41 and 41 | 86 and 83: the first window still reads 4 instruction lines through the one load port |
| `tests/tpu/addresses.S`, 64 loads with their `addi`s | 76 | 105 |
| `tests/tpuv3/scratchpad.S`, a load and a store in turn | | at most 1 load, 1 store, 2 accesses a cycle |

[norrie]: https://www.gwern.net/doc/ai/scaling/hardware/2021-norrie.pdf
[cacm]: https://www.gwern.net/doc/ai/scaling/hardware/2020-jouppi.pdf
[isca]: https://www.cs.cmu.edu/~18742/papers/Jouppi2021.pdf

## Keys read from the machine description

| Key | Default | Meaning |
|---|---|---|
| `vpu_num_lanes` | 128 | the array's width and depth; 1 to 2^19 |
| `tpu_mxu_bound_to_lanes` | 0 | 1 makes the array lanes x lanes; this model's array is that either way |
| `tpu_mxu_queue_entries` | 256 | rows each queue of an array holds (a placeholder); at least 2 |
| `tpu_mxu_delay_cycles` | 2 x `vpu_num_lanes` - 1 | slots of the delay line |
| `tpu_mxu_units` | 1 | arrays of the matrix unit |
| `tpu_sfu_latency_cycles` | 10 | cycles of a special function (a placeholder) |
| `tpu_misc_issue_width` | 2 | misc instructions a cycle (a placeholder) |
| `tpu_xlu_latency_cycles`, `tpu_xlu_queue_entries` | 10, 256 | the cross-lane unit's delay and queue (placeholders) |
| `tpu_trace` | 0 | 1 prints each issue and commit, with the matrix unit's queues |
| `vpu_spad_size_kb_per_lane`, `vpu_spad_base_vaddr`, `run_base_path` | | the DMA's, described in `examples/tpu/README.md` |

A simulator names the model by its library, `libtpuv3.so` (Spike's
`--extlib`, the gem5 fixture's `--model`).

## Tests

`tests/tpuv3/run.sh` checks the matrix unit's timing on gem5 (`mxu.S` on 4
and 128 lanes, `mxu_rate.S` on queues of 8 and 16), the units' names and
order (`ports.cc` and gem5's dump), the gem5 machine (its `config.ini`, the
programs of the table above, `mxu.S` timed as on the fixture, and no warning
that it is the generic machine), and a refused queue size.
`functional/run.sh` checks what the unit computes on Spike against a formula
(`mxu.c`, on 4 and 8 lanes) and what it refuses, on the machine of
`tests/tpu/functional` with the kernels of `tests/tpuv6e/functional`.

`tpu_mxu_bound_to_lanes` is tested here for both models: `libtpuv6e.so` bound
on 4 and 128 lanes on gem5 and on 4 and 8 lanes on Spike, where its output is
`libtpuv3.so`'s; `libtpuv3.so` on 8 lanes with and without the key; the key at
2 refused.
