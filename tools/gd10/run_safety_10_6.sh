#!/usr/bin/env bash
# ==========================================================================
# run_safety_10_6.sh — Phase 10.6: safety latches S1..S7 of the master's
# CiA402 layer, through SOEM / ecm_run against the virtual drive of soft_bus.
# Plan: claude/giai_doan_10_ke_hoach.md §8, docs/safety_boundary.md.
# Offline closed loop + negative controls of all seven latches:
#   make -C libecm_cia402 test negctl   (S-01..S-07, built with each latch off)
#
#   sudo -E tools/gd10/run_safety_10_6.sh
#
#   s01  S1 drive-side quick stop (DI) while enabled: axis error "left
#        Operation enabled", never walked back up; slave out of OP while
#        enabled (L5-05 path: safeop + ack + OP): slave back in OP, axis not
#        re-enabled
#   s02  S2 drive at 50000, ENABLE, no setpoints: the drive never moves
#   s04  S4 drive at 50000, app sends absolute 0: refused (step limit),
#        quick stop, drive never moves; negative control --axis-max-step 0:
#        the jump goes out, the drive moves to 0
#   s05  S5 bus lost (mute 200) while enabled: bus back, axis stays disabled
#        for 3 s, enabled once only
#   s06  S6 SIGINT / SIGTERM / --duration-sec while running a sine: the drive
#        sees 0x07, 0x06, 0x00 and is in Switch on disabled BEFORE the slave
#        leaves OP; negative control ecm_run_neg6 (latch off): no walk down,
#        the drive is left in Operation enabled (soft_bus steps on frames)
#   s07  S7 drive fault, no command for 2 s: stays in Fault, no reset
#        (0x80) sent; reset -> Switch on disabled, stays there
# (S3 is offline only: the hold of the last position is exact there.)
# Env: SOFT_BUS ECM_RUN ECM_RUN_NEG6 IF_M IF_S SB_PRIO SB_CPU
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
ECM_RUN_NEG6=${ECM_RUN_NEG6:-$ROOT/apps/ecm_run/ecm_run_neg6}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
STRICT=${STRICT:-0}
CASES=${CASES:-"s01 s02 s04 s05 s06 s07"}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof
ENI=$ROOT/config/eni/eni_mixed.enicfg
CTL=/tmp/soft_bus_10_6.ctl

LOG=${LOG:-log_gd10_safety_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0; INFO=0
ok   () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad  () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
info () { echo "  [INFO] $1"; INFO=$((INFO+1)); }
chk  () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
tchk () { if eval "$2"; then ok "$1"; elif [ "$STRICT" = 1 ]; then bad "$1"; else info "$1 -- not met (timing, STRICT=0)"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN" "$ECM_RUN_NEG6"; do [ -x "$f" ] || { echo "missing $f (make all, or make -C apps/ecm_run ecm_run_neg6)"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done
IS=$LOG/is620n_min_6502.prof          # reduced IS620N + 0x6502 (test assumption, see run_axes_10_3.sh)
{ cat "$PROF/is620n_min.prof"; echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro a5010000"; } > "$IS"

run () {   # run TAG N DURATION "soft_bus args" "ecm_run args" "script" STEP...
    local tag=$1 n=$2 dur=$3 sbargs=$4 eargs=$5 script=$6; shift 6
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n "$n" --dc 32 --no-sm-wd --ctl "$CTL" $sbargs > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "${ER:-$ECM_RUN}" --iface "$IF_M" --n "$n" --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" $eargs --hook cia402 --cia402-script "$script" > "$LOG/er_$tag.log" 2>&1 &
    local ep=$! t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        case $cmd in
            kill:*) kill -"${cmd#kill:}" $ep ;;
            *) echo "$cmd" > "$CTL" ;;
        esac
    done
    wait $ep; RC=$?
    sleep 0.3
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log; S=$LOG/sb_$tag.log
}
# tick of the first state event "-> STATE" of an axis, and of a script command
evt  () { grep "\[CIA402\] t=.* axis $1 .* -> $2 (" "$E" | grep -v "axis $1 $2 -> $2 (" | head -1 | grep -o 'tick=[0-9]*' | cut -d= -f2; }
cmdt () { grep "\[CIA402-APP\] t=.* axis $1: $2$" "$E" | head -1 | grep -o 'tick=[0-9]*' | cut -d= -f2; }
fin  () { grep "  \[CIA402\] axis $1 " "$E" | grep -o "$2=[-0-9a-zA-Z]*" | head -1 | cut -d= -f2; }
final_state () { grep "  \[CIA402\] axis $1 " "$E" | sed 's/.*): //; s/ sw=.*//'; }
never () { ! grep "\[CIA402\] t=.* axis $1 .* -> $2 (" "$E" | grep -qv "axis $1 $2 -> $2 ("; }


drvlog () { grep "cia402: node 0 axis 0" "$S"; }
# line number of the first drive log line matching $1 (0 = none)
dln () { drvlog | grep -n "$1" | head -1 | cut -d: -f1 | grep . || echo 0; }

for c in $CASES; do
case $c in
s01)
    echo "=== S-01 never enabled by the master"
    run s01 1 5 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "2:drv_quickstop 0 0"
    chk "S-01 drive-side quick stop: axis error 'left Operation enabled'" "grep -q 'ERROR left Operation enabled without a command' $E"
    chk "S-01 enabled once only, ends Switch on disabled" \
        "[ \$(grep -c 'axis 0 Switched on -> Operation enabled (' $E) = 1 ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    chk "S-01 the drive saw one Switched on -> Operation enabled" "[ \$(drvlog | grep -c 'Switched on -> Operation enabled') = 1 ]"
    run s01r 1 6 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "2:safeop 0 0x001A"
    chk "S-01 recovery: slave 1 back in OP" "grep -q 'slave 1 back in OP' $E"
    chk "S-01 recovery: enabled once only, not Operation enabled at the end" \
        "[ \$(drvlog | grep -c 'Switched on -> Operation enabled') = 1 ] && [ \"\$(final_state 0)\" != 'Operation enabled' ]"
    ;;
s02)
    echo "=== S-02 target = actual before enable"
    run s02 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp --axis-no-6502" "1 enable 0; 3 disable 0" "0.7:drv_pos 0 0 50000"
    chk "S-02 enabled" "[ \$(drvlog | grep -c 'Switched on -> Operation enabled') = 1 ]"
    chk "S-02 the drive never moved: pos 50000 at the end (drive and master)" \
        "drvlog | tail -1 | grep -q 'pos=50000 ' && [ \"\$(fin 0 pos)\" = 50000 ]"
    ;;
s04)
    echo "=== S-04 setpoint step limit"
    run s04 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp --axis-no-6502 --axis-max-step 1000" \
        "1 enable 0; 1.5 pos 0 0" "0.7:drv_pos 0 0 50000"
    chk "S-04 absolute 0 at 50000: axis error step, refused once" \
        "grep -q 'ERROR setpoint step above the limit' $E && grep -q 'S4 axis 0 step_refused=1 last_step=-50000' $E"
    chk "S-04 quick stop, drive never moved (pos 50000), ends Switch on disabled" \
        "drvlog | grep -q 'Operation enabled -> Quick stop active' && drvlog | tail -1 | grep -q 'pos=50000 ' && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    run s04n 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp --axis-no-6502 --axis-max-step 0" \
        "1 enable 0; 1.5 pos 0 0; 3 disable 0" "0.7:drv_pos 0 0 50000"
    chk "S-04 negative control (limit off): the jump goes out, the drive ends at 0" "drvlog | tail -1 | grep -q 'pos=0 '"
    ;;
s05)
    echo "=== S-05 bus lost latches disabled"
    run s05 1 7 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "2:mute 200"
    chk "S-05 the bus went LOST and came back to RUN" "grep '\[POLICY\] final bus state RUN' $E | grep -q 'LOST=[1-9]'"
    chk "S-05 axis error bus lost" "grep -q 'ERROR bus lost' $E"
    chk "S-05 enabled once only (drive side), Switch on disabled for the last ~4 s" \
        "[ \$(drvlog | grep -c 'Switched on -> Operation enabled') = 1 ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    ;;
s06)
    echo "=== S-06 shutdown sequence"
    for v in INT TERM dur neg; do
        er=$ECM_RUN; steps="2.5:kill:$v"; dur=20
        [ $v = dur ] && { steps=""; dur=3; }
        [ $v = neg ] && { er=$ECM_RUN_NEG6; steps="2.5:kill:INT"; }
        # shellcheck disable=SC2086
        ER=$er run s06_$v 1 $dur "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp --axis-no-6502" \
            "1 enable 0; 1.5 sine 0 2000 1" $steps
        L7=$(dln 'Operation enabled -> Switched on (controlword, cw 0x0007)')
        L6=$(dln 'Switched on -> Ready to switch on (controlword, cw 0x0006)')
        L0=$(dln 'Ready to switch on -> Switch on disabled (controlword, cw 0x0000)')
        LF=$(dln 'left OP while enabled')
        if [ $v = neg ]; then
            # soft_bus steps the drive on frames only: after SAFE-OP it is left
            # as it was -- a real drive would trip its SM watchdog while enabled
            chk "S-06 negative control (latch off, SIGINT): no walk down, the drive is left enabled or faults" \
                "[ $L7 = 0 ] && { [ $LF -gt 0 ] || drvlog | tail -1 | grep -q 'axis 0 Operation enabled sw='; }"
        else
            chk "S-06 $v: drive saw 0x07, 0x06, 0x00 in that order (lines $L7 $L6 $L0), no fault" \
                "[ $L7 -gt 0 ] && [ $L7 -lt $L6 ] && [ $L6 -lt $L0 ] && [ $LF = 0 ] && drvlog | tail -1 | grep -q 'axis 0 Switch on disabled sw='"
            chk "S-06 $v: ecm_run walked every axis down before leaving OP" "grep -q 'shutdown (S6): every axis walked down' $E"
        fi
    done
    ;;
s07)
    echo "=== S-07 fault reset only on command"
    run s07 1 5 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "2:drv_fault 0 0 0x2310" "2.2:drv_clear 0 0"
    chk "S-07 no command: still Fault at the end, no reset sent" \
        "[ \"\$(final_state 0)\" = Fault ] && ! drvlog | grep -q 'Fault -> Switch on disabled'"
    run s07r 1 6 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0; 3 reset 0" "2:drv_fault 0 0 0x2310" "2.2:drv_clear 0 0"
    chk "S-07 reset -> Switch on disabled, enabled once only, stays disabled" \
        "drvlog | grep -q 'Fault -> Switch on disabled' && [ \$(drvlog | grep -c 'Switched on -> Operation enabled') = 1 ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail, $INFO info   (logs: $LOG)"
[ $FAIL = 0 ]
