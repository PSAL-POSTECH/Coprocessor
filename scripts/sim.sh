# shellcheck shell=bash
# Sourced by the run scripts: which simulators to use, and how each one is started.
# Reads the caller's positional arguments [build-dir [spike [pk [gem5.opt]]]]. A missing one ends a script with 2;
# sourced in a terminal, it returns 2 and the terminal stays open.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../setup/versions.env
source "$REPO/setup/versions.env"

BUILD="${1:-${BUILD:-$REPO/build}}"
SPIKE="${2:-${SPIKE:-$SPIKE_BIN}}"
PK="${3:-${PK:-$PK_BIN}}"
GEM5="${4:-${GEM5:-$GEM5_BIN}}"

for needed in "$BUILD" "$SPIKE" "$PK" "$GEM5"; do
  [ -e "$needed" ] && continue
  echo "$needed: not found -- run setup/setup.sh, or pass [build-dir [spike [pk [gem5.opt]]]]" >&2
  [[ $- == *i* ]] && return 2
  exit 2
done
BUILD="$(cd "$BUILD" && pwd)" || {
  echo "$BUILD: cannot resolve the build directory" >&2
  [[ $- == *i* ]] && return 2
  exit 2
}

[ -d "$TOOLCHAIN_ROOT/bin" ] && PATH="$TOOLCHAIN_ROOT/bin:$PATH"

# rv_program <source.S> <elf> [link options]
rv_program() {
  rm -f "$2" "$2.o" || return
  clang --target=riscv64 -march=rv64gcv_xsfvcp -c "$1" -o "$2.o" || return
  riscv64-unknown-elf-gcc -static "${@:3}" "$2.o" -o "$2" || return
}

# spike_run <model.so> <machine.yml, or ""> <elf> [spike options]
# A machine description says the lanes, the scratchpad and VLEN; without one, VLEN is given here.
spike_run() {
  local machine=(--varch=vlen:256,elen:64)
  [ -n "$2" ] && machine=("--machine-config=$2")
  "$SPIKE" --extlib="$1" --isa=rv64gcv_zfh_xcoprocessor "${machine[@]}" "${@:4}" "$PK" "$3"
}

# gem5_run <output-dir> <model.so> <machine.yml, or ""> <elf> [fixture options]
gem5_run() {
  local description=()
  [ -n "$3" ] && description=(--config "$3")
  "$GEM5" -d "$1" "$REPO/examples/gem5_se.py" --model "$2" "${description[@]}" "${@:5}" "$4"
}

# gem5_bare_run <output-dir> <model.so> <elf>
gem5_bare_run() {
  "$GEM5" -d "$1" "$REPO/tests/contract/gem5_bare.py" --model "$2" "$3"
}

# gem5_config <m5out> <section> <key>: the key's value in the section of the run's config.ini
gem5_config() {
  awk -v section="[$2]" -v key="$3=" '/^\[/ { in_section = ($0 == section) }
    in_section && index($0, key) == 1 { print substr($0, length(key) + 1); exit }' "$1/config.ini"
}

# gem5_window <m5out> <dump> <name>: the value of the stat named <name> in the dump-th dump, 0 if the dump does not print it
gem5_window() {
  awk -v want="$3" -v n="$2" '/Begin Simulation Statistics/ { s++ } s == n && $1 == want { v = $2 } END { print v + 0 }' \
    "$1/stats.txt"
}

# spike_illegal <log> [insn as 8 hex digits]
spike_illegal() {
  local insn=${2:-'[0-9a-f]{8}'}
  grep -Fxq 'An illegal instruction was executed!' "$1" &&
    grep -Eq "^pc [0-9a-f]+ va/inst 0*$insn sr [0-9a-f]+\$" "$1"
}

# gem5_illegal <log> [insn as 8 hex digits]
gem5_illegal() {
  local insn=${2:-'[0-9a-f]{8}'}
  grep -Eq "panic: Illegal instruction 0x[0-9a-f]*$insn at pc .*: no Coprocessor model owns it\$" "$1"
}
