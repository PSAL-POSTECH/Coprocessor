#!/usr/bin/env bash
# The tpu model's unit statistics in gem5's dumps: m5op windows, their boundaries, and utilized cycles; the vector unit's busy cycles. gem5 only.
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
  echo "$(stat "$1" "$2" vcix.SYSTOLIC.admitted) $(stat "$1" "$2" vcix.committed.systolic_input_push)" \
    "$(stat "$1" "$2" vcix.committed.systolic_pop)"
}

# consistent <program> <dump>: how many of the five units have cycles = numCycles and utilized_cycles = admitted / capacity
consistent() {
  local unit admitted capacity cycles utilized agree=0
  local num_cycles=$(stat "$1" "$2" system.cpu.numCycles)
  for unit in SFU MISC SYSTOLIC XLU MSA; do
    admitted=$(stat "$1" "$2" "vcix.$unit.admitted")
    capacity=$(stat "$1" "$2" "vcix.$unit.capacity")
    cycles=$(stat "$1" "$2" "vcix.$unit.cycles")
    utilized=$(stat "$1" "$2" "vcix.$unit.utilized_cycles")
    [ "$cycles" = "$num_cycles" ] || continue
    awk -v a="$admitted" -v c="$capacity" -v u="$utilized" \
      'BEGIN { d = u - a / c; exit !(d < 5e-7 && d > -5e-7) }' && agree=$((agree + 1))
  done
  echo $agree
}

# utilized <program> <dump>: the systolic array's utilized cycles, as a number
utilized() { printf '%g' "$(stat "$1" "$2" vcix.SYSTOLIC.utilized_cycles)"; }

# beyond_units <program>: vcix lines in any dump that are neither vcix.<unit>.<stat> nor vcix.committed.<encoding>
beyond_units() {
  awk '$1 ~ /\.vcix\./ { sub(/.*\.vcix\./, "", $1); if (split($1, part, ".") != 2) n++ } END { print n + 0 }' \
    "$BUILD/m5out-stats-$1/stats.txt"
}

# vpu <program> <dump>: the vector unit's busy cycles, its capacity, and whether its cycles is numCycles and its
# utilized_cycles admitted / capacity
vpu() {
  local admitted=$(stat "$1" "$2" system.cpu.units.Vpu.admitted) capacity=$(stat "$1" "$2" system.cpu.units.Vpu.capacity)
  local agree=no
  [ "$(stat "$1" "$2" system.cpu.units.Vpu.cycles)" = "$(stat "$1" "$2" system.cpu.numCycles)" ] &&
    [ "$(stat "$1" "$2" system.cpu.units.Vpu.utilized_cycles)" = "$admitted" ] && agree=agree
  echo "$admitted $capacity $agree"
}

for program in markers rows vpu; do
  rv_program "$HERE/$program.S" "$BUILD/stats-$program" || { echo "FAIL  $program.S does not build"; exit 2; }
done

echo "-- m5op windows"
on_gem5 markers
report "exit ${rc[markers]}, dumps $(dumps markers)" "exit 0, dumps 5" \
  "four markers and the end of the run dump five times: dumpreset's 1000 ns period in a1 is ignored"
for dump in 1 2 3 4 5; do
  report "$(consistent markers $dump) of 5" "5 of 5" \
    "dump $dump: each unit's cycles is the CPU's numCycles and its utilized_cycles admitted / capacity"
done
report "$(counts markers 1)" "12 3 3" "reset, three pushes of four and three pops, dump: 12 rows in, 3 pushes, 3 pops"
report "$(counts markers 2)" "0 0 0" "reset, then dump at once: nothing, and the push right after the dump is not in it"
report "$(counts markers 3)" "4 1 1" \
  "the push after the dump and its pop, then dumpreset: dumped at the marker, not 2000 ns later as a0 asks"
report "$(counts markers 4)" "8 2 2" "two pushes and their pops between two dumpresets, the second also with a0 and a1 set"
report "$(counts markers 5)" "0 0 0" "after the last dumpreset, to the end of the run: nothing"
report "$(utilized markers 1)" "12" "dump 1 in utilized cycles: 12 rows at 1 a cycle"
report "$(for d in 1 2 3 4 5; do utilized markers $d; echo; done | awk '{ s += $1 } END { printf "%g", s }')" "24" \
  "the five dumps' utilized cycles add up to the run's: 24 rows in"
report "$(beyond_units markers), $(stat markers 2 vcix.SYSTOLIC.capacity)" "0, 1" \
  "only units and commit counts are printed, no port and no primary flag; dump 2, right after a reset, still has the capacity"

echo "-- utilized cycles"
on_gem5 rows
report "exit ${rc[rows]}, rows $(stat rows 1 vcix.SYSTOLIC.admitted), $(utilized rows 1)" "exit 0, rows 1, 1" \
  "one row alone in its window: one utilized cycle at the systolic array"
window=$(stat rows 2 system.cpu.numCycles)
report "rows $(stat rows 2 vcix.SYSTOLIC.admitted), $(utilized rows 2), $(utilized rows 2 | awk -v y="$window" '{ print ($1 <= y) ? "within" : "beyond" }')" \
  "rows 64, 64, within" "64 rows back to back: 64 utilized cycles, never more than the window's $window cycles"
for dump in 1 2; do
  report "$(consistent rows $dump) of 5" "5 of 5" "rows, dump $dump: each unit's cycles and utilized_cycles agree"
done

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
