#!/usr/bin/env bash
# The tpu example's values on Spike: each program's output against what the Spike its units came from printed
# (from-old-spike.sha256), against this model's own where it has gone past that Spike (this-model.sha256), or against a
# formula. Called by tests/tpu/run.sh.
# Usage: tests/tpu/functional/run.sh <build-dir> <spike> <pk> [old spike: also run, outputs compared byte by byte]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../../../setup/versions.env
source "$HERE/../../../setup/versions.env"
[ $# -ge 3 ] || { sed -n '2,4p' "${BASH_SOURCE[0]}" >&2; exit 2; }
BUILD=$(cd "$1" && pwd) || exit 2
SPIKE=$2 PK=$3 OLD=${4:-}
MODEL="$BUILD/libtpu.so"
OUT="$BUILD/tpu-functional"
for needed in "$MODEL" "$SPIKE" "$PK" ${OLD:+"$OLD"}; do
  [ -e "$needed" ] || { echo "$needed: not found" >&2; exit 2; }
done
[ -d "$TOOLCHAIN_ROOT/bin" ] && PATH="$TOOLCHAIN_ROOT/bin:$PATH"
failed=0
rm -rf "$OUT" && mkdir -p "$OUT" || exit 2

# Where kernels.S is linked, and the range Spike runs in every lane.
KERNEL_BASE=0x2000000
KERNEL_RANGE=2000000:2100000

clang --target=riscv64 -march=rv64gcv_xsfvcp -c "$HERE/kernels.S" -o "$OUT/kernels.o" || { echo "FAIL  kernels.S does not build"; exit 2; }
for program in special systolic cross_lane msa dma refused pack unpack eup xlu_fold xlu_pattern; do
  riscv64-unknown-elf-gcc -O1 -Wall -Werror -static -march=rv64gc -mabi=lp64d -Wl,--section-start=.kernel=$KERNEL_BASE \
    "$HERE/$program.c" "$OUT/kernels.o" -o "$OUT/$program" || { echo "FAIL  $program.c does not build"; exit 2; }
done

# on_model <name> [--base-path=<dir>] -- <program> [arguments]: standard output to $OUT/<name>.out
on_model() {
  local name=$1 options=(); shift
  while [ "$1" != -- ]; do options+=("$1"); shift; done; shift
  timeout 900 "$SPIKE" --extlib="$MODEL" --isa=rv64gcv_zfh_xcoprocessor --machine-config="$HERE/machine.yml" \
    --kernel-addr=$KERNEL_RANGE "${options[@]}" "$PK" "$OUT/$1" "${@:2}" < /dev/null > "$OUT/$name.out" 2> "$OUT/$name.err"
}
# The same machine, as the old Spike takes it.
on_old() {
  local name=$1 options=(); shift
  while [ "$1" != -- ]; do options+=("$1"); shift; done; shift
  timeout 900 "$OLD" --isa=rv64gcv_zfh --varch=vlen:256,elen:64 --vectorlane-size=4 --scratchpad-size=$((512 * 1024)) \
    --scratchpad-base-vaddr=$((0xD0000000)) --kernel-addr=$KERNEL_RANGE "${options[@]}" "$PK" "$OUT/$1" "${@:2}" \
    < /dev/null > "$OUT/$name.old.out" 2> "$OUT/$name.old.err"
}
# same <name> <expected.sha256> <what> [--base-path] -- <program> [arguments]; --base-path: the old Spike's, which
# writes the indices of an indirect transfer under it
same() {
  local name=$1 expected=$2 what=$3 base=() rc; shift 3
  if [ "$1" = --base-path ]; then base=(--base-path); shift; fi
  shift
  on_model "$name" -- "$@"; rc=$?
  local sums; sums=$(cd "$OUT" && grep -F "  $name." "$HERE/$expected" | sha256sum --check --quiet 2>&1)
  if [ "$rc" = 0 ] && [ -z "$sums" ] && grep -Fq "  $name.out" "$HERE/$expected"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  $what (exit $rc, $(wc -c < "$OUT/$name.out") bytes${sums:+, $sums})"
  [ -n "$OLD" ] && [ "$expected" = from-old-spike.sha256 ] || return 0
  if [ ${#base[@]} != 0 ]; then mkdir -p "$OUT/$name.old.base/indirect_access"; base=("--base-path=$OUT/$name.old.base"); fi
  on_old "$name" "${base[@]}" -- "$@"; rc=$?
  if [ "$rc" = 0 ] && cmp -s "$OUT/$name.out" "$OUT/$name.old.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  the old Spike prints the same (exit $rc)"
}

# close <name> <what>: the program's output against what the old Spike printed (<name>.from-old-spike.xz), element
# by element and not bit for bit: the special functions are the host's libm, which differs a little from one host to
# another. A NaN is to be a NaN, of any sign or payload (x86-64 and AArch64 make different default NaNs); every other
# result within 4 ulps of the old one, each libm being a couple of ulps from the true value.
close() {
  local name=$1 what=$2 rc why within
  on_model "$name" -- "$name"; rc=$?
  why=$(python3 - "$OUT/$name.out" "$HERE/$name.from-old-spike.xz" <<'EOF'
import lzma, struct, sys
got, was = open(sys.argv[1], "rb").read(), lzma.open(sys.argv[2]).read()
if len(got) != len(was):
    sys.exit(f"{len(got)} bytes, the old Spike printed {len(was)}")
# special.c's runs: seven of halves, seven of singles, two of doubles, each the four lanes in turn
runs = [(2, 65536 // 4)] * 7 + [(4, 4096)] * 7 + [(8, 1024)] * 2
ULPS = 4
at, worst, beyond = 0, 0, 0
for size, count in runs:
    bits = size * 8
    exponent = {2: 0x7C00, 4: 0x7F800000, 8: 0x7FF0000000000000}[size]
    magnitude = (1 << (bits - 1)) - 1
    n = 4 * count
    fmt = "<" + {2: "H", 4: "I", 8: "Q"}[size] * n
    for a, b in zip(struct.unpack_from(fmt, got, at), struct.unpack_from(fmt, was, at)):
        nan_a = a & exponent == exponent and a & magnitude != exponent
        nan_b = b & exponent == exponent and b & magnitude != exponent
        if nan_a or nan_b:
            beyond += nan_a != nan_b
            continue
        order = lambda x: -(x & magnitude) if x >> (bits - 1) else x & magnitude
        apart = abs(order(a) - order(b))
        worst = max(worst, apart)
        beyond += apart > ULPS
    at += size * n
print(f"at most {worst} ulps apart, {beyond} beyond {ULPS}")
sys.exit(1 if beyond else 0)
EOF
)
  within=$?
  if [ "$rc" = 0 ] && [ "$within" = 0 ]; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  $what (exit $rc, $why)"
}

echo "-- the EUP, which the old Spike did not have"
on_model eup -- eup; rc=$?
if [ "$rc" = 0 ] && grep -q "^eup ok" "$OUT/eup.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  sfu_push and sfu_pop: two groups of 2^x popped in push order, every element exact ($(head -1 "$OUT/eup.out"), exit $rc)"

echo "-- as the old Spike did it"
close special "the seven special functions: every half, 4096 singles a lane, 1024 doubles a lane for vlog and vatan"
same systolic from-old-spike.sha256 "the systolic array: weights that slide, pops in pieces, every element width, both floats of 8 bits, every value of frm" -- systolic
same lanes from-old-spike.sha256 "vlane_idx at every element width" -- cross_lane lanes
same cross this-model.sha256 "the cross-lane unit: each of its 24 operations, with the patterns it reads, and a move's tile behind the last one's rest" -- cross_lane 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23
same msa from-old-spike.sha256 "the multi-precision array: singles, halves and both floats of 8 bits to a word, every width, pops at every element width" -- msa
same dma from-old-spike.sha256 "the DMA: 300 descriptors, an mvin and an mvout each" --base-path -- dma 0 300

# The same cases, each named by dma_index_key first, under a --base-path: the key moves nothing, and the model
# writes nothing there (the indices of an indirect transfer reach TOGSim as the compiler's runtime attributes).
keyed() {
  local base="$OUT/dma-keyed.base" rc; rm -rf "$base" && mkdir -p "$base/indirect_access"
  on_model dma-keyed "--base-path=$base" -- dma 0 300 keyed; rc=$?
  local written; written=$(find "$base" -type f | wc -l)
  if [ "$rc" = 0 ] && [ "$written" = 0 ] && cmp -s "$OUT/dma.out" "$OUT/dma-keyed.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  the DMA under dma_index_key and a --base-path: the data unchanged, $written file(s) written (exit $rc)"
}
keyed

echo "-- checked against a formula, which the old Spike has no answer for"
on_model systolic-bf16 -- systolic bf16; rc=$?
if [ "$rc" = 0 ]; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  bfloat16 weights and inputs (rs1 3) popped as singles: $(tail -n1 "$OUT/systolic-bf16.out") (exit $rc)"

on_model pack -- pack; rc=$?
if [ "$rc" = 0 ] && grep -Fq ", 0 wrong" "$OUT/pack.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  the bf16 pack (sf.vc.v.vv): $(tail -n1 "$OUT/pack.out") (exit $rc)"
on_model unpack -- unpack; rc=$?
if [ "$rc" = 0 ] && grep -Fq ", 0 wrong" "$OUT/unpack.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  the bf16 unpack (sf.vc.v.iv 8, 9): $(tail -n1 "$OUT/unpack.out") (exit $rc)"
on_model xlu_fold -- xlu_fold; rc=$?
if [ "$rc" = 0 ] && grep -Fxq "xlu_fold: all pass" "$OUT/xlu_fold.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  the cross-lane fold (sf.vc.ivv 2, max, sum, min, argmax, argmin; bf16 max, min): $(tail -n1 "$OUT/xlu_fold.out") (exit $rc)"
on_model xlu_pattern -- xlu_pattern; rc=$?
if [ "$rc" = 0 ] && grep -Fxq "xlu_pattern: all pass" "$OUT/xlu_pattern.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
echo "  the cross-lane set pattern (sf.vc.ivv 3): kept across pops, taken by each push as it issues: $(tail -n1 "$OUT/xlu_pattern.out") (exit $rc)"

# refused <which> <exit> <text>: the run ends there, with this on standard error.
refused() {
  local rc
  on_model "refused-$1" -- refused "$1"; rc=$?
  if [ "$rc" = "$2" ] && grep -Fq -- "$3" "$OUT/refused-$1.err" && ! grep -Fq "it ran" "$OUT/refused-$1.out"; then printf 'PASS'; else printf 'FAIL'; failed=1; fi
  echo "  refused: $3 (exit $rc)"
}
echo "-- what the model does not carry out"
refused doubles 1 "tpu: vexp: an element of 64 bits is not supported"
refused pop 1 "tpu: systolic pop: 1 elements asked, 0 computed"
refused input 1 "tpu: systolic input push: no weight was pushed before it"
refused format 1 "tpu: systolic: format 3 names no element of 8 bits"
refused bf16-pop 1 "tpu: systolic pop: a pop to bfloat16 is not modeled"
refused pop-format 1 "tpu: systolic: format 1 names no element of 32 bits"
refused pattern 201 "XLU ERROR: the pre stage walks 1 columns but lane 0 carries 0 pattern entries (SIMM5 16)"
refused xlu-pop 1 "tpu: xlu pop: 8 elements asked, 3 left"
refused bf16-sum 1 "tpu: xlu push_reduce: fold 1 of 16-bit elements; every fold takes f32, max and min also bf16"
refused msa-pop 1 "tpu: msa pop: 8 elements asked, 3 computed"
refused words 1 "tpu: msa push: its elements are words of 32 bits, not of 16"
refused overflow 200 "MVIN ERROR: Scratchpad address overflow: 0xd0080000"

exit $failed
