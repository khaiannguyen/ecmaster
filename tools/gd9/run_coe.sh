#!/usr/bin/env bash
# ==========================================================================
# run_coe.sh — Giai doan 9.3: SDO Complete Access and normal/segmented
# download against soft_bus over veth. Plan: claude/giai_doan_9_ke_hoach.md §5.
#
#   sudo -E ./run_coe.sh                  all cases
#   sudo -E CASES="c02 l4" ./run_coe.sh
#
#   c02   SOEM maps process data three ways -- SII fallback (default
#         soft_bus), CoE without CA (--coe-pdo-od, ecx_readPDOmap), CoE with
#         CA (--coe-ca, ecx_readPDOmapCA) -- on N=8 and N=1: the IOmap and
#         the CYCLIC/SHUTDOWN structure are identical; the wire shows which
#         path SOEM took (pcap_coe.py)
#   c02b  --pdo-size 70 (three PDO entries 31+31+8 byte), N=2: IOmap
#         identical with and without --coe-ca, CA 0x1600 = 14 byte
#   l4    l4_test L4-01..L4-07 on default soft_bus; L4-08 (--ca) against
#         --coe-ca; negative: L4-08 against default soft_bus is refused with
#         abort 0x06010000
#   c03   ecm_run --eni tools/gd9/eni_8node_ca_test.enicfg (hand-written: CA
#         PDO assign InitCmds 0x1C12/0x1C13 on all 8 slaves + a 16 byte
#         normal download) against --coe-ca -> OP; on the wire every CA
#         download is answered without abort (the TwinCAT ENI of step 9.4
#         replaces this file)
#   c04   negative: the same file against default soft_bus -> refused before
#         SAFE-OP, every failure names its own slave, object and abort
#   c05   golden: default soft_bus still matches golden_ecm_run.txt;
#         --coe-ca matches golden_ecm_run_ca.txt and its CoE listing
#         golden_coe_ca.txt; negative: default soft_bus against the CA golden
#         differs
#   (C-01 / C-01-neg are offline: make -C tools/soft_bus test -> test_coe)
#
# Env: SOFT_BUS ECM_RUN L4_TEST IF_M IF_S
#      SB_PRIO SB_CPU  run soft_bus with chrt -f SB_PRIO / taskset -c SB_CPU
#                      (Jetson: SB_PRIO=79 SB_CPU=2; empty = normal process)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
L4_TEST=${L4_TEST:-$ROOT/apps/l4_test/l4_test}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-}
CASES=${CASES:-"c02 c02b l4 c03 c04 c05"}
ENI_CA=${ENI_CA:-$HERE/eni_8node_ca_test.enicfg}
GOLD=$ROOT/tools/golden

LOG=${LOG:-log_gd9_coe_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN" "$L4_TEST"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
command -v tshark >/dev/null || { echo "tshark not installed"; exit 2; }

SB_WRAP=()
[ -n "$SB_PRIO" ] && SB_WRAP+=(chrt -f "$SB_PRIO")
[ -n "$SB_CPU" ]  && SB_WRAP+=(taskset -c "$SB_CPU")
export SOFT_BUS ECM_RUN IF_M IF_S

sb_start () {  # name, args...
    local name=$1; shift
    ${SB_WRAP[@]+"${SB_WRAP[@]}"} "$SOFT_BUS" --iface "$IF_S" "$@" > "$LOG/sb_$name.log" 2>&1 &
    SBP=$!
    sleep 0.5
}
sb_stop () { kill -INT "$SBP" 2>/dev/null; wait "$SBP" 2>/dev/null; }

# Section of a pcap2struct.py structure file, [CYCLIC] to the end.
tail_sections () { sed -n '/^\[CYCLIC\]/,$p' "$1"; }
iomap_line () { grep -m1 "IOmap)" "$1"; }

for c in $CASES; do
case $c in
c02)
    for NN in 8 1; do
        if [ "$NN" = 8 ]; then GA="--motion-slaves 4"; else GA=""; fi
        echo "=== C-02 N=$NN: SII / CoE / CoE+CA process data mapping"
        for v in sii pdo ca; do
            case $v in sii) A="";; pdo) A="--coe-pdo-od";; ca) A="--coe-ca";; esac
            N=$NN GROUP_ARGS="$GA" SB_ARGS="$A" "$GOLD/capture_ecm_run.sh" "$LOG/c02_n${NN}_$v.pcap" \
                > "$LOG/c02_n${NN}_$v.struct" 2> "$LOG/c02_n${NN}_$v.err"
            echo $? > "$LOG/c02_n${NN}_$v.rc"
            cp /tmp/ecm_run_golden.log "$LOG/c02_n${NN}_$v.ecm_run.log" 2>/dev/null
            python3 "$HERE/pcap_coe.py" "$LOG/c02_n${NN}_$v.pcap" > "$LOG/c02_n${NN}_$v.coe"
        done
        P=$LOG/c02_n$NN
        for v in sii pdo ca; do chk "C-02 N=$NN $v: ecm_run rc 0 (OP, 2 s cyclic, INIT)" "[ \"\$(cat ${P}_$v.rc)\" = 0 ]"; done
        chk "C-02 N=$NN IOmap line identical (sii/pdo/ca)" \
            "[ \"\$(iomap_line ${P}_sii.ecm_run.log)\" = \"\$(iomap_line ${P}_pdo.ecm_run.log)\" ] && [ \"\$(iomap_line ${P}_sii.ecm_run.log)\" = \"\$(iomap_line ${P}_ca.ecm_run.log)\" ] && [ -n \"\$(iomap_line ${P}_sii.ecm_run.log)\" ]"
        echo "    $(iomap_line "${P}_ca.ecm_run.log")"
        chk "C-02 N=$NN CYCLIC+SHUTDOWN structure identical (sii/pdo/ca)" \
            "diff <(tail_sections ${P}_sii.struct) <(tail_sections ${P}_pdo.struct) >/dev/null && diff <(tail_sections ${P}_sii.struct) <(tail_sections ${P}_ca.struct) >/dev/null"
        chk "C-02 N=$NN sii: 0x1C00 aborted (0x06020000) on all $NN slave(s)" \
            "[ \$(grep -c 'ABORT     1C00:00 06020000' ${P}_sii.coe) = $NN ]"
        chk "C-02 N=$NN pdo: plain reads of 0x1C12:01 on all $NN, no CA request, no abort" \
            "[ \$(grep -cE 'req  [0-9A-F]{4} UP +1C12:01' ${P}_pdo.coe) = $NN ] && ! grep -q 'CA-' ${P}_pdo.coe && ! grep -q ABORT ${P}_pdo.coe"
        chk "C-02 N=$NN ca: 5 CA uploads per slave (1C00 1C12 1C13 1600 1A00), no plain UP, no abort" \
            "[ \$(grep -c 'req .* CA-UP' ${P}_ca.coe) = $((5 * NN)) ] && ! grep -qE 'req  [0-9A-F]{4} UP ' ${P}_ca.coe && ! grep -q ABORT ${P}_ca.coe"
        chk "C-02 N=$NN ca: CA 0x1C12 answered 4 byte expedited (SI0 16 bit + U16)" \
            "[ \$(grep -c 'CA-UP-EXP 1C12:00 4 byte' ${P}_ca.coe) = $NN ]"
    done
    ;;
c02b)
    echo "=== C-02b --pdo-size 70 N=2 (3 PDO entries)"
    for v in sii ca; do
        A=""; [ "$v" = ca ] && A="--coe-ca"
        sb_start c02b_$v --n 2 --pdo-size 70 --no-sm-wd $A
        tshark -q -i "$IF_M" -F pcap -w "$LOG/c02b_$v.pcap" -f "ether proto 0x88a4" >/dev/null 2>&1 &
        TSP=$!
        for _ in $(seq 50); do [ -s "$LOG/c02b_$v.pcap" ] && break; sleep 0.1; done
        sleep 1
        "$ECM_RUN" --iface "$IF_M" --n 2 --duration-sec 1 --no-tx-ts > "$LOG/er_c02b_$v.log" 2>&1
        echo $? > "$LOG/c02b_$v.rc"
        sleep 0.3; kill -INT $TSP; wait $TSP 2>/dev/null
        sb_stop
        python3 "$HERE/pcap_coe.py" "$LOG/c02b_$v.pcap" > "$LOG/c02b_$v.coe"
    done
    chk "C-02b both runs rc 0" "[ \"\$(cat $LOG/c02b_sii.rc)\$(cat $LOG/c02b_ca.rc)\" = 00 ]"
    chk "C-02b IOmap line identical" \
        "[ \"\$(iomap_line $LOG/er_c02b_sii.log)\" = \"\$(iomap_line $LOG/er_c02b_ca.log)\" ] && [ -n \"\$(iomap_line $LOG/er_c02b_ca.log)\" ]"
    echo "    $(iomap_line "$LOG/er_c02b_ca.log")"
    chk "C-02b CA 0x1600 = 14 byte (2 + 3 x U32) on both slaves" \
        "[ \$(grep -c 'CA-UP     1600:00 14 byte' $LOG/c02b_ca.coe) = 2 ]"
    ;;
l4)
    echo "=== L4 (with L4-07 download normal/segmented) and L4-08 Complete Access"
    sb_start l4 --n 8 --pdo-size 4 --no-sm-wd
    timeout --foreground 60 "$L4_TEST" --iface "$IF_M" --n 8 > "$LOG/l4_default.log" 2>&1
    sb_stop
    grep -E "^\[(PASS|FAIL)\]" "$LOG/l4_default.log" | sed 's/ -- .*//; s/^/    /'
    chk "L4 default soft_bus: 8 passed, 0 failed" "grep -q 'Summary: 8 passed, 0 failed' $LOG/l4_default.log"
    sb_start l4ca --n 8 --pdo-size 4 --no-sm-wd --coe-ca
    timeout --foreground 60 "$L4_TEST" --iface "$IF_M" --n 8 --ca > "$LOG/l4_ca.log" 2>&1
    sb_stop
    chk "L4 --ca vs --coe-ca: 11 passed, 0 failed" "grep -q 'Summary: 11 passed, 0 failed' $LOG/l4_ca.log"
    sb_start l4neg --n 8 --pdo-size 4 --no-sm-wd
    timeout --foreground 60 "$L4_TEST" --iface "$IF_M" --n 8 --ca > "$LOG/l4_caneg.log" 2>&1
    sb_stop
    chk "L4-08 neg: CA read without --coe-ca fails" "grep -q '\[FAIL\] L4-08 CA read' $LOG/l4_caneg.log"
    ;;
c03|c04)
    if [ "$c" = c03 ]; then A="--coe-ca"; else A=""; fi
    echo "=== ${c^^} --eni $(basename "$ENI_CA") against soft_bus ${A:-(default)}"
    sb_start $c --n 8 --pdo-size 4 --dc 32 --no-sm-wd $A
    tshark -q -i "$IF_M" -F pcap -w "$LOG/$c.pcap" -f "ether proto 0x88a4" >/dev/null 2>&1 &
    TSP=$!
    for _ in $(seq 50); do [ -s "$LOG/$c.pcap" ] && break; sleep 0.1; done
    sleep 1
    "$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 --duration-sec 2 --no-tx-ts --eni "$ENI_CA" \
        > "$LOG/er_$c.log" 2>&1
    RC=$?
    sleep 0.3; kill -INT $TSP; wait $TSP 2>/dev/null
    sb_stop
    python3 "$HERE/pcap_coe.py" "$LOG/$c.pcap" > "$LOG/$c.coe"
    E=$LOG/er_$c.log
    if [ "$c" = c03 ]; then
        chk "C-03 ecm_run rc 0 (OP with ENI CA InitCmds)" "[ $RC = 0 ]"
        chk "C-03 16 CA InitCmds ok (0x1C12/0x1C13 x 8)" \
            "[ \$(grep -c 'CoE download 0x1C1[23]:00 (complete access) ok (4 byte)' $E) = 16 ]"
        chk "C-03 16 byte normal download 0x8002 ok" "grep -q 'CoE download 0x8002:00 ok (16 byte)' $E"
        chk "C-03 wire: 16 CA download responses, no abort" \
            "[ \$(grep -cE 'resp .* CA-DOWN +1C1[23]:00' $LOG/$c.coe) = 16 ] && ! grep -q ABORT $LOG/$c.coe"
    else
        chk "C-04 ecm_run refuses (rc != 0) before SAFE-OP" "[ $RC != 0 ] && grep -q 'refusing SAFE-OP' $E"
        chk "C-04 16 CA InitCmds reported FAILED" \
            "[ \$(grep -c '(complete access) FAILED' $E) = 16 ]"
        chk "C-04 each failure names its own slave/object/abort (slave 1 and 8)" \
            "grep -q 'slave 1 0x1C12:00 abort 0x06010000' $E && grep -q 'slave 8 0x1C13:00 abort 0x06010000' $E"
        chk "C-04 no other abort reported (stale 0x1C00 errors not misattributed)" \
            "[ \$(grep -c 'abort 0x' $E) = 16 ]"
    fi
    ;;
c05)
    echo "=== C-05 golden: default unchanged, CA golden, negative control"
    chk "C-05 default soft_bus == golden_ecm_run.txt" \
        "\"$GOLD/check_golden.sh\" > $LOG/c05_default.txt 2>&1"
    chk "C-05 --coe-ca == golden_ecm_run_ca.txt" \
        "SB_ARGS=--coe-ca GOLDEN=$GOLD/golden_ecm_run_ca.txt PCAP=$LOG/c05_ca.pcap \"$GOLD/check_golden.sh\" > $LOG/c05_ca.txt 2>&1"
    python3 "$HERE/pcap_coe.py" "$LOG/c05_ca.pcap" | sort -u > "$LOG/c05_ca.coe"
    chk "C-05 --coe-ca CoE listing == golden_coe_ca.txt" "diff $GOLD/golden_coe_ca.txt $LOG/c05_ca.coe > $LOG/c05_coe.diff"
    chk "C-05 neg: default soft_bus against the CA golden differs" \
        "GOLDEN=$GOLD/golden_ecm_run_ca.txt EXPECT_DIFF=1 \"$GOLD/check_golden.sh\" > $LOG/c05_neg.txt 2>&1"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

[ -n "${SB_WRAP[*]:-}" ] && echo "NOTE: soft_bus ran as: ${SB_WRAP[*]}"
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ "$FAIL" = 0 ]
