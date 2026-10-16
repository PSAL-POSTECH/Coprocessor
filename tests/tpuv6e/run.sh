#!/usr/bin/env bash
# The tpuv6e example: its timing on gem5, then what it computes on Spike (functional/run.sh). Usage: tests/tpuv6e/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
failed=0
declare -A rc
MODEL="$BUILD/libtpuv6e.so"
MACHINE="$REPO/examples/tpuv6e/gem5/script_systolic.py"

report() {
  if [ "$1" = "$2" ]; then echo "PASS  $3 ($1)"; else echo "FAIL  $3 ($1, expected $2)"; failed=1; fi
}

# on_gem5 <run> <program> <machine.yml>
on_gem5() {
  gem5_run "$BUILD/m5out-tpuv6e-$1" "$MODEL" "$3" "$BUILD/tpuv6e-$2" --max-ticks 100000000 > "$BUILD/tpuv6e-$1.gem5.log" 2>&1
  rc[$1]=$?
}

said() {
  local line count=0
  for line in "${@:2}"; do
    [ "$(grep -Fxc "[tpu] $line" "$BUILD/tpuv6e-$1.gem5.log")" = 1 ] && count=$((count + 1))
  done
  echo $count
}

lines() { grep -c "^\[tpu\] $2 " "$BUILD/tpuv6e-$1.gem5.log"; }

# unit_stat <run> <unit> <stat>: the value of vcix.<unit>.<stat> in the run's one dump
unit_stat() { awk -v want=".vcix.$2.$3" 'substr($1, length($1) - length(want) + 1) == want { print $2; exit }' "$BUILD/m5out-tpuv6e-$1/stats.txt"; }

# units <run>: the units in the run's dump, as gem5 sorts them
units() { sed -n 's/^.*\.vcix\.\([^.]*\)\.admitted .*$/\1/p' "$BUILD/m5out-tpuv6e-$1/stats.txt" | tr '\n' ' ' | sed 's/ $//'; }

for program in mxu mxu_rate mxu_arrays mxu_slots mxu_pops scratchpad valu ../tpu/loads ../tpu/addresses; do
  rv_program "$HERE/$program.S" "$BUILD/tpuv6e-${program##*/}" || { echo "FAIL  $program.S does not build"; exit 2; }
done

echo "-- the units' statistics, with no simulator"
"$BUILD/tpuv6e_ports" "$MODEL" > "$BUILD/tpuv6e-ports.log" 2>&1
rc[ports]=$?
report "exit ${rc[ports]}, $(grep -c '^PASS' "$BUILD/tpuv6e-ports.log") passed" "exit 0, 1 passed" \
  "tpuv6e_ports: the model's units under the names it gives them, in its order (tpuv6e-ports.log)"

echo "-- matrix unit, 4 lanes: an array of 8 x 8, 15 slots, a word a cycle, queues of 8"
on_gem5 mxu mxu "$HERE/mxu.yml"

pairs=$(said mxu "issue vmatprep: the first, input queue 0, output queues 0 and 0" \
  "issue vmatpush: 1 cycles after the last issue, 0 in flight, input queue 0, output queues 0 and 0" \
  "issue vmatprep: 0 cycles after the last issue, 1 in flight, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 1 cycles after the last issue, 0 in flight, input queue 0, output queues 0 and 0")
report "exit ${rc[mxu]}, lines $pairs" "exit 0, lines 4" \
  "a vmatprep and the half that follows it are issued a cycle apart, one slot each operand's, so the inputs' vmatprep goes with the weights' vmatpush, and neither weights nor a vmatprep enter the array"
pops=$(said mxu "issue vpop: 17 cycles after the last issue, 0 in flight, input queue 0, output queues 4 and 4" \
  "issue vpop: 1 cycles after the last issue, 0 in flight, input queue 0, output queues 2 and 6" \
  "issue vpop: 1 cycles after the last issue, 0 in flight, input queue 0, output queues 4 and 4" \
  "issue vpop: 1 cycles after the last issue, 0 in flight, input queue 0, output queues 0 and 4")
report "lines $pops of $(lines mxu issue)" "lines 4 of 8" \
  "eight rows of bfloat16 enter two a cycle: the pop of four is issued 2 + 15 cycles after the vmatmul, and each half is popped apart"
report "units $(units mxu); TPUV6E_MXU admitted $(unit_stat mxu TPUV6E_MXU admitted), capacity $(unit_stat mxu TPUV6E_MXU capacity), occupancy $(unit_stat mxu TPUV6E_MXU occupancy)" \
  "units TPUV6E_MISC TPUV6E_MXU TPUV6E_SFU TPUV6E_XLU; TPUV6E_MXU admitted 8, capacity 2, occupancy 120" \
  "gem5 reports the model's four units under the names it gives them, the eight rows in at the matrix unit two a cycle"

printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 16\n' > "$BUILD/tpuv6e-mxu-16.yml"
on_gem5 mxu_rate mxu_rate "$BUILD/tpuv6e-mxu-16.yml"
queues=$(grep -o "input queue [0-9]*" "$BUILD/tpuv6e-mxu_rate.gem5.log" | cut -d' ' -f3 | tr '\n' ' ')
report "exit ${rc[mxu_rate]}, input queue at each issue ${queues% }" "exit 0, input queue at each issue 0 0 0 0 3 2 10 9 8 6 0 0 0 0 0 0" \
  "with queues of 16, a word a cycle enters: four of singles one row each, then four of bfloat16 queued behind them two rows each, the weights' vmatprep in the cycle of the second vmatmul"
pops=$(sed -n 's/^\[tpu\] issue vpop: \([0-9]*\) cycles after .*output queues \([0-9]*\) and \([0-9]*\)$/\1:\2\/\3/p' \
  "$BUILD/tpuv6e-mxu_rate.gem5.log" | tr '\n' ' ')
report "cycles after the last issue:output queues ${pops% }" "cycles after the last issue:output queues 14:4/4 1:2/6 1:4/4 1:2/6 1:4/4 1:0/4" \
  "the four singles are out 4 + 15 cycles after their vmatmul, 14 after the last issue, and the bfloat16 rows two a cycle after them"

echo "-- matrix unit, 4 lanes, tpu_mxu_units: 2: two arrays, each its own queues of 8 and line"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 8\ntpu_mxu_units: 2\n' > "$BUILD/tpuv6e-mxu-arrays.yml"
on_gem5 mxu_arrays mxu_arrays "$BUILD/tpuv6e-mxu-arrays.yml"
arrays=$(said mxu_arrays "issue vmatprep: 0 cycles after the last issue, 1 in flight, array 0, input queue 0, output queues 0 and 0" \
  "issue vmatprep: 1 cycles after the last issue, 0 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 0 cycles after the last issue, 1 in flight, array 0, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 1 cycles after the last issue, 0 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vpop: 16 cycles after the last issue, 0 in flight, array 0, input queue 0, output queues 4 and 4" \
  "issue vpop: 1 cycles after the last issue, 0 in flight, array 1, input queue 0, output queues 4 and 4" \
  "issue vpop: 0 cycles after the last issue, 1 in flight, array 0, input queue 0, output queues 2 and 6" \
  "issue vpop: 1 cycles after the last issue, 0 in flight, array 1, input queue 0, output queues 2 and 6")
report "exit ${rc[mxu_arrays]}, lines $arrays of $(lines mxu_arrays issue); TPUV6E_MXU capacity $(unit_stat mxu_arrays TPUV6E_MXU capacity)" \
  "exit 0, lines 8 of 12; TPUV6E_MXU capacity 4" \
  "with tpu_mxu_units: 2, each array has its own queues, line and slots: two vmatmuls a cycle apart enter together, each array's pop comes 2 + 15 cycles after its own vmatmul, the two arrays' pops go together, and the input port takes 2 x 2 rows a cycle"

timeout 300 "$GEM5" -d "$BUILD/m5out-tpuv6e-mxu_arrays-script" "$MACHINE" \
  -c "$BUILD/tpuv6e-mxu_arrays" --model "$MODEL" --machine-config "$BUILD/tpuv6e-mxu-arrays.yml" \
  > "$BUILD/tpuv6e-mxu_arrays-script.gem5.log" 2>&1
rc[mxu_arrays-script]=$?
slots=$(said mxu_arrays-script "issue vmatprep: 0 cycles after the last issue, 1 in flight, array 0, input queue 0, output queues 0 and 0" \
  "issue vmatprep: 1 cycles after the last issue, 0 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 0 cycles after the last issue, 1 in flight, array 0, input queue 0, output queues 0 and 0")
report "exit ${rc[mxu_arrays-script]}, lines $slots of $(lines mxu_arrays-script issue)" "exit 0, lines 3 of 12" \
  "the v6e machine, whose CPU issues 12 a cycle: of vmatprep, vmatpush and vmatmul two a cycle over both arrays, so array 1's vmatprep of inputs waits a cycle behind array 0's and array 1's vmatpush"

echo "-- matrix unit, 4 lanes: a vpop and a vmatmul of one array in one cycle, on gem5_se.py and on the v6e machine"
on_gem5 mxu_slots mxu_slots "$HERE/mxu.yml"
timeout 300 "$GEM5" -d "$BUILD/m5out-tpuv6e-mxu_slots-script" "$MACHINE" \
  -c "$BUILD/tpuv6e-mxu_slots" --model "$MODEL" --machine-config "$HERE/mxu.yml" > "$BUILD/tpuv6e-mxu_slots-script.gem5.log" 2>&1
rc[mxu_slots-script]=$?
for run in mxu_slots mxu_slots-script; do
  together=$(said $run "issue vpop: 16 cycles after the last issue, 0 in flight, input queue 0, output queues 4 and 4" \
    "issue vmatmul: 0 cycles after the last issue, 1 in flight, input queue 0, output queues 0 and 4")
  report "exit ${rc[$run]}, lines $together of $(lines $run issue)" "exit 0, lines 2 of 14" \
    "$run: the first input's lower pop and the second input's vmatmul, a vpop slot and an inputs' slot, issue in one cycle"
done

echo "-- matrix unit, 4 lanes, tpu_mxu_units: 2, queues of 4: two arrays' vpops share the pop port's rows"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 4\ntpu_mxu_units: 2\n' > "$BUILD/tpuv6e-mxu-pops.yml"
on_gem5 mxu_pops mxu_pops "$BUILD/tpuv6e-mxu-pops.yml"
pops=$(said mxu_pops "issue vpop: 19 cycles after the last issue, 0 in flight, array 0, input queue 0, output queues 4 and 4" \
  "issue vpop: 1 cycles after the last issue, 0 in flight, array 1, input queue 0, output queues 4 and 4")
report "exit ${rc[mxu_pops]}, lines $pops of $(lines mxu_pops issue)" "exit 0, lines 2 of 4" \
  "each array's four rows are out in one cycle and the pop port takes four rows a cycle, so array 1's vpop waits a cycle behind array 0's"

echo "-- the machine PyTorchSim measures on (examples/tpuv6e/gem5)"

# on_machine <program>: runs it on the v6e machine
on_machine() {
  timeout 300 "$GEM5" -d "$BUILD/m5out-tpuv6e-machine-$1" "$MACHINE" -c "$BUILD/tpuv6e-$1" --model "$MODEL" \
    > "$BUILD/tpuv6e-machine-$1.gem5.log" 2>&1
  rc[machine-$1]=$?
}

# config <run> <section> <key>, window <run> <dump> <name>: scripts/sim.sh's, of the run on the v6e machine
config() { gem5_config "$BUILD/m5out-tpuv6e-machine-$1" "${@:2}"; }
window() { gem5_window "$BUILD/m5out-tpuv6e-machine-$1" "${@:2}"; }

for program in scratchpad valu loads addresses; do on_machine $program; done
report "exit ${rc[machine-scratchpad]}, ports $(config scratchpad system.spad load_ports) $(config scratchpad system.spad store_ports) $(config scratchpad system.spad access_ports), latency $(config scratchpad system.spad latency); requests $(config scratchpad system.cpu executeLSQRequestsQueueSize); integer units $(grep -c '^opClass=IntAlu$' "$BUILD/m5out-tpuv6e-machine-scratchpad/config.ini"), VALUs $(grep -c '^unit=Vpu$' "$BUILD/m5out-tpuv6e-machine-scratchpad/config.ini")" \
  "exit 0, ports 3 2 4, latency 1; requests 2; integer units 6, VALUs 4" \
  "the v6e machine: a scratchpad of 3 load, 2 store and 4 access ports a cycle, two LSQ requests, six integer units and four VALUs"
report "at most $(window scratchpad 1 system.spad.loadsPerCycle::max_value) loads, $(window scratchpad 1 system.spad.storesPerCycle::max_value) stores, $(window scratchpad 1 system.spad.accessesPerCycle::max_value) accesses a cycle" \
  "at most 3 loads, 2 stores, 4 accesses a cycle" \
  "scratchpad.S's independent loads and stores, three and two in turn: the scratchpad takes 3 loads, 2 stores and 4 accesses in a cycle"
report "$(window loads 2 system.cpu.numCycles) and $(window loads 4 system.cpu.numCycles), at most $(window loads 2 system.spad.loadsPerCycle::max_value) loads a cycle" \
  "41 and 41, at most 3 loads a cycle" \
  "loads.S's 64 loads take the same from lines 128 and 64 bytes apart: the scratchpad has no banks, its ports take any address"
report "$(window addresses 2 system.cpu.numCycles)" "76" \
  "addresses.S's 64 vector loads, each with the addi that steps its pointer, take 76 cycles"
report "busy $(window valu 1 system.cpu.units.Vpu.admitted) of $(window valu 1 system.cpu.numCycles) cycles" "busy 16 of 34 cycles" \
  "valu.S's 64 independent vfadds and vfmuls keep the VALUs busy 16 cycles: 4 a cycle, whatever the mix"

echo "-- the model"
printf 'tpu_mxu_queue_entries: 1\n' > "$BUILD/tpuv6e-queue-1.yml"
on_gem5 queue-1 mxu "$BUILD/tpuv6e-queue-1.yml"
why=$(grep -Fc "tpuv6e: machine description: tpu_mxu_queue_entries: '1' is not a queue size: a word of two rows needs 2" \
  "$BUILD/tpuv6e-queue-1.gem5.log")
report "exit ${rc[queue-1]}, reason given $why" "exit 1, reason given 1" "gem5 stops on tpu_mxu_queue_entries: 1"

echo "-- what it computes"
"$HERE/functional/run.sh" "$BUILD" "$SPIKE" "$PK" || failed=1

exit $failed
