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
`examples/tpuv6/README.md` ("The matrix unit"). The `Shape` in `tpuv3.cc`
sets what differs between models:

| | tpuv3 | tpuv6 | The v3's value |
|---|---|---|---|
| `REGISTERS`: an operand's registers; the array is this x `vpu_num_lanes` each way | 1 | 2 | 128 x 128 on 128 lanes |
| `SLOTS`: `vmatprep`, `vmatpush` and `vmatmul` a cycle, all arrays | 1 | 2 | a placeholder |
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

## Keys read from the machine description

| Key | Default | Meaning |
|---|---|---|
| `vpu_num_lanes` | 128 | the array's width and depth; 1 to 2^19 |
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
order (`ports.cc` and gem5's dump), and a refused queue size.
`functional/run.sh` checks what the unit computes on Spike against a formula
(`mxu.c`, on 4 and 8 lanes) and what it refuses, on the machine of
`tests/tpu/functional` with the kernels of `tests/tpuv6/functional`.
