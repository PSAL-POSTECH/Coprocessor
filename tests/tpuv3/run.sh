#!/usr/bin/env bash
# The tpuv3 example: its timing on gem5, then what it computes on Spike (functional/run.sh). Usage: tests/tpuv3/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
failed=0
declare -A rc
MODEL="$BUILD/libtpuv3.so"
MACHINE="$REPO/examples/tpuv3/gem5/script_systolic.py"

report() {
  if [ "$1" = "$2" ]; then echo "PASS  $3 ($1)"; else echo "FAIL  $3 ($1, expected $2)"; failed=1; fi
}

# on_gem5 <run> <program> <machine.yml> [model]
on_gem5() {
  gem5_run "$BUILD/m5out-tpuv3-$1" "${4:-$MODEL}" "$3" "$BUILD/tpuv3-$2" --max-ticks 100000000 > "$BUILD/tpuv3-$1.gem5.log" 2>&1
  rc[$1]=$?
}

said() {
  local line count=0
  for line in "${@:2}"; do
    [ "$(grep -Fxc "[tpu] $line" "$BUILD/tpuv3-$1.gem5.log")" = 1 ] && count=$((count + 1))
  done
  echo $count
}

lines() { grep -c "^\[tpu\] $2 " "$BUILD/tpuv3-$1.gem5.log"; }

# unit_stat <run> <unit> <stat>: the value of vcix.<unit>.<stat> in the run's one dump
unit_stat() { awk -v want=".vcix.$2.$3" 'substr($1, length($1) - length(want) + 1) == want { print $2; exit }' "$BUILD/m5out-tpuv3-$1/stats.txt"; }

# units <run>: the units in the run's dump, as gem5 sorts them
units() { sed -n 's/^.*\.vcix\.\([^.]*\)\.admitted .*$/\1/p' "$BUILD/m5out-tpuv3-$1/stats.txt" | tr '\n' ' ' | sed 's/ $//'; }

# issues <run>: of each issue, the cycles after the last issue and the input queue
issues() {
  sed -n 's/^\[tpu\] issue \([a-z]*\): \([0-9]*\) cycles after .*input queue \([0-9]*\), output queue \([0-9]*\)$/\1:\2:\3\/\4/p' \
    "$BUILD/tpuv3-$1.gem5.log" | tr '\n' ' ' | sed 's/ $//'
}

# commits <run>: of each commit, the cycles after its own issue; an instruction waits at the head of its unit, so this
# is where its latency shows
commits() {
  sed -n 's/^\[tpu\] commit \([a-z]*\): \([0-9]*\) cycles after its issue$/\1:\2/p' "$BUILD/tpuv3-$1.gem5.log" | tr '\n' ' ' | sed 's/ $//'
}

for program in mxu mxu_rate; do
  rv_program "$HERE/$program.S" "$BUILD/tpuv3-$program" || { echo "FAIL  $program.S does not build"; exit 2; }
done
rv_program "$HERE/../tpuv6e/mxu.S" "$BUILD/tpuv3-tpuv6e_mxu" || { echo "FAIL  tpuv6e/mxu.S does not build"; exit 2; }
for program in scratchpad ../tpuv6e/valu ../tpu/loads ../tpu/addresses; do
  rv_program "$HERE/$program.S" "$BUILD/tpuv3-${program##*/}" || { echo "FAIL  $program.S does not build"; exit 2; }
done

echo "-- the units' statistics, with no simulator"
"$BUILD/tpuv3_ports" "$MODEL" > "$BUILD/tpuv3-ports.log" 2>&1
rc[ports]=$?
report "exit ${rc[ports]}, $(grep -c '^PASS' "$BUILD/tpuv3-ports.log") passed" "exit 0, 1 passed" \
  "tpuv3_ports: the model's 20 units, one per component and per block, under the names it gives them, in its order (tpuv3-ports.log)"

echo "-- matrix unit, 4 lanes: an array of 4 x 4, 7 slots, a row a cycle, one MXU slot, queues of 8"
on_gem5 mxu mxu "$HERE/mxu.yml"
report "exit ${rc[mxu]}, $(issues mxu); commits $(commits mxu)" "exit 0, vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0; commits vmatpush:1 vmatmul:2 vpop:13 vpop:17" \
  "one MXU slot a cycle, no vmatprep: the vmatmul issues beside the vmatpush and is carried out the cycle after it; both pops are accepted at once and wait at the head: eight bfloat16 rows enter one a cycle, the first pop commits 13 cycles after its issue (4 rows + 7 slots + a cycle at the input and the output queue), the next 4 later"
report "units $(units mxu); TPUV3_MXU admitted $(unit_stat mxu TPUV3_MXU admitted), capacity $(unit_stat mxu TPUV3_MXU capacity), occupancy $(unit_stat mxu TPUV3_MXU occupancy); insns $(unit_stat mxu TPUV3_MXU_insns admitted), $(unit_stat mxu TPUV3_MXU_insns capacity), $(unit_stat mxu TPUV3_MXU_insns occupancy); array0_line $(unit_stat mxu TPUV3_MXU_array0_line admitted), $(unit_stat mxu TPUV3_MXU_array0_line capacity), $(unit_stat mxu TPUV3_MXU_array0_line occupancy)" \
  "units TPUV3_MISC TPUV3_MISC_insns TPUV3_MXU TPUV3_MXU_array0_input TPUV3_MXU_array0_line TPUV3_MXU_array0_output TPUV3_MXU_insns TPUV3_SFU TPUV3_SFU_eup TPUV3_SFU_eup_input TPUV3_SFU_eup_output TPUV3_SFU_insns TPUV3_XLU TPUV3_XLU_buffer TPUV3_XLU_insns TPUV3_XLU_line TPUV3_XLU_moves TPUV3_XLU_out TPUV3_XLU_queue TPUV3_XLU_stages; TPUV3_MXU admitted 8, capacity 1, occupancy 56; insns 4, 2, 33; array0_line 8, 1, 56" \
  "gem5 reports the model's 20 units, each component and each of its blocks as <component>_<block>: the eight rows in at the matrix unit one a cycle, 7 cycles each in the line; its insns() took the 4 instructions, 2 a cycle at most"

on_gem5 mxu_rate mxu_rate "$HERE/mxu.yml"
report "exit ${rc[mxu_rate]}, $(issues mxu_rate); commits $(commits mxu_rate)" \
  "exit 0, vmatmul:0:0/0 vmatmul:1:0/0 vpop:0:0/0 vpop:1:4/0 vpop:1:3/0; commits vmatpush:1 vmatmul:2 vmatmul:6 vpop:13 vpop:17 vpop:20" \
  "queues of 8: the second vmatmul, eight bfloat16 rows, is accepted a cycle after the first and waits at the head until the four singles have left the queue, committing 6 cycles after its issue; the three pops are accepted one a cycle and commit 13, 17 and 20 after theirs"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 16\n' > "$BUILD/tpuv3-mxu-16.yml"
on_gem5 mxu_rate-16 mxu_rate "$BUILD/tpuv3-mxu-16.yml"
report "exit ${rc[mxu_rate-16]}, $(issues mxu_rate-16 | cut -d' ' -f1-2); commits $(commits mxu_rate-16 | cut -d' ' -f1-3)" \
  "exit 0, vmatmul:0:0/0 vmatmul:1:0/0; commits vmatpush:1 vmatmul:2 vmatmul:2" \
  "with queues of 16, the second vmatmul waits only for the one MXU slot: it commits 2 cycles after its issue, as the first does, a cycle behind it"

printf 'tpu_trace: 1\n' > "$BUILD/tpuv3-128.yml"
on_gem5 lanes-128 mxu "$BUILD/tpuv3-128.yml"
report "exit ${rc[lanes-128]}, $(issues lanes-128); commits $(commits lanes-128)" "exit 0, vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0; commits vmatpush:1 vmatmul:2 vpop:261 vpop:265" \
  "128 lanes, the default: an array of 128 x 128, so 255 slots; the pops are accepted at once, and the first commits 261 cycles after its issue (4 rows + 255 slots + a cycle at each queue), the next 4 later"

echo "-- tpu_mxu_bound_to_lanes: 1, an array of lanes x lanes whatever the model's"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 8\ntpu_mxu_bound_to_lanes: 1\n' > "$BUILD/tpuv3-bound-4.yml"
on_gem5 tpuv6e-bound-4 mxu "$BUILD/tpuv3-bound-4.yml" "$BUILD/libtpuv6e.so"
report "exit ${rc[tpuv6e-bound-4]}, $(issues tpuv6e-bound-4); commits $(commits tpuv6e-bound-4); TPUV6E_MXU capacity $(unit_stat tpuv6e-bound-4 TPUV6E_MXU capacity), occupancy $(unit_stat tpuv6e-bound-4 TPUV6E_MXU occupancy)" \
  "exit 0, vmatmul:0:0/0 vpop:1:8/0 vpop:0:8/0; commits vmatpush:1 vmatmul:1 vpop:10 vpop:12; TPUV6E_MXU capacity 2, occupancy 56" \
  "libtpuv6e.so on 4 lanes, bound: an array of 4 x 4, 7 slots, a word a cycle in two slots: the vmatmul is carried out beside the vmatpush, the pops are accepted with its 8 rows already queued, and the first commits 10 cycles after its issue (2 words + 7 slots + a cycle at each queue), the next 2 later"
printf 'tpu_trace: 1\ntpu_mxu_bound_to_lanes: 1\n' > "$BUILD/tpuv3-bound-128.yml"
on_gem5 tpuv6e-bound-128 mxu "$BUILD/tpuv3-bound-128.yml" "$BUILD/libtpuv6e.so"
report "exit ${rc[tpuv6e-bound-128]}, $(issues tpuv6e-bound-128); commits $(commits tpuv6e-bound-128)" "exit 0, vmatmul:0:0/0 vpop:1:8/0 vpop:0:8/0; commits vmatpush:1 vmatmul:1 vpop:258 vpop:260" \
  "libtpuv6e.so on 128 lanes, bound: an array of 128 x 128, 255 slots: the first pop commits 258 cycles after its issue (2 words + 255 slots + a cycle at each queue), the next 2 later"
printf 'tpu_trace: 1\nvpu_num_lanes: 8\ntpu_mxu_queue_entries: 8\n' > "$BUILD/tpuv3-8.yml"
{ cat "$BUILD/tpuv3-8.yml"; echo "tpu_mxu_bound_to_lanes: 1"; } > "$BUILD/tpuv3-bound-8.yml"
on_gem5 lanes-8 mxu "$BUILD/tpuv3-8.yml"
on_gem5 bound-8 mxu "$BUILD/tpuv3-bound-8.yml"
same=$(cmp -s <(grep '^\[tpu\]' "$BUILD/tpuv3-lanes-8.gem5.log") <(grep '^\[tpu\]' "$BUILD/tpuv3-bound-8.gem5.log") && echo same || echo different)
report "exit ${rc[lanes-8]} and ${rc[bound-8]}, $(issues lanes-8); commits $(commits lanes-8), bound: trace $same" \
  "exit 0 and 0, vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0; commits vmatpush:1 vmatmul:2 vpop:21 vpop:25, bound: trace same" \
  "libtpuv3.so on 8 lanes: an array of 8 x 8, 15 slots: the first pop commits 21 cycles after its issue (4 rows + 15 slots + a cycle at each queue), the next 4 later; bound, the same"
{ cat "$HERE/../tpuv6e/mxu.yml"; echo "tpu_mxu_bound_to_lanes: 0"; } > "$BUILD/tpuv3-tpuv6e-unbound.yml"
on_gem5 tpuv6e-mxu tpuv6e_mxu "$HERE/../tpuv6e/mxu.yml" "$BUILD/libtpuv6e.so"
on_gem5 tpuv6e-unbound tpuv6e_mxu "$BUILD/tpuv3-tpuv6e-unbound.yml" "$BUILD/libtpuv6e.so"
same=$(cmp -s <(grep '^\[tpu\]' "$BUILD/tpuv3-tpuv6e-mxu.gem5.log") <(grep '^\[tpu\]' "$BUILD/tpuv3-tpuv6e-unbound.gem5.log") && echo same || echo different)
report "exit ${rc[tpuv6e-mxu]} and ${rc[tpuv6e-unbound]}, $(lines tpuv6e-unbound issue) issues, trace $same" "exit 0 and 0, 8 issues, trace same" \
  "libtpuv6e.so, tests/tpuv6e/mxu.S with tpu_mxu_bound_to_lanes: 0: the model's own array of 8 x 8, as without the key"
printf 'tpu_mxu_bound_to_lanes: 2\n' > "$BUILD/tpuv3-bound-2.yml"
on_gem5 bound-2 mxu "$BUILD/tpuv3-bound-2.yml" "$BUILD/libtpuv6e.so"
why=$(grep -Fc "tpuv6e: machine description: tpu_mxu_bound_to_lanes: '2' is not 0 (the model's array) or 1 (an array of lanes x lanes)" \
  "$BUILD/tpuv3-bound-2.gem5.log")
report "exit ${rc[bound-2]}, reason given $why" "exit 1, reason given 1" "gem5 stops on tpu_mxu_bound_to_lanes: 2"

echo "-- tpu_mxu_registers, tpu_mxu_issue_slots, tpu_mxu_word_a_cycle: a machine's shape over the model's own"
{ cat "$HERE/mxu.yml"; printf 'tpu_mxu_registers: 1\ntpu_mxu_issue_slots: 1\ntpu_mxu_word_a_cycle: 0\n'; } > "$BUILD/tpuv3-shape.yml"
on_gem5 tpuv6e-shape-mxu mxu "$BUILD/tpuv3-shape.yml" "$BUILD/libtpuv6e.so"
on_gem5 tpuv6e-shape-rate mxu_rate "$BUILD/tpuv3-shape.yml" "$BUILD/libtpuv6e.so"
same=$(cmp -s <(grep '^\[tpu\]' "$BUILD/tpuv3-mxu.gem5.log") <(grep '^\[tpu\]' "$BUILD/tpuv3-tpuv6e-shape-mxu.gem5.log") &&
  cmp -s <(grep '^\[tpu\]' "$BUILD/tpuv3-mxu_rate.gem5.log") <(grep '^\[tpu\]' "$BUILD/tpuv3-tpuv6e-shape-rate.gem5.log") && echo same || echo different)
report "exit ${rc[tpuv6e-shape-mxu]} and ${rc[tpuv6e-shape-rate]}, traces $same" "exit 0 and 0, traces same" \
  "libtpuv6e.so with the v3's shape as keys (registers 1, issue slots 1, a row a cycle): mxu.S and mxu_rate.S as libtpuv3.so runs them"
for bad in 'tpu_mxu_registers: 3' 'tpu_mxu_word_a_cycle: 2' 'tpu_mxu_issue_slots: 0' $'tpu_mxu_bound_to_lanes: 1\ntpu_mxu_registers: 2'; do
  printf '%s\n' "$bad" > "$BUILD/tpuv3-shape-bad.yml"
  on_gem5 shape-bad mxu "$BUILD/tpuv3-shape-bad.yml" "$BUILD/libtpuv6e.so"
  key=$(printf '%s' "$bad" | tail -1 | cut -d: -f1)
  why=$(grep -Fc "tpuv6e: machine description: $key:" "$BUILD/tpuv3-shape-bad.gem5.log")
  report "exit ${rc[shape-bad]}, reason given $why" "exit 1, reason given 1" "gem5 stops on $(printf '%s' "$bad" | tr '\n' ' ')"
done

echo "-- the machine PyTorchSim measures on (examples/tpuv3/gem5)"

# on_machine <program> [machine.yml]: runs it on the v3 machine
on_machine() {
  local description=()
  [ -n "${2:-}" ] && description=(--machine-config "$2")
  timeout 300 "$GEM5" -d "$BUILD/m5out-tpuv3-machine-$1" "$MACHINE" -c "$BUILD/tpuv3-$1" --model "$MODEL" "${description[@]}" \
    > "$BUILD/tpuv3-machine-$1.gem5.log" 2>&1
  rc[machine-$1]=$?
}

# config <run> <section> <key>, window <run> <dump> <name>: scripts/sim.sh's, of the run on the v3 machine
config() { gem5_config "$BUILD/m5out-tpuv3-machine-$1" "${@:2}"; }
window() { gem5_window "$BUILD/m5out-tpuv3-machine-$1" "${@:2}"; }

for program in scratchpad valu loads addresses; do on_machine $program; done
on_machine mxu "$HERE/mxu.yml"
report "exit ${rc[machine-scratchpad]}, ports $(config scratchpad system.spad load_ports) $(config scratchpad system.spad store_ports) $(config scratchpad system.spad access_ports), latency $(config scratchpad system.spad latency); requests $(config scratchpad system.cpu executeLSQRequestsQueueSize); integer units $(grep -c '^opClass=IntAlu$' "$BUILD/m5out-tpuv3-machine-scratchpad/config.ini"), VALUs $(grep -c '^unit=Vpu$' "$BUILD/m5out-tpuv3-machine-scratchpad/config.ini"); clock $(config scratchpad system.clk_domain clock) ps" \
  "exit 0, ports 1 1 2, latency 1; requests 2; integer units 6, VALUs 2; clock 1064 ps" \
  "the v3 machine: a scratchpad of 1 load, 1 store and 2 access ports a cycle, two LSQ requests, six integer units, two VALUs, and 940 MHz, a cycle of 1064 ticks of a picosecond"
report "at most $(window scratchpad 1 system.spad.loadsPerCycle::max_value) loads, $(window scratchpad 1 system.spad.storesPerCycle::max_value) stores, $(window scratchpad 1 system.spad.accessesPerCycle::max_value) accesses a cycle" \
  "at most 1 loads, 1 stores, 2 accesses a cycle" \
  "scratchpad.S's independent loads and stores, one and one in turn: the scratchpad takes 1 load, 1 store and 2 accesses in a cycle"
report "$(window loads 2 system.cpu.numCycles) and $(window loads 4 system.cpu.numCycles), $(window loads 2 system.spad.loads) and $(window loads 4 system.spad.loads) reads, at most $(window loads 2 system.spad.loadsPerCycle::max_value) a cycle" \
  "86 and 83, 68 and 64 reads, at most 1 a cycle" \
  "loads.S's 64 loads from lines 128 and 64 bytes apart: no banks, so only the 4 instruction lines the 128-apart window still fetches, on the one load port, set them apart"
report "$(window addresses 2 system.cpu.numCycles)" "104" \
  "addresses.S's 64 vector loads, each with the addi that steps its pointer, take 104 cycles"
report "busy $(window valu 1 system.cpu.units.Vpu.admitted) of $(window valu 1 system.cpu.numCycles) cycles" "busy 32 of 51 cycles" \
  "tests/tpuv6e/valu.S's 64 independent vfadds and vfmuls keep the VALUs busy 32 cycles: 2 a cycle, whatever the mix"
report "exit ${rc[machine-mxu]}, $(issues machine-mxu); commits $(commits machine-mxu)" "exit 0, vmatmul:0:0/0 vpop:1:0/0 vpop:0:0/0; commits vmatpush:1 vmatmul:2 vpop:13 vpop:17" \
  "mxu.S on the machine as on gem5_se.py: the matrix unit's timing is the model's, not the machine's; its insns() takes 2 a cycle, so the machine's wider issue changes nothing"
report "$(grep -c 'generic machine' "$BUILD/tpuv3-machine-scratchpad.gem5.log") warnings" "0 warnings" \
  "the v3 machine is a model's own: the tpu machine's warning that it is the generic one is not printed"

echo "-- the model"
printf 'tpu_mxu_queue_entries: 1\n' > "$BUILD/tpuv3-queue-1.yml"
on_gem5 queue-1 mxu "$BUILD/tpuv3-queue-1.yml"
why=$(grep -Fc "tpuv3: machine description: tpu_mxu_queue_entries: '1' is not a queue size: a word of two rows needs 2" \
  "$BUILD/tpuv3-queue-1.gem5.log")
report "exit ${rc[queue-1]}, reason given $why" "exit 1, reason given 1" \
  "gem5 stops on tpu_mxu_queue_entries: 1, where a vmatmul of a word of two bfloat16 rows would never be accepted"

echo "-- what it computes"
"$HERE/functional/run.sh" "$BUILD" "$SPIKE" "$PK" || failed=1

exit $failed
