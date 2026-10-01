#!/usr/bin/env bash
# ==========================================================================
# run_fresh_9_8.sh — Giai doan 9.8: input freshness per slave, against
# soft_bus over veth. Plan: claude/giai_doan_9_ke_hoach.md §10.
#
#   sudo -E ./run_fresh_9_8.sh            all cases
#   sudo -E CASES="f02a f02b" ./run_fresh_9_8.sh
#
# soft_bus --app-seq 0: every node puts a 16-bit counter at input byte 0
# that it increments every PD frame; ctl "stale <node> <n>" freezes one
# node's inputs for n PD frames while WKC stays correct (L5-09).
#
#   f01   regression: L5-09 + negative control through run_l5_io.sh
#         (--fresh-offset 0 = every slave, unchanged)
#   f02a  --fresh 1=0 (slave 1 only): freeze slave 1 -> detected for slave
#         1; startup says slaves 2..8 are NOT checked
#   f02b  --fresh 1=0,2=off: freeze slave 2 -> NOT detected (slave 2 off);
#         control f02bp: same freeze with --fresh all=0 -> detected, so the
#         injection itself works
#   f02c  --fresh 3=0:8 (8-bit counter, the low byte of soft_bus's): freeze
#         slave 3 -> detected; the 8-bit wrap (every 256 cycles) gives no
#         false REGRESSION in 6 s
#   f02d  refusals before SAFE-OP: counter outside the inputs (4=3:16 with 4
#         input bytes), bad syntax, slave number out of range
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU; SB_ARGS (default --no-sm-wd)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
SB_ARGS=${SB_ARGS---no-sm-wd}
CASES=${CASES:-"f01 f02a f02b f02c f02d"}
CTL=/tmp/soft_bus_9_8.ctl

LOG=${LOG:-log_gd9_fresh_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing (veth pairs do not survive a reboot):"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done

# run TAG DURATION "ecm_run args" STEP... ; STEP = "<s>:<ctl command>"
run () {
    local tag=$1 dur=$2 eargs=$3; shift 3
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n 8 --pdo-size 4 --dc 32 --app-seq 0 --ctl "$CTL" $SB_ARGS \
        > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" $eargs > "$LOG/er_$tag.log" 2>&1 &
    local ep=$! t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        echo "$cmd" > "$CTL"
    done
    wait $ep; RC=$?
    sleep 0.3
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log
}
stale_slaves () { grep -oE 'slave [0-9]+: inputs UNCHANGED' "$1" | awk '{print $2}' | tr -d ':' | sort -u | tr '\n' ' '; }

for c in $CASES; do
case $c in
f01)
    echo "=== F-01 regression: L5-09 (--fresh-offset 0) + negative control"
    (cd "$ROOT/apps/ecm_run" && LOG="$LOG/f01" ECM_ARGS="--no-tx-ts" CASES="l509 l509neg" ./run_l5_io.sh) \
        > "$LOG/f01.log" 2>&1
    chk "F-01 L5-09 + L5-09neg pass" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/f01.log"
    ;;
f02a)
    echo "=== F-02a --fresh 1=0: only slave 1 checked, slave 1 frozen"
    run f02a 6 "--fresh 1=0" "2:stale 0 60"
    chk "F-02a rc 0" "[ $RC = 0 ]"
    chk "F-02a startup: slave 1 checked (16-bit at byte 0)" "grep -q 'freshness slave 1: 16-bit counter at input byte 0' $E"
    chk "F-02a startup: slaves 2..8 OFF, said so" "[ \$(grep -c 'freshness slave [2-8]: OFF -- \"WKC correct but data old\" is NOT detected' $E) = 7 ]"
    chk "F-02a slave 1 detected and back to normal" \
        "grep -q 'slave 1: inputs UNCHANGED' $E && grep -q 'slave 1: inputs changing again' $E"
    chk "F-02a no other slave reported" "[ '$(stale_slaves "$E")' = '1 ' ]"
    ;;
f02b)
    echo "=== F-02b --fresh 1=0,2=off: slave 2 frozen -> not detected (off)"
    run f02b 6 "--fresh 1=0,2=off" "2:stale 1 60"
    chk "F-02b rc 0" "[ $RC = 0 ]"
    chk "F-02b freeze really happened (soft_bus)" "grep -q 'stale' $LOG/sb_f02b.log"
    chk "F-02b nothing detected: slave 2 is off, slave 1 not frozen" "! grep -q 'inputs UNCHANGED' $E"
    chk "F-02b summary has no slave 2 line" "! grep -q '\[FRESH\] slave 2:' $E"
    echo "=== F-02bp control: same freeze, --fresh all=0"
    run f02bp 6 "--fresh all=0" "2:stale 1 60"
    chk "F-02bp slave 2 detected with all=0 (the injection works)" "[ '$(stale_slaves "$E")' = '2 ' ]"
    ;;
f02c)
    echo "=== F-02c --fresh 3=0:8 (8-bit counter), slave 3 frozen"
    run f02c 6 "--fresh 3=0:8" "2:stale 2 60"
    chk "F-02c rc 0" "[ $RC = 0 ]"
    chk "F-02c startup: slave 3 8-bit at byte 0" "grep -q 'freshness slave 3: 8-bit counter at input byte 0' $E"
    chk "F-02c slave 3 detected" "[ '$(stale_slaves "$E")' = '3 ' ]"
    chk "F-02c 8-bit wrap gives no REGRESSION" "! grep -q 'inputs went BACKWARDS' $E && grep -q '\[FRESH\] total: samples=[1-9][0-9]* stale_episodes=1 regressions=0' $E"
    ;;
f02d)
    echo "=== F-02d refusals"
    run f02d1 2 "--fresh all=0,4=3:16"
    chk "F-02d counter outside slave 4's inputs: refused, named" \
        "[ $RC != 0 ] && grep -q 'freshness slave 4: 16-bit counter at input byte 3 needs 5 input byte(s), the slave maps 4' $E"
    run f02d2 2 "--fresh 2=foo"
    chk "F-02d bad value: refused" "[ $RC != 0 ] && grep -q \"expected off or BYTE\" $E"
    run f02d3 2 "--fresh 9=0"
    chk "F-02d slave 9 on an 8-slave bus: refused" "[ $RC != 0 ] && grep -q 'must be within 1..8' $E"
    chk "F-02d none of them reached SAFE-OP" "! grep -q 'Failed to reach\\|DC on' $LOG/er_f02d1.log $LOG/er_f02d2.log $LOG/er_f02d3.log"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
