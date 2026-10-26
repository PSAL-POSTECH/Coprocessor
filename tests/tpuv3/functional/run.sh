#!/usr/bin/env bash
# The functional face of the tpuv3 example, on Spike: its matrix unit, of one register an operand, against a formula
# on 4 lanes and on 8, and libtpuv6e.so bound to the lanes on both; what it refuses. tests/tpu/functional's machine
# and common.h, tests/tpuv6e/functional's kernels. Usage: tests/tpuv3/functional/run.sh <build-dir> <spike> <pk>
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../../../setup/versions.env
source "$HERE/../../../setup/versions.env"
[ $# -ge 3 ] || { sed -n '2,4p' "${BASH_SOURCE[0]}" >&2; exit 2; }
BUILD=$(cd "$1" && pwd) || exit 2
SPIKE=$2 PK=$3
OUT="$BUILD/tpuv3-functional"
MACHINE="$HERE/../../tpu/functional"
KERNELS="$HERE/../../tpuv6e/functional/kernels.S"
for needed in "$BUILD/libtpuv3.so" "$BUILD/libtpuv6e.so" "$SPIKE" "$PK" "$MACHINE/machine.yml" "$MACHINE/common.h" "$KERNELS"; do
  [ -e "$needed" ] || { echo "$needed: not found" >&2; exit 2; }
done
[ -d "$TOOLCHAIN_ROOT/bin" ] && PATH="$TOOLCHAIN_ROOT/bin:$PATH"
failed=0
rm -rf "$OUT" && mkdir -p "$OUT" || exit 2

# Where kernels.S is linked, and the range Spike runs in every lane.
KERNEL_BASE=0x2000000
KERNEL_RANGE=2000000:2100000

clang --target=riscv64 -march=rv64gcv_xsfvcp -c "$KERNELS" -o "$OUT/kernels.o" || { echo "FAIL  kernels.S does not build"; exit 2; }
for width in 4 8; do
  riscv64-unknown-elf-gcc -O1 -Wall -Werror -static -march=rv64gc -mabi=lp64d -Wl,--section-start=.kernel=$KERNEL_BASE \
    -I "$MACHINE" -DWIDTH=$width "$HERE/mxu.c" "$OUT/kernels.o" -o "$OUT/mxu-$width" || { echo "FAIL  mxu.c does not build"; exit 2; }
done
# The machine of tests/tpu/functional with 8 lanes: mxu-8 is built for it.
printf 'vpu_num_lanes: 8\nvpu_spad_size_kb_per_lane: 512\nvpu_vector_length_bits: 256\n' > "$OUT/machine-8.yml"

# on_model <name> <model> <machine.yml> -- <program> [arguments]: standard output to $OUT/<name>.out.
on_model() {
  local name=$1 model=$2 machine=$3; shift 4
  timeout 900 "$SPIKE" --extlib="$BUILD/lib$model.so" --isa=rv64gcv_zfh_xcoprocessor --machine-config="$machine" \
    --kernel-addr=$KERNEL_RANGE "$PK" "$OUT/$1" "${@:2}" < /dev/null > "$OUT/$name.out" 2> "$OUT/$name.err"
}
# formula <name> <model> <machine.yml> <program> <what>: the run as the formula says, exit 0.
formula() {
  local rc
  on_model "$1" "$2" "$3" -- "$4"; rc=$?
  if [ "$rc" = 0 ]; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  $5: $(tail -n1 "$OUT/$1.out") (exit $rc)"
}
# refused <name> <model> <machine.yml> <which> <text>: the run of mxu-4 ends there, with this on standard error.
refused() {
  local rc
  on_model "$1" "$2" "$3" -- mxu-4 ${4:+"$4"}; rc=$?
  if [ "$rc" = 1 ] && grep -Fq -- "$5" "$OUT/$1.err" && ! grep -Fq "it ran" "$OUT/$1.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  refused, $2: $5 (exit $rc)"
}

echo "-- the matrix unit, checked against a formula"
formula mxu-4 tpuv3 "$MACHINE/machine.yml" mxu-4 \
  "4 lanes, an array of 4 x 4: an operand one register, words of singles, halves and bfloat16, weights staged and latched"
formula mxu-8 tpuv3 "$OUT/machine-8.yml" mxu-8 "8 lanes, an array of 8 x 8"

echo "-- libtpuv6e.so with tpu_mxu_bound_to_lanes: 1, an array of lanes x lanes: the same program, the same results"
{ cat "$MACHINE/machine.yml"; echo "tpu_mxu_bound_to_lanes: 1"; } > "$OUT/machine-bound.yml"
{ cat "$OUT/machine-8.yml"; echo "tpu_mxu_bound_to_lanes: 1"; } > "$OUT/machine-8-bound.yml"
formula tpuv6e-bound-4 tpuv6e "$OUT/machine-bound.yml" mxu-4 "4 lanes, an array of 4 x 4"
formula tpuv6e-bound-8 tpuv6e "$OUT/machine-8-bound.yml" mxu-8 "8 lanes, an array of 8 x 8"
for width in 4 8; do
  if cmp -s "$OUT/mxu-$width.out" "$OUT/tpuv6e-bound-$width.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  $width lanes: libtpuv6e.so bound writes what libtpuv3.so does, byte for byte"
done

echo "-- what the matrix unit does not carry out"
refused prep tpuv3 "$MACHINE/machine.yml" prep "tpu: vmatprep: an operand of an array of lanes x lanes is one register, with no upper half"
refused push2 tpuv3 "$MACHINE/machine.yml" push2 "tpu: vmatpush: [4:3] 2 names a second half of the depth, which an array of lanes x lanes has not"
refused pop-upper tpuv3 "$MACHINE/machine.yml" pop-upper "tpu: vpop: [2] names the upper half of the columns, which an array of lanes x lanes has not"
refused on-tpuv6e tpuv6e "$MACHINE/machine.yml" "" "tpu: vmatpush: no vmatprep of its upper half came before it"
refused tpuv6e-bound-prep tpuv6e "$OUT/machine-bound.yml" prep \
  "tpu: vmatprep: an operand of an array of lanes x lanes is one register, with no upper half"

exit $failed
