#!/usr/bin/env bash
# The tpu example: its timing on gem5, then what it computes on Spike (functional/run.sh). Usage: tests/tpu/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
failed=0
declare -A rc

report() {
  if [ "$1" = "$2" ]; then echo "PASS  $3 ($1)"; else echo "FAIL  $3 ($1, expected $2)"; failed=1; fi
}

on_gem5() {
  gem5_run "$BUILD/m5out-tpu-$1" "$BUILD/libtpu.so" "$3" "$BUILD/tpu-$2" --max-ticks 100000000 \
    > "$BUILD/tpu-$1.gem5.log" 2>&1
  rc[$1]=$?
}

said() {
  local line count=0
  for line in "${@:2}"; do
    [ "$(grep -Fxc "[tpu] $line" "$BUILD/tpu-$1.gem5.log")" = 1 ] && count=$((count + 1))
  done
  echo $count
}

times() { grep -Fxc "[tpu] $2" "$BUILD/tpu-$1.gem5.log"; }

lines() { grep -c "^\[tpu\] $2 " "$BUILD/tpu-$1.gem5.log"; }

for program in sfu one_cycle systolic_stream systolic_full msa xlu; do
  rv_program "$HERE/$program.S" "$BUILD/tpu-$program" || { echo "FAIL  $program.S does not build"; exit 2; }
done
printf 'tpu_trace: 1\ntpu_sfu_latency_cycles: 4\n' > "$BUILD/tpu-latency-4.yml"
printf 'tpu_sfu_latency_cycles: 0\n' > "$BUILD/tpu-latency-0.yml"

echo "-- the units' ports, with no simulator"
"$BUILD/tpu_ports" "$BUILD/libtpu.so" > "$BUILD/tpu-ports.log" 2>&1
rc[ports]=$?
report "exit ${rc[ports]}, $(grep -c '^PASS' "$BUILD/tpu-ports.log") passed" "exit 0, 18 passed" \
  "tpu_ports: each unit's ports count what the cases worked out by hand say (tpu-ports.log)"

echo "-- special-function unit"
on_gem5 sfu sfu "$HERE/trace.yml"
on_gem5 latency-4 sfu "$BUILD/tpu-latency-4.yml"

burst=$(said sfu "issue vexp: the first" \
  "issue vexp: 1 cycles after the last issue, 1 in flight" \
  "issue vexp: 1 cycles after the last issue, 2 in flight" \
  "issue vexp: 1 cycles after the last issue, 3 in flight" \
  "issue vexp: 1 cycles after the last issue, 4 in flight")
report "exit ${rc[sfu]}, issues $burst, commits $(times sfu "commit vexp: 10 cycles after its issue")" \
  "exit 0, issues 5, commits 5" \
  "gem5 issues five back-to-back vexp one per cycle and commits each 10 cycles after its issue"

chain() {
  local name readers=() commits=()
  for name in vtanh verf vsin vcos vlog vatan; do
    readers+=("issue $name: $2 cycles after the last issue, 0 in flight")
    commits+=("commit $name: $2 cycles after its issue")
  done
  echo "readers $(said "$1" "${readers[@]}"), commits $(said "$1" "${commits[@]}") and $(lines "$1" commit)"
}
report "$(chain sfu 10)" "readers 6, commits 6 and 11" \
  "gem5 issues the reader of a special function's result 10 cycles after it, down a chain of the seven"
report "exit ${rc[latency-4]}, $(chain latency-4 4)" "exit 0, readers 6, commits 6 and 11" \
  "gem5 follows tpu_sfu_latency_cycles: 4 from the machine description"

echo "-- systolic array, 4 lanes: 7 slots, queues of 8"
on_gem5 systolic_stream systolic_stream "$HERE/systolic.yml"
on_gem5 systolic_full systolic_full "$HERE/systolic.yml"

delay=$(said systolic_full "issue systolic weight push: the first, input queue 0, output queue 0" \
  "issue systolic input push: 1 cycles after the last issue, 0 in flight, input queue 0, output queue 0" \
  "issue systolic pop: 11 cycles after the last issue, 0 in flight, input queue 0, output queue 4")
report "exit ${rc[systolic_full]}, lines $delay" "exit 0, lines 3" \
  "a pop of four is issued 4 + 7 cycles after the push of four, as the fourth element is in the output queue"

pushes=("issue systolic input push: the first, input queue 0, output queue 0")
for entries in 1 2 3 4 5 6; do
  pushes+=("issue systolic input push: 1 cycles after the last issue, 0 in flight, input queue $entries, output queue 0")
done
for entries in 1 3 5; do
  pushes+=("issue systolic input push: 2 cycles after the last issue, 0 in flight, input queue 6, output queue $entries")
done
stream="$(said systolic_stream "${pushes[@]}") of $(lines systolic_stream issue)"
report "exit ${rc[systolic_stream]}, lines $stream" "exit 0, lines 10 of 10" \
  "pushes of two go in one per cycle while the input queue has room, then one every two cycles, as the array drains"

full=$(said systolic_full \
  "issue systolic weight push: 1 cycles after the last issue, 0 in flight, input queue 1, output queue 8" \
  "issue systolic pop: 1 cycles after the last issue, 0 in flight, input queue 1, output queue 8" \
  "issue systolic weight push: 1 cycles after the last issue, 0 in flight, input queue 0, output queue 5")
report "lines $full of $(lines systolic_full issue)" "lines 3 of 11" \
  "with the output queue full the array stops, and moves again in the cycle after a pop"

echo "-- multi-precision array, 4 lanes: 7 slots, queues of 8"
on_gem5 msa msa "$HERE/systolic.yml"

delay=$(said msa "issue msa push: the first, input queue 0, output queue 0" \
  "issue msa push: 1 cycles after the last issue, 0 in flight, input queue 0, output queue 0" \
  "issue msa pop: 11 cycles after the last issue, 0 in flight, input queue 0, output queue 4")
report "exit ${rc[msa]}, lines $delay of $(lines msa issue)" "exit 0, lines 3 of 3" \
  "a weight push enters nothing, and a pop of four is issued 4 + 7 cycles after the input push of four"

echo "-- cross-lane unit: a delay line of 3 slots, queues of 8"
on_gem5 xlu xlu "$HERE/xlu.yml"

delay=$(said xlu "issue xlu_push: the first, input queue 0, output queue 0" \
  "issue xlu_pop: 5 cycles after the last issue, 0 in flight, input queue 0, output queue 2")
report "exit ${rc[xlu]}, lines $delay" "exit 0, lines 2" \
  "a pop of two is issued 2 + 3 cycles after the push of two, as the second element is in the output queue"

pushes=()
for queues in "0 0" "1 0" "2 0" "3 0" "4 1" "5 2" "6 3"; do
  pushes+=("issue xlu_push: 1 cycles after the last issue, 0 in flight, input queue ${queues% *}, output queue ${queues#* }")
done
pushes+=("issue xlu_push: 2 cycles after the last issue, 0 in flight, input queue 6, output queue 5")
report "lines $(said xlu "${pushes[@]}")" "lines 8" \
  "pushes of two go in one per cycle while the input queue has room, then wait for it"

full=$(said xlu \
  "issue xlu_pop: 1 cycles after the last issue, 0 in flight, input queue 7, output queue 8" \
  "issue xlu_pop: 1 cycles after the last issue, 0 in flight, input queue 6, output queue 7")
report "lines $full of $(lines xlu issue)" "lines 2 of 13" \
  "with the output queue full the unit stops, and moves again in the cycle after a pop"

echo "-- one-cycle instructions"
on_gem5 one_cycle one_cycle "$HERE/trace.yml"

one_cycle=()
for name in vlane_idx compute dma_config_desc dma_index_key mvin mvin2 mvin3 mvout; do
  one_cycle+=("commit $name: 1 cycles after its issue")
done
drained=$(times one_cycle \
  "issue systolic weight push: 10 cycles after the last issue, 0 in flight, input queue 0, output queue 0")
commits="$(said one_cycle "${one_cycle[@]}") of $(lines one_cycle commit)"
report "exit ${rc[one_cycle]}, behind an empty model $drained, commits $commits" \
  "exit 0, behind an empty model 1, commits 8 of 10" \
  "gem5 takes each custom-2 and DMA instruction of tpu::Misc and commits it one cycle after its issue"

echo "-- the model"
on_gem5 latency-0 sfu "$BUILD/tpu-latency-0.yml"
why=$(grep -Fc "tpu: machine description: tpu_sfu_latency_cycles: '0' is not a pipeline depth: at least 1" \
  "$BUILD/tpu-latency-0.gem5.log")
report "exit ${rc[latency-0]}, reason given $why" "exit 1, reason given 1" "gem5 stops on tpu_sfu_latency_cycles: 0"

spike_run "$BUILD/libtpu.so" "" "$BUILD/tpu-sfu" > "$BUILD/tpu-sfu.spike.log" 2>&1
report "exit $?" "exit 0" "spike runs sfu.S to the program's exit"

echo "-- what it computes"
"$HERE/functional/run.sh" "$BUILD" "$SPIKE" "$PK" || failed=1

echo "-- the machine PyTorchSim measures on (examples/tpu/gem5)"
timeout 300 "$GEM5" -d "$BUILD/m5out-tpu-machine" "$REPO/examples/tpu/gem5/script_systolic.py" -c "$BUILD/tpu-sfu" \
  --model "$BUILD/libtpu.so" --machine-config "$HERE/trace.yml" > "$BUILD/tpu-machine.gem5.log" 2>&1
rc[machine]=$?
report "exit ${rc[machine]}, $(chain machine 10)" "exit 0, readers 6, commits 6 and 11" \
  "script_systolic.py runs sfu.S to the program's exit, the reader of a special function's result issued 10 cycles after it"


# machine_stat <run> <name>: the value of the stat ending in .<name> in the run's one dump
machine_stat() { awk -v want=".$2" 'substr($1, length($1) - length(want) + 1) == want { print $2; exit }' "$BUILD/m5out-tpu-$1/stats.txt"; }

# misc_per_cycle <run>: the most tpu::Misc instructions issued in one cycle, and how many it issued, from the trace
misc_per_cycle() {
  awk '/^\[tpu\] issue / { split($0, part, ": "); name = substr(part[1], 13); split(part[2], after, " ")
      if (after[1] == "the") at = 0; else at += after[1]
      if (name ~ /^(vlane_idx|compute|dma_config_desc|dma_index_key|mvin|mvin2|mvin3|mvout)$/) { n[at]++; all++ } }
    END { for (c in n) if (n[c] > most) most = n[c]; print most + 0, all + 0 }' "$BUILD/tpu-$1.gem5.log"
}

timeout 300 "$GEM5" -d "$BUILD/m5out-tpu-machine-misc" "$REPO/examples/tpu/gem5/script_systolic.py" \
  -c "$BUILD/tpu-one_cycle" --model "$BUILD/libtpu.so" --machine-config "$HERE/trace.yml" \
  > "$BUILD/tpu-machine-misc.gem5.log" 2>&1
rc[machine-misc]=$?
committed=0
for name in vlane_idx compute dma_config_desc dma_index_key mvin mvin2 mvin3 mvout; do
  committed=$((committed + $(machine_stat machine-misc "committed.$name")))
done
report "exit ${rc[machine-misc]}, at most and in all $(misc_per_cycle machine-misc)" "exit 0, at most and in all 2 8" \
  "script_systolic.py issues one_cycle.S's eight tpu::Misc instructions at most two a cycle, though the CPU issues 12"
report "capacity $(machine_stat machine-misc Misc.capacity), admitted $(machine_stat machine-misc Misc.admitted) of $committed committed" \
  "capacity 2, admitted 8 of 8 committed" \
  "Misc, at its primary port issue, takes 2 a cycle and admits each committed custom-2 and DMA instruction of tpu::Misc once"

printf 'tpu_trace: 1\ntpu_misc_issue_width: 3\n' > "$BUILD/tpu-misc-3.yml"
printf 'tpu_misc_issue_width: 0\n' > "$BUILD/tpu-misc-0.yml"
for width in 3 0; do
  timeout 300 "$GEM5" -d "$BUILD/m5out-tpu-machine-misc-$width" "$REPO/examples/tpu/gem5/script_systolic.py" \
    -c "$BUILD/tpu-one_cycle" --model "$BUILD/libtpu.so" --machine-config "$BUILD/tpu-misc-$width.yml" \
    > "$BUILD/tpu-machine-misc-$width.gem5.log" 2>&1
  rc[machine-misc-$width]=$?
done
report "exit ${rc[machine-misc-3]}, capacity $(machine_stat machine-misc-3 Misc.capacity), at most and in all $(misc_per_cycle machine-misc-3)" \
  "exit 0, capacity 3, at most and in all 3 8" "gem5 follows tpu_misc_issue_width: 3 from the machine description"
why=$(grep -Fc "tpu: machine description: tpu_misc_issue_width: '0' is not an issue width: at least 1" \
  "$BUILD/tpu-machine-misc-0.gem5.log")
report "exit ${rc[machine-misc-0]}, reason given $why" "exit 1, reason given 1" "gem5 stops on tpu_misc_issue_width: 0"

exit $failed
