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

for program in sfu one_cycle systolic_stream systolic_full msa xlu xlu_fold xlu_pattern loads addresses; do
  rv_program "$HERE/$program.S" "$BUILD/tpu-$program" || { echo "FAIL  $program.S does not build"; exit 2; }
done
printf 'tpu_trace: 1\ntpu_sfu_latency_cycles: 4\n' > "$BUILD/tpu-latency-4.yml"
printf 'tpu_sfu_latency_cycles: 0\n' > "$BUILD/tpu-latency-0.yml"

echo "-- the units' ports, with no simulator"
"$BUILD/tpu_ports" "$BUILD/libtpu.so" > "$BUILD/tpu-ports.log" 2>&1
rc[ports]=$?
report "exit ${rc[ports]}, $(grep -c '^PASS' "$BUILD/tpu-ports.log") passed" "exit 0, 17 passed" \
  "tpu_ports: each unit's ports count what the cases worked out by hand say (tpu-ports.log)"

echo "-- special-function unit"
on_gem5 sfu sfu "$HERE/trace.yml"
on_gem5 latency-4 sfu "$BUILD/tpu-latency-4.yml"

burst=$(said sfu "issue vexp: the first" \
  "issue vexp: 1 cycles after the last issue, 1 in flight" \
  "issue vexp: 1 cycles after the last issue, 2 in flight" \
  "issue vexp: 1 cycles after the last issue, 3 in flight" \
  "issue vexp: 1 cycles after the last issue, 4 in flight")
report "exit ${rc[sfu]}, issues $burst, commits $(times sfu "commit vexp: 11 cycles after its issue")" \
  "exit 0, issues 5, commits 5" \
  "gem5 issues five back-to-back vexp one per cycle and commits each 11 cycles after its issue: it enters the EUP the cycle after, and leaves 10 later"

chain() {
  local name readers=() commits=()
  for name in vtanh verf vsin vcos vlog vatan; do
    readers+=("issue $name: $2 cycles after the last issue, 0 in flight")
    commits+=("commit $name: $2 cycles after its issue")
  done
  echo "readers $(said "$1" "${readers[@]}"), commits $(said "$1" "${commits[@]}") and $(lines "$1" commit)"
}
report "$(chain sfu 11)" "readers 6, commits 6 and 11" \
  "gem5 issues the reader of a special function's result 11 cycles after it, down a chain of the seven"
report "exit ${rc[latency-4]}, $(chain latency-4 5)" "exit 0, readers 6, commits 6 and 11" \
  "gem5 follows tpu_sfu_latency_cycles: 4 from the machine description: each step of the chain is 4 + 1"

echo "-- systolic array, 4 lanes: 7 slots, queues of 8"
on_gem5 systolic_stream systolic_stream "$HERE/systolic.yml"
on_gem5 systolic_full systolic_full "$HERE/systolic.yml"

delay=$(said systolic_full "issue systolic weight push: the first, input queue 0, output queue 0" \
  "issue systolic input push: 1 cycles after the last issue, 0 in flight, input queue 0, output queue 0" \
  "issue systolic pop: 1 cycles after the last issue, 0 in flight, input queue 4, output queue 0")
report "exit ${rc[systolic_full]}, lines $delay, commits $(times systolic_full "commit systolic pop: 12 cycles after its issue")" \
  "exit 0, lines 3, commits 1" \
  "a pop of four is issued the cycle after its push of four, with the four rows in the input queue, and commits when the fourth is in the output queue, 4 + 7 + 1 cycles after"

pushes=("issue systolic input push: 1 cycles after the last issue, 0 in flight, input queue 0, output queue 0")
for entries in 2 3 4 5 6 7; do
  pushes+=("issue systolic input push: 1 cycles after the last issue, 0 in flight, input queue $entries, output queue 0")
done
pushes+=("issue systolic input push: 1 cycles after the last issue, 1 in flight, input queue 6, output queue 0")
for entries in 1 3; do
  pushes+=("issue systolic input push: 2 cycles after the last issue, 1 in flight, input queue 6, output queue $entries")
done
stream="$(said systolic_stream "${pushes[@]}") of $(lines systolic_stream issue)"
report "exit ${rc[systolic_stream]}, lines $stream" "exit 0, lines 10 of 11" \
  "pushes of two are issued one per cycle while the input queue has room (the rows of the push before are in it), then one every two cycles behind the push waiting at the head, as the array drains"

full=$(said systolic_full \
  "issue systolic weight push: 1 cycles after the last issue, 0 in flight, input queue 1, output queue 8" \
  "issue systolic pop: 1 cycles after the last issue, 0 in flight, input queue 1, output queue 8" \
  "issue systolic weight push: 1 cycles after the last issue, 0 in flight, input queue 1, output queue 4")
report "lines $full of $(lines systolic_full issue)" "lines 3 of 11" \
  "with the output queue full the array stops; the pop takes its four rows at the head the cycle after its issue, and the array moves again in the cycle after that"

echo "-- multi-precision array, 4 lanes: 7 slots, queues of 8"
on_gem5 msa msa "$HERE/systolic.yml"

delay=$(said msa "issue msa push: the first, input queue 0, output queue 0" \
  "issue msa push: 1 cycles after the last issue, 0 in flight, input queue 0, output queue 0" \
  "issue msa pop: 1 cycles after the last issue, 0 in flight, input queue 4, output queue 0")
report "exit ${rc[msa]}, lines $delay of $(lines msa issue), commits $(times msa "commit msa pop: 12 cycles after its issue")" \
  "exit 0, lines 3 of 3, commits 1" \
  "a weight push enters nothing, and a pop of four is issued the cycle after the input push of four and commits 4 + 7 + 1 cycles after, as the fourth row is in the output queue"

echo "-- cross-lane unit: a delay line of 3 slots, an input queue of 8, a buffer of any tile"
on_gem5 xlu xlu "$HERE/xlu.yml"
on_gem5 xlu-fold xlu_fold "$HERE/xlu.yml"
on_gem5 xlu-wide xlu_fold "$HERE/xlu_wide.yml"
on_gem5 xlu-issue xlu_fold "$HERE/xlu_issue.yml"
on_gem5 xlu-pattern xlu_pattern "$HERE/xlu.yml"
on_gem5 xlu-move xlu_pattern "$HERE/xlu_move.yml"
on_gem5 xlu-move-exchange xlu "$HERE/xlu_move.yml"
folded=$(said xlu-fold "issue xlu_push_reduce: the first, input queue 0, output queue 0" \
  "issue xlu_pop: 1 cycles after the last issue, 0 in flight, input queue 2, output queue 0")
report "exit ${rc[xlu-fold]}, lines $folded, commits $(times xlu-fold "commit xlu_pop: 8 cycles after its issue")" \
  "exit 0, lines 2, commits 1" \
  "a fold push of 2 (xlu_push_reduce) is timed as xlu_push: its pop is issued the cycle after, with both elements in the input queue; they enter the 3 stages one a cycle from the next, and the pop commits in 8"
wide=$(said xlu-wide "issue xlu_pop: 1 cycles after the last issue, 0 in flight, input queue 2, output queue 0")
report "exit ${rc[xlu-wide]}, lines $wide, commits $(times xlu-wide "commit xlu_pop: 7 cycles after its issue")" \
  "exit 0, lines 1, commits 1" \
  "with tpu_xlu_elements_per_cycle: 2 both elements enter the stages in one cycle: the pop ends a cycle sooner, in 7"
issued=$(said xlu-issue "issue xlu_pop: 0 cycles after the last issue, 1 in flight, input queue 0, output queue 0")
report "exit ${rc[xlu-issue]}, lines $issued, commits $(times xlu-issue "commit xlu_pop: 9 cycles after its issue")" \
  "exit 0, lines 1, commits 1" \
  "with tpu_xlu_issue_per_cycle: 2 the pop issues in the push's cycle, before the push's elements are in the input queue, and ends in the same cycle as when issued a cycle later: 9 after its own issue"

delay=$(said xlu "issue xlu_push: the first, input queue 0, output queue 0" \
  "issue xlu_pop: 1 cycles after the last issue, 0 in flight, input queue 2, output queue 0")
report "exit ${rc[xlu]}, lines $delay, commits $(times xlu "commit xlu_pop: 8 cycles after its issue")" \
  "exit 0, lines 2, commits 2" \
  "a pop is issued the cycle after its tile's last push, whose two elements are in the input queue; they enter the stages one a cycle from the next, the last is in the buffer 3 later, the tile is taken in the next, its slots leave the line in the next and are taken in the next: both tiles' first pops commit in 8"

report "exit ${rc[xlu-pattern]}, sets $(lines xlu-pattern "issue xlu_set_pattern:"), commits $(lines xlu-pattern "commit xlu_pop:")" \
  "exit 0, sets 3, commits 2" \
  "a set pattern is taken by the pushes after it on gem5 too: both tiles' first pops are accepted, the second's with a set between its pushes"

report "commits $(times xlu-pattern "commit xlu_pop: 8 cycles after its issue") and $(times xlu-move "commit xlu_pop: 11 cycles after its issue"), $(times xlu-move-exchange "commit xlu_pop: 8 cycles after its issue")" \
  "commits 2 and 2, 2" \
  "a move's tile passes tpu_xlu_move_latency_cycles (6 in xlu_move.yml): its first pops commit in 11, 3 cycles later than through 3 stages; an exchange's tile keeps tpu_xlu_latency_cycles, 8"

pushes=("issue xlu_push: 1 cycles after the last issue, 1 in flight, input queue 1, output queue 0" \
  "issue xlu_push: 8 cycles after the last issue, 0 in flight, input queue 1, output queue 0")
for queues in "2 0" "3 0" "4 1" "5 2" "6 3" "7 4"; do
  pushes+=("issue xlu_push: 1 cycles after the last issue, 0 in flight, input queue ${queues% *}, output queue ${queues#* }")
done
report "lines $(said xlu "${pushes[@]}"), waits $(times xlu "commit xlu_push: 7 cycles after its issue") and $(times xlu "commit xlu_push: 2 cycles after its issue")" \
  "lines 8, waits 1 and 1" \
  "the push after the pop is issued behind it and waits at the head until it ends (7 cycles); then pushes of two are issued one per cycle while the input queue has room, the last waiting at the head 2 cycles for it"

full=$(said xlu \
  "issue xlu_pop: 1 cycles after the last issue, 0 in flight, input queue 2, output queue 16" \
  "issue xlu_pop: 1 cycles after the last issue, 1 in flight, input queue 1, output queue 16" \
  "commit xlu_pop: 7 cycles after its issue")
report "lines $full of $(lines xlu issue)" "lines 3 of 13" \
  "the buffer holds the whole tile, 16 + 2 elements, and the line never stops; the second pop is issued the cycle after the first and waits behind it at the head, then takes 2 more slots from what is left: it commits in 7, the cycle after the first"

echo "-- one-cycle instructions"
on_gem5 one_cycle one_cycle "$HERE/trace.yml"

one_cycle=()
for name in vlane_idx compute dma_config_desc dma_index_key mvin mvin2 mvin3 mvout; do
  one_cycle+=("commit $name: 1 cycles after its issue")
done
drained=$(times one_cycle \
  "issue systolic weight push: 11 cycles after the last issue, 0 in flight, input queue 0, output queue 0")
commits="$(said one_cycle "${one_cycle[@]}") of $(lines one_cycle commit)"
report "exit ${rc[one_cycle]}, behind an empty model $drained, commits $commits" \
  "exit 0, behind an empty model 1, commits 8 of 10" \
  "gem5 takes each custom-2 and DMA instruction of tpu::Misc and commits it one cycle after its issue (the weight push reading the vexp's result is issued 11 after it)"

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
report "exit ${rc[machine]}, $(chain machine 11)" "exit 0, readers 6, commits 6 and 11" \
  "script_systolic.py runs sfu.S to the program's exit, the reader of a special function's result issued 11 cycles after it"


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
report "capacity $(machine_stat machine-misc MISC.capacity), admitted $(machine_stat machine-misc MISC.admitted) of $committed committed" \
  "capacity 2, admitted 8 of 8 committed" \
  "MISC, at its primary port issue, takes 2 a cycle and admits each committed custom-2 and DMA instruction of tpu::Misc once"

printf 'tpu_trace: 1\ntpu_misc_issue_width: 3\nvpu_spad_size_kb_per_lane: 512\n' > "$BUILD/tpu-misc-3.yml"
printf 'tpu_misc_issue_width: 0\n' > "$BUILD/tpu-misc-0.yml"
for width in 3 0; do
  timeout 300 "$GEM5" -d "$BUILD/m5out-tpu-machine-misc-$width" "$REPO/examples/tpu/gem5/script_systolic.py" \
    -c "$BUILD/tpu-one_cycle" --model "$BUILD/libtpu.so" --machine-config "$BUILD/tpu-misc-$width.yml" \
    > "$BUILD/tpu-machine-misc-$width.gem5.log" 2>&1
  rc[machine-misc-$width]=$?
done
report "exit ${rc[machine-misc-3]}, capacity $(machine_stat machine-misc-3 MISC.capacity), at most and in all $(misc_per_cycle machine-misc-3)" \
  "exit 0, capacity 3, at most and in all 3 8" "gem5 follows tpu_misc_issue_width: 3 from the machine description"
why=$(grep -Fc "tpu: machine description: tpu_misc_issue_width: '0' is not an issue width: at least 1" \
  "$BUILD/tpu-machine-misc-0.gem5.log")
report "exit ${rc[machine-misc-0]}, reason given $why" "exit 1, reason given 1" "gem5 stops on tpu_misc_issue_width: 0"

# memories <run>: each memory of the scratchpad, from the run's config.ini: its address range at its bandwidth (ps a byte)
memories() {
  awk '/^\[/ { in_mem = ($0 ~ /^\[system\.mem_ctrls[0-9]*\]$/) } in_mem && /^bandwidth=/ { bw = substr($0, 11) }
    in_mem && /^range=/ { out = out sep substr($0, 7) " at " bw; sep = ", " } END { print out }' "$BUILD/m5out-tpu-$1/config.ini"
}

# window <run> <dump> <name>: the value of the stat named <name> in the dump-th dump, 0 if the dump does not print it
window() {
  awk -v want="$3" -v n="$2" '/Begin Simulation Statistics/ { s++ } s == n && $1 == want { v = $2 } END { print v + 0 }' \
    "$BUILD/m5out-tpu-$1/stats.txt"
}

# data_reads <run> <dump>: each memory's reads of data in the dump-th dump, in the order of config.ini
data_reads() {
  local memory reads=()
  for memory in $(grep -o '^\[system\.mem_ctrls[0-9]*\]$' "$BUILD/m5out-tpu-$1/config.ini" | tr -d '[]'); do
    reads+=("$(window "$1" "$2" "$memory.numReads::cpu.data")")
  done
  echo "${reads[*]}"
}

timeout 300 "$GEM5" -d "$BUILD/m5out-tpu-machine-loads" "$REPO/examples/tpu/gem5/script_systolic.py" \
  -c "$BUILD/tpu-loads" --model "$BUILD/libtpu.so" > "$BUILD/tpu-machine-loads.gem5.log" 2>&1
rc[machine-loads]=$?
report "exit ${rc[machine-loads]}, $(memories machine-loads); requests $(grep -Po '^executeLSQRequestsQueueSize=\K.*' \
  "$BUILD/m5out-tpu-machine-loads/config.ini")" \
  "exit 0, 0:17179869184:0:64 at 29.000000, 0:17179869184:1:64 at 29.000000; requests 2" \
  "script_systolic.py's scratchpad is two banks, interleaved by the 64-byte line at 32 GB/s each, and its LSQ takes two requests"
report "$(data_reads machine-loads 2); $(data_reads machine-loads 4)" "64 0; 32 32" \
  "loads.S's 64 loads from lines 128 bytes apart all read bank 0; from lines 64 bytes apart, 32 each bank"
report "$(window machine-loads 2 system.cpu.numCycles) and $(window machine-loads 4 system.cpu.numCycles)" "85 and 51" \
  "the 64 loads over both banks take 34 cycles fewer than in one: two of them commit a cycle, not one"

timeout 300 "$GEM5" -d "$BUILD/m5out-tpu-machine-addresses" "$REPO/examples/tpu/gem5/script_systolic.py" \
  -c "$BUILD/tpu-addresses" --model "$BUILD/libtpu.so" > "$BUILD/tpu-machine-addresses.gem5.log" 2>&1
rc[machine-addresses]=$?
report "exit ${rc[machine-addresses]}, integer units $(grep -c '^opClass=IntAlu$' "$BUILD/m5out-tpu-machine-addresses/config.ini")" \
  "exit 0, integer units 6" "script_systolic.py's CPU has six units of IntAlu, two more than the original's four"
report "$(window machine-addresses 2 system.cpu.numCycles)" "91" \
  "addresses.S's 64 vector loads, each with the addi that steps its pointer, take 91 cycles (99 with four integer units, 95 with five)"

exit $failed
