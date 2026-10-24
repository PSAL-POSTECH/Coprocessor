#!/usr/bin/env bash
# The tpuv6e example's matrix unit on Spike, against a formula, and what it refuses; on tests/tpu/functional's machine
# (also with tpu_mxu_units: 2) and common.h. Called by tests/tpuv6e/run.sh.
# Usage: tests/tpuv6e/functional/run.sh <build-dir> <spike> <pk>
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../../../setup/versions.env
source "$HERE/../../../setup/versions.env"
[ $# -ge 3 ] || { sed -n '2,4p' "${BASH_SOURCE[0]}" >&2; exit 2; }
BUILD=$(cd "$1" && pwd) || exit 2
SPIKE=$2 PK=$3
MODEL="$BUILD/libtpuv6e.so"
OUT="$BUILD/tpuv6e-functional"
MACHINE="$HERE/../../tpu/functional"
for needed in "$MODEL" "$SPIKE" "$PK" "$MACHINE/machine.yml" "$MACHINE/common.h"; do
  [ -e "$needed" ] || { echo "$needed: not found" >&2; exit 2; }
done
[ -d "$TOOLCHAIN_ROOT/bin" ] && PATH="$TOOLCHAIN_ROOT/bin:$PATH"
failed=0
rm -rf "$OUT" && mkdir -p "$OUT" || exit 2

# Where kernels.S is linked, and the range Spike runs in every lane.
KERNEL_BASE=0x2000000
KERNEL_RANGE=2000000:2100000

clang --target=riscv64 -march=rv64gcv_xsfvcp -c "$HERE/kernels.S" -o "$OUT/kernels.o" || { echo "FAIL  kernels.S does not build"; exit 2; }
riscv64-unknown-elf-gcc -O1 -Wall -Werror -static -march=rv64gc -mabi=lp64d -Wl,--section-start=.kernel=$KERNEL_BASE \
  -I "$MACHINE" "$HERE/mxu.c" "$OUT/kernels.o" -o "$OUT/mxu" || { echo "FAIL  mxu.c does not build"; exit 2; }

# on_model <name> -- <program> [arguments]: standard output to $OUT/<name>.out, on $MACHINE_YML if set.
on_model() {
  local name=$1; shift 2
  timeout 900 "$SPIKE" --extlib="$MODEL" --isa=rv64gcv_zfh_xvcixaccel --machine-config="${MACHINE_YML:-$MACHINE/machine.yml}" \
    --kernel-addr=$KERNEL_RANGE "$PK" "$OUT/$1" "${@:2}" < /dev/null > "$OUT/$name.out" 2> "$OUT/$name.err"
}

echo "-- the matrix unit, checked against a formula"
on_model mxu -- mxu; rc=$?
if [ "$rc" = 0 ]; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  operands as two registers, words of singles, halves and bfloat16, weights staged a half of the depth at a time and latched, each operand's upper half held apart in the v6e's orders, popped a half at a time: $(tail -n1 "$OUT/mxu.out") (exit $rc)"

# refused <which> <text>: the run ends there, with this on standard error.
refused() {
  local rc
  on_model "mxu-$1" -- mxu "$1"; rc=$?
  if [ "$rc" = 1 ] && grep -Fq -- "$2" "$OUT/mxu-$1.err" && ! grep -Fq "it ran" "$OUT/mxu-$1.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  refused: $2 (exit $rc)"
}
echo "-- what the matrix unit does not carry out"
refused pop "tpu: vpop: 1 rows asked of the upper half, 0 computed"
refused matmul "tpu: vmatmul: no vmatprep of its upper half came before it"
refused push "tpu: vmatpush: no vmatprep of its upper half came before it"
refused rows "tpu: vmatpush: 1 rows, and the vmatprep before it held 2"
refused weights "tpu: vmatmul: no weight was pushed before it"
refused latch "tpu: vmatmul: no weights were latched before it, and it does not latch"
refused depth "tpu: vmatpush: field 3 names no half of the depth: [4:3] is 1 or 2"
refused push3 "tpu: vmatpush: [4:3] 3 is the v6e's vmatpush3, which is not modelled"
refused prep-weights "vcix_cp: vmatprep waits, and nothing in its unit moves any more"
refused prep-inputs "vcix_cp: vmatprep waits, and nothing in its unit moves any more"
refused format "tpu: vmatprep: format 2 names nothing a word holds"
refused words "tpu: vmatpush: its elements are words of 32 bits, not of 16"
refused pop-array "tpu: vpop: array 1, and the machine has 1 (tpu_mxu_units)"

echo "-- the matrix unit with tpu_mxu_units: 2"
{ cat "$MACHINE/machine.yml"; echo "tpu_mxu_units: 2"; } > "$OUT/machine-mxu2.yml"
MACHINE_YML="$OUT/machine-mxu2.yml" on_model mxu-arrays -- mxu arrays; rc=$?
if [ "$rc" = 0 ]; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  two arrays, each its own tile and its own held halves, named in sf.vc.iv's rd and sf.vc.v.i's rs2, popped the other way round: $(tail -n1 "$OUT/mxu-arrays.out") (exit $rc)"
MACHINE_YML="$OUT/machine-mxu2.yml" refused array "tpu: vmatmul: array 2, and the machine has 2 (tpu_mxu_units)"

exit $failed
