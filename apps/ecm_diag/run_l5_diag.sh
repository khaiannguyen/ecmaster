#!/usr/bin/env bash
# ==========================================================================
# run_l5_diag.sh — L5-06 and L5-11: ecm_diag locates the fault.
#
# One ecm_run session (with diagnostics) against soft_bus, faults injected
# through soft_bus's control FIFO, ecm_diag snapshot graded after each step:
#   0. baseline           -> "findings: none"                 (negative control)
#   1. bad_cable <K> 5    -> "fault between slave K and slave K+1", and no
#                            other cable finding            (L5-06)
#   2. drop_node <D>      -> "chain broken after slave D"      (L5-11a)
#   3. restore_node <D>   -> "slave D+1 is on the bus ... ignores station
#                            address"                         (L5-11b diagnosis)
# ecm_run runs with --no-recover: this script grades DIAGNOSIS only. Since
# Giai doan 7.3 ecm_run would otherwise re-address the restored slave (path
# B) before the snapshot is taken; the recovery itself is graded by
# apps/ecm_run/run_l5_policy.sh (case l511b).
# soft_bus node k = SOEM slave k+1.
#
#   ./run_l5_diag.sh [extra ecm_run args, e.g. --no-tx-ts]
#
# Env: SOFT_BUS ECM_RUN ECM_DIAG SBCTL IF_M IF_S N K D
#      SB_PRIO / SB_CPU (soft_bus SCHED_FIFO + core, as in run_l6_tests.sh)
#      SETTLE (s after start, default 5), STEP (s between steps, default 3)
#      SB_ARGS (extra soft_bus args, e.g. "--no-sm-wd" on a non-RT host, where
#               the 3 ms motion watchdog trips on scheduling hiccups — say so
#               in the log if you use it)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SOFT_BUS=${SOFT_BUS:-$HERE/../../tools/soft_bus/soft_bus}
SBCTL=${SBCTL:-$HERE/../../tools/soft_bus/sbctl.sh}
ECM_RUN=${ECM_RUN:-$HERE/../ecm_run/ecm_run}
ECM_DIAG=${ECM_DIAG:-$HERE/ecm_diag}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
N=${N:-8}; K=${K:-5}; D=${D:-6}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
SETTLE=${SETTLE:-5}; STEP=${STEP:-3}; SB_ARGS=${SB_ARGS:-}
CTL=/tmp/soft_bus_l5diag.ctl
SNAP=/tmp/ecm_diag_l5diag.txt

LOG=log_l5_diag_$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

for f in "$SOFT_BUS" "$ECM_RUN" "$ECM_DIAG" "$SBCTL"; do
    [ -x "$f" ] || { echo "missing $f (build soft_bus, ecm_run, ecm_diag first)"; exit 2; }
done

rm -f "$CTL" "$SNAP"
SB_CMD=("$SOFT_BUS")
[ -n "$SB_PRIO" ] && SB_CMD=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
"${SB_CMD[@]}" --iface "$IF_S" --n "$N" --pdo-size 4 --dc 32 --dc-report-s 100 --ctl "$CTL" $SB_ARGS \
    > "$LOG/soft_bus.log" 2>&1 &
SB_PID=$!
sleep 0.5
kill -0 $SB_PID 2>/dev/null || { echo "soft_bus did not start, see $LOG/soft_bus.log"; exit 2; }

DUR=$(( SETTLE + 4 * STEP + 3 ))
timeout --foreground $((DUR + 60)) "$ECM_RUN" --iface "$IF_M" --n "$N" --motion-slaves $((N / 2)) \
    --duration-sec "$DUR" --diag-file "$SNAP" --no-recover "$@" > "$LOG/ecm_run.log" 2>&1 &
ECM_PID=$!
trap 'kill -INT $ECM_PID $SB_PID 2>/dev/null' EXIT

snap () {   # $1 = tag: save the snapshot as it is now
    "$ECM_DIAG" --file "$SNAP" > "$LOG/diag_$1.txt" 2>&1
    echo $? > "$LOG/diag_$1.rc"
}
ctl () { SB_CTL=$CTL "$SBCTL" "$@"; }

sleep "$SETTLE"; snap 0_baseline
ctl bad_cable "$K" 5;   sleep "$STEP"; snap 1_bad_cable
ctl drop_node "$D";     sleep "$STEP"; snap 2_drop
ctl restore_node "$D";  sleep "$STEP"; snap 3_restore

wait $ECM_PID; ECM_RC=$?
ctl status; sleep 0.3
kill -INT $SB_PID 2>/dev/null
for _ in $(seq 50); do kill -0 $SB_PID 2>/dev/null || break; sleep 0.1; done
trap - EXIT

[ -n "$SB_ARGS" ] && echo "NOTE: soft_bus ran with extra args: $SB_ARGS"
echo "=== L5 diag checks (N=$N, bad cable into node $K = slave $((K+1)), drop node $D = slave $((D+1))) ==="
f0=$LOG/diag_0_baseline.txt; f1=$LOG/diag_1_bad_cable.txt; f2=$LOG/diag_2_drop.txt; f3=$LOG/diag_3_restore.txt
grep -q "source=ecm_run" "$f0" && ok "snapshot written by ecm_run" || bad "no ecm_run snapshot ($f0)"
grep -q "^findings: none" "$f0" && [ "$(cat $LOG/diag_0_baseline.rc)" = 0 ] \
    && ok "baseline: no findings, exit 0 (negative control)" || bad "baseline not clean: $(sed -n '/findings/,$p' $f0 | head -3)"

want="fault between slave $K and slave $((K+1)):"
grep -q "$want" "$f1" && ok "L5-06: '$want'" || bad "L5-06: expected '$want'"
nc=$(grep -c "fault between\|fault on slave" "$f1")
[ "$nc" = 1 ] && ok "L5-06: exactly one cable finding (forwarded errors not blamed)" \
               || bad "L5-06: $nc cable findings"
# The fault lasts 5 frames; by the time of the snapshot it is history
# (WARN, "not in last read"). What must hold: the monitor reported it as
# [active] when it happened.
grep -q "$want.*\[active\]" "$LOG/ecm_run.log" && ok "L5-06: monitor reported it [active] when it happened" \
               || bad "L5-06: no [active] report in ecm_run.log"

want="chain broken after slave $D:"
grep -q "$want" "$f2" && ok "L5-11a: '$want'" || bad "L5-11a: expected '$want'"
grep -q "answering(BRD)=$D " "$f2" && ok "L5-11a: BRD counts $D slaves" || bad "L5-11a: BRD count wrong: $(grep answering $f2)"

want="slave $((D+1)) is on the bus (BRD counts it) but ignores station address"
grep -q "$want" "$f3" && ok "L5-11b: restored slave $((D+1)) detected as unconfigured" \
                        || bad "L5-11b: expected '$want'"
grep -q "chain broken" "$f3" && bad "L5-11b: chain still reported broken after restore" \
                             || ok "L5-11b: chain no longer broken"

grep -q "\[DIAG\] reads=" "$LOG/ecm_run.log" && ok "ecm_run final [DIAG] summary present" \
                                              || bad "no [DIAG] summary in ecm_run.log (rc=$ECM_RC)"
grep "\[DIAG\] reads=" "$LOG/ecm_run.log"

echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ "$FAIL" = 0 ]
