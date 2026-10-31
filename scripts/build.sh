#!/usr/bin/env bash
# Configure and build this repository.
# Usage: scripts/build.sh [-j N] [build-dir]      (default: <repo>/build)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS=$(nproc)
if [ "${1:-}" = -j ]; then JOBS="$2"; shift 2; fi
BUILD="${1:-$REPO/build}"

# shellcheck source=../setup/versions.env
source "$REPO/setup/versions.env"
[ -x "$VERILATOR_BIN" ] && PATH="$VERILATOR_PREFIX/bin:$PATH"

cmake -G Ninja -S "$REPO" -B "$BUILD" -DTOOLCHAIN_ROOT="$TOOLCHAIN_ROOT"
# -k 0: a target that fails does not stop the others; the build still fails.
cmake --build "$BUILD" -j "$JOBS" -- -k 0
