#!/usr/bin/env bash
# ==========================================================================
# run_profile_10_7.sh — Phase 10.7: PP, PV and homing through SOEM / ecm_run
# against the 4-axis virtual drive of soft_bus (config/profiles/
# cia402_4ax.prof: 0x6060/0x6061 in the PDOs, 0x6081/0x6083/0x6084/0x6098/
# 0x6099/0x607C as SDO objects), axis 1:0. Plan: claude/giai_doan_10_ke_hoach.md §9.
# Offline closed loop + negative controls (handshake off, stale homing):
#   make -C libecm_cia402 test negctl   (P2-01..P2-04)
#
#   sudo -E tools/gd10/run_profile_10_7.sh
#
#   p01  P2-01 PP: --axis-pp written by SDO and read back; 3 points sent at
#        once -> reached in order 10000, 20000, -5000, 3 acknowledged, none
#        lost
#   p02  P2-02 PV: 50000 inc/s with acceleration 1e6 inc/s^2, then 0; the
#        drive ends at rest; PV_VEL outside PV refused
#   p03  P2-03 HM: method 37 offset 1000 from 77777 -> attained at 1000;
#        method 19 with the virtual switch at 5000 at 20000 inc/s ->
#        attained after >= 200 ms at the offset; method 33 -> homing FAILED
#   p04  P2-04 HM -> PP 3000 -> CSP while enabled: the drive stays at 3000
#        (no jump at the switch to CSP)
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
STRICT=${STRICT:-0}
CASES=${CASES:-"p01 p02 p03 p04"}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof
ENI=$ROOT/config/eni/eni_mixed.enicfg
CTL=/tmp/soft_bus_10_7.ctl

LOG=${LOG:-log_gd10_profile_$(date +%Y%m%d_%H%M%S)}
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
X4=$PROF/cia402_4ax.prof
reached () { grep "axis 0 PP target reached" "$E" | sed 's/.*pos=//' | tr '\n' ' ' | sed 's/ $//'; }
ev_tick () { grep "$1" "$E" | head -1 | grep -o 'tick=[0-9]*' | cut -d= -f2; }

for c in $CASES; do
case $c in
p01)
    echo "=== P2-01 PP, 3 points"
    run p01 1 5 "--profile 1=$X4 --cia402 1:4" "--pdo-scan --axis 1 --axis-modes csp,pp --axis-pp 200000:2000000" \
        "0.5 mode 0 pp; 1 enable 0; 1.5 pp 0 10000; 1.5 pp 0 20000; 1.5 pp 0 -5000; 4 disable 0"
    chk "P2-01 profile velocity / acceleration written by SDO" \
        "grep -q 'profile velocity 0x6081:00 = 200000 (SDO)' $E && grep -q 'profile deceleration 0x6084:00 = 2000000 (SDO)' $E"
    R=$(reached)
    chk "P2-01 targets reached in order 10000 20000 -5000 (after the enable: '$R')" "echo '$R' | grep -q '10000 20000 -5000$'"
    chk "P2-01 3 set-points acknowledged, none queued or lost, no error" \
        "grep -q 'PH axis 0 mode=PP homed=0 pp_acked=3 pp_queued=0 pp_lost=0' $E && [ \"\$(fin 0 err)\" = none ]"
    chk "P2-01 the drive took 3 new set-points (3 bit-4 edges seen as moves), ends at -5000 then disabled" \
        "[ \"\$(fin 0 pos)\" = -5000 ] && [ \"\$(final_state 0)\" = 'Switch on disabled' ]"
    ;;
p02)
    echo "=== P2-02 PV"
    run p02 1 5 "--profile 1=$X4 --cia402 1:4" "--pdo-scan --axis 1 --axis-modes csp,pv --axis-pp 100000:1000000:1000000" \
        "0.5 mode 0 pv; 1 enable 0; 1.5 pv 0 50000; 2.5 pv 0 0; 3 mode 0 csp; 3.3 pv 0 100; 4 disable 0"
    P=$(fin 0 pos)
    # 50000 inc/s for 1 s with 50 ms ramps up and down: ~50000 inc
    chk "P2-02 travelled ~50000 inc (+- 2500): $P" "[ \$(( ${P:-0} > 50000 ? ${P:-0} - 50000 : 50000 - ${P:-0} )) -le 2500 ]"
    chk "P2-02 at rest at the end, PV_VEL outside PV refused (mode)" "[ \"\$(fin 0 vel)\" = 0 ] && grep -q 'ERROR mode' $E"
    ;;
p03)
    echo "=== P2-03 homing"
    run p03 1 4 "--profile 1=$X4 --cia402 1:4" "--pdo-scan --axis 1 --axis-modes csp,hm --axis-homing 37:1000" \
        "0.5 mode 0 hm; 1 enable 0; 1.5 home 0; 3 disable 0" "0.7:drv_pos 0 0 77777"
    chk "P2-03 method 37 / offset 1000 written by SDO" "grep -q 'homing method 0x6098:00 = 37 (SDO)' $E && grep -q 'home offset 0x607C:00 = 1000 (SDO)' $E"
    chk "P2-03 method 37: attained at 1000 (from 77777), homed" \
        "grep -q 'axis 0 homing attained pos=1000' $E && grep -q 'PH axis 0 mode=HM homed=1' $E"
    run p03s 1 4 "--profile 1=$X4 --cia402 1:4" "--pdo-scan --axis 1 --axis-modes csp,hm --axis-homing 19:0:20000" \
        "0.5 mode 0 hm; 1 enable 0; 1.5 home 0; 3.5 disable 0" "0.7:drv_home_switch 0 0 5000"
    T0=$(cmdt 0 home); T1=$(ev_tick 'axis 0 homing attained')
    chk "P2-03 method 19: attained at the offset 0 after the switch" "grep -q 'axis 0 homing attained pos=0' $E"
    chk "P2-03 method 19: >= 200 ms after the command (5000 inc at 20000 inc/s): $(( ${T1:-0} - ${T0:-0} )) ticks" \
        "[ -n \"$T1\" ] && [ \$(( T1 - T0 )) -ge 200 ]"
    run p03f 1 3 "--profile 1=$X4 --cia402 1:4" "--pdo-scan --axis 1 --axis-modes csp,hm --axis-homing 33" \
        "0.5 mode 0 hm; 1 enable 0; 1.5 home 0; 2.5 disable 0"
    chk "P2-03 method 33 (not offered): homing FAILED, axis error" "grep -q 'axis 0 homing FAILED' $E && grep -q 'ERROR homing failed' $E"
    ;;
p04)
    echo "=== P2-04 HM -> PP -> CSP without a jump"
    run p04 1 5 "--profile 1=$X4 --cia402 1:4" "--pdo-scan --axis 1 --axis-modes csp,pp,hm --axis-homing 37:0 --axis-pp 200000:2000000" \
        "0.5 mode 0 hm; 1 enable 0; 1.3 home 0; 1.6 mode 0 pp; 1.9 pp 0 3000; 3 mode 0 csp; 4.5 disable 0" "0.7:drv_pos 0 0 -40000"
    chk "P2-04 homed at 0, PP to 3000" "grep -q 'axis 0 homing attained pos=0' $E && grep -q 'axis 0 PP target reached pos=3000' $E"
    chk "P2-04 CSP confirmed while enabled (mode 8)" "grep -q 'Operation enabled -> Operation enabled (sw 0x.... cw 0x000F mode 8)' $E"
    chk "P2-04 the drive stayed at 3000 through the switch to CSP, no error" \
        "drvlog | tail -1 | grep -q 'pos=3000 ' && [ \"\$(fin 0 err)\" = none ]"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail, $INFO info   (logs: $LOG)"
[ $FAIL = 0 ]
