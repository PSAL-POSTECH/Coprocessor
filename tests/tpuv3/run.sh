#!/usr/bin/env bash
# The tpuv3 example: its timing on gem5, then what it computes on Spike (functional/run.sh). Usage: tests/tpuv3/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
failed=0
declare -A rc
MODEL="$BUILD/libtpuv3.so"

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

for program in mxu mxu_rate; do
  rv_program "$HERE/$program.S" "$BUILD/tpuv3-$program" || { echo "FAIL  $program.S does not build"; exit 2; }
done

echo "-- the units' statistics, with no simulator"
"$BUILD/tpuv3_ports" "$MODEL" > "$BUILD/tpuv3-ports.log" 2>&1
rc[ports]=$?
report "exit ${rc[ports]}, $(grep -c '^PASS' "$BUILD/tpuv3-ports.log") passed" "exit 0, 1 passed" \
  "tpuv3_ports: the model's units under the names it gives them, in its order (tpuv3-ports.log)"

echo "-- matrix unit, 4 lanes: an array of 4 x 4, 7 slots, a row a cycle, one MXU slot, queues of 8"
on_gem5 mxu mxu "$HERE/mxu.yml"
report "exit ${rc[mxu]}, $(issues mxu)" "exit 0, vmatmul:1:0/0 vpop:11:0/4 vpop:4:0/4" \
  "one MXU slot a cycle, no vmatprep; eight bfloat16 rows enter one a cycle: the first pop 4 + 7 cycles after the vmatmul, the next 4 later"
report "units $(units mxu); TPUV3_MXU admitted $(unit_stat mxu TPUV3_MXU admitted), capacity $(unit_stat mxu TPUV3_MXU capacity), occupancy $(unit_stat mxu TPUV3_MXU occupancy)" \
  "units TPUV3_MISC TPUV3_MXU TPUV3_SFU TPUV3_XLU; TPUV3_MXU admitted 8, capacity 1, occupancy 56" \
  "gem5 reports the model's four units under the names it gives them, the eight rows in at the matrix unit one a cycle, 7 cycles each in the line"

on_gem5 mxu_rate mxu_rate "$HERE/mxu.yml"
report "exit ${rc[mxu_rate]}, $(issues mxu_rate)" "exit 0, vmatmul:1:0/0 vmatmul:4:0/0 vpop:7:1/4 vpop:4:0/4 vpop:4:0/4" \
  "queues of 8: the eight bfloat16 rows wait 4 cycles for the four singles to enter; the singles out 4 + 7 cycles after their vmatmul"
printf 'tpu_trace: 1\nvpu_num_lanes: 4\ntpu_mxu_queue_entries: 16\n' > "$BUILD/tpuv3-mxu-16.yml"
on_gem5 mxu_rate-16 mxu_rate "$BUILD/tpuv3-mxu-16.yml"
report "exit ${rc[mxu_rate-16]}, $(issues mxu_rate-16 | cut -d' ' -f1-2)" "exit 0, vmatmul:1:0/0 vmatmul:1:3/0" \
  "with queues of 16, the second vmatmul waits only for the one MXU slot, a cycle, in which one of the four singles entered"

printf 'tpu_trace: 1\n' > "$BUILD/tpuv3-128.yml"
on_gem5 lanes-128 mxu "$BUILD/tpuv3-128.yml"
report "exit ${rc[lanes-128]}, $(issues lanes-128)" "exit 0, vmatmul:1:0/0 vpop:259:0/4 vpop:4:0/4" \
  "128 lanes, the default: an array of 128 x 128, so 255 slots, and the pop of four is issued 4 + 255 cycles after the vmatmul"

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
