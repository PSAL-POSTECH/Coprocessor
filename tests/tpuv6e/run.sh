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

# commits <run> [insn]: every commit as insn:cycles-after-its-issue, in commit order; of one instruction if named
commits() {
  sed -n "s/^\[tpu\] commit \(${2:-[a-z_]*}\): \([0-9]*\) cycles after its issue\$/\1:\2/p" "$BUILD/tpuv6e-$1.gem5.log" |
    tr '\n' ' ' | sed 's/ $//'
}

# unit_stat <run> <unit> <stat>: the value of vcix.<unit>.<stat> in the run's one dump
unit_stat() { awk -v want=".vcix.$2.$3" 'substr($1, length($1) - length(want) + 1) == want { print $2; exit }' "$BUILD/m5out-tpuv6e-$1/stats.txt"; }

# units <run>: the units in the run's dump, as gem5 sorts them
units() { sed -n 's/^.*\.vcix\.\([^.]*\)\.admitted .*$/\1/p' "$BUILD/m5out-tpuv6e-$1/stats.txt" | tr '\n' ' ' | sed 's/ $//'; }

for program in mxu mxu_rate mxu_arrays mxu_slots mxu_pops sfu scratchpad valu ../tpu/loads ../tpu/addresses; do
  rv_program "$HERE/$program.S" "$BUILD/tpuv6e-${program##*/}" || { echo "FAIL  $program.S does not build"; exit 2; }
done

echo "-- the units' statistics, with no simulator"
"$BUILD/tpuv6e_ports" "$MODEL" > "$BUILD/tpuv6e-ports.log" 2>&1
rc[ports]=$?
report "exit ${rc[ports]}, $(grep -c '^PASS' "$BUILD/tpuv6e-ports.log") passed" "exit 0, 1 passed" \
  "tpuv6e_ports: a unit per component and per block, under the names the model gives them, in its order (tpuv6e-ports.log)"

echo "-- matrix unit, 4 lanes: an array of 8 x 8, 15 slots, a word a cycle, queues of 8"
on_gem5 mxu mxu "$HERE/mxu.yml"

pairs=$(said mxu "issue vmatprep: the first, input queue 0, output queues 0 and 0" \
  "issue vmatpush: 0 cycles after the last issue, 1 in flight, input queue 0, output queues 0 and 0" \
  "issue vmatprep: 1 cycles after the last issue, 1 in flight, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 0 cycles after the last issue, 2 in flight, input queue 0, output queues 0 and 0")
report "exit ${rc[mxu]}, lines $pairs, commits $(commits mxu vmatprep) $(commits mxu vmatpush) $(commits mxu vmatmul)" \
  "exit 0, lines 4, commits vmatprep:1 vmatprep:1 vmatpush:2 vmatmul:2" \
  "a vmatprep and the half that follows it enter insns() in one cycle and are carried out a cycle apart, one slot each operand's: the weights' vmatpush goes with the inputs' vmatprep, the vmatmul a cycle later, and neither weights nor a vmatprep enter the array (input queue 0 at every issue)"
pops=$(said mxu "issue vpop: 1 cycles after the last issue, 1 in flight, input queue 0, output queues 0 and 0" \
  "issue vpop: 0 cycles after the last issue, 2 in flight, input queue 0, output queues 0 and 0" \
  "issue vpop: 1 cycles after the last issue, 2 in flight, input queue 8, output queues 0 and 0" \
  "issue vpop: 0 cycles after the last issue, 3 in flight, input queue 8, output queues 0 and 0")
report "lines $pops of $(lines mxu issue), commits $(commits mxu vpop)" "lines 4 of 8, commits vpop:19 vpop:20 vpop:20 vpop:21" \
  "the four vpops enter insns() two a cycle, before any row is out, and wait at the head: eight rows of bfloat16 enter the line two a cycle, so the lower half's first pop commits 1 + 1 + 15 + 1 cycles after the vmatmul is carried out (into the queue, the second word into the line, the line, into the output queue), the upper half's a cycle later (one vpop a cycle per array), the second pair's two later"
report "units $(units mxu); TPUV6E_MXU admitted $(unit_stat mxu TPUV6E_MXU admitted), capacity $(unit_stat mxu TPUV6E_MXU capacity), occupancy $(unit_stat mxu TPUV6E_MXU occupancy)" \
  "units TPUV6E_MISC TPUV6E_MISC_insns TPUV6E_MXU TPUV6E_MXU_array0_input TPUV6E_MXU_array0_line TPUV6E_MXU_array0_lower TPUV6E_MXU_array0_upper TPUV6E_MXU_insns TPUV6E_SFU TPUV6E_SFU_eup TPUV6E_SFU_eup_input TPUV6E_SFU_eup_output TPUV6E_SFU_insns TPUV6E_XLU TPUV6E_XLU_buffer TPUV6E_XLU_insns TPUV6E_XLU_line TPUV6E_XLU_moves TPUV6E_XLU_out TPUV6E_XLU_queue TPUV6E_XLU_stages; TPUV6E_MXU admitted 8, capacity 2, occupancy 120" \
  "gem5 reports a unit per component and per block (its insns, queues and pipelines) under the names the model gives them; the eight rows in at the matrix unit two a cycle"

printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 16\n' > "$BUILD/tpuv6e-mxu-16.yml"
on_gem5 mxu_rate mxu_rate "$BUILD/tpuv6e-mxu-16.yml"
queues=$(grep -o "input queue [0-9]*" "$BUILD/tpuv6e-mxu_rate.gem5.log" | cut -d' ' -f3 | tr '\n' ' ')
report "exit ${rc[mxu_rate]}, input queue at each issue ${queues% }" "exit 0, input queue at each issue 0 0 0 0 0 0 4 4 3 3 10 9 9 8 6 4" \
  "with queues of 16, a word a cycle enters; an issue's input queue is the rows held as it enters insns(): 0 until the singles' vmatmul is carried out, 4 then 3 as they leave one a cycle, 10 once the bfloat16's eight rows join the two singles left, and 9 9 8 6 4 as the six vpops enter while the line takes a word a cycle"
pops=$(sed -n 's/^\[tpu\] issue vpop: \([0-9]*\) cycles after .*output queues \([0-9]*\) and \([0-9]*\)$/\1:\2\/\3/p' \
  "$BUILD/tpuv6e-mxu_rate.gem5.log" | tr '\n' ' ')
report "cycles after the last issue:output queues ${pops% }, commits $(commits mxu_rate vpop)" \
  "cycles after the last issue:output queues 1:0/0 1:0/0 0:0/0 1:0/0 1:0/0 1:0/0, commits vpop:18 vpop:18 vpop:19 vpop:19 vpop:19 vpop:19" \
  "the six vpops enter insns() before any row is out (output queues 0 and 0) and wait at the head: the four singles are out 4 + 15 + 1 cycles after their rows entered the queue, the bfloat16 words behind them in the line two rows a cycle, so the pops commit 18 to 19 cycles after their issues, each half's a cycle apart"

echo "-- matrix unit, 4 lanes, tpu_mxu_units: 2: two arrays, each its own queues of 8 and line"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 8\ntpu_mxu_units: 2\n' > "$BUILD/tpuv6e-mxu-arrays.yml"
on_gem5 mxu_arrays mxu_arrays "$BUILD/tpuv6e-mxu-arrays.yml"
arrays=$(said mxu_arrays "issue vmatmul: 1 cycles after the last issue, 1 in flight, array 0, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 0 cycles after the last issue, 2 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vpop: 1 cycles after the last issue, 1 in flight, array 0, input queue 8, output queues 0 and 0" \
  "issue vpop: 0 cycles after the last issue, 2 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vpop: 1 cycles after the last issue, 2 in flight, array 0, input queue 6, output queues 0 and 0" \
  "issue vpop: 0 cycles after the last issue, 3 in flight, array 1, input queue 8, output queues 0 and 0")
report "exit ${rc[mxu_arrays]}, lines $arrays of $(lines mxu_arrays issue), commits $(commits mxu_arrays vmatmul) $(commits mxu_arrays vpop); TPUV6E_MXU capacity $(unit_stat mxu_arrays TPUV6E_MXU capacity)" \
  "exit 0, lines 6 of 12, commits vmatmul:1 vmatmul:2 vpop:18 vpop:19 vpop:18 vpop:19; TPUV6E_MXU capacity 4" \
  "with tpu_mxu_units: 2, each array has its own queues, line and slots: the two vmatmuls enter insns() in one cycle and are carried out a cycle apart (array 1's behind its own inputs' vmatprep, so array 0's rows are in its queue a cycle before array 1's), each array's rows are out 1 + 1 + 15 + 1 cycles after its vmatmul, so array 0's pops commit 18 and array 1's 19 cycles after their issues, and the input port takes 2 x 2 rows a cycle"

timeout 300 "$GEM5" -d "$BUILD/m5out-tpuv6e-mxu_arrays-script" "$MACHINE" \
  -c "$BUILD/tpuv6e-mxu_arrays" --model "$MODEL" --machine-config "$BUILD/tpuv6e-mxu-arrays.yml" \
  > "$BUILD/tpuv6e-mxu_arrays-script.gem5.log" 2>&1
rc[mxu_arrays-script]=$?
slots=$(said mxu_arrays-script "issue vmatprep: 0 cycles after the last issue, 2 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vmatpush: 0 cycles after the last issue, 3 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 0 cycles after the last issue, 5 in flight, array 0, input queue 0, output queues 0 and 0" \
  "issue vmatmul: 0 cycles after the last issue, 6 in flight, array 1, input queue 0, output queues 0 and 0")
report "exit ${rc[mxu_arrays-script]}, lines $slots of $(lines mxu_arrays-script issue), commits $(commits mxu_arrays-script)" \
  "exit 0, lines 4 of 12, commits vmatprep:1 vmatpush:2 vmatprep:2 vmatpush:3 vmatprep:2 vmatprep:3 vmatmul:3 vmatmul:4 vpop:20 vpop:20 vpop:20 vpop:20" \
  "the v6e machine, whose CPU issues 12 a cycle: insns() takes both arrays' vmatprep and vmatpush in one cycle and both vmatmuls in the next, but the two MXU slots carry out two a cycle over both arrays, each operand's slot one a cycle, so array 1's vmatprep of inputs still waits behind array 0's and array 1's vmatpush: the commits step from 1 to 4 cycles after their issues, and every pop lands 20 after its own"

echo "-- matrix unit, 4 lanes: a vpop and a vmatmul of one array in one cycle, on gem5_se.py and on the v6e machine"
on_gem5 mxu_slots mxu_slots "$HERE/mxu.yml"
timeout 300 "$GEM5" -d "$BUILD/m5out-tpuv6e-mxu_slots-script" "$MACHINE" \
  -c "$BUILD/tpuv6e-mxu_slots" --model "$MODEL" --machine-config "$HERE/mxu.yml" > "$BUILD/tpuv6e-mxu_slots-script.gem5.log" 2>&1
rc[mxu_slots-script]=$?
report "exit ${rc[mxu_slots]}, commits $(commits mxu_slots)" \
  "exit 0, commits vmatprep:1 vmatpush:2 vmatprep:1 vmatmul:2 vmatprep:2 vpop:19 vmatmul:18 vpop:19 vpop:19 vpop:20 vpop:34 vpop:18 vpop:19 vpop:19" \
  "mxu_slots: the first input's lower pop enters insns() a cycle before the second input's vmatmul and commits a cycle more after its issue, so the two are carried out in one cycle, a vpop slot and an inputs' slot; the second input's rows are out 34 cycles after its first pop's issue, its words behind the first input's in the line"
report "exit ${rc[mxu_slots-script]}, commits $(commits mxu_slots-script)" \
  "exit 0, commits vmatprep:1 vmatpush:2 vmatprep:2 vmatmul:2 vmatprep:3 vpop:20 vmatmul:19 vpop:19 vpop:20 vpop:20 vpop:34 vpop:18 vpop:19 vpop:19" \
  "mxu_slots-script: on the v6e machine, whose CPU issues 12 a cycle, insns() takes three a cycle, so the inputs' vmatpreps, the first vmatmul and the first pop enter a cycle earlier and wait a cycle longer at the head; every instruction is carried out in the cycle it is on gem5_se.py, the pop and the vmatmul still in one"

echo "-- matrix unit, 4 lanes, tpu_mxu_units: 2, queues of 4: two arrays' vpops share the pop port's rows"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 4\ntpu_mxu_units: 2\n' > "$BUILD/tpuv6e-mxu-pops.yml"
on_gem5 mxu_pops mxu_pops "$BUILD/tpuv6e-mxu-pops.yml"
pops=$(said mxu_pops "issue vmatmul: 0 cycles after the last issue, 1 in flight, array 1, input queue 0, output queues 0 and 0" \
  "issue vpop: 1 cycles after the last issue, 0 in flight, array 0, input queue 4, output queues 0 and 0" \
  "issue vpop: 0 cycles after the last issue, 1 in flight, array 1, input queue 4, output queues 0 and 0")
report "exit ${rc[mxu_pops]}, lines $pops of $(lines mxu_pops issue), commits $(commits mxu_pops vmatmul) $(commits mxu_pops vpop)" \
  "exit 0, lines 3 of 10, commits vmatmul:1 vmatmul:1 vpop:20 vpop:21" \
  "the two vmatmuls enter insns() in one cycle and are carried out together in the next (an MXU slot and an inputs' slot each); the two vpops enter in one cycle and wait at the head: each array's four rows are out 1 + 3 + 15 + 1 cycles after its vmatmul and the pop port takes four rows a cycle, so array 1's vpop commits a cycle behind array 0's"

echo "-- special-function unit, 4 lanes: an EUP push of eight registers, then their pop"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\n' > "$BUILD/tpuv6e-sfu.yml"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_sfu_latency_cycles: 4\n' > "$BUILD/tpuv6e-sfu-4.yml"
on_gem5 sfu sfu "$BUILD/tpuv6e-sfu.yml"
on_gem5 sfu-4 sfu "$BUILD/tpuv6e-sfu-4.yml"
eup=$(said sfu "issue sfu_push: the first, input queue 0, output queue 0" \
  "commit sfu_push: 1 cycles after its issue" \
  "issue sfu_pop: 1 cycles after the last issue, 0 in flight, input queue 8, output queue 0" \
  "commit sfu_pop: 19 cycles after its issue")
eup4=$(said sfu-4 "issue sfu_pop: 1 cycles after the last issue, 0 in flight, input queue 8, output queue 0" \
  "commit sfu_pop: 13 cycles after its issue")
report "exit ${rc[sfu]} ${rc[sfu-4]}, lines $eup of 4, $eup4 of 2" "exit 0 0, lines 4 of 4, 2 of 2" \
  "the push queues its eight registers in one cycle (it commits a cycle after its issue; the pop enters insns() the next cycle, input queue 8) and they enter the EUP one a cycle, each leaving tpu_sfu_latency_cycles later, so the pop commits 8 + 10 + 1 cycles after its issue, and 8 + 4 + 1 with a latency of 4"

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
  "40 and 40, at most 3 loads a cycle" \
  "loads.S's 64 loads take the same from lines 128 and 64 bytes apart: the scratchpad has no banks, its ports take any address"
report "$(window addresses 2 system.cpu.numCycles)" "76" \
  "addresses.S's 64 vector loads, each with the addi that steps its pointer, take 76 cycles"
report "busy $(window valu 1 system.cpu.units.Vpu.admitted) of $(window valu 1 system.cpu.numCycles) cycles" "busy 16 of 35 cycles" \
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
