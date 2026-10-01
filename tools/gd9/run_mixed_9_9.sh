#!/usr/bin/env bash
# ==========================================================================
# run_mixed_9_9.sh — Giai doan 9.9: soft_bus nodes that stand in for real
# slaves (profile from the ESI), and a mixed virtual bus P1 + IS620N driven
# by the ENI. Plan: claude/giai_doan_9_ke_hoach.md §11.
#
#   sudo -E ./run_mixed_9_9.sh             all cases
#   sudo -E CASES="v02 n01" ./run_mixed_9_9.sh
#   IS620N_ESI=/path/IS620N-Ecat_v2.6.9.xml sudo -E ./run_mixed_9_9.sh
#
#   v01   offline: test_profile (SII, dictionary, 0x1C12/0x1C13 rules,
#         0x001D/0x001E, power cycle); esi2profile regenerates the committed
#         P1 profile byte for byte; esi_check: ESI vs the SII soft_bus builds
#         from the profile. The IS620N ESI is the vendor's file and neither
#         it nor anything generated from it is in the repo (licence note
#         30/9): with IS620N_ESI=<file> the full profile is generated into
#         the log dir, checked (esi_check, the 3 DefaultData quirks,
#         test_profile) and used for every case below; without it the
#         hand-written reduced config/profiles/is620n_min.prof is used and
#         the IS620N V-01 checks are SKIP.
#   v02   eni_mixed (P1, IS620N) on soft_bus --profile 1=P1 --profile 2=IS620N:
#         identity ok, every CoE InitCmd ok, OP; PDO table ENI == bus (CoE
#         scan, 27 entries); DC: P1 is the reference clock
#   v03   eni_mixed_rev (IS620N, P1) on the reversed bus: OP, ENI == bus,
#         IS620N is the reference clock (DC-02 with the vendor slave first);
#         eni_mixed on the reversed bus: refused at the identity check
#   b02m  bind on the mixed bus (B-02 of 9.10): set 0x607A of the IS620N and
#         0x6040 of P1, soft_bus sees those bytes; 0x6041 of the IS620N set
#         by soft_bus comes back through --pdo-get
#   n01   PDO mismatch, the classic one: the master maps the IS620N from SII
#         (profile without 0x1C00, so SOEM's CoE read fails), the ENI assigns
#         0x1B02 over CoE -> SM3 28 byte vs 25 assigned -> the drive refuses
#         SAFE-OP with 0x001E, ecm_run: CONFIGURATION error, no retry.
#         n01c control: same profile, ENI keeps 0x1B01 -> OP
#   n02   ENI with a CA assignment the ESI forbids (0x1A00 twice for P1):
#         SDO abort 0x06090030, SAFE-OP refused by ecm_run
#   n03   firmware != ESI with the SAME size (the IS620N ESI 0x1B04 kind of
#         quirk, here on 0x1B01: 0x60F4 and 0x60FD swapped in the mapping
#         object): without --pdo-scan the bus goes to OP and the data is
#         silently misread (size checks cannot see it -- documented, not a
#         pass condition of the master); with --pdo-scan the entry-by-entry
#         check refuses and names both entries
#   v04   IgH (optional, IGH=1 and the ethercat tool present): ethercat
#         slaves / pdos on the mixed bus show both identities and the PDOs
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU IS620N_ESI IGH
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"v01 v02 v03 b02m n01 n02 n03 v04"}
IS620N_ESI=${IS620N_ESI:-}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof; IS=$PROF/is620n_min.prof
ENI=$ROOT/config/eni/eni_mixed.enicfg; ENIR=$ROOT/config/eni/eni_mixed_rev.enicfg
CTL=/tmp/soft_bus_9_9.ctl

LOG=${LOG:-log_gd9_mixed_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0; SKIP=0
ok   () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad  () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip () { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
chk  () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

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
    "${sb[@]}" --iface "$IF_S" --n 2 --dc 32 --no-sm-wd --ctl "$CTL" $sbargs > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "$ECM_RUN" --iface "$IF_M" --n 2 --duration-sec "$dur" --no-tx-ts \
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

if [ -n "$IS620N_ESI" ]; then
    [ -f "$IS620N_ESI" ] || { echo "IS620N_ESI=$IS620N_ESI: no such file"; exit 2; }
    python3 "$ROOT/tools/esi/esi2profile.py" "$IS620N_ESI" -o "$LOG/is620n.prof" 2> "$LOG/esi2profile_is.err" \
        || { cat "$LOG/esi2profile_is.err"; exit 2; }
    IS=$LOG/is620n.prof
fi
echo "IS620N profile: $IS"

for c in $CASES; do
case $c in
v01)
    echo "=== V-01 offline: profiles, SII against the ESI"
    make -s -C "$ROOT/tools/soft_bus" test_profile sii_dump >/dev/null 2>&1
    (cd "$ROOT/tools/soft_bus" && ./test_profile "$PROF") > "$LOG/test_profile.log" 2>&1
    chk "V-01 test_profile: 0 fail" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/test_profile.log"
    python3 "$ROOT/tools/esi/esi2profile.py" "$ROOT/config/esi/p1_draft_esi.xml" -o "$LOG/p1_draft.prof" 2> "$LOG/esi2profile_p1.err"
    chk "V-01 esi2profile P1 == committed p1_draft.prof" "cmp -s $LOG/p1_draft.prof $P1"
    "$ROOT/tools/soft_bus/sii_dump" --profile "$P1" > "$LOG/p1.sii" 2>/dev/null
    python3 "$ROOT/tools/esi/esi_check.py" "$ROOT/config/esi/p1_draft_esi.xml" "$LOG/p1.sii" > "$LOG/esi_check_p1.log" 2>&1
    chk "V-01 esi_check P1 ESI vs profile SII: 0 fail" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/esi_check_p1.log"
    if [ -n "$IS620N_ESI" ]; then
        (cd "$ROOT/tools/soft_bus" && ./test_profile "$PROF" "$IS") > "$LOG/test_profile_full.log" 2>&1
        chk "V-01 test_profile on the full IS620N profile too: 0 fail" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/test_profile_full.log"
        chk "V-01 the 3 vendor DefaultData quirks reported (0x1702 0x1B03 0x1B04)" \
            "[ \$(grep -c 'differs from the' $LOG/esi2profile_is.err) = 3 ]"
        "$ROOT/tools/soft_bus/sii_dump" --profile "$IS" > "$LOG/is620n.sii" 2>/dev/null
        python3 "$ROOT/tools/esi/esi_check.py" "$IS620N_ESI" "$LOG/is620n.sii" > "$LOG/esi_check_is.log" 2>&1
        chk "V-01 esi_check IS620N ESI vs profile SII: 0 fail" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/esi_check_is.log"
    else
        skip "V-01 IS620N: set IS620N_ESI=<vendor ESI file> (not in the repo); is620n_min.prof used"
    fi
    ;;
v02)
    echo "=== V-02 eni_mixed on the mixed bus P1 + IS620N"
    run v02 3 "--profile 1=$P1 --profile 2=$IS" "--eni $ENI --pdo-scan --pdo-dump"
    chk "V-02 soft_bus: node 1 P1, node 2 IS620N" \
        "grep -q 'node 1 = profile \"P1-H723-CIA402-DRAFT\"' $S && grep -q 'node 2 = profile \"IS620N\"' $S"
    chk "V-02 rc 0, identity ok, OP" \
        "[ $RC = 0 ] && grep -q 'identity check OK' $E && grep -q 'all slaves in OPERATIONAL' $E"
    chk "V-02 all 9 CoE InitCmds ok (2 CA on P1, 7 on IS620N)" \
        "[ \$(grep -c 'CoE download .* ok' $E) = 9 ] && [ \$(grep -c 'slave 1 CoE download .*(complete access) ok' $E) = 2 ]"
    chk "V-02 both scanned over CoE" \
        "grep -q 'slave 1: 14 entries from coe' $E && grep -q 'slave 2: 13 entries from coe' $E"
    chk "V-02 PDO table ENI == bus (27 entries)" "grep -q 'PDO table: ENI == bus (27 entries)' $E"
    chk "V-02 DC: ref = slave 1 (P1), both SYNC0" \
        "grep -q 'DC on -- ref=slave 1, SYNC0 1000000 ns on 2' $E && [ \$(grep -c 'dc_total\[[01]\] sync0_active=1' $S) = 2 ]"
    chk "V-02 diag: both OP, code 0" "grep -Eq '^ +1 0x1001 OP +0x0000' $LOG/diag_v02.txt && grep -Eq '^ +2 0x1002 OP +0x0000' $LOG/diag_v02.txt"
    ;;
v03)
    echo "=== V-03 eni_mixed_rev (IS620N first)"
    run v03 3 "--profile 1=$IS --profile 2=$P1" "--eni $ENIR --pdo-scan"
    chk "V-03 rc 0, OP" "[ $RC = 0 ] && grep -q 'all slaves in OPERATIONAL' $E"
    chk "V-03 PDO table ENI == bus (27 entries)" "grep -q 'PDO table: ENI == bus (27 entries)' $E"
    chk "DC-02 IS620N is the reference clock (no drift trim on node 0)" \
        "grep -q 'DC on -- ref=slave 1' $E && grep -q 'dc_total\[0\] sync0_active=1 .* dt_writes=1 ' $S && grep -Eq 'dc_total\[1\] sync0_active=1 .* dt_writes=[0-9]{3,}' $S"
    run v03n 2 "--profile 1=$IS --profile 2=$P1" "--eni $ENI"
    chk "V-03n eni_mixed on the reversed bus: refused at the identity check, both positions named" \
        "[ $RC != 0 ] && grep -q 'identity check FAILED' $E && grep -q 'position 1 ' $E && grep -q 'position 2 ' $E"
    ;;
b02m)
    echo "=== B-02 mixed: bind by (slave, index, sub) on IS620N + P1"
    run b02m 4 "--profile 1=$IS --profile 2=$P1" \
        "--eni $ENIR --pdo-set 1:0x607A:0=0x11223344,2:0x6040:0=0x000F --pdo-get 1:0x6041:0" \
        "1.5:pdo_in 0 2 3712" "3:pdo_out 0" "3.2:pdo_out 1"
    chk "B-02m rc 0" "[ $RC = 0 ]"
    chk "B-02m IS620N outputs: 0x607A at byte 2 = 44 33 22 11 (12 byte, 0x1701)" \
        "grep -q 'pdo_out: node 0 (SOEM slave 1) 12 byte: 00 00 44 33 22 11 00 00 00 00 00 00' $S"
    chk "B-02m P1 outputs: 0x6040 = 0F 00 (13 byte, 0x1600)" \
        "grep -q 'pdo_out: node 1 (SOEM slave 2) 13 byte: 0F 00 00' $S"
    chk "B-02m get 1:0x6041:0 = 0x1237 (set by soft_bus at byte 2)" "grep -q '\[PDO\] get 1:0x6041:0 = 0x1237' $E"
    ;;
n01)
    echo "=== N-01 PDO mismatch: SM3 from SII (0x1B01, 28 byte), CoE assigns 0x1B02 (25 byte)"
    grep -v '^obj 0x1C00\|^sub 0x1C00 ' "$IS" > "$LOG/is620n_no1c00.prof"
    sed 's/^\(coe 2 trans PS ccs 1 index 0x1C13 sub 0x01 .* data \)011b$/\1021b/' "$ENI" > "$LOG/eni_n01.enicfg"
    chk "N-01 test files really edited" \
        "! grep -q '^obj 0x1C00' $LOG/is620n_no1c00.prof && grep -q 'index 0x1C13 sub 0x01 .* data 021b' $LOG/eni_n01.enicfg"
    run n01 2 "--profile 1=$P1 --profile 2=$LOG/is620n_no1c00.prof" "--eni $LOG/eni_n01.enicfg"
    chk "N-01 refused, slave 2 named with 0x001E and CONFIGURATION error" \
        "[ $RC != 0 ] && grep -q 'slave 2 .*code 0x001E (Invalid input configuration) -- CONFIGURATION error' $E"
    chk "N-01 no retry of the same configuration" "grep -q 'retrying would send the same configuration again' $E"
    run n01c 2 "--profile 1=$P1 --profile 2=$LOG/is620n_no1c00.prof" "--eni $ENI"
    chk "N-01c control: same profile, 0x1B01 kept -> OP" "[ $RC = 0 ] && grep -q 'all slaves in OPERATIONAL' $E"
    ;;
n02)
    echo "=== N-02 ENI assigns 0x1A00 twice to P1 (Complete Access)"
    sed 's/^\(coe 1 trans PS ccs 1 index 0x1C13 sub 0x00 ca 1 .* data \)0200001a011a$/\10200001a001a/' "$ENI" > "$LOG/eni_n02.enicfg"
    chk "N-02 test file really edited" "grep -q 'data 0200001a001a' $LOG/eni_n02.enicfg"
    run n02 2 "--profile 1=$P1 --profile 2=$IS" "--eni $LOG/eni_n02.enicfg"
    chk "N-02 CoE download 0x1C13:00 refused (0x06090030), SAFE-OP refused" \
        "[ $RC != 0 ] && grep -q 'slave 1 CoE download 0x1C13:00 (complete access) FAILED' $E && grep -qi '06090030' $E && grep -q 'refusing SAFE-OP' $E"
    ;;
n03)
    echo "=== N-03 mapping differs from the ESI, same size (0x60F4 <-> 0x60FD in 0x1B01)"
    sed -e 's/^sub 0x1B01 5 bits 32 ro 2000f460$/sub 0x1B01 5 bits 32 ro 2000fd60/' \
        -e 's/^sub 0x1B01 9 bits 32 ro 2000fd60$/sub 0x1B01 9 bits 32 ro 2000f460/' "$IS" > "$LOG/is620n_swap.prof"
    chk "N-03 test file really edited (2 lines)" "[ \$(diff $IS $LOG/is620n_swap.prof | grep -c '^>') = 2 ]"
    run n03a 2 "--profile 1=$P1 --profile 2=$LOG/is620n_swap.prof" "--eni $ENI"
    chk "N-03a without --pdo-scan: OP (sizes match, the swap is invisible)" "[ $RC = 0 ] && grep -q 'all slaves in OPERATIONAL' $E"
    run n03b 2 "--profile 1=$P1 --profile 2=$LOG/is620n_swap.prof" "--eni $ENI --pdo-scan"
    chk "N-03b with --pdo-scan: refused, 0x60F4/0x60FD named" \
        "[ $RC != 0 ] && grep -q 'PDO table of the ENI differs from the bus' $E && grep -q '0x60F4:00' $E && grep -q '0x60FD:00' $E"
    ;;
v04)
    echo "=== V-04 IgH on the mixed bus (optional)"
    if [ "${IGH:-0}" = 1 ] && command -v ethercat >/dev/null 2>&1; then
        rm -f "$CTL"
        "$SOFT_BUS" --iface "$IF_S" --n 2 --dc 32 --no-sm-wd --ctl "$CTL" --profile 1=$P1 --profile 2=$IS > "$LOG/sb_v04.log" 2>&1 &
        sbp=$!
        sleep 3
        ethercat slaves -v > "$LOG/igh_slaves.txt" 2>&1
        ethercat pdos > "$LOG/igh_pdos.txt" 2>&1
        kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
        chk "V-04 IgH sees P1 (0x499/0x102) and IS620N (0x100000/0xC0108)" \
            "grep -qi '0x00000499' $LOG/igh_slaves.txt && grep -qi '0x00100000' $LOG/igh_slaves.txt"
        chk "V-04 IgH names from the SII Strings" "grep -q 'IS620N' $LOG/igh_slaves.txt && grep -q 'P1-H723' $LOG/igh_slaves.txt"
        chk "V-04 IgH PDOs: 0x1701 and 0x1B01 on the IS620N" "grep -q '0x1701' $LOG/igh_pdos.txt && grep -q '0x1B01' $LOG/igh_pdos.txt"
    else
        skip "V-04 IgH: IGH=1 with the IgH master bound to $IF_M (GD8 X-01 setup)"
    fi
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail, $SKIP skip   (logs: $LOG)"
[ $FAIL = 0 ]
