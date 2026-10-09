#!/usr/bin/env bash
# ==========================================================================
# run_recover_10_0.sh — Phase 10.0 R-07 follow-up: a power-cycled slave gets
# the ENI's PS CoE InitCmds again during recovery, and is NOT taken to OP
# when they fail. Found on the real IS620N drives (power loss at OP: 14/14
# InitCmds FAILED wkc=0, both drives back in OP anyway) and reproduced on
# soft_bus (restore_node). Virtual rig: 2 x IS620N (eni_2servo, --pdo-scan).
#
#   sudo -E tools/gd10/run_recover_10_0.sh
#   sudo -E CASES="rc01 rc03" tools/gd10/run_recover_10_0.sh
#
#   rc01  slave 2 power-cycled (drop_node 1 / restore_node 1): its 7 PS
#         InitCmds ok in the reconfiguration, first attempt, back in OP
#   rc02  both slaves power-cycled: bus LOST -> full-bus RECOVER (as with the
#         real drives), 14 InitCmds ok, 2/2 in OP
#   rc03  negative control of the fix: rc01 with --recover-cyclic-mbx (mailbox
#         left in SOEM's cyclic mode during the reconfiguration, the pre-0018
#         behaviour): InitCmds FAILED -> slave 2 FAILED(config), not in OP
#   rc04  refusal: rc01 + coe_delay 3000 ms on the fresh slave (InitCmds time
#         out): kept in PRE-OP, FAILED(config), its CiA402 axis gets the
#         configuration error; slave 1 and its axis untouched
#   rc05  without --eni (no InitCmds): power-cycled slave back in OP
#   rc06  negative control, full-bus (the real R-07 case): both slaves
#         power-cycled with --recover-cyclic-mbx: InitCmds FAILED on both ->
#         both FAILED(config), 0/2 in OP
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU CASES LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO-}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"rc01 rc02 rc03 rc04 rc05 rc06"}
ENI2=$ROOT/config/eni/eni_2servo.enicfg
CTL=/tmp/soft_bus_10_0r.ctl
LOG=${LOG:-log_gd10_recover_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done
IS=$LOG/is620n_min_6502.prof
{ cat "$ROOT/config/profiles/is620n_min.prof"; echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro a5010000"; } > "$IS"

ctl () { printf '%s\n' "$*" | dd of="$CTL" conv=nocreat,notrunc oflag=nonblock status=none; }

# run TAG DURATION "ecm_run args" "events: ctl commands and 'sleep N', ';'-separated"
run () {
    local tag=$1 dur=$2 eargs=$3 ev=$4
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    "${sb[@]}" --iface "$IF_S" --n 2 --dc 32 --no-sm-wd --ctl "$CTL" \
        --profile 1="$IS" --cia402 1 --profile 2="$IS" --cia402 2 > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "$ECM_RUN" --iface "$IF_M" --n 2 --pdo-scan --no-tx-ts --duration-sec "$dur" \
        --diag-file "$LOG/diag_$tag.txt" $eargs > "$LOG/er_$tag.log" 2>&1 &
    local erp=$!
    for _ in $(seq 1 100); do grep -q 'all slaves in OPERATIONAL' "$LOG/er_$tag.log" 2>/dev/null && break; sleep 0.1; done
    sleep 2
    local IFS=';' c
    for c in $ev; do
        c=$(echo "$c" | sed 's/^ *//; s/ *$//')
        case $c in sleep\ *) sleep "${c#sleep }" ;; "") ;; *) ctl "$c" ;; esac
    done
    unset IFS
    wait $erp; RC=$?
    sleep 0.3
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log; D=$LOG/diag_$tag.txt
}
# InitCmd lines of slave S once the bus was in OP (i.e. from recoveries)
after_rec () { awk '/all slaves in OPERATIONAL/{r=1} r' "$E" | grep -c "slave $1 CoE download .* $2"; }

for c in $CASES; do
case $c in
rc01)
    echo "=== RC-01 slave 2 power-cycled"
    run rc01 14 "--eni $ENI2" "drop_node 1; sleep 2; restore_node 1"
    chk "RC-01 rc 0" "[ $RC = 0 ]"
    chk "RC-01 7 InitCmds ok in the reconfiguration ($(after_rec 2 ok))" "[ \$(after_rec 2 ok) = 7 ]"
    chk "RC-01 no InitCmd FAILED" "! grep -q 'CoE download .* FAILED' $E"
    chk "RC-01 slave 2 back in OP, first attempt" \
        "grep -q 'slave 2 back in OP' $E && ! grep -q 'slave 2: .*attempt 2/5' $E"
    ;;
rc02)
    echo "=== RC-02 both slaves power-cycled"
    run rc02 16 "--eni $ENI2" "drop_node 1; drop_node 0; sleep 3; restore_node 0; restore_node 1"
    chk "RC-02 rc 0" "[ $RC = 0 ]"
    chk "RC-02 14 InitCmds ok in the reconfigurations ($(( $(after_rec 1 ok) + $(after_rec 2 ok) )))" \
        "[ \$(( \$(after_rec 1 ok) + \$(after_rec 2 ok) )) = 14 ]"
    chk "RC-02 no InitCmd FAILED" "! grep -q 'CoE download .* FAILED' $E"
    chk "RC-02 bus LOST -> full-bus RECOVER (as with the real drives), 2/2 in OP" \
        "grep -q 'LOST -> RECOVER' $E && grep -q '\[RECOVER\] .* 2/2 in OP' $E"
    ;;
rc06)
    echo "=== RC-06 negative control, full-bus: both slaves power-cycled with the mailbox left cyclic"
    run rc06 16 "--eni $ENI2 --recover-cyclic-mbx" "drop_node 1; drop_node 0; sleep 3; restore_node 0; restore_node 1"
    chk "RC-06 InitCmds FAILED on both slaves (the R-07 finding on the real drives)" \
        "[ \$(after_rec 1 FAILED) -ge 1 ] && [ \$(after_rec 2 FAILED) -ge 1 ]"
    chk "RC-06 both FAILED(config), 0/2 in OP after RECOVER" \
        "[ \$(grep -c 'ENI CoE InitCmd(s) failed during reconfiguration' $E) = 2 ] && grep -q '\[RECOVER\] .* 0/2 in OP' $E"
    ;;
rc03)
    echo "=== RC-03 negative control: mailbox left cyclic during the reconfiguration"
    run rc03 16 "--eni $ENI2 --recover-cyclic-mbx" "drop_node 1; sleep 2; restore_node 1"
    chk "RC-03 InitCmds FAILED in the reconfiguration (the bug the fix removes)" "[ \$(after_rec 2 FAILED) -ge 1 ]"
    chk "RC-03 slave 2 FAILED(config), never back in OP" \
        "grep -q 'slave 2: ENI CoE InitCmd(s) failed during reconfiguration' $E && ! grep -q 'slave 2 back in OP' $E"
    chk "RC-03 diag: slave 2 in PREOP at the end" "grep -q 'slave 2: AL state PREOP' $D"
    ;;
rc04)
    echo "=== RC-04 InitCmds time out on the fresh slave: refused"
    run rc04 16 "--eni $ENI2 --axis 1:0,2:0 --axis-modes csp --hook cia402" \
        "drop_node 1; sleep 2; restore_node 1; coe_delay 1 3000"
    chk "RC-04 rc 0 (ecm_run keeps running)" "[ $RC = 0 ]"
    chk "RC-04 InitCmds FAILED (timeout)" "[ \$(after_rec 2 FAILED) -ge 1 ]"
    chk "RC-04 slave 2 kept in PRE-OP, FAILED(config), not back in OP" \
        "grep -q 'state after = 0x02 PRE-OP (InitCmd failed)' $E && ! grep -q 'slave 2 back in OP' $E && grep -q 'slave 2: AL state PREOP' $D"
    chk "RC-04 axis 2:0 configuration error, axis 1:0 untouched" \
        "grep -q 'axis 2:0 (slave 2): slave refused its configuration -> axis disabled' $E && ! grep -q 'axis 1:0 (slave 1): .*axis disabled' $E"
    chk "RC-04 policy: slave 2 FAILED(config) at exit" "grep -q 'slave 2: .*FAILED(config)' $E"
    ;;
rc05)
    echo "=== RC-05 no ENI: power-cycled slave back in OP"
    run rc05 14 "" "drop_node 1; sleep 2; restore_node 1"
    chk "RC-05 rc 0, slave 2 back in OP, no refusal" \
        "[ $RC = 0 ] && grep -q 'slave 2 back in OP' $E && ! grep -q 'InitCmd(s) failed during reconfiguration' $E"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
