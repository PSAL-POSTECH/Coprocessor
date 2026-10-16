# vcix-accelerator

A third-party accelerator model interface for RISC-V simulators, built on the
observation that such an accelerator talks to the core through one channel
only: custom instructions. Two opcodes go to the model: custom-2, read by the
operand rules of VCIX (SiFive's Vector Coprocessor Interface, which gives the
project its name), and custom-1, read as R-type on integer registers.

## Goal

A user who wants to add a new accelerator writes **one shared library** that
describes both what the accelerator computes and how long it takes. They do not
patch Spike, and they do not patch gem5: each was changed once, to load such a
library.

Without this, adding a unit means editing two simulators by hand, in two
different styles:

- **Spike** (functional): a `DECLARE_INSN` in `encoding.h`, a body in
  `insns/*.h`, and unit state stored as members of `processor_t`.
- **gem5** (timing): a custom `OpClass`, a decoder entry per encoding, a
  functional unit class, and a branch in `Execute::issue` of the MinorCPU.

The two descriptions drift apart. In the tree this work started from, the
cross-lane unit exists in Spike and has no decoder entry in gem5 at all.

## Design

Each simulator gets an adapter, written once, that hands these instructions to
a loaded model. After that, a new accelerator is a new `.so`.

```
user model (.so)      includes one header; knows nothing about Spike or gem5
--------------------  C ABI: a struct of function pointers
Spike adapter         one extension that registers the encodings the model owns
gem5 adapter          two decode entries, one OpClass, one functional unit class
```

- **Spike is modified once**, in branch `vcix` of `PSAL-POSTECH/riscv-isa-sim`,
  the Spike whose vector unit has lanes. It has no accelerator unit of its own:
  the extension `vcixaccel`, built into it, hands every instruction of the two
  opcodes that a loaded model owns to that model. It reads the machine
  description itself (`--machine-config`) and refuses an extension whose
  encodings overlap an instruction it implements. See `adapters/spike/`.
- **gem5 is modified once**, in a branch that starts over from upstream gem5.
  It has a decode entry for each of the two opcodes, one `OpClass`
  (`VcixAccel`), one functional unit class (`MinorVcixAccelFU`), and code in
  the MinorCPU's `Execute` that tells the model what happens to each
  instruction -- issued, committed, squashed -- and ticks it every cycle.
  None of it is about a particular accelerator.

A model has two faces, and each simulator calls only its own:

| Face       | Called by | Entry points                          | Answers                         |
|------------|-----------|---------------------------------------|---------------------------------|
| Functional | Spike     | `execute`                             | what values the instruction produces |
| Timing     | gem5      | `can_accept`, `issue`, `commit`, `tick`, `ready` | whether the unit can take the instruction now, when its result is ready, and what the unit's state is afterwards |

Stall and latency are delegated to the model. gem5 has one functional unit
class for all of these instructions, and a unit of that class names the model
and has its own instance of it.
An accepted instruction does not occupy the unit, so the model keeps whatever
internal structure it needs (several sub-units, queues, pipelines) and decides
how many instructions overlap.

**On gem5 only MinorCPU is supported.** The other CPU models do not ask the
model, and what they do instead is wrong rather than an error: see
`adapters/gem5/README.md`.

## Writing your hardware model

There are three kinds of code here. Only the first is yours.

```
+-- you write this, once per accelerator ---------------------------+
|  one model class  ->  one shared library (libyour_model.so)       |
|    owns()         which encodings are mine                        |
|    execute()      what the instruction computes   <- Spike calls  |
|    can_accept()   can the unit take it now        <- gem5 calls   |
|    issue()        it enters; when is its result ready <- gem5     |
|    commit()       it can no longer be taken back   <- gem5 calls  |
|    tick()         a cycle passed                   <- gem5 calls  |
+-------------------------------------------------------------------+
            ^ the same .so is loaded by both simulators ^
+-- provided, written once -----+  +-- provided, written once ------+
|  Spike adapter                |  |  gem5 adapter                  |
|  branch vcix of the Spike repo|  |  branch vcix of the gem5 repo  |
|  (see adapters/spike)         |  |  (see adapters/gem5)           |
+-------------------------------+  +--------------------------------+
```

### A unit that is RTL

A unit that exists as RTL needs no class: `examples/rtl/rtl_model.hpp` wraps
one Verilated module with standard ports (command, in/out beats, memory,
`busy`), and the model is a table of the encodings it owns. Spike clocks the
unit only while an instruction needs it and gives the values; gem5 clocks it
once a cycle and gives the timing. See `examples/rtl/README.md`.

### What you implement

One class derived from `vcix_accel::Model` (`include/vcix_accel.hpp`), built
into one `.so`. It needs neither simulator's source tree.

| Method | Called by | You say |
|---|---|---|
| `name()` | both | the model's name; a simulator prints it when the model cannot be configured |
| `owns()` | both | the `{match, mask}` encodings that belong to this model |
| `configure(config)` | both | nothing; read the machine's numbers by key (need not be overridden) |
| `execute(host, insn)` | Spike | what the instruction does to registers and memory, through `host` |
| `can_accept(insn, now)` | gem5 | whether the unit can take this instruction in this cycle; must not change state |
| `issue(insn, id, now)` | gem5 | cycles until the result is ready; the instruction enters the unit here, so the state changes |
| `commit(insn, id, now)` | gem5 | nothing; the instruction can no longer be squashed (need not be overridden) |
| `tick(now)` | gem5 | nothing; one cycle of the unit's own time (need not be overridden) |
| `ready(id, now)` | gem5 | whether the result of an instruction issued as `Unknown` is ready; must not change state (need not be overridden) |
| `reset()` | Spike | return to the state right after `configure` (need not be overridden) |

`insn` is the instruction bits together with the vector configuration it was
decoded under: `vl`, SEW and LMUL. Both faces receive the same thing, so
latency can depend on how much data the instruction moves.

A simulator makes one object of the class for each instance of the accelerator:
Spike one per hart, gem5 one per `MinorVcixAccelFU`. Instances share nothing,
so a model's state belongs in the object's members; a static or a global is
shared by every instance in the process.

`configure` is called once on each object, before any instruction reaches it.
`name()` and `owns()` are asked earlier, once, when the library is loaded, of
an object that is then destroyed. A model's name and the encodings it owns
therefore cannot depend on the machine description. The name, and the name in
each encoding, must stay valid as long as the library is loaded; string
literals do. What `config.get` returns is valid only until `configure` returns,
so a model copies what it wants to keep.

`config.get("key")` returns the value of a top-level key of the machine
description, as written there, or nothing if the key is absent. "As written" is
the scalar's text with no typing applied: `010` arrives as the text `010`.
"Absent" covers a key the file does not have, a key whose value is YAML null
(`k:`, `k: ~`, `k: null`) and a key whose value is not a scalar; `k: ""` is
present, with empty text. `tests/contract` holds both simulators to this rule.
The model never sees the file. Spike reads it (`--machine-config`); on gem5 the
config script reads it and passes keys and values as parameters, so gem5 parses
no file. This repository defines no configuration format of its own; the file
is the machine description a setup already has.

`config.uint("key", fallback)` reads a value as an unsigned decimal number. The
fallback is for an absent key only. A value that is present and is not such a
number -- `abc`, `-1`, `1e3`, `7.9`, `0x10`, `8 cycles`, an empty string, a
number too large for 64 bits, or one with a leading zero such as `010` -- is a
configuration error: no instance is made, and the simulator stops with the
model's name, the key and the value in its message and exit status 1. It is
never read as 0 or replaced by the fallback. A model that reads a value itself
reports a bad one the same way, by throwing `vcix_accel::ConfigError` from
`configure`. Any other `std::exception` thrown there stops the run too, with
its `what()` as the reason.
An exception from the model's constructor is caught as well: the library
writes the reason to stderr and hands over no table.

`config.hex("key", fallback)` reads a value as a hexadecimal number, for the
addresses a machine description holds: `0x` and then hex digits, within 64
bits. Anything else that is present -- `8000`, `0x`, `0x80zz`, `0X80` -- is the
same configuration error.

Then one line registers it:

```cpp
VCIX_ACCEL_REGISTER(YourModel)
```

Build the library with hidden visibility (`-fvisibility=hidden
-fvisibility-inlines-hidden`; in CMake, `CXX_VISIBILITY_PRESET hidden`). The
macro exports the one symbol a simulator looks up, `vcix_accel_model`, by
itself, so nothing else has to be marked. A symbol the library leaves visible
is not its own any more:

- Both simulators export their symbols, and theirs win. A model that defined
  `f16_to_f32` with default visibility called the simulator's softfloat
  function instead, on Spike and on gem5.
- A simulator can hold more than one model, and two libraries' visible symbols
  of the same name are merged: two models whose classes were both called
  `Accel` used to answer with one table. The table is now hidden whatever the
  flags.

The flag covers code compiled with it. A static library linked into the model
that was not built hidden needs `-Wl,-Bsymbolic` on the model's link line as
well. `tests/contract` checks that two same-named models each get their own
table and that a hidden model calls its own `f16_to_f32`.

The timing face is four events, and gem5 keeps to these rules:

- Calls come in cycle order. Within a cycle `tick` is first, then `ready`,
  then `commit`, then `can_accept` and `issue`.
- `issue` follows a `can_accept` that returned true in the same cycle. Its
  answer is when the result is ready: an instruction that reads the result
  waits that long, and the instruction itself leaves the processor then or
  when the instructions before it have left, whichever is later.
- `commit` is called once for every instruction that really ran, in the order
  they were issued. `id` names the instruction in both calls.
- `tick(now)` is called once in every cycle the processor runs, whether or not
  an instruction is in flight.
- **A squashed instruction was never issued.** gem5 issues instructions before
  it knows they will run, and takes them back after a mispredicted branch. The
  model does not see that: write `issue` as if every issued instruction runs.

The last rule is kept by copying. The wrapper copies the model before each
`issue`; when gem5 squashes, it puts the copy back and calls `tick` and
`commit` again for the cycles and the commits since. So a model must be
copyable, keep all its state in its members, and reach the same state from the
same calls. `issue` and `can_accept` are never called again. In `tick` and
`commit`, what must happen once -- a line of output -- is skipped while
`replaying()` is true.

The state may change in `issue`, in `commit` and in `tick`:

| Put an effect in | and it lands |
|---|---|
| `issue` | in the cycle the instruction enters the unit |
| `commit` | when the instruction can no longer be squashed, which the instructions before it can delay |
| `tick` | every cycle |

An accepted instruction does not hold the unit: how many overlap is the model's
decision, made in `can_accept` from its own state. A unit that takes one at a
time accepts when it has none, a pipelined unit accepts while it holds fewer
than its depth, and a unit with a queue accepts while the queue has room. gem5
bounds that decision: a unit with `vcixMaxInFlight` instructions in the
processor (8192 unless the CPU config sets it) is issued no more.

When the cycle a result is ready is not known at issue, `issue` returns
`Unknown` and the model says so later:

- From the next cycle on, after each `tick`, gem5 asks `ready(id, now)` for
  that instruction, until it answers true; then it is not asked again.
- In the cycle it answers true, an instruction that reads the result can be
  issued, and the instruction itself can leave the processor if the ones
  before it have left.
- Until then it stays in flight, and so can be squashed like any other. What
  comes after it is still issued, unless it reads the result.
- Two such instructions writing one register are waited for together: a reader
  of the later one is held until both are ready.
- A model whose `ready` never turns true stalls the program without an error.
  gem5 warns once for an instruction that has waited `vcixReadyWarnCycles`
  cycles (a parameter of the unit, 1000000 by default, 0 for never).

`tests/contract/timing.cc` has such a pop: it is taken at once and waits
inside for its data.

Spike calls none of this. When the program exits, the simulation ends,
whatever the unit is still doing. `tick` moves the model's own time only; it
asks nothing of the memory system. In a table written by hand `tick` may be
NULL, and `squash` is the model's to implement.

A queue of two commands, worked on one at a time for ten cycles each
(`tests/contract/timing.cc` has this queue, with an instruction that waits for
it to empty):

```cpp
bool can_accept(const Insn &, Cycle) const override { return queue_.size() < 2; }
Cycle issue(const Insn &, Id, Cycle) override {
  queue_.push_back(10);
  return 1;
}
void tick(Cycle) override {
  if (!queue_.empty() && --queue_.front() == 0) queue_.pop_front();
}
std::deque<Cycle> queue_;  // cycles left of each command, the one worked on first
```

The model has two sets of methods because the two simulators know different
things: Spike knows values and not time, gem5 knows time and not values. They
live in one class and share `owns()`, but they are still two descriptions, and
both are yours to write.

### What you do not implement

- **Decoding, registration, dispatch.** The adapters register your encodings
  and route matching instructions to your model.
- **Rejecting what you do not support.** `owns()` is the whole definition of
  what the model supports. An instruction it does not list never reaches the
  model and ends as an illegal instruction on both simulators.
- **Anything inside Spike or gem5.** No `encoding.h` entry, no instruction
  header, no `OpClass`, no decoder entry, no functional unit class.
- **The adapters.** They do not change when the accelerator changes.

### What you configure, per run

Not code, only where the model is:

| Simulator | How the adapter is enabled | How the model is named | How the machine description is given |
|---|---|---|---|
| Spike | the ISA suffix `_xvcixaccel` | `--extlib=<path>` | `--machine-config=<yaml>`; Spike reads it, and takes its lanes, scratchpad and VLEN from it |
| gem5 | one `MinorVcixAccelFU` in the CPU's functional unit pool | that unit's `vcixModel = "<path>"` | that unit's `vcixConfigKeys` / `vcixConfigValues`; the config script reads the same YAML |

`MinorVcixAccelFU` is defined in the gem5 branch, beside gem5's own units. A
machine's CPU config stays wherever it already lives and gains that one unit.
`examples/gem5_se.py` is only a fixture, for the example and the tests: gem5's
default pool plus the one unit. `examples/tpu/gem5/`, `examples/tpuv6e/gem5/`
and `examples/tpuv3/gem5/` are whole machines, the CPU and memory configurations
PyTorchSim measures on, each with the one unit naming its model.

## Constraints the interface is built around

- **The timing face cannot see data.** gem5 does not compute vector values for
  these instructions, so the timing face may depend only on the instruction
  (its bits, `vl`, SEW, LMUL) and on state the model tracks itself in `issue`,
  `commit` and `tick` (for example, how many commands a queue holds). Data-dependent
  latency is out of scope.
- **The two faces run in different processes.** A Spike run and a gem5 run share
  nothing at run time. Functional state and timing state live in the same model
  class but must not depend on each other.
- **The boundary is a C ABI.** Spike installs the headers of fesvr and of its
  MMIO plug-in interface, not the ones an extension needs, and a model that
  subclasses simulator types would have to be built against both simulator
  source trees. A plain C boundary keeps the model independent of both, and
  leaves room for models written in other languages. `include/vcix_accel.h` is
  the contract for such a model and for an adapter: how an instance is made
  and what it says when it cannot be, which members of the table may be NULL,
  how long each string lives, and what the machine description hands over.

## Non-goals

- Co-simulation. Spike and gem5 are not run together; they consume the same
  model separately.
- Modelling memory transfers (DMA) in gem5. Their timing is owned by the
  system-level simulator downstream.
- Any channel between core and accelerator other than the two custom opcodes.

## Layout

```
include/vcix_accel.h      the C ABI: vcix_model, vcix_insn,
                          vcix_config, vcix_host, vcix_encoding
include/vcix_accel.hpp    C++ wrapper: subclass Model, VCIX_ACCEL_REGISTER
include/vcix_uarch.hpp    units as hardware blocks (Port, Pipeline, CommandQueue,
                          Register) and a route per instruction; Machine derives
                          the Model from them
adapters/spike/           what the Spike branch does; its code is in the Spike
                          repository
adapters/gem5/            what the gem5 branch does; its code is in the gem5
                          repository
examples/print_args/      the example: a model, a program that exercises it,
                          a machine description, and a script that runs both
examples/tpu/             a model made of units: the timing of a TPU's special-function
                          unit, systolic array, cross-lane unit, multi-precision
                          array and one-cycle instructions, and what every
                          instruction computes
examples/tpu/gem5/        the machine the tpu example is measured on: PyTorchSim's
                          gem5 CPU and memory configuration, with two load/store
                          ports and two more integer units
examples/copier/          examples/rtl/copier/copier.sv as a vcix_uarch unit
examples/tpuv6e/          a TPU v6e: the tpu example's units with a matrix unit
                          as the array, its own model and library
examples/tpuv6e/gem5/     the machine the tpuv6e example is measured on: the tpu
                          example's, with the v6e's scratchpad ports and four
                          vector ALUs
examples/tpuv3/           a TPU v3: the tpu example's units with a matrix unit
                          of 128 x 128, its own model and library
examples/tpuv3/gem5/      the machine the tpuv3 example is measured on: the tpu
                          example's, with the v3's scratchpad ports, two vector
                          ALUs and 940 MHz
examples/rtl/             models whose unit is Verilated RTL behind one wrapper: a
                          systolic array checked against the tpu example's, and an
                          asynchronous memory copier; built when Verilator is found
examples/gem5_se.py       the gem5 fixture the example and the tests run on
examples/machine_description.py
                          how a gem5 config script reads the machine description
tools/timing_probe.cc     drives a model's timing face without gem5
tests/run.sh              every test below
tests/contract/           the rules of the interface: ownership, the machine
                          description, the model table, instances, tick,
                          squash, a late result, processor state
tests/pipeline/           a pipelined model overlaps instructions on gem5
tests/print_args/         the example reaches the model once per instruction
tests/tpu/                the tpu example's timing on gem5; functional/: what it
                          computes on Spike, against the Spike its units came from
tests/tpuv6e/             the tpuv6e example's timing on gem5; functional/: what
                          its matrix unit computes on Spike, against a formula
tests/tpuv3/              the same, of the tpuv3 example
tests/rtl/                the examples/rtl models on both simulators, under the
                          default and the check schedules; skipped when not built
setup/                    the pinned environment: versions.env, the scripts
                          that build it, and the image
scripts/                  build this repository; how each simulator is started
.github/workflows/        CI: publish the image, then build and test in it
```

## Example

The example is not a prescribed structure. It shows one way a model can be put
together on top of the interface; a real accelerator will pick its own
encodings, state, and timing.

`print_args` is a unit that only observes. It owns the whole custom-2 and
custom-1 spaces, computes nothing, and prints what each face is given: on the
functional face the fields and the registers the encoding names, on the timing
face the instruction and the cycle. It takes one instruction at a time and is
busy for 3 cycles after a commit. An instruction takes a configured number of
cycles, once per register of the operand group if it has a vector operand (so
LMUL shows up in the timing) and once otherwise.

`machine.yml` stands in for the machine description. `vcix.S` drives the model
with seven `sf.vc.*` forms (`x`, `i`, `vv`, `v.vv`, `v.xv`, `v.fv`, `v.ivv`),
`v.vv` again under LMUL=2, and one custom-1 instruction. It does not cover the
widening forms, the three-operand forms without `vd`, or fractional LMUL.

## Environment

Building and running needs a RISC-V toolchain, the proxy kernel, Spike, the
gem5 branch and, for the RTL models of `examples/rtl`, Verilator.
`setup/versions.env` pins all of them -- repository and commit for Spike, pk
and Verilator, the gem5 release and the Python it embeds -- and is
the only place a version is written. gem5 is not built here: a `vcix-v*` tag in
`PSAL-POSTECH/gem5` publishes `gem5.opt`, built for the packages
`setup/system.sh` installs, and the setup downloads it. There are two ways to get what it describes.

**The image.** CI publishes the environment, already built, to GHCR. Its tag is
derived from the contents of `setup/`, so a checkout names the image it needs:

```
docker run --rm -it -v "$PWD":/work -w /work "$(setup/image.sh ref)" bash
```

`setup/image.sh build` builds the same image locally. It holds the toolchain,
pk, Spike (the binary, and its source for the copy of the header the tests
compare), the gem5 binary and Verilator, but nothing of this repository: a
checkout is built inside it.

**The scripts**, on Ubuntu 22.04:

```
sudo setup/system.sh       # system packages
setup/setup.sh -j 16       # toolchain, pk, spike, gem5, verilator, then this repository
```

Everything lands under `/opt/vcix-env`; set `VCIX_ENV_ROOT` to put it elsewhere
(and keep it set when running). `setup/setup.sh spike repo` runs only those
steps.

## Build and run

```
scripts/build.sh                 # cmake + ninja into build/
tests/run.sh                     # every test; non-zero if any fails
examples/print_args/run.sh       # the example, printing what each face is given
```

The run scripts use the simulators the setup produced. To use others, pass them
-- `tests/run.sh <build-dir> <spike> <pk> <gem5.opt>` -- or set `SPIKE`, `PK`,
`GEM5`; `SPIKE_ROOT` is the source tree of that Spike, whose copy of the header
`tests/contract` compares with this one.

By hand, the build is

```
cmake -G Ninja -S . -B build
ninja -C build
```

It needs no simulator tree: the models, the probe and nothing else are built.
With Verilator on `PATH` (or `VERILATOR_ROOT` set) it also builds the RTL
models of `examples/rtl`; without it they are skipped, and so is `tests/rtl`.
`scripts/build.sh` puts the Verilator of `setup/versions.env` on `PATH` when it
is installed there.

How each simulator is given the adapter, the model and the machine description
is in the table under "What you configure, per run"; `scripts/sim.sh` is where
the run scripts keep both command lines.

## Status

- Interface: ABI version 11 (`VCIX_ACCEL_ABI_VERSION`); a simulator refuses a
  model of another version. Each simulator carries a copy of
  `include/vcix_accel.h` (Spike `riscv/vcix_accel.h`, gem5
  `src/cpu/minor/vcix_accel.h`) that must be kept identical to this one;
  `tests/contract` compares Spike's.
- Spike adapter: working; branch `vcix` of `PSAL-POSTECH/riscv-isa-sim`
  (see `adapters/spike/`).
- gem5 adapter: working, on MinorCPU only; branch `vcix` of
  `PSAL-POSTECH/gem5`, on upstream gem5 25.1.0.1 (see `adapters/gem5/`).
