#!/usr/bin/env bash
# The cross-lane all-reduce sum unit on 128 lanes: on Spike, sum against the same tree in C; on gem5, when
# each instruction issues and how many cycles it takes. scripts/build.sh builds the programs, in build/xlane_all_reduce.
# Usage: tests/xlane_all_reduce/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
MODEL="$BUILD/libxlane_all_reduce.so"
OUT="$BUILD/xlane_all_reduce"
KERNEL_RANGE=2000000:2100000
failed=0
for program in values timing; do
  [ -e "$OUT/$program" ] || { echo "$OUT/$program: not found -- scripts/build.sh builds it" >&2; exit 2; }
done
rm -rf "$OUT/m5" "$OUT/values.out" "$OUT/timing.log" || exit 2
report() {
  if [ "$1" = "$2" ]; then echo "PASS  $3 ($1)"; else echo "FAIL  $3 ($1, expected $2)"; failed=1; fi
}
echo "-- on Spike: sum of vl = 1 .. 8 rows, against the same tree in C"
spike_run "$MODEL" "$HERE/machine.yml" "$OUT/values" --kernel-addr=$KERNEL_RANGE > "$OUT/values.out" 2>&1
report "exit $?, $(grep -c '^PASS' "$OUT/values.out") of 8" "exit 0, 8 of 8" "every lane's vd bit for bit"
echo "-- on gem5: when each instruction issues and how many cycles it takes"
gem5_run "$OUT/m5" "$MODEL" "$HERE/machine.yml" "$OUT/timing" > "$OUT/timing.log" 2>&1
report "exit $?" "exit 0" "timing.S runs"
log="$OUT/timing.log"
alone=$(awk '/^\[xlane_all_reduce\]/ { n++; if (n >= 5) printf "%s%s", (n > 5 ? " " : ""), $11 } END { print "" }' "$log")
report "$alone" "18 18 18 14 11" "alone, done 3 + vl + 7 cycles after issue: vl 8, 8, 8, 4, 1"
ends=$(awk '/^\[xlane_all_reduce\]/ && ++n <= 4 { c = $10; if (n == 1) first = c; printf "%s%d", (n > 1 ? " " : ""), c - first } END { print "" }' "$log")
report "$ends" "0 8 16 24" "four that need nothing from each other end vl = 8 cycles apart: one row a cycle"
exit $failed
