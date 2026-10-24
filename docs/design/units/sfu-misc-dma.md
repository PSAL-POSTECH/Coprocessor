# Units sfu / misc / dma against `vcix_cp.hpp.design`

Files (new versions, nothing in the repo edited):
- `units/examples/tpu/sfu.hpp` (166 lines; today 127)
- `units/examples/tpu/misc.hpp` (143 lines; today 140)
- `units/examples/tpu/dma.hpp` (271 lines; today 271: `+reset()`, `-dma_index_key`)
- `units/check/{units.cc,spike.cc}`: throwaway checks; rebuilt against the revised headers in `verify/sm/` (FACT: the
  same output, byte for byte, as against the first header).

Labels: **FACT** means read in the repo (`file:line`) or measured by the throwaway check. The check compiles
`g++ -std=c++17 -Wall -Wextra -Werror` against the design header and the repo's `vcix_accel.hpp`, with no simulator,
in a gem5-style loop that polls `ready()`. **DERIVED** means worked out from the rules and not run in any simulator.
**OPINION** means a judgement.

All three files compile without warnings. Both checks pass: the timing numbers below, the Spike values (vexp, the
sfu_push/sfu_pop round trip of two registers over two lanes, vlane_idx), and the three refusal messages with exit 1
(FACT, `units/check`).

---

## 1. Sfu (`tpu::Sfu`, a `tpu::Component`)

### Blocks

| block | kind | sized by | role |
|---|---|---|---|
| `insns()` | `Queue<Insn>` | default `(2, 1)` | one a cycle, exactly (§4) |
| `entry` | `Port`, PRIMARY, "instructions", width 1 | — | spent once per instruction as it leaves `insns()`: the `SFU` unit in stats |
| `function` | `Pipeline<Insn>`, unit "instructions" | (`tpu_sfu_latency_cycles`, 1) | the seven special functions |
| `eup_input` | `Queue<Register>`, unit "registers" | `tpu_sfu_queue_registers` | the registers sfu_push queues |
| `eup` | `Pipeline<Register>`, unit "registers" | (`tpu_sfu_latency_cycles`, 1) | the EUP: one register enters a cycle |
| `eup_output` | `Queue<Register>`, unit "registers" | `tpu_sfu_queue_registers` | the results sfu_pop takes |
| `moved_` | plain | — | registers the head has moved so far |

Deleted: the REPORTING `eup` port. It only timed one register a cycle, which the pipeline's interval 1 now does. It was
never a statistic (FACT `vcix_accel.h:66-67`, "No other port is listed").

### Diagram

```
 gem5 issue / Spike execute
          |
          v
   +-------------+   head, one a cycle   +--------------------------+
   | insns (2)   |---------------------->| entry port (1/cycle)     |  PRIMARY "SFU"
   +-------------+                       +--------------------------+
          |  verf..vexp           sfu_push |                 ^ sfu_pop
          v                                v                 |
   +----------------+          +----------------+     +-----------------+
   | function       |          | eup_input (Q)  |     | eup_output (Q)  |
   | Pipeline L, 1  |          +----------------+     +-----------------+
   +----------------+                  | 1/cycle               ^ take if not full
          | take: values, complete     v                       |
          v                    +--------------------------------+
     ready(id)                 | eup  Pipeline L, 1             |
                               +--------------------------------+
```

### Per instruction

The order inside `cycle()` is:
1. take a finished `function` row;
2. move `eup` → `eup_output`, then `eup_input` → `eup`;
3. work the head of `insns()`.

Because pop runs before push, a full pipeline still takes one row in and one row out each cycle.

- **verf, vtanh, vsin, vcos, vlog, vatan, vexp**
  - Accept: always (`valid()` is flow control only). An element width the unit does not compute is a `fault()` as the
    instruction reaches the head: it computes 16 or 32 bits, 64 bits only for vlog and vatan, and anything when vl is
    0. The rule is unchanged.
  - Path: issued at t, then at `tick(t+1)` the head goes into `function` (and `entry` is spent). Its row is done at
    t+1+L. In that cycle `cycle()` takes it, writes `vd = f(vs2)` through `values()`, and calls `complete()`.
  - Result: ready at **t+L+1** (FACT check: 11 for L=10).
- **sfu_push** (sew 32 only)
  - Path: at `tick(t+1)` the head pushes into `eup_input` as many of its group's registers as fit, one row each.
    Each row's 2^x values are computed through `values()` as the row is pushed.
  - It stays the head until the last register is in. With room for all of them it completes at t+1, as today.
  - After that, `eup` takes one register a cycle and holds each for L cycles.
- **sfu_pop** (sew 32 only)
  - Path: it is accepted at once. As the head, it takes from `eup_output` every visible row it still needs, each cycle,
    and writes register j of vd's group up to vl. It completes with its last register.
  - A pop with nothing left to take anywhere (input queue, EUP and output queue empty) is a `fault()`:
    `tpu: sfu_pop: lane 0 has no pushed result left`, exit 1 on Spike as its execute returns, on gem5 as it commits.
    This text is unchanged (FACT check).
  - Change from today: the pop used to be refused until its rows were out (FACT today's `sfu.hpp:85`, receive at issue).
    It now waits in `insns()`. Decision 3 makes waiting the job of `cycle()`.
- **Stall:** the EUP uses the MinorFU rule. It holds only while its finished head cannot enter a full `eup_output`.
  Today it stopped whenever the output queue was full (`Stall::WHEN_OUTPUT_FULL`, FACT today's `sfu.hpp:54`). The
  values are the same, and the cycles change only under back-pressure. No test pins EUP back-pressure.
- **Refusals:** the head's `fault(in, message(...))`. Every refusal prints
  `tpu: <name>: an element of <sew> bits is not supported`. `tpu: vexp: an element of 64 bits is not supported` is
  grepped at `tests/tpu/functional/run.sh:156` (FACT check).
- **Trace:** for sfu_push and sfu_pop it prints `, input queue N, output queue M` from `eup_input.size()` and
  `eup_output.size()`, as today.

## 2. Misc (`tpu::Misc`, a plain `cp::Component`; the trace adds nothing, as today)

### Blocks

| block | kind | sized by |
|---|---|---|
| `insns()` | `Queue<Insn>` | `(2W, W)`, W = `tpu_misc_issue_width` |
| `issue` | `Port`, PRIMARY, "instructions" | width = W: the `MISC` unit |
| `dma_` | plain `tpu::Dma` (copyable: three `uint64_t`) | `dma_ = Dma{}; dma_.configure(config)` (a second configure without the key keeps no stale size); `reset()` clears the descriptor |

Deleted: `cp::Register<Dma>`. The `Dma configured; … dma_.configure(configured)` dance is replaced by `dma_.reset()` in
`Misc::reset()`.

### Diagram

```
 gem5 issue / Spike execute (at most W a cycle, see section 4)
          |
          v
   +----------------+  up to W a cycle   +---------------------+
   | insns (2W)     |------------------->| issue port (W)      | PRIMARY "MISC"
   +----------------+                    +---------------------+
                                                   |
                                                   v
                                values(): vlane_idx | pack | unpack_lo/hi |
                                          dma_config_desc | mvin/mvin2/mvin3 | mvout
                                          (compute, dma_index_key: nothing)
                                                   |
                                                   v
                                               complete -> ready(id)
```

### Per instruction

All eleven instructions take the same path. Issued at t, the instruction leaves `insns()` at `tick(t+1)` as long as
`issue` has room, so up to W a cycle. It spends 1 at `issue`, writes its values through `values()`, and completes. It is
ready at **t+1**, the same as today.

Values on Spike are identical, because Spike runs each instruction to its end before the next:
- `dma_config_desc`, `mvin*` and `mvout` used to write at issue (`on_issue`) and now write at t+1;
- the others used to write at done (t+1) and still do.

`MVIN ERROR: Scratchpad address overflow: 0x…`, with exit 200 (grepped at `tests/tpu/functional/run.sh:167`), is a
`fault(in, why, Dma::INVALID_SPAD_ACCESS)` raised by `Misc::cycle`: `Dma::mvin` / `Dma::mvout` return the why ("" when
the transfer stayed inside the lane's scratchpad; `MVOUT ERROR: …` the same way), `apply()` passes it up, and the
instruction is faulted instead of completed. The elements before the overflowing one are already written, as before.
Spike exits 200 as the execute returns (FACT: throwaway `verify/exitcode/faultcode_check`, `0xd0080000`, exit 200).
On gem5 the values never run, so neither does the check — as before.

A transfer (mvin, mvin2, mvin3, mvout) on a machine without `vpu_spad_size_kb_per_lane` is accepted and faulted as it
leaves `insns()`: `fault()` with `tpu: mvin: the machine description has no vpu_spad_size_kb_per_lane`, using `mvout`
for mvout and `mvin` for mvin2 and mvin3, as today (FACT check, mvin3). Misc includes `machine.hpp` for
`tpu::message`.

## 3. Dma (values only, no timing)

- Unchanged except for two things:
  - `reset()` is added (descriptor ← 0, as configure left it);
  - `dma_index_key` is deleted, because nothing calls it. Misc's `apply()` treats the instruction as writing nothing,
    and its comment keeps the reason.
- All messages, exit codes 200, key names and the default base `0xD0000000` are verbatim; the two `std::exit(200)`
  became return values (`std::string mvin/mvout`, built by `Dma::overflow`), the exit itself is `fault()`'s.

---

## 4. The one-a-cycle and W-a-cycle accept rule (adopted: `Queue::configure(capacity, per_cycle)`)

**FACT (check):** a slot popped at t is free from t+1, also for `can_accept(t+1)`, so a depth alone sustains d/2 and
bursts d. Today `entry` (width 1) and `issue` (width W) were checked at accept, so the limit was exact (FACT today's
`vcix_cp.hpp:386`).

The tests that pin the per-cycle limit:
- `tests/tpu/run.sh:47-51`: "1 cycles after the last issue", four times;
- `tests/tpu/run.sh:209,223`: "at most 2" and "at most 3";
- `tests/tpu/ports.cc:228`: `now == 2` every cycle.

The config key's meaning, "W a cycle", is a decision-8 keep.

**What the files do now:** the framework's per-cycle limit. Sfu keeps the default `(2, 1)`; Misc calls
`insns().configure(2 * width, width)`. `held_` and `valid()` are deleted from both.
- FACT (`verify/sm/units`): 5 vexp go in one a cycle; Misc takes exactly 2, 2, 2, 2, 2 (W=2) and 3, 3, 3, 1 (W=3) —
  identical to the `held_` version.

## 5. Statistics: gem5 `stats.txt` lines

Each block is its own unit (design §6); `.` becomes `_`. Today SFU and MISC had one line group each. The new lines for
the tpu model are:

```
# prefix system.cpu.executeFuncUnits.funcUnits<N>. (FACT README.md:837)
vcix.SFU.{admitted,capacity,cycles,occupancy,utilized_cycles}  # entry: instructions, capacity 1
vcix.SFU_insns.*        # instructions issued, capacity 1 (per cycle), occupancy = instructions held as cycle() began
vcix.SFU_function.*     # instructions issued, capacity 1, occupancy = instructions in its L stages
vcix.SFU_eup_input.*    # registers pushed, capacity tpu_sfu_queue_registers
vcix.SFU_eup.*          # registers into the EUP, capacity 1, occupancy = registers in its stages
vcix.SFU_eup_output.*   # registers out, capacity tpu_sfu_queue_registers
vcix.MISC.*             # issue: instructions, capacity W
vcix.MISC_insns.*       # instructions issued, capacity W (per cycle)
```

Example, tpuv6e `sfu.S` (push m8, then pop m8, L=10) (FACT check):
- `SFU.admitted 2`
- `SFU_eup.admitted 8`, occupancy 80 (8 × 10, no stall)
- `SFU_eup_input` occupancy 36
- `SFU_eup_output` occupancy 8
- `SFU_insns` occupancy 20

With L=4, `SFU_eup` occupancy is 32 and `SFU_insns` is 14.

`tests/stats/run.sh:47` `consistent` still finds `vcix.SFU.*` and `vcix.MISC.*`, each with one primary (DERIVED).

## 6. Re-baselined tests (gem5 numbers DERIVED, not run in gem5)

Assumption A (OPINION): with an `Unknown` latency, Minor issues a dependent instruction in the cycle in which its
producer's `ready()` turns true. Today's fixed-latency path did the same.

| test | old | new (derived) | why |
|---|---|---|---|
| `tests/tpu/run.sh:47-51` vexp burst issues | first, then "1 cycles after", ×4 | same | `insns()` 2 + one a cycle (§4); FACT check: issues 1,2,3,4,5 |
| `tests/tpu/run.sh:52` "commit vexp: N cycles after its issue" ×5 | 10 | **11** | pop at t+1, `function` L, take at t+L+1 (FACT check: 11 ×5) |
| `tests/tpu/run.sh:64` chain, sfu (L=10): readers / commits | 10 / 10, 11 commit lines | **11 / 11**, 11 lines | producer ready at t+11; reader per A |
| `tests/tpu/run.sh:66` chain, `tpu_sfu_latency_cycles: 4` | 4 / 4 | **5 / 5** | the same, L=4 |
| `tests/tpu/run.sh:186` chain on script_systolic.py | 10 | **11** | the same |
| `tests/tpu/run.sh:161` Misc "commit X: 1 cycles after its issue" (8 of 10) | 1 | **1** | pop and complete at t+1 (FACT check: ≤ 1) |
| `tests/tpu/run.sh:164` "issue systolic weight push: 10 cycles after the last issue…" | 10 | **11** | it reads the vexp's v4, so per A it comes 11 after (the systolic queue numbers belong to the systolic design) |
| `tests/tpu/run.sh:209` misc_per_cycle, W=2 | at most 2, all 8 | **2, 8** | §4 accept rule |
| `tests/tpu/run.sh:211` MISC capacity / admitted | 2 / 8 of 8 | **2 / 8 of 8** | `issue` is spent once per instruction |
| `tests/tpu/run.sh:223` W=3 | capacity 3, 3, 8 | **3, 3, 8** | §4 |
| `tests/tpu/run.sh:172,225` config errors (`'0' is not a pipeline depth` / `is not an issue width`) | exit 1 | **exit 1**, same text | `ConfigError` is thrown before `Pipeline::configure` |
| `tests/tpu/ports.cc:180-190` 1000 vexp, one a cycle | all accepted; SFU.entry admitted 1000, cycles 1000 | all accepted; **admitted 999**, cycles 1000 | `entry` is spent at the pop in `tick(t+1)`, and the 1000th is never ticked; `SFU_insns.admitted` = 1000 (FACT check) |
| `tests/tpu/ports.cc:216-237` misc: 10 offered | 2 a cycle × 5, admitted 10, capacity 2, committed 10 | **same** | FACT check; the driver must poll `ready()` (design §8) |
| `tests/tpu/ports.cc:108-123` list: "5 units … no other port" | 4×5 + encodings | **FAIL, not a cycle number**: it gains `SFU_insns`, `SFU_function`, `SFU_eup_input`, `SFU_eup`, `SFU_eup_output`, `MISC_insns` (plus the other components' units) | blocks are units (decision 4); rewrite the expected list |
| `tests/tpuv3/ports.cc:37`, `tests/tpuv6e/ports.cc:37` lists | 4 units | grows the same way | the same |
| `tests/tpuv3/run.sh:62`, `tests/tpuv6e/run.sh:63` `units …` | `TPUV*_MISC … TPUV*_SFU …` | the list also gets `*_MISC_insns`, `*_SFU_eup`, `*_SFU_eup_input`, `*_SFU_eup_output`, `*_SFU_function`, `*_SFU_insns` | the same |
| `tests/tpuv6e/run.sh:126-128` push m8 → pop, L=10 | "issue sfu_pop: **18** cycles after the last issue, 0 in flight, input queue 0, output queue 8"; pop commit 1 after its issue | "issue sfu_pop: **1** cycles after the last issue, **0 or 1** in flight, input queue **8**, output queue **0**"; pop commit **19** after its issue; push issue → pop ready **20** (was 19) | the pop now waits in `insns()`; the push is ready at t+1, and Minor may commit it before the pop's issue in that same cycle; the 8 registers enter one a cycle from t+2, and each is visible in the output L+1 after it entered the EUP (FACT check: push 1→2, pop 2→21) |
| the same, L=4 | "issue sfu_pop: 12 …" | "issue sfu_pop: 1 …"; pop commit **13**; push → pop ready **14** (was 13) | FACT check: pop 2→15 |
| `tests/tpu/functional/run.sh:156` refused doubles | `tpu: vexp: an element of 64 bits is not supported`, exit 1 | same | FACT check |
| `tests/tpu/functional/run.sh:167` overflow | `MVIN ERROR: …0xd0080000`, exit 200 | same (FACT check) | `fault(in, why, 200)` from `Misc::cycle`, the why returned by `Dma::mvin` inside `values()` |
| `tests/tpu/functional/run.sh:107-109` eup, special, pack, unpack, dma, cross_lane… | values | same | values code unchanged; FACT check for the eup round trip, vexp and vlane_idx |

The tpuv6e line at `:126-128` must be rewritten to pin the new issue line and the pop's commit line. The current test
pins only issue lines, and the wait has moved from the issue to the commit.

Docs to refresh (not tests):
- `README.md:859-880` (DERIVED, push m1 then pop, L=10): `issue sfu_pop: 1 cycles after the last issue, … input queue
  1, output queue 0` and `commit sfu_pop: 12 cycles after its issue`. The `sfu.hpp:54,67-92` line references go stale.
- `README.md:921` quotes `in.require(...)`.

## 7. Requests and notes for the framework / machine designs

1. **R1 (done):** `Queue::configure(capacity, per_cycle)` (§4); `held_` deleted.
2. **ENGINE.md correction (done):** `p_t = min(W, d − p_{t−1})`; depth 2W with limit W sustains W.
3. **machine.hpp (trace designer):**
   - `tpu::Component::trace_queues` must take `const vcix_insn &`, since the trace no longer has a `cp::Insn`.
     Sfu overrides it with that signature, matching the stub in `units/check/machine.hpp`.
   - Misc stays a plain `cp::Component`, so it prints nothing, as today.
4. `Queue::available(now)` is now in the header; sfu_push does not need it (one row at a time while `!full(now)`).
6. A memory trap inside Misc's DMA `values()` (an mvin or mvout to an untouched page) is restored by the framework
   (ENGINE §5), so a retried transfer is neither repeated nor falsely stuck.
5. **Spike progress rule:** no time-only wait in these units lives in a plain member. Every wait is a Pipeline, a Queue,
   or a Port, so the motion counter is enough (FACT check: the stuck pop is reported, and the EUP drain is not judged
   stuck).
