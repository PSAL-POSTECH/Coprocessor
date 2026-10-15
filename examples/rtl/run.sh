#!/usr/bin/env bash
# Runs the RTL models: the array's values.c on Spike beside libtpu.so's, the copier's copy with a wait on Spike, then
# both timing programs on gem5. Needs a build that found Verilator; tests/rtl/run.sh checks every schedule.
# Usage: examples/rtl/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
FUNCTIONAL="$REPO/tests/tpu/functional"
OUT="$BUILD/rtl-example"
[ -e "$BUILD/librtl_systolic.so" ] ||
  { echo "$BUILD/librtl_systolic.so: not built -- configure with Verilator on PATH or VERILATOR_ROOT set" >&2; exit 2; }
mkdir -p "$OUT"

clang --target=riscv64 -march=rv64gcv_xsfvcp -c "$FUNCTIONAL/kernels.S" -o "$OUT/kernels.o"
riscv64-unknown-elf-gcc -O1 -static -march=rv64gc -mabi=lp64d -I"$FUNCTIONAL" -Wl,--section-start=.kernel=0x2000000 \
  "$HERE/systolic/values.c" "$OUT/kernels.o" -o "$OUT/values"
riscv64-unknown-elf-gcc -O1 -static -march=rv64gcv -mabi=lp64d -DWAIT "$HERE/copier/copy.c" -o "$OUT/copy-wait"
riscv64-unknown-elf-gcc -O1 -static -march=rv64gcv -mabi=lp64d -DWAIT -DTIMING "$HERE/copier/copy.c" \
  -o "$OUT/copy-timing"
rv_program "$HERE/systolic/timing.S" "$OUT/timing"

echo "== spike: the array's values, libtpu.so and rtl_systolic"
for model in tpu rtl_systolic; do
  spike_run "$BUILD/lib$model.so" "$FUNCTIONAL/machine.yml" "$OUT/values" --kernel-addr=2000000:2100000 \
    < /dev/null > "$OUT/values-$model.out"
done
if cmp -s "$OUT/values-tpu.out" "$OUT/values-rtl_systolic.out"; then
  echo "the same $(wc -c < "$OUT/values-tpu.out") bytes"
else
  echo "they differ"
fi

echo "== spike: a copy, then a copy wait"
spike_run "$BUILD/librtl_copier.so" "" "$OUT/copy-wait" || true

echo "== gem5: the array's timing"
gem5_run "$OUT/m5out-systolic" "$BUILD/librtl_systolic.so" "$FUNCTIONAL/machine.yml" "$OUT/timing" 2>&1 |
  grep -E '^\[rtl\]|^exit:'
echo "== gem5: the copier's timing (gem5 moves no data; -DTIMING exits 0)"
gem5_run "$OUT/m5out-copier" "$BUILD/librtl_copier.so" "" "$OUT/copy-timing" 2>&1 | grep -E '^\[rtl\]|^exit:'
