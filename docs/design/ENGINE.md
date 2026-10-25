# vcix_cp engine: exact semantics of the cycle()-function framework

Headers (design only, nothing in the repo edited):
- `vcix_cp.hpp.design` (about 470 lines; today's `include/vcix_cp.hpp` has 1093), namespace `cp`;
- `vcix_cp_base.hpp.design`, today's `include/vcix_accel.hpp` renamed and moved to namespace `cp` (diff:
  `vcix_cp_base.hpp.diff`): `vcix_accel::Port` becomes `cp::Stats`, plus the Spike trap restore and the layout check.
  The C ABI (`vcix_accel.h`) and the symbol `vcix_accel_model` are unchanged; `VCIX_ACCEL_REGISTER` is
  `VCIX_CP_REGISTER`. Repo citations `vcix_accel.hpp:N` below are today's file, before the rename.

Labels:
- **FACT**: read in the repo (`file:line`, branch `cp-component`), or measured by a throwaway build in
  `cpdesign/verify/` (g++ 11.5 `-std=c++17 -Wall -Wextra -Werror` against the two design headers, no simulator).
- **DERIVED**: worked out from the rules here, or produced by a throwaway in-order driver; not run on gem5 or Spike.
- **OPINION**: a judgement or a proposal.

## 1. The model

A model is a `cp::Coprocessor` whose members are `cp::Component`s. A component:
- registers its encodings (`register_insn`);
- holds blocks as members (`cp::Port`, `cp::Queue<T>`, `cp::Pipeline<T>`) and plain copyable members;
- writes `cycle()`, which is everything that moves;
- calls `complete(insn)` when an instruction is done, `fault(insn, why, exit_code = 1)` when it never can be (a wrong
  field, an order the program got wrong): a wrong program is accepted and ends in fault, never refused.

The framework routes each instruction to its owner's built-in queue `insns()`, runs every `cycle()` and then
every block's `cycle()`, answers `ready(id)` from `complete()`, reports each component's statistics, and on Spike runs the
same cycles until the instruction completes. Nothing happens at issue except the push into `insns()`.

## 2. gem5 (timing) semantics

Call order within one gem5 cycle t (FACT `include/vcix_accel.h:99`; gem5 `execute.cc:1735-1736` tick, `:1767-1787`
commit, `:1807` issue): `tick(t)`, then `ready`, `commit` and `squash`, then `can_accept` and `issue`.

| call | what the framework does |
|---|---|
| `tick(t)` | **(1)** `cycle()` of every component, in construction order. **(2)** `cycle()` of every block: a Pipeline advances one stage or holds; a Queue adds its occupancy; a Port does nothing (its availability is kept per cycle number). Blocks never touch each other here. |
| `can_accept(insn, t)` | `owner != null && owner.valid(Insn{insn}) && !owner.insns().full(t)`. `full` covers both the depth and the per-cycle limit. `valid()` is flow control only (an RTL ready), never a check of the program. Const, reads only. |
| `issue(insn, id, t)` | `owner.insns().push(Insn{insn, id}, t)`; returns `Unknown`, always. |
| `ready(id, t)` | `id ∈ done_`. `complete()` in `cycle()` makes it true in the same gem5 cycle. FACT (gem5 `execute.cc:858-859, 630-631`): the dependent instruction may then issue at t. |
| `commit(insn, id, t)` | A faulted `id` prints its `why` and exits with its `exit_code` (a squashed one never commits); else erases `id` from `done_`. |
| `squash(first, t)` | Unchanged `cp::Instance`: copy of the whole model before every issue, restore on squash, then the ticks `issued+1..t` and the commits since run again on the copy (FACT `vcix_cp_base.hpp.design:208-263`). |
| `reset()` | Every block `clear()`s (Queues incl. `insns()`, Pipelines, `Port::reset()`); then each component's `reset()`; then `done_`, the Spike clock and ids. `configure()` ends with `reset()`. |

Rules written on `Component` (OPINION, they follow from the squash copy and the Spike progress test):
- all mutable state is copied by value on assignment: no pointer or reference to model state, no pointer to state
  outside the model; components never read each other;
- `cycle()` is a function of model state and `now` only, and moves only when it has work;
- time spent waiting lives in a Pipeline (or a Queue/Port), not in a countdown on a plain member;
- side effects only inside `values()`, which never runs on gem5; printing belongs in `commit`, behind
  `replaying()` (FACT `examples/tpu/machine.hpp:79-85`).

### Offset registration, and its check

- A `Component` stores `to_cp_ = &coprocessor − this`; the Coprocessor stores each component's offset in a
  `shared_ptr` vector that copies share. A `Block` stores `to_owner_` and enlists its offset in the component's
  copy-on-write `Layout` (blocks + encodings). Implicit copies run no Block constructor, so they never re-enlist.
- **Check (new).** `cp::Instance::configure` calls `check_layout(sizeof(M))` before and after
  `model.configure` (blocks made in configure, e.g. MXU arrays in `std::optional`, are checked too).
  `cp::Coprocessor::check_layout` throws if a component or a block offset lies outside `[this, this + sizeof(M))`;
  create reports it. FACT (`verify/probe2`): blocks in a `std::vector` member →
  `create refused: vcix_cp: a block of VEC lies outside the model object` (before: the copy's `ports()` segfaulted).
- A reference member fails at compile time (FACT `vcix_cp_base.hpp.design:211` static_assert). A raw pointer to a sibling
  compiles and in a copy points at the original (review FACT P3): the rule above forbids it; no check can.
- `Pipeline::on_stage(&D::fn)` keeps a `void (Component::*)(T&, uint32_t)`; it throws unless the owner is a `D`
  (FACT `verify/probe2`: `vcix_cp: W.p: on_stage of another class`).
- `Coprocessor::host_` is the one raw pointer: set only inside Spike's `execute` and cleared by a scope guard,
  also on a throw.
- Names: component names and block names are interned in a set allocated once and never destroyed, so every
  `const char *` that a stats Port or gem5 holds stays valid, in every copy and at exit.

## 3. Per-cycle order and the visibility table

Per cycle t: `cycle()` of all components → `cycle()` of all blocks → gem5's `ready / commit / squash /
can_accept / issue` at t.

| event at t | by | first seen by |
|---|---|---|
| `issue(insn, t)` pushes into `insns()` | gem5 / Spike | `tick(t+1)` |
| `q.push(row)` at t | user | `tick(t+1)`: `length()`, `front()`, `pop()` of t do not see it |
| `q.pop()` at t | user | its slot: `full()` / `available()` from `tick(t+1)`; for `insns()`, `can_accept(t+1)` |
| per-cycle limit: `per_cycle` pushes at t | user / gem5 | `full()` for the rest of t; `available()` counts afresh from `tick(t+1)` |
| `p.push(row)` at t | user | the row is in stage k = 0 at the block's `cycle()` |
| a row reaches age `latency` at the block's `cycle()` | pipeline | `length()`, `front()`, `pop()` from `tick(t+1)` |
| `p.pop()` at t | user | `available()` at once (MinorFU stage hand-off) |
| finished head not taken in the component's `cycle()` | user | the block's `cycle()` holds the whole pipeline (MinorFU stall) |
| `port.utilize(n)` at t | user | `available()` lower for the rest of t; width again from `tick(t+1)` |
| `complete(insn)` in `cycle()` | user | `ready(id, t)` true; Spike's `execute` returns after `tick(t)` |
| plain member written in `cycle()` | user | at once, in this component: later code in `cycle()`, and `valid()` at `can_accept(t)` |
| `values(f)` in `cycle()` | user | Spike's registers and memory, at once |

**Throughput of a queue (FACT `verify/probe2`, gem5 offering 4 a cycle, the unit popping every visible row):**
accepts per cycle follow `p_t = min(W, d − p_{t−1})` for depth d and limit W. So depth 2W with limit W sustains
exactly W a cycle with no burst; a depth alone gives bursts (depth d alone: d, 0, d, 0, …).

| `insns().configure(d, W)` | accepts per cycle |
|---|---|
| default `(2, 1)` | 1 1 1 1 1 1 1 1 |
| `(4, 2)` | 2 2 2 2 2 2 2 2 |
| `(6, 3)` | 3 3 3 3 3 3 3 3 |
| `(3, 2)` | 2 1 2 1 2 1 2 1 |

Consequences (DERIVED):

| path | issue at t → ready |
|---|---|
| `cycle()` pops and completes at once | t+1 |
| pops into a Pipeline of latency L, completes on `take` | t+1+L (FACT `verify/sm/units`: L=10 → 11) |
| a group of m registers one a cycle through that Pipeline, completes as the last leaves | t+m+L (FACT `verify/renamed2/sm_mix.out`: vexp m2 → 12, m8 → 18; m1 right after a push m8 → 19) |
| … then into a user Queue, popped and completed by the next stage | t+2+L |
| n beats through a 1-wide Port | t+n |
| `insns()` at its default | one accept a cycle sustained, no burst |

## 4. The blocks: exact semantics

### `Port(c, name, unit_of_work, width, role = REPORTING)`

- Holds a `cp::Stats` member (no inheritance) with unit = the component's name: stats exactly today's.
- `available()` = width − utilized at `now` (`Stats::available(now)`); `utilize(n)` = `Stats::admit(n, now)`;
  more than `available()` aborts: `vcix_cp: port U.N admitted …` (FACT `vcix_cp_base.hpp.design:121-127`).
- No refill step and no stamp of its own: every call names its cycle (same as Queue and Pipeline).
- `width()` / `set_width()` in configure (`UNLIMITED` allowed); `occupy(n)` adds to OCCUPANCY (nothing adds
  implicitly).

### `Queue<T>(c, name, unit_of_work = "rows")`

- `configure(capacity, per_cycle = UNLIMITED)`: both at least 1 (else `std::invalid_argument`, reported by create);
  empties the queue. Stats CAPACITY = `min(capacity, per_cycle)`, what it can admit in one cycle.
- `length()` = size − pushed at now. `available()` = `min(capacity − size − freed at now, per_cycle − pushed
  at now)` (UNLIMITED-safe). `full()` = `available() == 0`. `size()` counts every row held.
- Aborts: `push` when full → `vcix_cp: C.N pushed to while full in cycle T`; `front`/`pop` with `length() == 0`
  → `vcix_cp: C.N read with no visible row in cycle T`.
- `insns()` is a `Queue<Insn>` named `insns`, unit of work `instructions`, configured `(2, 1)` by the Component
  constructor; a component overrides it in `configure`.

### `Pipeline<T>(c, name, unit_of_work = "rows")`, gem5 MinorFU (opLat = latency, issueLat = interval)

- `configure(latency, interval)`, each 1 … UINT32_MAX (else `std::invalid_argument`; FACT `verify/probe2`: interval
  UNLIMITED is refused at create, where before it wrapped and issued every cycle).
- `available() = !blocked && (!pushed || now − last_push ≥ interval)` (subtraction: no wrap).
- `push(row)` pushes `{row, age 0}`, counts 1 ADMITTED; aborts when `!available`.
- the block's `cycle()`: unblocked and not empty → every row ages; a row reaching latency gets `done_at = t`; on
  Spike `on_stage(row, age−1)`. Blocked → nothing moves. OCCUPANCY += rows held.
- `length() = blocked && head.done_at < now`; `front`/`pop` require it. Only the head can be finished.
- FACT (`verify/eng/stall`): latency 3, rows pushed at 1–4, head unpopped at 5 and 6 → popped at 4, 7,
  8, 9.

## 5. Spike semantics

```
execute(host, insn):                       # inside export_model: M saved = model; on a throw: model = saved; rethrow
  c = owner(insn), or abort "vcix_cp: instruction %08x has no unit"
  host_ = &host (cleared by a scope guard); executing_ = Insn{insn, ++spike_id_}
  until(c, can_accept(executing_, now_))     # a slot in insns(), and valid()
  c.insns().push(executing_, now_)
  until(c, ready(executing_.id, now_))       # complete() or fault() in some cycle()
  commit(insn, id, now_)                     # a faulted id: print its why, exit(exit_code)
until(c, done):
  while !done():
    before = c.motion_; tick(++now_)
    if done() or c.motion_ != before: continue
    print "vcix_cp: <encoding> waits, and nothing in its unit moves any more: on Spike, which runs each
           instruction to its end before the next, it never ends"; exit(1)
```

`fault(insn, why, exit_code = 1)` completes the instruction as failed: Spike prints `why` and exits as its `execute`
returns, gem5 when the instruction commits. The generic message above is what remains when no fault was called.

- The clock stands still between instructions, as today (FACT `vcix_cp.hpp:850-851`).
- Spike calls the non-virtual `Coprocessor::can_accept/ready/commit`, so a trace never prints on Spike (as today).

**Progress test (concrete, O(1)).** One `uint64_t motion_` per Component. Bumped by: Queue `push`/`pop`; Pipeline
`issue`/`take`, a `cycle` that ages rows, a `cycle` while the interval still runs; Port `utilize(n > 0)`;
`complete()`. A tick that leaves `done()` false and the owner's `motion_` unchanged is stuck.

Why sound (DERIVED): if nothing of component c moved in tick t+1, then at t+2 no new row of c is visible, no slot
is freed, no interval ended, and a Port's availability is the same full width. Components never read each other, so
`c.cycle()` at `tick(t+2)` sees what it saw at `tick(t+1)` (given "waiting lives in a block"). Per component, an endless mover in
another component cannot hide the stuck owner: FACT (`verify/probe2`): a component spending a port every cycle next
to a stuck one → the generic message, exit 1.

**Spike traps (new, FACT `verify/probe2`).** A `mem_write` that throws inside `values()` (pk demand paging) leaves
the model half-ticked. `export_model`'s execute now keeps `M saved = model` and restores it before rethrowing, so
Spike's re-execute runs from the state before: `effects after trap + one re-execute: 1 (1 expected)` (was 2). This
is the C++ wrapper only (`vcix_cp_base.hpp.design`), no ABI change; `rtl_model.hpp:226-238` does the same for the RTL.
OPINION: it costs one model copy per Spike instruction; a register the instruction both read and wrote before the
trap is re-read after its write, the same hazard as before.

**Spike error messages that tests grep (all still produced; FACT for those the checks ran):**

| message | grepped at | producer |
|---|---|---|
| `vcix_cp: vmatprep waits, and nothing in its unit moves any more` | `tests/tpuv6e/functional/run.sh:58-59` | framework `until`; no fault was called |
| `tpu: vexp: an element of 64 bits is not supported`; `tpu: systolic pop: 1 elements asked, 0 computed`; `tpu: systolic input push: no weight was pushed before it`; `tpu: systolic: format 3 names no element of 8 bits`; `tpu: systolic pop: a pop to bfloat16 is not modeled`; `tpu: systolic: format 1 names no element of 32 bits`; `tpu: xlu pop: 8 elements asked, 3 left`; `tpu: xlu push_reduce: fold 1 of 16-bit elements; every fold takes f32, max and min also bf16`; `tpu: msa pop: 8 elements asked, 3 computed`; `tpu: msa push: its elements are words of 32 bits, not of 16` (exit 1) | `tests/tpu/functional/run.sh:156-166` | the component's `fault()` at the head of `insns()` |
| `XLU ERROR: the pre stage walks 1 columns but lane 0 carries 0 pattern entries (SIMM5 16)` (exit 201) | `tests/tpu/functional/run.sh:162` | xlu `fault()` with exit_code 201 |
| `MVIN ERROR: Scratchpad address overflow: 0xd0080000` (exit 200) | `tests/tpu/functional/run.sh:167` | `Dma::mvin` inside `values()` |
| the 12 `tpu: vpop/vmatmul/vmatpush/vmatprep: …` texts | `tests/tpuv6e/functional/run.sh:50-69` | MXU `carried` → `fault()` |
| the 4 lanes x lanes texts | `tests/tpuv3/functional/run.sh:71-76` | MXU `carried` → `fault()` |
| `… '0' is not a pipeline depth: at least 1`, `… '0' is not an issue width: at least 1`, `tpu_mxu_queue_entries: '1' …`, `tpu_mxu_bound_to_lanes: '2' …`, `reports: … latency: '8 cycles' …` | `tests/tpu/run.sh:172,225`, `tests/tpuv3/run.sh:103,145`, `tests/tpuv6e/run.sh:163`, `tests/contract/run.sh:74` | `configure()` throwing `ConfigError` before any block's own `invalid_argument` |

## 6. Statistics

**Mechanism (no ABI change).** Every block has a `stat()`, a `cp::Stats`: a `cp::Port` holds one as a member; a
Queue or Pipeline a REPORTING one under its component's unit, so the table lists only the component's PRIMARY
`cp::Port` (FACT `vcix_cp_base.hpp:326-363`). Only the C ABI's kinds are used (FACT `vcix_accel.h:60-63`).
`Instance::describe/read_stats` unchanged; gem5 turns `.` into `_` (FACT gem5 `vcix_accel_model.cc:22-33,117-151`).

| unit | ADMITTED | CAPACITY | CYCLES | OCCUPANCY |
|---|---|---|---|---|
| `<C>` (its one PRIMARY `cp::Port`) | `utilize`s | width | ticks + reopened | `occupy`s |

As gem5's `stats.txt` prints it (prefix FACT `README.md:837`; values FACT `verify/cs/dump`, markers window 1):

```
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.admitted                12   # rows admitted at port input
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.capacity                 1
system.cpu.executeFuncUnits.funcUnits8.vcix.SYSTOLIC.occupancy               84
system.cpu.executeFuncUnits.funcUnits8.vcix.committed.systolic_pop            3
```

What `tests/stats/run.sh` pins (DERIVED): `beyond_units` stays 0; `consistent` finds `vcix.{SFU,MISC,SYSTOLIC,XLU,MSA}.*`,
each component keeps one PRIMARY `cp::Port`; `counts` keep their values. Two units whose names collide once `_`
replaces other characters are refused at create (FACT `vcix_cp_base.hpp:326-363`).

## 7. Trace hook for `tpu::Coprocessor`

- `tpu::Coprocessor` keeps overriding `issue` and `commit` (FACT `examples/tpu/machine.hpp:32-38`); `using Insn =
  vcix_insn` (the base layer's alias is gone; `cp::Insn` is the engine's id-carrying instruction); the owner is
  `dynamic_cast<const tpu::Component *>(owner(insn))`; `trace_queues(const vcix_insn &)`; the one-pipeline fallback
  (`machine.hpp:51-60`) is deleted.
- The queue numbers on an issue line are the state after `tick(t)`, before this instruction's push.
- The commit line and `replaying()` are unchanged.

## 8. Re-baselining (framework-level pins; per-unit tables are in `units/*.md` and DESIGN.md §5)

| test | old | new | why |
|---|---|---|---|
| `tests/contract/route_once.cc:59,63`: issue at 1, four beats one a cycle | done at 6 (FACT `vcix_cp.hpp:444,956`) | **5** for the copy, the assigned model and the original (FACT `verify/eng/check`) | no arrival stamp; rewrite "handler ran once" as "a copy's cycle() ticks its own blocks"; verdict text `tests/contract/run.sh:103` |
| `tests/tpu/run.sh:159-161` Misc "1 cycles after its issue" | 1 | **1** | pop and complete at t+1 |
| issue → Pipeline of L → ready (e.g. `tests/tpu/run.sh:52-54`, L=10) | L | **L+1** (FACT `verify/sm/units`: 11) | the pop happens in `tick(t+1)` |

Driver change (FACT `tests/tpu/ports.cc:63`, and tpuv3/tpuv6e copies): they commit at `now + latency`, which wraps
with `Unknown`; they must poll `ready()`, as `tests/rtl/stall.cc` does.

## 9. Today's `include/vcix_cp.hpp`: reused vs deleted

| today | fate |
|---|---|
| Offset registration, `components_` shared_ptr, `Layout` COW (`:46-104, 1079-1089`) | **reused**; Layout holds encodings + blocks; plus `check_layout` |
| `cp::Port` as a `vcix_accel::Port` under the component's name (`:106-113`) | **reused**, now a `Block` holding a `cp::Stats` member, with `available()/utilize(n)/occupy(n)` |
| `ready_` list, `commit` erase, `ready` lookup (`:868-872`) | **reused** as `done_` |
| stuck message + `exit(1)` (`:1034-1036`), "has no unit" (`:855`) | **reused** verbatim |
| stage function, k = 0 on the entering cycle, Spike only (`:434-438`) | **reused** in `Pipeline::cycle` |
| `Slots` (`:149-348`), `AnyPipeline` (`:350-417`), `Stall` | **deleted**: `deque<T>` + two stamps; MinorFU stall |
| `CmdQueue`, `Free`, `Done`, `arrival_cycles`, waiters (`:443-497`) | **deleted**: `insns()` + `cycle()` + `complete()` |
| `Instr`, `handle()`, `Handler`, `Step`, `Move`, `Hooks`, `MAX_STEPS`, `Through*`, `Output`, `require/use/send/receive/transfer/on_*` (`:499-833`) | **deleted** |
| `Register<S>`, `AnyRegister` (`:115-147`) | **deleted**: plain members + `Component::reset()` |
| `motion()` vectors, `arriving()`, `drained_receive()`, `queued()`, `Beats` (`:912-917, 1038-1077`) | **deleted**: one `motion_` per component |
| `with_owner`, `each_pipeline` (`:897-908`) | **replaced** by `owner(insn)` |

Copy cost (OPINION): every gem5 issue and every Spike instruction copies the model, O(rows held). Keep payloads empty
on gem5 (fill only in `values()`); `shared_ptr<const …>` for big read-only state (systolic weights already do).

## 10. Additions beyond decision 2's list, and the decisions taken

Additions (each needed by something FACT in the repo or a review finding):
- `cp::Insn = vcix_insn + id`; virtual `reset()`; `fault(insn, why, exit_code = 1)`; protected `owner(insn)`.
  `why_stuck()` and `executing()` are gone: a wrong program is faulted at the head of `insns()`, not explained at
  the stall.
- `Queue::configure(capacity, per_cycle)`, `available()`, `capacity()`, `size()`; `Pipeline::size()`;
  `Port::occupy/width/set_width`; optional `unit_of_work` on Queue and Pipeline.
- `vcix_cp_base.hpp`: namespace `cp`, `Stats` in place of `vcix_accel::Port`, `Model::check_layout(size_t)`
  (virtual, default empty) and the trap restore in `export_model`.
- SFU: the special functions and `sfu_push` share the one `eup` Pipeline (no separate function pipeline): a special
  function takes one EUP slot per register of its group and completes when the last leaves; queued push registers
  enter first (FACT `verify/renamed2/sm_mix.out`, L=10: vexp m1 unchanged at 11, m2 → 12, m8 → 18, m1 right after a
  push m8 → 19; no pinned gem5 timing test uses more than m1).

Decisions 1-5 are final (DESIGN.md §8):
1. No `Pipeline::hold()`: the MinorFU rule stands; systolic.sv's bubble freeze is a known, untested difference
   (`tests/rtl/run.sh:66-69` passes without it, FACT `verify`).
2. A pop whose rows are still to come is accepted and waits at the head of `insns()`; the `ports.cc` drivers are to
   offer instructions in program order.
3. The Spike progress rule "waiting lives in a block" stays; no `moved()` hatch.
4. The two base-layer changes (trap restore, `check_layout`) are approved.
5. Faults as in §5: wrong programs are accepted and end in `fault(insn, why, exit_code = 1)`; the generic message
   remains when no fault was called.
