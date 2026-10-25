#!/usr/bin/env bash
# The cross-lane reduction unit on 4 and 8 lanes: on Spike, sum and max against the same tree in C; on gem5, when
# each instruction issues and how many cycles it takes.
# Usage: tests/xlane_reduce/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
MODEL="$BUILD/libxlane_reduce.so"
OUT="$BUILD/xlane_reduce"
KERNEL_BASE=0x2000000
KERNEL_RANGE=2000000:2100000
failed=0
rm -rf "$OUT" && mkdir -p "$OUT" || exit 2
report() {
  if [ "$1" = "$2" ]; then echo "PASS  $3 ($1)"; else echo "FAIL  $3 ($1, expected $2)"; failed=1; fi
}
clang --target=riscv64 -march=rv64gcv_xsfvcp -c "$HERE/kernels.S" -o "$OUT/kernels.o" || exit 2
rv_program "$HERE/timing.S" "$OUT/timing" || exit 2
for n in 4 8; do
  levels=$([ $n = 4 ] && echo 2 || echo 3)
  riscv64-unknown-elf-gcc -O1 -Wall -Werror -static -march=rv64gc -mabi=lp64d -DLANES=${n}u \
    -Wl,--section-start=.kernel=$KERNEL_BASE "$HERE/values.c" "$OUT/kernels.o" -o "$OUT/values$n" || exit 2
  echo "-- $n lanes, on Spike: sum and max of vl = 1 .. 8 rows, against the same tree in C"
  "$SPIKE" --extlib="$MODEL" --isa=rv64gcv_zfh_xvcixaccel --machine-config="$HERE/lanes$n.yml" \
    --kernel-addr=$KERNEL_RANGE "$PK" "$OUT/values$n" > "$OUT/values$n.out" 2>&1
  report "exit $?, $(grep -c '^PASS' "$OUT/values$n.out") of 16" "exit 0, 16 of 16" "every lane's vd bit for bit"
  echo "-- $n lanes, on gem5: when each instruction issues and how many cycles it takes"
  gem5_run "$OUT/m5-$n" "$MODEL" "$HERE/lanes$n.yml" "$OUT/timing" > "$OUT/timing$n.log" 2>&1
  report "exit $?" "exit 0" "timing.S runs"
  log="$OUT/timing$n.log"
  alone=$(awk '/^\[xlane\]/ { n++; if (n >= 5) printf "%s%s", (n > 5 ? " " : ""), $10 } END { print "" }' "$log")
  report "$alone" "$((11 + levels)) $((11 + levels)) $((11 + levels)) $((7 + levels)) $((4 + levels))" \
    "alone, done 3 + vl + log2(lanes) cycles after issue: vl 8, 8, 8, 4, 1"
  ends=$(awk '/^\[xlane\]/ && ++n <= 4 { c = $6 + $10; if (n == 1) first = c; printf "%s%d", (n > 1 ? " " : ""), c - first } END { print "" }' "$log")
  report "$ends" "0 8 16 24" "four that need nothing from each other end vl = 8 cycles apart: one row a cycle"
done
exit $failed
