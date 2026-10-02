#!/usr/bin/env bash
# ==========================================================================
# run_servo_10_2.sh — Phase 10.2: the virtual CiA402 drive of soft_bus seen
# through the real master path (SOEM, ecm_run, veth). Plan:
# claude/giai_doan_10_ke_hoach.md §4. The drive logic itself is tested
# offline (tools/soft_bus/test_cia402, A-01..A-03, A-06, A-07); this script
# checks that it works on the wire: PDO mapping by the ENI/scan, SAFE-OP /
# OP rules, EMCY through the mailbox, power cycle, 4 axes, mixed bus.
#
# ecm_run has no CiA402 layer yet (10.3..10.5): the controlword comes from
# --pdo-set (the same value every cycle) and the statusword is read back with
# --pdo-get at exit, so each case checks one end state.
#
#   sudo -E tools/gd10/run_servo_10_2.sh
#   sudo -E CASES="d01 d04" tools/gd10/run_servo_10_2.sh
#
#   d01   P1 + --cia402: OP, statusword 0x0250 (Switch on disabled, VE,
#         remote) -- the drive powers up and stays there
#   d02   controlword 0x0006 -> 0x0231 (Ready to switch on)
#   d03   controlword 0x000F from the start: stays 0x0250 (no shutdown first
#         -> never enabled; the S1 basis on the drive side)
#   d04   drv_fault while Ready: statusword 0x0218 (Fault), EMCY 0x2310 in
#         ecm_run (Phase 9.7 path), 0x603F = 0x2310
#   d05   4-axis drive (cia402_4ax.prof, objects + 0x800): axis 0 and 2 get
#         0x0006, axis 1 nothing -> 0x0231 / 0x0250 / 0x0231
#   d06   mixed bus P1 + IS620N (eni_mixed, --pdo-scan), drive on both:
#         P1 0x6041 = 0x0231 (0x0006 via 0x1600), IS620N 0x6041 = 0x0250
#         (0x1701/0x1B01, mode 8 from the ENI InitCmd)
#   d07   drop_node / restore_node of the drive while the master runs: the
#         drive powers up again (Not ready -> Switch on disabled) in the log
#   a04   --cia402 off: the same bus as d01 reads 0x0000 (inputs untouched)
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"d01 d02 d03 d04 d05 d06 d07 a04"}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof; IS=$PROF/is620n_min.prof; AX4=$PROF/cia402_4ax.prof
ENI=$ROOT/config/eni/eni_mixed.enicfg
CTL=/tmp/soft_bus_10_2.ctl

LOG=${LOG:-log_gd10_servo_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0
ok   () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad  () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk  () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done

# run TAG N DURATION "soft_bus args" "ecm_run args" STEP... ; STEP = "<s>:<ctl>"
run () {
    local tag=$1 n=$2 dur=$3 sbargs=$4 eargs=$5; shift 5
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n "$n" --dc 32 --no-sm-wd --ctl "$CTL" $sbargs > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "$ECM_RUN" --iface "$IF_M" --n "$n" --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" $eargs > "$LOG/er_$tag.log" 2>&1 &
    local ep=$! t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        echo "$cmd" > "$CTL"
    done
    wait $ep; RC=$?
    sleep 0.3
    echo "drv_status" > "$CTL" 2>/dev/null
    sleep 0.2
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log; S=$LOG/sb_$tag.log
}
getv () { grep -o "\[PDO\] get $1 = 0x[0-9A-Fa-f]*" "$E" | tail -1 | awk '{print $NF}'; }
is () { local v; v=$(getv "$1"); [ -n "$v" ] && [ $((v)) = $(($2)) ]; }

for c in $CASES; do
case $c in
d01)
    echo "=== D-01 P1 drive powers up: Switch on disabled"
    run d01 1 3 "--profile 1=$P1 --cia402 1" "--pdo-scan --pdo-get 1:0x6041:0,1:0x6061:0"
    chk "D-01 rc 0, OP" "[ $RC = 0 ] && grep -q 'all slaves in OPERATIONAL' $E"
    chk "D-01 soft_bus: node 1 is a virtual CiA402 drive" "grep -q 'node 1 = virtual CiA402 drive, 1 axis' $S"
    chk "D-01 drive log: Not ready -> Switch on disabled" "grep -q 'node 0 axis 0 Not ready to switch on -> Switch on disabled' $S"
    chk "D-01 0x6041 = 0x0250 (SOD | voltage enabled | remote)" "is 1:0x6041:0 0x0250"
    chk "D-01 0x6061 = 0 (no mode requested)" "is 1:0x6061:0 0"
    ;;
d02)
    echo "=== D-02 controlword 0x0006 -> Ready to switch on"
    run d02 1 3 "--profile 1=$P1 --cia402 1" "--pdo-scan --pdo-set 1:0x6040:0=0x0006 --pdo-get 1:0x6041:0"
    chk "D-02 0x6041 = 0x0231" "is 1:0x6041:0 0x0231"
    chk "D-02 exactly one transition SOD -> RTSO" "[ \$(grep -c 'Switch on disabled -> Ready to switch on' $S) = 1 ]"
    ;;
d03)
    echo "=== D-03 controlword 0x000F from the start: never enabled"
    run d03 1 3 "--profile 1=$P1 --cia402 1" "--pdo-scan --pdo-set 1:0x6040:0=0x000F --pdo-get 1:0x6041:0"
    chk "D-03 0x6041 = 0x0250 (still Switch on disabled)" "is 1:0x6041:0 0x0250"
    chk "D-03 no transition after power-on" "! grep -q -- '-> Ready to switch on\|-> Operation enabled' $S"
    ;;
d04)
    echo "=== D-04 drive fault: statusword, EMCY, 0x603F"
    run d04 1 4 "--profile 1=$P1 --cia402 1" \
        "--pdo-scan --pdo-set 1:0x6040:0=0x0006 --pdo-get 1:0x6041:0,1:0x603F:0" "2:drv_fault 0 0 0x2310"
    chk "D-04 0x6041 = 0x0218 (Fault | VE | remote)" "is 1:0x6041:0 0x0218"
    chk "D-04 0x603F = 0x2310" "is 1:0x603F:0 0x2310"
    chk "D-04 ecm_run received the EMCY from slave 1, code 0x2310" "grep -q '\[EMCY\] tick=[0-9]* slave 1: code 0x2310' $E"
    chk "D-04 drive log: Ready to switch on -> Fault" "grep -q 'Ready to switch on -> Fault (drv_fault' $S"
    ;;
d05)
    echo "=== D-05 four axes on one node (objects + 0x800)"
    run d05 1 3 "--profile 1=$AX4 --cia402 1:4" \
        "--pdo-scan --pdo-set 1:0x6040:0=6,1:0x7040:0=6 --pdo-get 1:0x6041:0,1:0x6841:0,1:0x7041:0,1:0x7841:0"
    chk "D-05 rc 0, OP" "[ $RC = 0 ] && grep -q 'all slaves in OPERATIONAL' $E"
    chk "D-05 axis 0 0x6041 = 0x0231" "is 1:0x6041:0 0x0231"
    chk "D-05 axis 1 0x6841 = 0x0250" "is 1:0x6841:0 0x0250"
    chk "D-05 axis 2 0x7041 = 0x0231" "is 1:0x7041:0 0x0231"
    chk "D-05 axis 3 0x7841 = 0x0250" "is 1:0x7841:0 0x0250"
    ;;
d06)
    echo "=== D-06 mixed bus P1 + IS620N, drive model on both"
    run d06 2 4 "--profile 1=$P1 --cia402 1 --profile 2=$IS --cia402 2" \
        "--eni $ENI --pdo-scan --pdo-set 1:0x6040:0=0x0006 --pdo-get 1:0x6041:0,2:0x6041:0"
    chk "D-06 rc 0, OP, ENI == bus" "[ $RC = 0 ] && grep -q 'all slaves in OPERATIONAL' $E && grep -q 'PDO table: ENI == bus' $E"
    chk "D-06 P1 0x6041 = 0x0231" "is 1:0x6041:0 0x0231"
    chk "D-06 IS620N 0x6041 = 0x0250 (0x1B01 byte 2)" "is 2:0x6041:0 0x0250"
    chk "D-06 IS620N mode 8 from the ENI InitCmd (0x6060 by SDO, not in 0x1701)" "grep -q 'node 1 axis 0 mode 0 -> 8' $S"
    ;;
d07)
    echo "=== D-07 power cycle of the drive node while the master runs"
    run d07 2 8 "--profile 1=$P1 --cia402 1 --profile 2=$P1 --cia402 2" "--pdo-scan" "2:drop_node 1" "3:restore_node 1"
    chk "D-07 node 1 powered up twice (start + restore)" \
        "[ \$(grep -c 'node 1 axis 0 Not ready to switch on -> Switch on disabled' $S) = 2 ]"
    chk "D-07 node 0 untouched (powered up once)" \
        "[ \$(grep -c 'node 0 axis 0 Not ready to switch on -> Switch on disabled' $S) = 1 ]"
    ;;
a04)
    echo "=== A-04 --cia402 off: inputs untouched"
    run a04 1 3 "--profile 1=$P1" "--pdo-scan --pdo-get 1:0x6041:0"
    chk "A-04 0x6041 = 0 (no drive model)" "is 1:0x6041:0 0"
    chk "A-04 no cia402 line in the soft_bus log" "! grep -q 'cia402:\|virtual CiA402' $S"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
