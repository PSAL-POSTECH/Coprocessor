# gem5 adapter

The gem5 side lives in the gem5 repository, not here: branch `vcix` of
`PSAL-POSTECH/gem5`, on top of upstream gem5 `stable` (25.1.0.1,
`f5c5a6e390`). It carries no accelerator. **Only MinorCPU is supported**; see
the end of this page for what the other CPU models do.

1. **Decode** — the whole custom-2 opcode decodes to one instruction class,
   `Vcix` (`arch/riscv/insts/vcix.hh`). It computes no values.
2. **Operands** — `Vcix` declares its registers from the VCIX encoding, so gem5
   tracks dependencies through it: `vm` says whether `vd` is written,
   `funct6[5:2]` whether `vs2` and `vd` are read, `funct3` whether the `rs1`
   field is a vector, integer or floating-point register or an immediate. A
   vector operand is a group of LMUL registers; the widening forms use a `vd`
   group twice as large.
3. **OpClass** — `VcixAccel`, for every such instruction.
4. **Functional unit** — `MinorVcixAccelFU`, whose `vcixModel` parameter names
   the model `.so`. `vcixConfigKeys` / `vcixConfigValues` carry the machine
   description; the config script reads the YAML, so gem5 itself parses no
   file. Each unit gets its own instance of the model, made from that
   description when the CPU is built and destroyed with it: two units naming
   one library share no state. A unit that takes `VcixAccel` instructions
   without naming a model is rejected then, and so is a library that hands
   over no table, a table of another ABI version, or one without `create`,
   `destroy` or its timing functions. A description the model refuses is
   fatal too, with the model's name and its reason.
5. **Issue** — `Execute::issue` asks the unit's model `can_accept`, in the same
   chain of conditions that keeps any instruction from issuing; a refusal
   leaves it waiting. An accepted instruction is told to the model with
   `issue`, under an id that rises with every one, and then occupies no
   functional unit: it waits in the in-order queue until
   `issue cycle + the latency issue returned`, as gem5's own unit-less
   instructions do. When several units own an instruction, it goes to the
   first in the pool that accepts it. An instruction no model owns goes
   through untold. A unit that already has `vcixMaxInFlight` instructions in
   the in-order queue (a parameter of the unit, 8192 by default, at least 1)
   is issued no more and its model is not asked: the queue is sized to hold
   that many for each unit, and a model that accepts without limit would
   otherwise outgrow it.
6. **Commit** — in order, and not before that cycle. An instruction the model
   does not own becomes an illegal instruction here, on the same path as any
   other. Otherwise the instruction's own checks run first, through the
   helpers gem5's own instructions use: VS must be on and `vtype` legal, and FS
   must be on for a form that reads `f[rs1]`. Only if they raise no fault is
   the model's `commit` called. A form that writes `vd` leaves VS dirty.
7. **Squash** — each unit keeps the list of instructions its model was told
   were issued and has not been told were committed. The model is told
   `squash`, with the oldest id of the list, and the list is emptied, in three
   places: where Execute moves to a new stream (a mispredicted branch, an
   interrupt, a fault; every instruction still in flight is then on the old
   stream and will be discarded), where an instruction of the list faults at
   commit, and, as a net under those two, where an instruction still on the
   list leaves the in-order queue uncommitted. An instruction leaves the
   in-order queue at its head, so a squash always takes the whole list. That
   holds for one thread only: a CPU of more than one thread with an
   accelerator unit is refused.
8. **Result not known at issue** — when `issue` returns
   `VCIX_LATENCY_UNKNOWN`, the instruction's destination registers are marked
   in the scoreboard as gem5 marks a load's, so a reader is not issued. After
   each cycle's ticks `Execute` asks `ready` for every such instruction still
   in flight; at the first true it frees the registers
   (`Scoreboard::markInstDestsPredictable`, added for this). Until then the
   instruction is not committed, even at the head of the in-order queue. One
   squashed before it was ready has its registers freed when it leaves the
   queue. A table without `ready` whose `issue` returns that value is fatal.
   While one waits, Execute stays awake whether or not its model has `tick`,
   and one that has waited `vcixReadyWarnCycles` cycles (a parameter of the
   unit, 1000000 by default, 0 for never) is warned about once.
9. **Tick** — `Execute::evaluate` begins by calling `tick` with the current
   cycle on every unit's model whose table has one, before that cycle's commit
   and issue, and ends by keeping the pipeline awake, so no cycle the CPU runs
   passes without it. A drain does not wait for a model. A program that exits
   ends the simulation.
10. **Statistics** — a table with `num_stats` gives its unit gem5 statistics,
   read with `read_stats` at each dump: per unit of the model, at its primary
   port, `vcix.<unit>.admitted`, `capacity`, `cycles`, `occupancy` and
   `utilized_cycles` (admitted / capacity), and `vcix.committed.<encoding>` for
   each encoding.

The model receives the instruction bits with `vl`, SEW and LMUL.

**custom-1** goes to the model the same way, through a second instruction
class. That opcode has no operand rule of its own, so the instruction is taken
to be R-type on integer registers: it reads `x[rs1]` and `x[rs2]` and writes
nothing.

The branch also carries two changes that are not about accelerators: `SpmXBar`,
a zero-latency bus for scratchpad memory, and a MinorCPU change so that the
memory issue limit holds back only memory references.

A CPU config adds one `MinorVcixAccelFU` to its functional unit pool; nothing
else about the machine changes. The model is the same `.so` the Spike adapter
loads. A config with no such unit cannot run these instructions at all, as with
any OpClass that has no functional unit.

The copy of the interface header, `src/cpu/minor/vcix_accel.h`, must stay
byte-identical to `include/vcix_accel.h` here.

A model is loaded with `dlopen(RTLD_NOW | RTLD_LOCAL)`. gem5 exports its own
symbols, so a model's visible definition of a name gem5 also defines loses to
gem5's; build the model hidden (see the top-level README). `RTLD_DEEPBIND`
would let the model's definition win, and was tried and dropped: gem5 runs on
tcmalloc, and a deep-bound model frees with glibc what libc allocated with
tcmalloc (`strdup`, `asprintf`) and crashes in `std::cout`.

## Limits

**Only MinorCPU is supported.** No other CPU model asks the model, and none of
them fails cleanly:

- **O3** does not finish. With one such instruction in the program, commit
  stops and `iew.iqFullEvents` grows every cycle until the simulation limit.
- **The simple CPUs** (atomic, timing) execute it as an instruction that does
  nothing but its state checks: no model is asked, no latency is charged, and
  an instruction no model owns passes instead of trapping.

`cpu/minor/execute.cc` includes RISC-V headers for the fault, so the branch is
not ISA-neutral.
