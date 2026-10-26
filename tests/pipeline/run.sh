#!/usr/bin/env bash
# A model that accepts while others are in flight: gem5 commits each its latency after its issue. Usage: tests/pipeline/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
MODEL="$BUILD/libpipelined.so"
failed=0

rv_program "$HERE/burst.S" "$BUILD/burst" || { echo "FAIL  burst.S does not build"; exit 2; }

expect() {
  local what=$1 log=$2 want=$3 line=$4 got ok=0
  got=$(grep -Fxc -- "$line" "$log")
  [ "$got" = "$want" ] && ok=1
  if [ $ok = 1 ]; then echo "PASS  $what ($got of $want)"; else echo "FAIL  $what ($got of $want: $line)"; failed=1; fi
}

log="$BUILD/burst.gem5.log"
gem5_run "$BUILD/m5out-burst" "$MODEL" "" "$BUILD/burst" > "$log" 2>&1
expect "gem5 commits each of the six its latency after its issue" "$log" 6 "[model] commit, 10 cycles after its issue"

exit $failed
