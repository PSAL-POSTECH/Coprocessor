#!/usr/bin/env bash
# Build the environment from the pins in versions.env, then this repository; system.sh (as root) comes first.
# Usage: setup/setup.sh [-j N] [toolchain|pk|spike|gem5|verilator|repo ...]      (default: every step, nproc jobs)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
# shellcheck source=versions.env
source "$HERE/versions.env"

JOBS=$(nproc)
STEPS=()

while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS="$2"; shift 2 ;;
    -j*) JOBS="${1#-j}"; shift ;;
    -h|--help) sed -n '2,3p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
    -*) echo "unknown flag: $1" >&2; exit 2 ;;
    toolchain|pk|spike|gem5|verilator|repo) STEPS+=("$1"); shift ;;
    *) echo "unknown step: $1 (toolchain, pk, spike, gem5, verilator, repo)" >&2; exit 2 ;;
  esac
done
[ ${#STEPS[@]} -eq 0 ] && STEPS=(toolchain pk spike gem5 verilator repo)

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
want() { for s in "${STEPS[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

fetch_commit() {
  local dir="$1" repo="$2" sha="$3"
  mkdir -p "$dir"
  if [ ! -d "$dir/.git" ]; then
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$repo"
  fi
  git -C "$dir" remote set-url origin "$repo"
  if [ "$(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$sha" ]; then
    git -C "$dir" fetch -q --depth 1 origin "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
  fi
  echo "  $dir @ $(git -C "$dir" rev-parse HEAD)"
}

if want toolchain; then
  log "RISC-V toolchain"
  STAMP="$TOOLCHAIN_ROOT/.vcix-toolchain"
  if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$TOOLCHAIN_SHA256" ]; then
    echo "  already at $TOOLCHAIN_SHA256"
  else
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    curl -fSL --retry 3 -o "$TMP/toolchain.tar.xz" "$TOOLCHAIN_URL"
    echo "$TOOLCHAIN_SHA256  $TMP/toolchain.tar.xz" | sha256sum -c -
    rm -rf "$TOOLCHAIN_ROOT"; mkdir -p "$TOOLCHAIN_ROOT"
    tar -xJf "$TMP/toolchain.tar.xz" -C "$TOOLCHAIN_ROOT" --strip-components=1 --no-same-owner
    rm -rf "$TMP"; trap - EXIT
    echo "$TOOLCHAIN_SHA256" > "$STAMP"
  fi
  "$TOOLCHAIN_ROOT/bin/clang" --version | head -1
  "$TOOLCHAIN_ROOT/bin/riscv64-unknown-elf-gcc" --version | head -1
fi

if want pk; then
  log "riscv-pk"
  fetch_commit "$PK_ROOT" "$PK_REPO" "$PK_SHA"
  mkdir -p "$PK_ROOT/build"; cd "$PK_ROOT/build"
  export PATH="$TOOLCHAIN_ROOT/bin:$PATH"
  [ -f Makefile ] || ../configure --prefix="$PK_PREFIX" --host=riscv64-unknown-elf
  make -j"$JOBS"
  make install
  test -f "$PK_BIN" || { echo "pk did not land at $PK_BIN" >&2; exit 1; }
  echo "  $PK_BIN"
fi

if want spike; then
  log "spike"
  fetch_commit "$SPIKE_ROOT" "$SPIKE_REPO" "$SPIKE_SHA"
  mkdir -p "$SPIKE_BUILD"; cd "$SPIKE_BUILD"
  [ -f Makefile ] || ../configure --prefix="$SPIKE_PREFIX"
  make -j"$JOBS"
  make install
  test -x "$SPIKE_BIN" || { echo "spike did not land at $SPIKE_BIN" >&2; exit 1; }
  echo "  $SPIKE_BIN"
fi

if want gem5; then
  log "gem5 $GEM5_RELEASE"
  STAMP="$GEM5_ROOT/.vcix-gem5"
  if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$GEM5_SHA256" ]; then
    echo "  already at $GEM5_SHA256"
  else
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    curl -fSL --retry 3 -o "$TMP/gem5.tar.gz" "$GEM5_URL"
    echo "$GEM5_SHA256  $TMP/gem5.tar.gz" | sha256sum -c -
    rm -rf "$GEM5_ROOT"; mkdir -p "$GEM5_ROOT"
    tar -xzf "$TMP/gem5.tar.gz" -C "$GEM5_ROOT" --no-same-owner
    rm -rf "$TMP"; trap - EXIT
    echo "$GEM5_SHA256" > "$STAMP"
  fi
  test -x "$GEM5_BIN" || { echo "gem5 did not land at $GEM5_BIN" >&2; exit 1; }
  INFO="$(mktemp -d)"
  "$GEM5_BIN" --build-info > "$INFO/build-info"
  head -n 4 "$INFO/build-info"
  rm -rf "$INFO"
  echo "  $GEM5_BIN"
fi

if want verilator; then
  log "verilator"
  fetch_commit "$VERILATOR_SRC" "$VERILATOR_REPO" "$VERILATOR_SHA"
  cd "$VERILATOR_SRC"
  unset VERILATOR_ROOT
  [ -f configure ] || autoconf
  [ -f Makefile ] || ./configure --prefix="$VERILATOR_PREFIX"
  make -j"$JOBS"
  make install
  test -x "$VERILATOR_BIN" || { echo "verilator did not land at $VERILATOR_BIN" >&2; exit 1; }
  "$VERILATOR_BIN" --version
fi

if want repo; then
  log "Coprocessor"
  "$REPO/scripts/build.sh" -j "$JOBS"
fi

log "done -- now run: tests/run.sh"
