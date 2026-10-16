#!/usr/bin/env bash
# The examples/rtl models: the array's values against libtpu.so under every schedule and its stall against libtpu.so's
# timing, the copier's copy with and without a wait and into a page that faults, their gem5 timing lines, a wrong out_tag.
# Skipped (exit 0) when the build has no RTL models. Usage: tests/rtl/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
EXAMPLE="$REPO/examples/rtl"
FUNCTIONAL="$REPO/tests/tpu/functional"
OUT="$BUILD/rtl"
failed=0

for lib in rtl_systolic rtl_systolic_nowait rtl_systolic_badtag rtl_copier; do
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
for variant in plain:"" wait:-DWAIT untouched:-DUNTOUCHED wait-untouched:"-DWAIT -DUNTOUCHED" \
  timing:"-DWAIT -DTIMING"; do
  # shellcheck disable=SC2086
  riscv64-unknown-elf-gcc -O1 -Wall -Werror -static -march=rv64gcv -mabi=lp64d ${variant#*:} \
    "$EXAMPLE/copier/copy.c" -o "$OUT/copy-${variant%%:*}" ||
    { echo "FAIL  copier/copy.c (${variant%%:*}) does not build"; exit 2; }
done

# values <model> <schedule, or ""> <name>: the array's program, its output to $OUT/<name>.out; prints the exit status
values() {
  RTL_SCHEDULE=$2 spike_run "$BUILD/lib$1.so" "$FUNCTIONAL/machine.yml" "$OUT/values" --kernel-addr=2000000:2100000 \
    < /dev/null > "$OUT/$3.out" 2> "$OUT/$3.err"
  echo $?
}
same() { if cmp -s "$OUT/reference.out" "$OUT/$1.out"; then echo same; else echo differs; fi; }

echo "-- systolic array: values, against libtpu.so"
rc=$(values tpu "" reference)
report "exit $rc, $(wc -c < "$OUT/reference.out") bytes" "exit 0, 1536 bytes" "libtpu.so runs values.c"
for schedule in "" slow random:1:12 random:2:12 random:3:12 random:4:12 random:5:12 random:6:12 random:7:12 \
  random:8:12 random:9:12 random:10:12; do
  name=systolic-${schedule:-default}; name=${name//:/-}
  rc=$(values rtl_systolic "$schedule" "$name")
  report "exit $rc, $(same "$name")" "exit 0, same" \
    "rtl_systolic under ${schedule:-the default schedule}: values as libtpu.so's"
done
rc=$(values rtl_systolic_nowait "" systolic-nowait)
report "exit $rc, $(same systolic-nowait)" "exit 0, differs" \
  "rtl_systolic_nowait (WEIGHT_WAITS=0) under the default schedule: a weight push overtakes older inputs, values differ"

echo "-- systolic array: where the stall modes part, against libtpu.so's RTL keys, with no simulator"
"$BUILD/rtl_stall" "$BUILD/librtl_systolic.so" "$BUILD/libtpu.so" > "$OUT/stall.log" 2>&1
rc=$?
report "exit $rc, $(grep -c '^PASS' "$OUT/stall.log") passed" "exit 0, 7 passed" \
  "rtl_stall: the array stops while its output queue is full, slot empty or not; each instruction ready as on the RTL"

# copy <program> <schedule, or ""> <name>: prints the exit status and what the program and the unit said
copy() {
  RTL_SCHEDULE=$2 spike_run "$BUILD/librtl_copier.so" "" "$OUT/copy-$1" < /dev/null > "$OUT/$3.out" 2> "$OUT/$3.err"
  local rc=$? restored
  restored=$(sed -n 's/^\[rtl\] .* copies, \([0-9]*\) restored after a trap$/\1/p' "$OUT/$3.err")
  echo "exit $rc, $(cat "$OUT/$3.out"), ${restored:-0} restored"
}
echo "-- copier: memory, on Spike"
report "$(copy plain "" copy-default)" "exit 1, 0 of 256 bytes copied, 0 restored" \
  "a copy and no wait, default schedule: the loads run before the unit has copied anything"
report "$(copy wait "" copy-wait-default)" "exit 0, 256 of 256 bytes copied, 0 restored" \
  "a copy and a copy wait, default schedule: the wait clocks the unit until the copy is in memory"
report "$(copy wait slow copy-wait-slow)" "exit 0, 256 of 256 bytes copied, 0 restored" "the same, schedule slow"
report "$(copy wait-untouched "" copy-fault-default)" "exit 0, 256 of 256 bytes copied, 1 restored" \
  "into a page not yet touched, default schedule: the wait faults and is undone (the bytes come out exact either way)"
report "$(copy untouched slow copy-fault-slow)" "exit 0, 256 of 256 bytes copied, 1 restored" \
  "the same with no wait, schedule slow: the copy's own execute faults with its command taken, and is undone"

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
report "$(on_gem5 rtl_copier "" copy-timing copier)" "exit 0, exit line 1, 3 timing lines" \
  "rtl_copier runs copy.c -DWAIT -DTIMING: one line for each of its three instructions"
on_gem5 rtl_systolic_badtag "$FUNCTIONAL/machine.yml" timing badtag > /dev/null
report "$(grep -c '^rtl_systolic: a beat came out with out_tag [0-9]*, but the oldest command with beats out is' \
  "$OUT/badtag.gem5.log") message, $(grep -c '^exit: ' "$OUT/badtag.gem5.log") exit line" "1 message, 0 exit line" \
  "rtl_systolic_badtag (out_tag off by one) runs timing.S: the first out beat ends the run"

[ "$failed" = 0 ] || echo "---- outputs in $OUT"
exit $failed
