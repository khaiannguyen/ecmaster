#!/usr/bin/env bash
# ==========================================================================
# run_eni_9_6.sh — Phase 9.6: loader for real (vendor / TwinCAT) ENIs,
# against soft_bus over veth. Plan: claude/giai_doan_9_ke_hoach.md §8.
#
#   sudo -E ./run_eni_9_6.sh              all cases
#   sudo -E CASES="e06 e08" ./run_eni_9_6.sh
#
#   e06   eni_8node_lrdlwr (TwinCAT "Use RD/WR instead of RW") -> refused
#         before the bus is touched: "LRD/LWR not supported, use LRW", not
#         one EtherCAT frame on the wire; also with --eni-allow-unknown-regcmd
#   e07   eni_mixed / eni_mixed_rev / eni_2servo (TwinCAT, P1 draft + IS620N):
#         the loader accepts every register InitCmd (0 refusals), takes the
#         IS620N state timeouts (SAFEOP/OP 9 s; mixed: 10 s from the P1 draft
#         Box), and on today's 8 x SOFTBUS-PD4 bus refuses at the identity
#         check naming every position -- a list of reasons, no crash, no
#         silence. OP on a mixed bus needs the soft_bus profiles of step 9.9.
#   e08   eni_8node_dc_sdo.xml + one register InitCmd edited in by hand
#         (slave 3, PS, FPWR 0x0420 = 1000, a PD watchdog the ESI asks for):
#         refused before the bus is touched, the line names slave 3, PS,
#         FPWR, 0x0420 and the data. Negative control: the unedited file
#         reaches OP. --eni-allow-unknown-regcmd: OP + the warning banner.
#   e09   slow slave: soft_bus --coe-delay-ms 1500 (every SDO download
#         response posted 1.5 s late), ENI eni_8node_dc_sdo with its one CoE
#         InitCmd (0x8000:01, PS):
#           e09a  InitCmd timeout 0 = SOEM default 700 ms -> refused before
#                 SAFE-OP (negative control: the delay really bites)
#           e09b  InitCmd Timeout 5000 ms in the ENI -> OP
#           e09c  timeout 0 + ecm_run --sdo-timeout-ms 5000 -> OP
#   e10   regression: golden ENI mode (E-05) + its negative control,
#         run_groups g06 (E-03d), run_coe c03 c04
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S, SB_PRIO SB_CPU (as run_coe.sh)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-}
CASES=${CASES:-"e06 e07 e08 e09 e10"}
ENI=$ROOT/config/eni
E2C=$ROOT/tools/eni/eni2cfg.py

LOG=${LOG:-log_gd9_eni_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
command -v tshark >/dev/null || { echo "tshark not installed"; exit 2; }
for i in "$IF_M" "$IF_S"; do      # Phase 9.5: the 14/13 run of 1/10 was a lost veth pair
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing (veth pairs do not survive a reboot):"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done

SB_WRAP=()
[ -n "$SB_PRIO" ] && SB_WRAP+=(chrt -f "$SB_PRIO")
[ -n "$SB_CPU" ]  && SB_WRAP+=(taskset -c "$SB_CPU")

sb_start () {  # name, args...
    local name=$1; shift
    ${SB_WRAP[@]+"${SB_WRAP[@]}"} "$SOFT_BUS" --iface "$IF_S" "$@" > "$LOG/sb_$name.log" 2>&1 &
    SBP=$!
    sleep 0.5
}
sb_stop () { kill -INT "$SBP" 2>/dev/null; wait "$SBP" 2>/dev/null; }

# run NAME ENICFG [ecm_run args...]: soft_bus 8 nodes (as the golden), pcap,
# ecm_run 2 s; leaves RC, E (log) and NFR (EtherCAT frames on the wire).
run () {
    local name=$1 cfg=$2; shift 2
    tshark -q -i "$IF_M" -F pcap -w "$LOG/$name.pcap" -f "ether proto 0x88a4" >/dev/null 2>&1 &
    local tsp=$!
    for _ in $(seq 50); do [ -s "$LOG/$name.pcap" ] && break; sleep 0.1; done
    sleep 1
    # shellcheck disable=SC2086
    "$ECM_RUN" --iface "$IF_M" ${RUN_ARGS:---n 8 --motion-slaves 4} --duration-sec 2 --no-tx-ts --eni "$cfg" "$@" \
        > "$LOG/er_$name.log" 2>&1
    RC=$?
    sleep 0.3; kill -INT $tsp; wait $tsp 2>/dev/null
    E=$LOG/er_$name.log
    NFR=$(tshark -r "$LOG/$name.pcap" 2>/dev/null | wc -l)
}

for c in $CASES; do
case $c in
e06)
    echo "=== E-06 eni_8node_lrdlwr: process data by LRD/LWR"
    sb_start e06 --n 8 --pdo-size 4 --dc 32 --no-sm-wd
    run e06 "$ENI/eni_8node_lrdlwr.enicfg"
    chk "E-06 refused (rc != 0) with 'LRD/LWR not supported, use LRW'" \
        "[ $RC != 0 ] && grep -q 'LRD/LWR not supported, use LRW' $E"
    chk "E-06 refused before the bus is touched (0 EtherCAT frames, got $NFR)" "[ $NFR = 0 ]"
    run e06b "$ENI/eni_8node_lrdlwr.enicfg" --eni-allow-unknown-regcmd
    chk "E-06 also refused with --eni-allow-unknown-regcmd (0 frames)" \
        "[ $RC != 0 ] && grep -q 'use LRW' $E && [ $NFR = 0 ]"
    sb_stop
    ;;
e07)
    for f in eni_mixed eni_mixed_rev eni_2servo; do
        echo "=== E-07 $f (TwinCAT, IS620N) on the 8 x SOFTBUS-PD4 bus"
        sb_start e07_$f --n 8 --pdo-size 4 --dc 32 --no-sm-wd
        RUN_ARGS="--n 2" run e07_$f "$ENI/$f.enicfg"
        sb_stop
        NREG=$(grep -c '^reg ' "$ENI/$f.enicfg")
        chk "E-07 $f: loader accepts all $NREG register InitCmds" \
            "grep -q \"$NREG register InitCmd(s) checked\" $E && ! grep -q 'register InitCmd .*: not done' $E"
        if [ $f = eni_2servo ]; then W="SAFEOP 9000 ms, OP 9000 ms"; else W="SAFEOP 10000 ms, OP 10000 ms"; fi
        chk "E-07 $f: state timeouts from the ENI ($W)" "grep -q '$W' $E"
        chk "E-07 $f: refused at the identity check, every position named, no crash" \
            "[ $RC = 1 ] && grep -q 'identity check FAILED' $E && grep -q 'slave count: ENI 2, bus 8' $E && grep -q 'position 1 ' $E && grep -q 'position 2 ' $E"
    done
    ;;
e08)
    echo "=== E-08 hand-edited ENI: one unknown register InitCmd (slave 3, PS, FPWR 0x0420)"
    python3 - "$ENI/eni_8node_dc_sdo.xml" "$LOG/eni_e08.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
tree = ET.parse(sys.argv[1]); root = tree.getroot()
slave = root.findall("Config/Slave")[2]
ic = ET.SubElement(slave.find("InitCmds"), "InitCmd")
for tag, val in (("Transition", "PS"), ("Comment", "set PD watchdog (hand-edited, E-08)"),
                 ("Timeout", "0"), ("Requires", "cycle"), ("Cmd", "5"), ("Adp", "0"),
                 ("Ado", "1056"), ("Data", "e803"), ("Retries", "3")):
    ET.SubElement(ic, tag).text = val
tree.write(sys.argv[2], encoding="utf-8", xml_declaration=True)
PY
    python3 "$E2C" "$LOG/eni_e08.xml" -o "$LOG/eni_e08.enicfg"
    chk "E-08 eni2cfg records it" "grep -q '^reg 3 trans PS cmd 5 ado 0x0420 len 2 data e803' $LOG/eni_e08.enicfg"
    sb_start e08 --n 8 --pdo-size 4 --dc 32 --no-sm-wd
    run e08 "$LOG/eni_e08.enicfg"
    chk "E-08 refused, the line names slave 3, PS, FPWR, 0x0420, data" \
        "[ $RC != 0 ] && grep -q 'slave 3 register InitCmd PS FPWR 0x0420 len 2 data e803' $E"
    chk "E-08 exactly one refusal reason" "[ \$(grep -c 'register InitCmd' $E | tr -d ' ') -ge 1 ] && [ \$(grep -c 'ecm_eni: slave .* register InitCmd' $E) = 1 ]"
    chk "E-08 refused before the bus is touched (0 frames, got $NFR)" "[ $NFR = 0 ]"
    run e08n "$ENI/eni_8node_dc_sdo.enicfg"
    chk "E-08 negative control: the unedited ENI reaches OP (rc 0)" "[ $RC = 0 ]"
    run e08a "$LOG/eni_e08.enicfg" --eni-allow-unknown-regcmd
    chk "E-08 --eni-allow-unknown-regcmd: OP with the warning banner" \
        "[ $RC = 0 ] && grep -q 'NOT executed. Experiments only' $E && grep -q 'warning: slave 3 register InitCmd' $E"
    sb_stop
    ;;
e09)
    echo "=== E-09 slow slave: SDO download responses 1.5 s late"
    sed 's/^\(coe 1 trans PS .* index 0x8000 .*\) timeout_ms 0 /\1 timeout_ms 5000 /' \
        "$ENI/eni_8node_dc_sdo.enicfg" > "$LOG/eni_e09_t5000.enicfg"
    chk "E-09 test file: the InitCmd has Timeout 5000" "grep -q 'index 0x8000 .*timeout_ms 5000' $LOG/eni_e09_t5000.enicfg"
    sb_start e09 --n 8 --pdo-size 4 --dc 32 --no-sm-wd --coe-delay-ms 1500
    run e09a "$ENI/eni_8node_dc_sdo.enicfg"
    chk "E-09a timeout 0 (SOEM 700 ms): refused before SAFE-OP, 0x8000:01 named" \
        "[ $RC != 0 ] && grep -q 'refusing SAFE-OP' $E && grep -q 'CoE download 0x8000:01 FAILED' $E"
    sleep 2   # let soft_bus post the late response nobody waits for
    sb_stop
    sb_start e09b --n 8 --pdo-size 4 --dc 32 --no-sm-wd --coe-delay-ms 1500
    run e09b "$LOG/eni_e09_t5000.enicfg"
    chk "E-09b ENI Timeout 5000 ms: OP (rc 0), 0x8000:01 ok" \
        "[ $RC = 0 ] && grep -q 'CoE download 0x8000:01 ok' $E"
    sb_stop
    sb_start e09c --n 8 --pdo-size 4 --dc 32 --no-sm-wd --coe-delay-ms 1500
    run e09c "$ENI/eni_8node_dc_sdo.enicfg" --sdo-timeout-ms 5000
    chk "E-09c timeout 0 + --sdo-timeout-ms 5000: OP (rc 0)" \
        "[ $RC = 0 ] && grep -q 'CoE download 0x8000:01 ok' $E"
    sb_stop
    chk "E-09 soft_bus really delayed (held responses logged)" \
        "grep -q 'posted its held SDO download response' $LOG/sb_e09b.log"
    ;;
e10)
    echo "=== E-10 regression"
    G=$ROOT/tools/golden
    ENI=$ENI/eni_8node_dc_sdo.enicfg "$G/check_golden.sh" > "$LOG/e10_golden.log" 2>&1
    chk "E-10 golden ENI mode (E-05) identical" "[ $? = 0 ]"
    ENI=$ROOT/config/eni/eni_8node_dc.enicfg GOLDEN=$G/golden_ecm_run_eni.txt EXPECT_DIFF=1 \
        "$G/check_golden.sh" > "$LOG/e10_golden_neg.log" 2>&1
    chk "E-10 golden ENI negative control still detects the missing SDO" "[ $? = 0 ]"
    LOG=$LOG/e10_groups CASES=g06 "$HERE/run_groups.sh" > "$LOG/e10_groups.log" 2>&1
    chk "E-10 run_groups g06 (E-03d)" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/e10_groups.log"
    LOG=$LOG/e10_coe CASES="c03 c04" "$HERE/run_coe.sh" > "$LOG/e10_coe.log" 2>&1
    chk "E-10 run_coe c03 c04 (CA InitCmds, C-04 negative)" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/e10_coe.log"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
