#!/usr/bin/env bash
# ==========================================================================
# run_dc_9_5.sh — Giai doan 9.5: DC per slave from the ENI, against soft_bus
# over veth. Plan: claude/giai_doan_9_ke_hoach.md §7.
#
#   sudo -E ./run_dc_9_5.sh               all cases
#   sudo -E CASES="dc01 dc03" ./run_dc_9_5.sh
#
# Every test ENI is made from config/eni/eni_8node_dc.xml (TwinCAT, 8 x
# SOFTBUS-PD4, DC SYNC0 1 ms on all) by tools/gd9/eni_dc_variant.py, which
# edits the DC element AND the matching register InitCmds the way TwinCAT
# writes them, then tools/eni/eni2cfg.py. What the master wrote is read from
# the wire (tools/gd9/pcap_dc.py: last FPWR of 0x0980/0x0981/0x0990/0x09A0/
# 0x09A4 per station, and the station the cyclic FRMW 0x0910 reads).
#
#   dc01  per slave: slaves 6-8 FreeRun (no DC in the ENI), slave 2 shift
#         +50 us, slave 3 shift -20 us -> OP; 1001..1005 get 0x0981 = 03 and
#         SYNC0 1 ms, 1006..1008 get no DC write at all; SYNC0 start time
#         mod cycle = the shift of each slave
#   dc02  reference clock from the ENI: slave 1 has no DC unit (soft_bus
#         --no-dc-nodes 1, like a coupler) and no DC in the ENI, ENI ref =
#         slave 2 -> OP, FRMW reads 1002, DC locks, soft_bus spread < 1 us
#   dc02n negative: the same ENI on a bus whose slave 1 IS DC capable ->
#         refused before SAFE-OP (SOEM's reference is the first DC slave)
#   dc03  SYNC1 on slave 2 (CycleTime1 200 us, AssignActivate 0x0700) -> OP;
#         1002: 0x0981 = 07, 0x09A4 = 200000; every other slave 03, no 09A4
#   dc03n negative: DC element says SYNC1 but the register InitCmds were not
#         changed (the ENI says two things) -> refused by the loader, the
#         line names slave 2 and 0x09A4
#   dc04  negative (plan DC-04): ENI reference clock = slave 1, the bus slave
#         1 has no DC unit -> refused before SAFE-OP, named
#   dc04b negative: AssignActivate 0x0100 on slave 3 -> refused, named
#   dc05  regression: golden default (547), golden ENI E-05 (573), golden
#         N=1, and L6 (run_l6_tests.sh 32 bit, 40 s = 4 checked windows) -- the soft_bus DC
#         report now follows the detected reference instead of node 0
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU (as run_coe.sh)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-}
CASES=${CASES:-"dc01 dc02 dc02n dc03 dc03n dc04 dc04b dc05"}
BASE=$ROOT/config/eni/eni_8node_dc.xml
VAR=$HERE/eni_dc_variant.py
E2C=$ROOT/tools/eni/eni2cfg.py
PDC=$HERE/pcap_dc.py

LOG=${LOG:-log_gd9_dc_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
command -v tshark >/dev/null || { echo "tshark not installed"; exit 2; }
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing (veth pairs do not survive a reboot):"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done

SB_WRAP=()
[ -n "$SB_PRIO" ] && SB_WRAP+=(chrt -f "$SB_PRIO")
[ -n "$SB_CPU" ]  && SB_WRAP+=(taskset -c "$SB_CPU")

# eni NAME [variant args...] -> $LOG/NAME.enicfg
eni () {
    local name=$1; shift
    python3 "$VAR" "$BASE" "$LOG/$name.xml" "$@" &&
        python3 "$E2C" "$LOG/$name.xml" -o "$LOG/$name.enicfg"
}

# run NAME "SOFT_BUS ARGS" ENICFG: soft_bus 8 nodes (as the golden) + pcap +
# ecm_run 2 s (3 s with REPORT=1). Leaves RC, E (ecm_run log), W (pcap_dc).
run () {
    local name=$1 sbargs=$2 cfg=$3 dur=2
    [ "${REPORT:-0}" = 1 ] && dur=4
    # shellcheck disable=SC2086
    ${SB_WRAP[@]+"${SB_WRAP[@]}"} "$SOFT_BUS" --iface "$IF_S" --n 8 --pdo-size 4 --dc 32 --no-sm-wd \
        --ctl none --dc-report-s 1 $sbargs > "$LOG/sb_$name.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    tshark -q -i "$IF_M" -F pcap -w "$LOG/$name.pcap" -f "ether proto 0x88a4" >/dev/null 2>&1 &
    local tsp=$!
    for _ in $(seq 50); do [ -s "$LOG/$name.pcap" ] && break; sleep 0.1; done
    sleep 1
    "$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 --duration-sec $dur --no-tx-ts --eni "$cfg" \
        > "$LOG/er_$name.log" 2>&1
    RC=$?
    sleep 0.3
    kill -INT $tsp 2>/dev/null; wait $tsp 2>/dev/null
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$name.log
    python3 "$PDC" "$LOG/$name.pcap" > "$LOG/$name.dc" 2>&1
    W=$LOG/$name.dc
}
st () { grep "^$1 " "$W"; }          # wire line of one station

for c in $CASES; do
case $c in
dc01)
    echo "=== DC-01 per slave: 6-8 FreeRun, shift +50 us on 2, -20 us on 3"
    eni dc01 --no-dc 6,7,8 --shift 2=50000 --shift 3=-20000
    run dc01 "" "$LOG/dc01.enicfg"
    sed 's/^/    /' "$W"
    chk "DC-01 OP (rc 0)" "[ $RC = 0 ]"
    chk "DC-01 1001..1005: 0x0981 = 03, SYNC0 1000000" \
        "[ \$(grep -cE '^100[1-5] 0980=00 0981=03 09A0=1000000 ' $W) = 5 ]"
    chk "DC-01 1006..1008 (FreeRun in the ENI): no DC register written" "! grep -qE '^100[678] ' $W"
    chk "DC-01 shift: 1002 +50000, 1003 -20000, 1001/1004/1005 0" \
        "st 1002 | grep -q 'shift=50000\$' && st 1003 | grep -q 'shift=-20000\$' && st 1001 | grep -q 'shift=0\$' && st 1004 | grep -q 'shift=0\$' && st 1005 | grep -q 'shift=0\$'"
    chk "DC-01 ref clock on the wire = 1001" "grep -q '^ref 1001' $W"
    ;;
dc02|dc02n)
    eni dc02 --no-dc 1 --ref 2
    if [ $c = dc02 ]; then
        echo "=== DC-02 ENI reference clock = slave 2 (slave 1 without DC unit)"
        REPORT=1 run dc02 "--no-dc-nodes 1" "$LOG/dc02.enicfg"
        sed 's/^/    /' "$W"
        chk "DC-02 OP (rc 0), DC on with ref=slave 2" "[ $RC = 0 ] && grep -q 'DC on -- ref=slave 2' $E"
        chk "DC-02 FRMW reads 1002 (wire)" "grep -q '^ref 1002' $W"
        chk "DC-02 slave 1 gets no DC write, 1002..1008 SYNC0" \
            "! grep -q '^1001 ' $W && [ \$(grep -cE '^100[2-8] .*0981=03' $W) = 7 ]"
        chk "DC-02 DC locks (master side)" "grep -q 'state=LOCKED' $E"
        SP=$(grep '^dcsync' "$LOG/sb_dc02.log" | tail -1 | grep -oE 'spread_ns_max=[0-9]+' | cut -d= -f2)
        R=$(grep '^dcsync' "$LOG/sb_dc02.log" | tail -1 | grep -oE 'ref=node[0-9]+')
        chk "DC-02 soft_bus: reference node1 (= slave 2), spread ${SP:-?} ns < 1000" \
            "[ '$R' = ref=node1 ] && [ '${SP:-99999}' -lt 1000 ]"
    else
        echo "=== DC-02n negative: same ENI, slave 1 of the bus IS DC capable"
        run dc02n "" "$LOG/dc02.enicfg"
        chk "DC-02n refused before SAFE-OP, names slave 2 / first DC slave 1" \
            "[ $RC != 0 ] && grep -q 'ENI reference clock is slave 2, but the first DC-capable slave on the bus is slave 1' $E && grep -q 'refusing SAFE-OP' $E"
        chk "DC-02n no SYNC0 armed anywhere" "! grep -q '0981=0[37]' $W"
    fi
    ;;
dc03)
    echo "=== DC-03 SYNC1 on slave 2 (CycleTime1 200 us, AssignActivate 0x0700)"
    eni dc03 --sync1 2=200000
    chk "DC-03 eni2cfg: slave 2 sync1_ns 200000 assign 0x0700" \
        "grep -q '^slave 2 .*sync1_ns 200000 .*assign 0x0700' $LOG/dc03.enicfg"
    run dc03 "" "$LOG/dc03.enicfg"
    sed 's/^/    /' "$W"
    chk "DC-03 OP (rc 0)" "[ $RC = 0 ]"
    chk "DC-03 1002: 0x0981 = 07, 0x09A4 = 200000" "st 1002 | grep -q '0981=07 09A0=1000000 09A4=200000'"
    chk "DC-03 the others: 0x0981 = 03, no 0x09A4" \
        "[ \$(grep -cE '^100[13-8] .*0981=03 09A0=1000000 09A4=- ' $W) = 7 ]"
    ;;
dc03n)
    echo "=== DC-03n negative: DC element says SYNC1, register InitCmds unchanged"
    eni dc03n --sync1 2=200000 --stale-initcmds
    run dc03n "" "$LOG/dc03n.enicfg"
    chk "DC-03n refused by the loader, names slave 2 and 0x09A4" \
        "[ $RC != 0 ] && grep -q 'slave 2: DC register InitCmd 0x09A4' $E"
    chk "DC-03n refused before the bus is touched (no DC write)" "! grep -qE '^1[0-9]{3} ' $W"
    ;;
dc04)
    echo "=== DC-04 negative: ENI reference clock slave 1, bus slave 1 has no DC unit"
    run dc04 "--no-dc-nodes 1" "$ROOT/config/eni/eni_8node_dc.enicfg"
    chk "DC-04 refused before SAFE-OP, names slave 1 as reference clock" \
        "[ $RC != 0 ] && grep -q 'ENI slave 1 .* uses DC as reference clock, but the slave on the bus is not DC capable' $E && grep -q 'refusing SAFE-OP' $E"
    ;;
dc04b)
    echo "=== DC-04b negative: AssignActivate 0x0100 on slave 3"
    eni dc04b --assign 3=0x0100
    run dc04b "" "$LOG/dc04b.enicfg"
    chk "DC-04b refused before SAFE-OP, names slave 3 and 0x0100" \
        "[ $RC != 0 ] && grep -q 'ENI slave 3 DC AssignActivate 0x0100 not supported' $E"
    ;;
dc05)
    echo "=== DC-05 regression"
    G=$ROOT/tools/golden
    "$G/check_golden.sh" > "$LOG/dc05_golden.log" 2>&1
    chk "DC-05 golden default identical" "[ $? = 0 ]"
    ENI=$ROOT/config/eni/eni_8node_dc_sdo.enicfg "$G/check_golden.sh" > "$LOG/dc05_golden_eni.log" 2>&1
    chk "DC-05 golden ENI (E-05) identical" "[ $? = 0 ]"
    N=1 GROUP_ARGS= GOLDEN=$G/golden_ecm_run_n1.txt "$G/check_golden.sh" > "$LOG/dc05_golden_n1.log" 2>&1
    chk "DC-05 golden N=1 identical" "[ $? = 0 ]"
    if [ -x "$ROOT/apps/l6_test/l6_test" ]; then
        (cd "$ROOT/apps/l6_test" && ./run_l6_tests.sh 32 40) > "$LOG/dc05_l6.log" 2>&1
        chk "DC-05 L6 (32 bit, 40 s): slave and master side pass" "[ $? = 0 ]"
    else
        echo "  [SKIP] L6: apps/l6_test/l6_test not built"
    fi
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
