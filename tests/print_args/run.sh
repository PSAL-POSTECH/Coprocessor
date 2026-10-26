#!/usr/bin/env bash
# The print_args example on both simulators: it runs, and gem5's latency follows LMUL. Usage: tests/print_args/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
LOG="$BUILD/print_args.log"
failed=0

"$REPO/examples/print_args/run.sh" "$BUILD" "$SPIKE" "$PK" "$GEM5" > "$LOG" 2>&1; rc=$?

report() {
  if [ "$1" = 1 ]; then echo "PASS  $2"; else echo "FAIL  $2"; failed=1; fi
}

ok=0; [ "$rc" = 0 ] && ok=1
report $ok "print_args runs on both simulators (exit $rc)"

latency_of() {
  sed -n "s/^\[issue  \] insn=$1 .* cycle=\([0-9]*\) ready=\([0-9]*\)\$/\1 \2/p" "$LOG" | tail -n 1 | {
    read -r cycle ready && echo $((ready - cycle))
  }
}
m1=$(latency_of 2c2081db); m2=$(latency_of 2c23025b); custom1=$(latency_of 00b5002b)
ok=0; [ "$m1" = 7 ] && [ "$m2" = 14 ] && [ "$custom1" = 7 ] && ok=1
report $ok "gem5 latency follows LMUL only with a vector operand (m1 ${m1:-none}, m2 ${m2:-none}, custom-1 under m2 ${custom1:-none})"

[ "$failed" = 0 ] || { echo "---- $LOG"; cat "$LOG"; }
exit $failed
