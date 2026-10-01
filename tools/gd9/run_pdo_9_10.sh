#!/usr/bin/env bash
# ==========================================================================
# run_pdo_9_10.sh — Giai doan 9.10: bind process data by (slave, index,
# sub), against soft_bus over veth. Plan: claude/giai_doan_9_ke_hoach.md §12.
#
#   sudo -E ./run_pdo_9_10.sh             all cases
#   sudo -E CASES="b01 b02" ./run_pdo_9_10.sh
#
#   b01   table from the ENI (eni_8node_dc_sdo) == table from a scan of the
#         same bus, both scan paths: SII PDO categories (default soft_bus)
#         and CoE 0x1C12/0x1C13 + 0x16xx/0x1Axx (soft_bus --coe-pdo-od)
#   b01c  no ENI, --pdo-size 70 (3 entries per direction, 248+248+64 bit):
#         the SII scan and the CoE scan give the same table
#   b01n  negative: an .enicfg whose PDO entries say 0x7001 for slave 2 ->
#         "PDO table of the ENI differs from the bus", refused
#   b02   write outputs by (slave, index, sub) on a motion slave (2) and an
#         IO slave (6): soft_bus sees exactly those bytes; read an input of
#         slave 3 that soft_bus sets (pdo_in) -> the value comes back
#   b02b  --pdo-size 70: set the 64-bit entry 0x7000:03 of slave 2 ->
#         soft_bus sees it in output bytes 62..69, little endian
#   b03   negative: bind an object that is not mapped (named, with what the
#         slave does map); --pdo-set on an input; no table at all
#   b04   offline: test_pdo_offline (bit offsets not byte aligned, 1..64 bit),
#         test_eni_offline (PDO records of the ENI)
#   (B-02 on the mixed virtual bus, P1 + IS620N, comes with step 9.9)
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
CASES=${CASES:-"b01 b01c b01n b02 b02b b03 b04"}
ENI=$ROOT/config/eni/eni_8node_dc_sdo.enicfg
CTL=/tmp/soft_bus_9_10.ctl

LOG=${LOG:-log_gd9_pdo_$(date +%Y%m%d_%H%M%S)}
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

# run TAG DURATION "soft_bus args" "ecm_run args" STEP... ; STEP = "<s>:<ctl>"
run () {
    local tag=$1 dur=$2 sbargs=$3 eargs=$4; shift 4
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n 8 --dc 32 --no-sm-wd --ctl "$CTL" $sbargs > "$LOG/sb_$tag.log" 2>&1 &
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
    E=$LOG/er_$tag.log; S=$LOG/sb_$tag.log
}
table () { grep '^pdo slave ' "$1"; }

for c in $CASES; do
case $c in
b01)
    for v in sii coe; do
        echo "=== B-01 ENI table == bus table, scan via $v"
        A=""; [ $v = coe ] && A="--coe-pdo-od"
        run b01_$v 2 "--pdo-size 4 $A" "--eni $ENI --pdo-scan --pdo-dump"
        chk "B-01 $v: rc 0" "[ $RC = 0 ]"
        chk "B-01 $v: every slave scanned from $v" "[ \$(grep -c 'ecm_pdo: slave [1-8]: 2 entries from $v' $E) = 8 ]"
        chk "B-01 $v: ENI == bus (16 entries)" "grep -q 'PDO table: ENI == bus (16 entries)' $E"
    done
    ;;
b01c)
    echo "=== B-01c no ENI, 3 entries per direction: SII scan == CoE scan"
    run b01c_sii 2 "--pdo-size 70" "--pdo-scan --pdo-dump"
    R1=$RC; table "$E" > "$LOG/b01c_sii.table"
    run b01c_coe 2 "--pdo-size 70 --coe-pdo-od" "--pdo-scan --pdo-dump"
    table "$E" > "$LOG/b01c_coe.table"
    chk "B-01c both rc 0" "[ $R1 = 0 ] && [ $RC = 0 ]"
    chk "B-01c 48 entries" "[ \$(wc -l < $LOG/b01c_sii.table) = 48 ]"
    chk "B-01c SII table == CoE table" "cmp -s $LOG/b01c_sii.table $LOG/b01c_coe.table"
    chk "B-01c slave 2 0x7000:03 is 64 bit at 496" "grep -q 'pdo slave 2 out PDO 0x1600 0x7000:03 64 bit @496' $LOG/b01c_sii.table"
    ;;
b01n)
    echo "=== B-01n negative: ENI PDO entries differ from the bus"
    sed 's/^pdo 2 dir out pdo 0x1600 index 0x7000 /pdo 2 dir out pdo 0x1600 index 0x7001 /' "$ENI" > "$LOG/eni_b01n.enicfg"
    chk "B-01n test file really edited" "grep -q '^pdo 2 dir out pdo 0x1600 index 0x7001' $LOG/eni_b01n.enicfg"
    run b01n 2 "--pdo-size 4" "--eni $LOG/eni_b01n.enicfg --pdo-scan"
    chk "B-01n refused, the differing entry named" \
        "[ $RC != 0 ] && grep -q 'PDO table of the ENI differs from the bus (1)' $E && grep -q 'slave 2 out PDO 0x1600 0x7001:01 32 bit @0  vs  slave 2 out PDO 0x1600 0x7000:01' $E"
    ;;
b02)
    echo "=== B-02 write outputs / read inputs by (slave, index, sub)"
    run b02 4 "--pdo-size 4" \
        "--eni $ENI --pdo-set 2:0x7000:1=0x11223344,6:0x7000:1=0xDEADBEEF --pdo-get 3:0x6000:1,2:0x7000:1" \
        "2:pdo_in 2 0 a1b2c3d4" "3:pdo_out 1" "3.2:pdo_out 5" "3.4:pdo_out 2"
    chk "B-02 rc 0" "[ $RC = 0 ]"
    chk "B-02 binds: slave 2 motion (group 1), slave 6 IO (group 2)" \
        "grep -q 'pdo set slave 2 0x7000:01 -> group 1 out' $E && grep -q 'pdo set slave 6 0x7000:01 -> group 2 out' $E"
    chk "B-02 soft_bus slave 2 outputs = 44 33 22 11" "grep -q 'pdo_out: node 1 (SOEM slave 2) 4 byte: 44 33 22 11' $S"
    chk "B-02 soft_bus slave 6 outputs = EF BE AD DE (IO group)" "grep -q 'pdo_out: node 5 (SOEM slave 6) 4 byte: EF BE AD DE' $S"
    chk "B-02 slave 3 untouched (00 00 00 00)" "grep -q 'pdo_out: node 2 (SOEM slave 3) 4 byte: 00 00 00 00' $S"
    chk "B-02 get 3:0x6000:1 = 0xD4C3B2A1 (set by soft_bus)" "grep -q '\[PDO\] get 3:0x6000:1 = 0xD4C3B2A1' $E"
    chk "B-02 get of an output reads back what the master writes" "grep -q '\[PDO\] get 2:0x7000:1 = 0x11223344' $E"
    ;;
b02b)
    echo "=== B-02b 64-bit entry 0x7000:03 of slave 2 (bit 496)"
    run b02b 3 "--pdo-size 70" "--pdo-scan --pdo-set 2:0x7000:3=0x0123456789ABCDEF" "2:pdo_out 1"
    chk "B-02b rc 0" "[ $RC = 0 ]"
    EXP="70 byte: $(printf '00 %.0s' $(seq 62))EF CD AB 89 67 45 23 01 "
    echo "$EXP" > "$LOG/b02b.expected"
    chk "B-02b soft_bus slave 2 output bytes 62..69 = EF CD AB 89 67 45 23 01, the rest 0" \
        "grep 'pdo_out: node 1 ' $S | grep -qF -f $LOG/b02b.expected"
    ;;
b03)
    echo "=== B-03 negative binds"
    run b03a 2 "--pdo-size 4" "--eni $ENI --pdo-get 3:0x6041:0"
    chk "B-03 not mapped: refused, names slave 3 and what it maps" \
        "[ $RC != 0 ] && grep -q 'slave 3 does not map 0x6041:00; it maps: out 0x7000:01 in 0x6000:01' $E"
    run b03b 2 "--pdo-size 4" "--eni $ENI --pdo-set 3:0x6000:1=5"
    chk "B-03 --pdo-set on an input: refused" "[ $RC != 0 ] && grep -q 'is an input, refusing' $E"
    run b03c 2 "--pdo-size 4" "--pdo-get 3:0x6000:1"
    chk "B-03 no table (no ENI, no scan): refused" "[ $RC != 0 ] && grep -q 'need a PDO table' $E"
    chk "B-03 none reached SAFE-OP" "! grep -q 'DC on' $LOG/er_b03a.log $LOG/er_b03b.log $LOG/er_b03c.log"
    ;;
b04)
    echo "=== B-04 offline"
    make -s -C "$ROOT/libecmaster/pdo" test > "$LOG/b04_pdo.log" 2>&1
    chk "B-04 test_pdo_offline" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/b04_pdo.log"
    (cd "$ROOT" && make -s -C libecmaster/config test) > "$LOG/b04_eni.log" 2>&1
    chk "B-04 test_eni_offline (PDO records)" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/b04_eni.log"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
