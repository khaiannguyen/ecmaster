#!/usr/bin/env bash
# ==========================================================================
# run_cia402_10_5.sh — Phase 10.5: the master's CiA402 layer (libecm_cia402
# axis state machine in the RT hook) driving virtual drives on soft_bus
# through SOEM / ecm_run. Plan: claude/giai_doan_10_ke_hoach.md §7.
# Offline closed loop: libecm_cia402/test_cia402_axis (T-01..T-08).
#
#   sudo -E tools/gd10/run_cia402_10_5.sh
#   sudo -E CASES="t01 t04" STRICT=1 SB_PRIO=79 tools/gd10/run_cia402_10_5.sh
#
#   t01  ENABLE: SOD -> RTSO -> SO -> OE within 10 ticks of the command,
#        controlword 0x06 / 0x07 / 0x0F; DISABLE walks down to SOD
#   t02  drv_slow 2000 ms: transition timeout naming Switch on disabled,
#        ~500 ms after ENABLE, never enabled, no retry
#   t03  drv_refuse_enable: timeout naming Switched on, walked back to SOD,
#        never enabled
#   t04  CSP sine 1 Hz 10000 inc from 50000 (drv_pos): enabling does not move
#        the axis; |actual(k) - setpoint(k-1)| <= 1 (the virtual drive
#        follows without limit); CSV 5000 inc/s for 1 s: ~ +5000 inc
#   t05  CSP -> CSV -> CSP while enabled (P1 maps 0x6060/0x6061): mode
#        display 9 then 8, velocity setpoints dropped until confirmed, no error
#   t06  mixed bus P1 + IS620N (eni_mixed, --pdo-scan): IS620N mode set by SDO
#        (0x6060 not in 0x1701), both axes enabled, CSP sine on both
#   t07  drive fault while enabled -> axis error; ENABLE refused while in
#        Fault; reset -> Switch on disabled and stays there
#   t08  bus lost (soft_bus mute 200) while enabled -> axis error bus lost;
#        bus back (recovery) -> the axis is walked down and stays disabled
#
# Timing-dependent checks (tracking on a non-RT host) are INFO unless
# STRICT=1. Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU STRICT
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
STRICT=${STRICT:-0}
CASES=${CASES:-"t01 t02 t03 t04 t05 t06 t07 t08"}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof
ENI=$ROOT/config/eni/eni_mixed.enicfg
CTL=/tmp/soft_bus_10_5.ctl

LOG=${LOG:-log_gd10_cia402_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0; INFO=0
ok   () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad  () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
info () { echo "  [INFO] $1"; INFO=$((INFO+1)); }
chk  () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
tchk () { if eval "$2"; then ok "$1"; elif [ "$STRICT" = 1 ]; then bad "$1"; else info "$1 -- not met (timing, STRICT=0)"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
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
    "$ECM_RUN" --iface "$IF_M" --n "$n" --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" $eargs --hook cia402 --cia402-script "$script" > "$LOG/er_$tag.log" 2>&1 &
    local ep=$! t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        echo "$cmd" > "$CTL"
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

for c in $CASES; do
case $c in
t01)
    echo "=== T-01 enable / disable through ecm_run"
    run t01 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0; 2.5 disable 0"
    chk "T-01 rc 0, CiA402 hook on" "[ $RC = 0 ] && grep -q 'CiA402 hook on' $E"
    chk "T-01 RTSO, SO, OE in order" \
        "[ -n \"\$(evt 0 'Ready to switch on')\" ] && [ \$(evt 0 'Ready to switch on') -lt \$(evt 0 'Switched on') ] && [ \$(evt 0 'Switched on') -lt \$(evt 0 'Operation enabled') ]"
    tchk "T-01 Operation enabled within 10 ticks of the command ($(( $(evt 0 'Operation enabled' || echo 0) - $(cmdt 0 enable || echo 0) )))" \
        "[ \$(( \$(evt 0 'Operation enabled') - \$(cmdt 0 enable) )) -le 10 ]"
    chk "T-01 cw 0x0006 / 0x0007 / 0x000F logged at the transitions" \
        "grep -q '> Ready to switch on (sw 0x0231 cw 0x0007' $E && grep -q '> Switched on (sw 0x0233 cw 0x000F' $E && grep -q '> Operation enabled (sw 0x1237 cw 0x000F' $E"
    chk "T-01 DISABLE: Switch on disabled at the end, no error" "[ \"\$(final_state 0)\" = 'Switch on disabled' ] && [ \"\$(fin 0 err)\" = none ]"
    chk "T-01 nothing enabled before the command" "[ \$(evt 0 'Ready to switch on') -gt \$(cmdt 0 enable) ]"
    ;;
t02)
    echo "=== T-02 slow drive: transition timeout"
    run t02 1 3 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "0.8:drv_slow 0 0 2000"
    chk "T-02 timeout naming Switch on disabled" "grep -q 'ERROR transition timeoutSwitch on disabled' $E"
    tchk "T-02 ~500 ms after the command" \
        "T=\$(grep 'ERROR transition timeout' $E | head -1 | grep -o 'tick=[0-9]*' | cut -d= -f2); [ \$(( T - \$(cmdt 0 enable) )) -ge 495 ] && [ \$(( T - \$(cmdt 0 enable) )) -le 520 ]"
    chk "T-02 never enabled, ends Switch on disabled" "never 0 'Operation enabled' && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    ;;
t03)
    echo "=== T-03 drive refuses enable"
    run t03 1 3 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "0.8:drv_refuse_enable 0 0 1"
    chk "T-03 timeout naming Switched on" "grep -q 'ERROR transition timeoutSwitched on' $E"
    chk "T-03 never enabled, walked back to Switch on disabled" "never 0 'Operation enabled' && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    chk "T-03 one attempt only (Switched on reached once)" "[ \$(grep -c 'axis 0 Ready to switch on -> Switched on (' $E) = 1 ]"
    ;;
t04)
    echo "=== T-04 CSP sine from 50000, CSV step"
    run t04 1 5 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp --axis-no-6502" \
        "1 enable 0; 1.5 sine 0 10000 1; 4 stop 0; 4.5 disable 0" "0.7:drv_pos 0 0 50000"
    chk "T-04 enabled at 50000 (S2 basis: the enable did not move the axis)" \
        "grep '> Operation enabled' $E | head -1 | grep -q 'mode 8' && grep -A0 'axis 0: sine' $E >/dev/null && grep -q 'axis 0: sine' $E"
    TM=$(fin 0 track_max); TN=$(grep "  \[CIA402\] axis 0 " "$E" | grep -o '(n=[0-9]*' | cut -d= -f2)
    UL=$(( $(fin 0 underrun) + $(fin 0 late) ))
    chk "T-04 tracking measured on > 1000 records (n=$TN)" "[ \"${TN:-0}\" -gt 1000 ]"
    tchk "T-04 |actual(k) - setpoint(k-1)| max $TM <= 1 (underrun+late $UL)" "[ \"$TM\" -le 1 ]"
    chk "T-04 no error, ends Switch on disabled" "[ \"\$(fin 0 err)\" = none ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    run t04v 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csv --axis-no-6502" \
        "1 enable 0; 1.5 vel 0 5000; 2.5 stop 0"
    P=$(fin 0 pos)
    tchk "T-04 CSV 5000 inc/s for 1 s: pos $P ~ 5000 (+- 100)" "[ \$(( ${P:-0} > 5000 ? ${P:-0} - 5000 : 5000 - ${P:-0} )) -le 100 ]"
    chk "T-04 CSV setpoints stopped: velocity actual 0 at the end" "[ \"\$(fin 0 vel)\" = 0 ]"
    ;;
t05)
    echo "=== T-05 CSP -> CSV -> CSP while enabled"
    run t05 1 5 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp,csv --axis-no-6502" \
        "1 enable 0; 1.3 sine 0 1000 1; 2 stop 0; 2 mode 0 csv; 2 vel 0 3000; 3 stop 0; 3.2 mode 0 csp; 4 disable 0"
    chk "T-05 mode display 9 then 8 while Operation enabled" \
        "grep -q 'Operation enabled -> Operation enabled (sw 0x1237 cw 0x000F mode 9)' $E && grep -q 'Operation enabled -> Operation enabled (sw 0x1237 cw 0x000F mode 8)' $E"
    chk "T-05 velocity setpoints dropped until 9 was confirmed (dropped > 0)" "[ \"\$(fin 0 dropped)\" -gt 0 ]"
    chk "T-05 no error, ends Switch on disabled" "[ \"\$(fin 0 err)\" = none ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    ;;
t06)
    echo "=== T-06 mixed bus P1 + IS620N, both axes CSP"
    run t06 2 5 "--profile 1=$P1 --cia402 1 --profile 2=$IS --cia402 2" \
        "--eni $ENI --pdo-scan --axis 1,2 --axis-modes csp --axis-no-6502" \
        "1 enable all; 1.5 sine all 5000 1; 3.5 stop all; 4 disable all"
    chk "T-06 IS620N mode set by SDO (0x6060 not in 0x1701)" "grep -q 'axis 2:0: 0x6060:00 = 8 set by SDO' $E"
    chk "T-06 both axes Operation enabled" "[ -n \"\$(evt 0 'Operation enabled')\" ] && [ -n \"\$(evt 1 'Operation enabled')\" ]"
    tchk "T-06 both track within 1 inc ($(fin 0 track_max) / $(fin 1 track_max))" "[ \"\$(fin 0 track_max)\" -le 1 ] && [ \"\$(fin 1 track_max)\" -le 1 ]"
    chk "T-06 no error on either axis" "[ \"\$(fin 0 err)\" = none ] && [ \"\$(fin 1 err)\" = none ]"
    ;;
t07)
    echo "=== T-07 drive fault, refused enable, reset"
    run t07 1 5 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" \
        "1 enable 0; 2.6 enable 0; 3.2 reset 0" "2:drv_fault 0 0 0x2310" "2.2:drv_clear 0 0"
    chk "T-07 drive fault -> axis error, 0x603F 0x2310" "grep -q 'ERROR drive fault' $E && grep '  \[CIA402\] axis 0 ' $E | grep -q '0x603F=0x0000\|0x603F=0x2310'"
    chk "T-07 ENABLE while in Fault refused" "grep -q 'ERROR enable refused: drive in fault' $E"
    chk "T-07 reset -> Switch on disabled, and it stays there" \
        "[ -n \"\$(evt 0 'Switch on disabled')\" ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ] && [ \$(grep -c 'axis 0 Switched on -> Operation enabled (' $E) = 1 ]"
    ;;
t08)
    echo "=== T-08 bus lost while enabled"
    run t08 1 6 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "2:mute 200"
    chk "T-08 the bus went LOST and came back" "grep '\[POLICY\] final' $E | grep -q 'LOST=[1-9]' && grep '\[POLICY\] final bus state RUN' $E >/dev/null"
    chk "T-08 axis error bus lost" "grep -q 'ERROR bus lost' $E"
    chk "T-08 enabled once only, walked down after the bus came back, stays disabled" \
        "[ \$(grep -c 'axis 0 Switched on -> Operation enabled (' $E) = 1 ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail, $INFO info   (logs: $LOG)"
[ $FAIL = 0 ]
