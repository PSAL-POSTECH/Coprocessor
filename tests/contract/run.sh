#!/usr/bin/env bash
# The rules of the interface, one section each. Usage: tests/contract/run.sh [build-dir [spike [pk [gem5.opt]]]]
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../../scripts/sim.sh"
OWNED=062541db
UNOWNED=0a2541db
failed=0
missed=()

run() {
  local sim=$1 name=$2 model=$3 description=$4 program=$5
  log="$BUILD/$name.$sim.log"
  if [ "$sim" = spike ]; then spike_run "$BUILD/lib$model.so" "$description" "$BUILD/$program" > "$log" 2>&1
  else gem5_run "$BUILD/m5out-$name" "$BUILD/lib$model.so" "$description" "$BUILD/$program" "${@:6}" > "$log" 2>&1; fi
  rc=$?
}
miss() { missed+=("$1"); }
ended() { [ "$rc" = "$1" ] || miss "exit $rc, not $1"; }
tally() {
  local got=$1 want=$2 text=$3
  if [ "$want" = + ]; then [ "$got" -ge 1 ]; else [ "$got" = "$want" ]; fi || miss "$got of $want: $text"
}
line() { tally "$(grep -Fxc -- "$2" "$log")" "$1" "$2"; }
part() { tally "$(grep -Fc -- "$2" "$log")" "$1" "$2"; }
verdict() {
  if [ ${#missed[@]} = 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; printf '      %s\n' "${missed[@]}"; failed=1; fi
  missed=()
}

for name in unowned pair forty queue speculated waited asleep vstart frm status; do
  options=(); [ $name = status ] && options=(-nostdlib -Wl,-N,-Ttext=0x80000000,--no-warn-rwx-segments)
  rv_program "$HERE/$name.S" "$BUILD/$name" "${options[@]}" || { echo "FAIL  $name.S does not build"; exit 2; }
done
# What Spike takes its machine from; gem5 has its own in its configuration.
MACHINE='vpu_num_lanes: 4\nvpu_spad_size_kb_per_lane: 128\nvpu_vector_length_bits: 256\n'
printf "${MACHINE}latency: 8 cycles\n" > "$BUILD/malformed.yml"
printf -- '- plain: 8\n' > "$BUILD/not_a_mapping.yml"
printf 'latency: 10\ndepth: 1\n' > "$BUILD/one_at_a_time.yml"
printf 'latency: 100\n' > "$BUILD/slow.yml"

echo "-- one header"
log="$BUILD/header.log"
cmp "$REPO/include/vcix_accel.h" "$SPIKE_ROOT/riscv/vcix_accel.h" > "$log" 2>&1; rc=$?
ended 0
verdict "Spike's copy of vcix_accel.h, in $SPIKE_ROOT/riscv, is this repository's"

echo "-- ownership"
for sim in spike gem5; do
  run $sim unowned reports "" unowned
  [ "$rc" != 0 ] && ${sim}_illegal "$log" $UNOWNED || miss "exit $rc, not as illegal instruction $UNOWNED"
  if [ $sim = spike ]; then
    line 1 "[model] execute $OWNED, 0 in flight, 0 commits seen"
    part 1 "[model] "
  else
    line + "[model] accept $OWNED, 0 in flight, 0 commits seen"
    line + "[model] issue $OWNED, 0 in flight, 0 commits seen"
    line 1 "[model] commit $OWNED, 0 in flight, 1 commits seen"
    part 0 "[model] execute "
  fi
  part 0 " $UNOWNED, "
  verdict "$sim: an owned instruction reaches the model once; one no model owns is an illegal instruction"
done

echo "-- machine description"
for sim in spike gem5; do
  run $sim values reports "$HERE/values.yml" pair
  ended 0
  grep -F '[config] ' "$log" | diff "$HERE/values.expected" - > "$log.diff" || miss "$(cat "$log.diff")"
  verdict "$sim hands the model each value as written, and the model calls its own f16_to_f32"

  run $sim malformed reports "$BUILD/malformed.yml" pair
  ended 1
  part 1 "reports: machine description: latency: '8 cycles' is not an unsigned decimal number"
  part 0 "[model] "
  verdict "$sim stops at a malformed number, naming the model, the key and the value"
done
for sim in spike gem5; do
  run $sim not_a_mapping reports "$BUILD/not_a_mapping.yml" pair
  ended 1
  part 0 "[config] "
  verdict "$sim refuses a description whose top level is not a mapping"
done

echo "-- model table and instances"
log="$BUILD/direct.log"
"$BUILD/direct" "$BUILD" > "$log" 2>&1; rc=$?
grep -E '^(PASS|FAIL)  ' "$log"
ended 0
line 1 "vcix_cp: the model cannot be made: no such unit can be built"
verdict "direct: every check above passed, and a constructor that throws says why"

log="$BUILD/port_stats.log"
"$BUILD/port_stats" > "$log" 2>&1; rc=$?
grep -E '^(PASS|FAIL)  ' "$log"
ended 0
verdict "port_stats: the statistics a model counts at its ports, every check above passed"

log="$BUILD/route_once.log"
"$BUILD/route_once" > "$log" 2>&1; rc=$?
grep -E '^(PASS|FAIL)  ' "$log"
ended 0
verdict "route_once: a vcix_uarch route function runs once per issue, its queued steps replayed"

probe() { log="$BUILD/probe-$1.log"; "$BUILD/timing_probe" "$BUILD/lib$1.so" $OWNED 2 > "$log" 2>&1; rc=$?; }
probe no_optional
ended 0
part 2 ": issued at "
verdict "probe runs a table written by hand, tick, ready and reset NULL"
probe other_abi
ended 1
part 0 ": issued at "
verdict "probe refuses a table of another ABI version"
for sim in spike gem5; do
  run $sim no_optional no_optional "" pair
  ended 0
  verdict "$sim runs a table written by hand, tick, ready and reset NULL"
done
for sim in spike gem5; do
  run $sim null_table null_table "" pair
  ended 1
  part 1 "libnull_table.so: vcix_accel_model() returned no table"
  verdict "$sim refuses a library that hands over no table"
done
run spike other_abi other_abi "" pair
ended 1
part 1 "libother_abi.so has ABI 12, Spike has 11"
verdict "spike refuses a table of another ABI version"
run spike cannot_be_made cannot_be_made "" pair
ended 1
line 1 "vcix_cp: the model cannot be made: no such unit can be built"
verdict "spike refuses a model whose constructor throws, after the model said why"

run gem5 units reports "$BUILD/one_at_a_time.yml" pair --units 2
ended 0
line 2 "[config] plain is absent"
line 2 "[model] commit $OWNED, 0 in flight, 1 commits seen"
part 0 " 2 commits seen"
verdict "gem5 with two units naming one library: each has its own instance, and each sees one commit"

echo "-- in flight"
run gem5 in-flight-4 reports "$BUILD/slow.yml" forty --max-in-flight 4
ended 0
part + "[model] issue $OWNED, 3 in flight, "
part 0 ", 4 in flight, "
line 1 "[model] commit $OWNED, 0 in flight, 40 commits seen"
verdict "gem5 with vcixMaxInFlight 4 asks the model with three in flight and never with four"

run gem5 in-flight-default reports "$BUILD/slow.yml" forty
ended 0
part + "[model] issue $OWNED, 4 in flight, "
line 1 "[model] commit $OWNED, 0 in flight, 40 commits seen"
part 0 "No space to push data into queue"
verdict "gem5 with the default bound lets the model go past four, with no queue warning"

echo "-- tick"
run gem5 queue timing "" queue --max-ticks 100000000
ended 0
line 1 "[model] issue command: 2 issued, 2 committed, 0 pops waiting, 0 ticks missing, 1 finished, the last 0 cycles ago"
line 1 "[model] issue command: 3 issued, 3 committed, 0 pops waiting, 0 ticks missing, 2 finished, the last 0 cycles ago"
verdict "gem5 issues a command a full queue held in the cycle whose tick finished one"

line 1 "[model] issue wait: 4 issued, 4 committed, 0 pops waiting, 0 ticks missing, 4 finished, the last 0 cycles ago"
verdict "gem5 issues a wait in the cycle whose tick emptied the queue"

part 8 "[model] issue "
part 8 " pops waiting, 0 ticks missing, "
verdict "gem5 ticks an instance in every cycle, the core idle or not: no issue sees a cycle without its tick"

echo "-- squash"
run gem5 speculated timing "" speculated --max-ticks 100000000
ended 0
line 1 "[model] issue wait: 0 issued, 0 committed, 0 pops waiting, 0 ticks missing, 0 finished"
line 1 "[model] issue wait: 1 issued, 0 committed, 0 pops waiting, 0 ticks missing, 0 finished"
line 1 "[model] issue wait: 2 issued, 0 committed, 0 pops waiting, 0 ticks missing, 0 finished"
line 1 "[model] issue wait: 1 issued, 1 committed, 0 pops waiting, 0 ticks missing, 0 finished"
part 4 "[model] issue "
verdict "gem5 takes back the two it issued on a wrong path: the issues, and the ticks and the commit since"

echo "-- result time unknown"
run gem5 waited timing "" waited --max-ticks 100000000
line 1 "[model] issue command: 1 issued, 0 committed, 1 pops waiting, 0 ticks missing, 0 finished"
verdict "gem5 issues what follows a pop while the pop waits for its result"

line 1 "[model] issue use: 2 issued, 2 committed, 0 pops waiting, 0 ticks missing, 1 finished, the last 0 cycles ago, the last pop ready 0 cycles ago"
verdict "gem5 issues the reader of a pop's register in the cycle the model says the result is ready"

line 1 "[model] commit pop, ready 0 cycles ago"
verdict "gem5 commits the pop in that cycle, not before"

ended 0
part 1 "[model] issue pop: 3 issued, 3 committed, 0 pops waiting, 0 ticks missing, "
part 1 "[model] issue use: 3 issued, 3 committed, 0 pops waiting, 0 ticks missing, "
verdict "gem5 frees the register of a pop squashed while it waited: its reader is issued and the program ends"

run gem5 asleep ready_only "" asleep --max-ticks 100000000
ended 0
line 1 "[model] issue use, 15 cycles after the pop"
verdict "gem5 asks ready in every cycle: a table without tick, and a core with nothing else to do"

run gem5 stuck timing "" asleep --ready-warn-cycles 100 --max-ticks 30000000
ended 1
part 1 " its accelerator model has not said its result is ready 100 cycles after its issue"
part 0 "[model] issue use: "
verdict "gem5 warns once of a result that is never ready"

echo "-- processor state"
run spike vstart print_args "" vstart
ended 0
verdict "spike: vstart reads 0 after a model's instruction that began with vstart 3"

run spike frm reports "" frm
ended 0
line 1 "[host] frm 2"
verdict "spike: a model reads a CSR through the host, as the program left it"

log="$BUILD/status.gem5.log"
gem5_bare_run "$BUILD/m5out-status" "$BUILD/libprint_args.so" "$BUILD/status" > "$log" 2>&1; rc=$?
ended 0
part 0 "[commit ] insn=2c2552db "
part 1 "[commit ] insn=2825c1db "
verdict "gem5: writing vd dirties VS, and FS off refuses only a form that reads f[rs1]"

exit $failed
