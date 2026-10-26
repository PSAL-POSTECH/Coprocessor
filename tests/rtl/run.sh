#!/usr/bin/env bash
# The examples/rtl models: the array's values and stall against libtpu.so's, its gem5 timing lines, a wrong out_tag.
# Skipped (exit 0) when the build has no RTL models. Usage: tests/rtl/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
EXAMPLE="$REPO/examples/rtl"
FUNCTIONAL="$REPO/tests/tpu/functional"
OUT="$BUILD/rtl"
failed=0

for lib in rtl_systolic rtl_systolic_nowait rtl_systolic_badtag; do
  [ -e "$BUILD/lib$lib.so" ] && continue
  echo "SKIP  examples/rtl: $BUILD has no lib$lib.so (not built: no Verilator, or -DVCIX_RTL=OFF)"
  exit 0
done
rm -rf "$OUT" && mkdir -p "$OUT" || exit 2

report() {
  if [ "$1" = "$2" ]; then echo "PASS  $3 ($1)"; else echo "FAIL  $3 ($1, expected $2)"; failed=1; fi
}

clang --target=riscv64 -march=rv64gcv_xsfvcp -c "$FUNCTIONAL/kernels.S" -o "$OUT/kernels.o" &&
  riscv64-unknown-elf-gcc -O1 -Wall -Werror -static -march=rv64gc -mabi=lp64d -I"$FUNCTIONAL" \
    -Wl,--section-start=.kernel=0x2000000 "$EXAMPLE/systolic/values.c" "$OUT/kernels.o" -o "$OUT/values" ||
  { echo "FAIL  systolic/values.c does not build"; exit 2; }
rv_program "$EXAMPLE/systolic/timing.S" "$OUT/timing" || { echo "FAIL  systolic/timing.S does not build"; exit 2; }

# values <model> <schedule, or ""> <name> [machine]: the array's program, its output to $OUT/<name>.out; prints the exit status
values() {
  RTL_SCHEDULE=$2 spike_run "$BUILD/lib$1.so" "${4:-$FUNCTIONAL/machine.yml}" "$OUT/values" --kernel-addr=2000000:2100000 \
    < /dev/null > "$OUT/$3.out" 2> "$OUT/$3.err"
  echo $?
}
same() { if cmp -s "$OUT/${2:-reference}.out" "$OUT/$1.out"; then echo same; else echo differs; fi; }

{ cat "$FUNCTIONAL/machine.yml"
  printf 'tpu_systolic_queue_entries: 64\ntpu_systolic_command_queue: 2\ntpu_systolic_in_rows_per_cycle: 1\n'
  printf 'tpu_systolic_pop_rows_per_cycle: 1\ntpu_systolic_weight_waits: 0\n'; } > "$OUT/rtl-nowait.yml"
echo "-- systolic array: values, against libtpu.so"
values tpu "" reference > /dev/null
for schedule in "" slow; do
  name=systolic-${schedule:-default}
  rc=$(values rtl_systolic "$schedule" "$name")
  report "exit $rc, $(same "$name")" "exit 0, same" \
    "rtl_systolic under ${schedule:-the default schedule}: values as libtpu.so's"
done
differing=
for seed in 1 2 3 4 5 6 7 8 9 10; do
  rc=$(values rtl_systolic "random:$seed:12" "systolic-random-$seed-12")
  [ "$rc $(same "systolic-random-$seed-12")" = "0 same" ] || differing="$differing $seed"
done
report "seeds not exit 0 and same:${differing:- none}" "seeds not exit 0 and same: none" \
  "rtl_systolic under random:1:12 to random:10:12: values as libtpu.so's"
rc=$(values rtl_systolic_nowait "" systolic-nowait)
report "exit $rc, $(same systolic-nowait)" "exit 0, differs" \
  "rtl_systolic_nowait (WEIGHT_WAITS=0) under the default schedule: a weight push overtakes older inputs, values differ"
rc=$(values tpu "" nowait-reference "$OUT/rtl-nowait.yml")
report "exit $rc, $(same systolic-nowait nowait-reference)" "exit 0, same" \
  "libtpu.so under tpu_systolic_weight_waits: 0: older inputs meet the new weights in the same PEs as on the RTL"

echo "-- systolic array: where the stall modes part, against libtpu.so's RTL keys, with no simulator"
"$BUILD/rtl_stall" "$BUILD/librtl_systolic.so" "$BUILD/libtpu.so" > "$OUT/stall.log" 2>&1
rc=$?
report "exit $rc, $(grep -c '^PASS' "$OUT/stall.log") passed" "exit 0, 7 passed" \
  "rtl_stall: the array stops while its output queue is full, slot empty or not; each instruction ready as on the RTL"

# on_gem5 <model> <machine, or ""> <program> <name>: prints gem5's exit status and how many [rtl] lines it printed
on_gem5() {
  gem5_run "$OUT/m5out-$4" "$BUILD/lib$1.so" "$2" "$OUT/$3" --max-ticks 20000000000 > "$OUT/$4.gem5.log" 2>&1
  local rc=$?
  echo "exit $rc, exit line $(grep -c '^exit: ' "$OUT/$4.gem5.log")," \
    "$(grep -c '^\[rtl\] .*: issued @' "$OUT/$4.gem5.log") timing lines"
}
echo "-- gem5: a timing line at each commit"
report "$(on_gem5 rtl_systolic "$FUNCTIONAL/machine.yml" timing systolic)" "exit 0, exit line 1, 6 timing lines" \
  "rtl_systolic runs timing.S: one line for each of its six instructions"
on_gem5 rtl_systolic_badtag "$FUNCTIONAL/machine.yml" timing badtag > /dev/null
report "$(grep -c '^rtl_systolic: a beat came out with out_tag [0-9]*, but the oldest command with beats out is' \
  "$OUT/badtag.gem5.log") message, $(grep -c '^exit: ' "$OUT/badtag.gem5.log") exit line" "1 message, 0 exit line" \
  "rtl_systolic_badtag (out_tag off by one) runs timing.S: the first out beat ends the run"

[ "$failed" = 0 ] || echo "---- outputs in $OUT"
exit $failed
