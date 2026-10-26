#!/usr/bin/env bash
# The tpu model's counters in gem5's dumps: m5op windows and their boundaries; the vector unit's busy cycles. gem5 only.
# Usage: tests/stats/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
failed=0
declare -A rc

report() {
  if [ "$1" = "$2" ]; then echo "PASS  $3 ($1)"; else echo "FAIL  $3 ($1, expected $2)"; failed=1; fi
}

# on_gem5 <program>: runs it on the tpu model with a 4 x 4 array and queues of 8
on_gem5() {
  gem5_run "$BUILD/m5out-stats-$1" "$BUILD/libtpu.so" "$HERE/../tpu/systolic.yml" "$BUILD/stats-$1" \
    --max-ticks 100000000 > "$BUILD/stats-$1.gem5.log" 2>&1
  rc[$1]=$?
}

# on_vpu <program>: runs it on examples/tpu/gem5/script_systolic.py, the tpu machine whose vector FUs are the unit vpu
on_vpu() {
  timeout 300 "$GEM5" -d "$BUILD/m5out-stats-$1" "$REPO/examples/tpu/gem5/script_systolic.py" -c "$BUILD/stats-$1" \
    --model "$BUILD/libtpu.so" --machine-config "$HERE/../tpu/systolic.yml" > "$BUILD/stats-$1.gem5.log" 2>&1
  rc[$1]=$?
}

# stat <program> <dump> <name>: the value of the stat named <name>, or ending in .<name>, in the dump-th dump
stat() {
  awk -v want="$3" -v n="$2" '/Begin Simulation Statistics/ { s++ }
    s == n && ($1 == want || substr($1, length($1) - length(want)) == "." want) { print $2; exit }' \
    "$BUILD/m5out-stats-$1/stats.txt"
}

dumps() { grep -c 'Begin Simulation Statistics' "$BUILD/m5out-stats-$1/stats.txt"; }

# counts <program> <dump>: rows in at the systolic array, its input pushes and its pops committed
counts() {
  echo "$(stat "$1" "$2" vcix.SYSTOLIC.rows) $(stat "$1" "$2" vcix.committed.systolic_input_push)" \
    "$(stat "$1" "$2" vcix.committed.systolic_pop)"
}

# listed <program> <dump>: the dump's vcix.<group>.<counter> statistics but the commit counts, sorted
listed() {
  awk -v n="$2" '/Begin Simulation Statistics/ { s++ } s == n && $1 ~ /\.vcix\./ { sub(/.*\.vcix\./, "", $1); print $1 }' \
    "$BUILD/m5out-stats-$1/stats.txt" | grep -v '^committed\.' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'
}
COUNTERS="MISC.instructions MSA.rows MSA.rows_in_flight SFU.instructions SYSTOLIC.rows SYSTOLIC.rows_in_flight XLU.elements XLU.elements_in_flight"

# vpu <program> <dump>: the vector unit's busy cycles, its capacity, and whether its cycles is numCycles and its
# utilized_cycles admitted / capacity
vpu() {
  local admitted=$(stat "$1" "$2" system.cpu.units.Vpu.admitted) capacity=$(stat "$1" "$2" system.cpu.units.Vpu.capacity)
  local agree=no
  [ "$(stat "$1" "$2" system.cpu.units.Vpu.cycles)" = "$(stat "$1" "$2" system.cpu.numCycles)" ] &&
    [ "$(stat "$1" "$2" system.cpu.units.Vpu.utilized_cycles)" = "$admitted" ] && agree=agree
  echo "$admitted $capacity $agree"
}

for program in markers vpu; do
  rv_program "$HERE/$program.S" "$BUILD/stats-$program" || { echo "FAIL  $program.S does not build"; exit 2; }
done

echo "-- m5op windows"
on_gem5 markers
report "exit ${rc[markers]}, dumps $(dumps markers)" "exit 0, dumps 5" \
  "four markers and the end of the run dump five times: dumpreset's 1000 ns period in a1 is ignored"
report "$(listed markers 1)" "$COUNTERS" "dump 1: each component's counters, and nothing else of the model but the commits"
report "$(counts markers 1)" "12 3 3" "reset, three pushes of four and three pops, dump: 12 rows in, 3 pushes, 3 pops"
report "$(counts markers 2)" "0 0 0" "reset, then dump at once: nothing, and the push right after the dump is not in it"
report "$(counts markers 3)" "4 1 1" \
  "the push after the dump and its pop, then dumpreset: dumped at the marker, not 2000 ns later as a0 asks"
report "$(counts markers 4)" "8 2 2" "two pushes and their pops between two dumpresets, the second also with a0 and a1 set"
report "$(counts markers 5)" "0 0 0" "after the last dumpreset, to the end of the run: nothing"

echo "-- vector unit busy cycles"
on_vpu vpu
report "exit ${rc[vpu]}, dumps $(dumps vpu)" "exit 0, dumps 4" "three windows and the end of the run"
report "$(vpu vpu 1)" "8 1 agree" "eight vector adds, each alone: eight busy cycles at capacity 1"
report "$(vpu vpu 2)" "0 1 agree" "scalar adds only: no busy cycle"
report "$(vpu vpu 3)" "2 1 agree" "eight independent vector adds on the four MinorVecAdders: two busy cycles"
report "$(vpu vpu 4)" "2 1 agree" "no reset after dump 3: the end of the run carries its window on"
report "$(grep -c '\.units\.' "$BUILD/m5out-stats-markers/stats.txt")" "0" \
  "gem5_se.py's pool tags no FU with a unit: no units statistics"

exit $failed
