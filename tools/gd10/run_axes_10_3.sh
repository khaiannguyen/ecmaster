#!/usr/bin/env bash
# ==========================================================================
# run_axes_10_3.sh — Phase 10.3: CiA402 axis configuration in ecm_run
# (libecm_cia402) against virtual drives on soft_bus (Phase 10.2). Plan:
# claude/giai_doan_10_ke_hoach.md §5. Offline part: libecm_cia402/
# test_cia402_cfg (K-01, K-03o, K-04o, parsing); K-05: tools/gd10/check_k05.py.
#
#   sudo -E tools/gd10/run_axes_10_3.sh
#   sudo -E CASES="k02 k03" tools/gd10/run_axes_10_3.sh
#   IS620N_ESI=/path/IS620N-Ecat_v2.6.9.xml sudo -E ...   (full IS620N profile)
#
#   k02   2 x IS620N (eni_2servo, --pdo-scan), axes 1:0,2:0 CSP: OP; the
#         axis' controlword/statusword bit offsets equal what --pdo-set /
#         --pdo-get bind for 0x6040/0x6041 (two independent paths); mode by
#         SDO/InitCmd (no 0x6060 in 0x1701)
#   k02n  the same without --pdo-scan: refused (Phase 10.1 rule), vendor named
#   k01n  IS620N 0x1701 asked for CSV: refused before SAFE-OP, 0x60FF named
#   k06   IS620N asked for CSP + PP: refused, 0x6060 needed to change mode
#   k03   4-axis drive whose axis 0 lacks PP in 0x6502: PP on axis 1:0
#         refused before SAFE-OP with the 0x6502 value; control: PP on 1:1 OP
#   k04   4 axes on one node, CSP + CSV: OP, cw at bit 0/88/176/264 and sw
#         at 352/456/560/664 of the group IOmap (outputs first), mode by PDO
#   k07   P1 draft (no 0x6502 in its ESI): refused (SDO abort 0x06020000);
#         with --axis-no-6502: OP + WARNING
#   k08   --axes-cfg file with a named axis; --axis and --axes-cfg together
#         refused
#   k05   tools/gd10/check_k05.py (+ its self-test, the negative control)
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU IS620N_ESI
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"k02 k02n k01n k06 k03 k04 k07 k08 k05"}
IS620N_ESI=${IS620N_ESI:-}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof; AX4=$PROF/cia402_4ax.prof
ENI2=$ROOT/config/eni/eni_2servo.enicfg
CTL=/tmp/soft_bus_10_3.ctl

LOG=${LOG:-log_gd10_axes_$(date +%Y%m%d_%H%M%S)}
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

# IS620N profile: the full one from the vendor ESI when given, else the
# reduced committed one plus 0x6502. The reduced profile has no 0x6502; the
# value added here (0x1A5: PP PV HM CSP CSV) is a TEST ASSUMPTION, not the
# drive's -- the real value comes from the ESI / the drive (Phase 10.0 R-04).
if [ -n "$IS620N_ESI" ]; then
    python3 "$ROOT/tools/esi/esi2profile.py" "$IS620N_ESI" -o "$LOG/is620n.prof" 2> "$LOG/esi2profile.err" || { cat "$LOG/esi2profile.err"; exit 2; }
    IS=$LOG/is620n.prof
else
    IS=$LOG/is620n_min_6502.prof
    { cat "$PROF/is620n_min.prof"; echo "# Phase 10.3 test assumption (not from the ESI): supported drive modes"
      echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro a5010000"; } > "$IS"
fi
# 4-axis drive whose axis 0 has no PP in 0x6502 (0x1A4)
AX4NOPP=$LOG/cia402_4ax_nopp0.prof
awk 'BEGIN{d=0} /^sub 0x6502 0 bits 32 ro a5010000$/ && !d {print "sub 0x6502 0 bits 32 ro a4010000"; d=1; next} {print}' "$AX4" > "$AX4NOPP"
echo "IS620N profile: $IS"

run () {   # run TAG N DURATION "soft_bus args" "ecm_run args"
    local tag=$1 n=$2 dur=$3 sbargs=$4 eargs=$5
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n "$n" --dc 32 --no-sm-wd --ctl "$CTL" $sbargs > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "$ECM_RUN" --iface "$IF_M" --n "$n" --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" $eargs > "$LOG/er_$tag.log" 2>&1
    RC=$?
    sleep 0.3
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log; S=$LOG/sb_$tag.log
}
op ()       { grep -q 'all slaves in OPERATIONAL' "$E"; }
no_safeop () { ! grep -q 'DC on' "$E"; }       # refused before SAFE-OP (DC is configured on the way there)
# bit of an axis' cw/sw as ecm_run prints it, and of a --pdo-set/--pdo-get bind
axbit ()  { grep -o "axis $1: .*" "$E" | grep -o "$2 group [0-9]* bit [0-9]*" | awk '{print $NF}'; }
pdobit () { grep -o "pdo $1 slave $2 0x$3:00 -> group [0-9]* [a-z]* bit [0-9]*" "$E" | awk '{print $NF}'; }

for c in $CASES; do
case $c in
k02)
    echo "=== K-02 two IS620N drives, axes 1:0 and 2:0 in CSP"
    run k02 2 3 "--profile 1=$IS --cia402 1 --profile 2=$IS --cia402 2" \
        "--eni $ENI2 --pdo-scan --axis 1:0,2:0 --axis-modes csp --pdo-set 1:0x6040:0=0,2:0x6040:0=0 --pdo-get 1:0x6041:0,2:0x6041:0"
    chk "K-02 rc 0, OP" "[ $RC = 0 ] && op"
    chk "K-02 2 axes configured" "grep -q '2 CiA402 axis/axes configured' $E"
    chk "K-02 0x6502 read from both drives" "[ \$(grep -c 'modes CSP, 0x6502 0x' $E) = 2 ]"
    for s in 1 2; do
        chk "K-02 axis $s:0 cw bit = --pdo-set 0x6040 bit ($(axbit $s:0 cw))" \
            "[ -n \"\$(axbit $s:0 cw)\" ] && [ \"\$(axbit $s:0 cw)\" = \"\$(pdobit set $s 6040)\" ]"
        chk "K-02 axis $s:0 sw bit = --pdo-get 0x6041 bit ($(axbit $s:0 sw))" \
            "[ -n \"\$(axbit $s:0 sw)\" ] && [ \"\$(axbit $s:0 sw)\" = \"\$(pdobit get $s 6041)\" ]"
    done
    # SOEM's group IOmap: every output of the group first, then the inputs
    chk "K-02 slave 2 cw after slave 1's 12 byte (bit 96); sw = 2 x 12 byte outputs + 28 + 2 (bit 432)" \
        "[ \"\$(axbit 2:0 cw)\" = 96 ] && [ \"\$(axbit 2:0 sw)\" = 432 ]"
    chk "K-02 mode by SDO/InitCmd (0x1701 has no 0x6060)" "[ \$(grep -c 'mode by SDO/InitCmd' $E) = 2 ]"
    ;;
k02n)
    echo "=== K-02n same bus, PDO table from the ENI alone"
    run k02n 2 3 "--profile 1=$IS --cia402 1 --profile 2=$IS --cia402 2" "--eni $ENI2 --axis 1:0,2:0"
    chk "K-02n refused before SAFE-OP, vendor named" \
        "[ $RC != 0 ] && no_safeop && grep -q 'axes on 2 slave(s) of another vendor .*slave 1 (vendor 0x00100000), slave 2 (vendor 0x00100000)' $E"
    ;;
k01n)
    echo "=== K-01n IS620N 0x1701 asked for CSV"
    run k01n 2 3 "--profile 1=$IS --cia402 1 --profile 2=$IS --cia402 2" "--eni $ENI2 --pdo-scan --axis 2:0 --axis-modes csv"
    chk "K-01n refused before SAFE-OP, axis/mode/object named" \
        "[ $RC != 0 ] && no_safeop && grep -q 'axis 2:0 (slave 2): CSV needs 0x60FF:00 (output) in the process data' $E"
    ;;
k06)
    echo "=== K-06 IS620N 0x1701 asked for CSP + PP (mode change at run time)"
    run k06 2 3 "--profile 1=$IS --cia402 1 --profile 2=$IS --cia402 2" "--eni $ENI2 --pdo-scan --axis 1:0 --axis-modes csp,pp"
    chk "K-06 refused before SAFE-OP: 0x6060 needed" \
        "[ $RC != 0 ] && no_safeop && grep -q 'changing mode at run time (PP,CSP) needs 0x6060:00 (output)' $E"
    ;;
k03)
    echo "=== K-03 0x6502 of axis 0 has no PP"
    run k03 1 3 "--profile 1=$AX4NOPP --cia402 1:4" "--pdo-scan --axis 1:0 --axis-modes pp"
    chk "K-03 PP on 1:0 refused before SAFE-OP, 0x6502 value named" \
        "[ $RC != 0 ] && no_safeop && grep -q 'axis 1:0 (slave 1): drive does not support PP (0x6502:00 supported drive modes = 0x000001A4' $E"
    run k03c 1 3 "--profile 1=$AX4NOPP --cia402 1:4" "--pdo-scan --axis 1:1 --axis-modes pp"
    chk "K-03 control: PP on 1:1 (0x6D02 = 0x1A5) -> OP" "[ $RC = 0 ] && op && grep -q 'axis 1:1: .*modes PP, 0x6502 0x000001A5' $E"
    ;;
k04)
    echo "=== K-04 four axes on one node"
    run k04 1 3 "--profile 1=$AX4 --cia402 1:4" "--pdo-scan --axis 1:0,1:1,1:2,1:3 --axis-modes csp,csv"
    chk "K-04 rc 0, OP, 4 axes" "[ $RC = 0 ] && op && grep -q '4 CiA402 axis/axes configured' $E"
    chk "K-04 cw at 0/88/176/264" "[ \"\$(axbit 1:0 cw) \$(axbit 1:1 cw) \$(axbit 1:2 cw) \$(axbit 1:3 cw)\" = '0 88 176 264' ]"
    chk "K-04 sw after the 44 byte outputs: 352/456/560/664" "[ \"\$(axbit 1:0 sw) \$(axbit 1:1 sw) \$(axbit 1:2 sw) \$(axbit 1:3 sw)\" = '352 456 560 664' ]"
    chk "K-04 mode by PDO (0x6060 mapped per axis)" "[ \$(grep -c 'mode by PDO' $E) = 4 ]"
    ;;
k07)
    echo "=== K-07 P1 draft: no 0x6502 in its ESI"
    run k07 1 3 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp"
    chk "K-07 refused before SAFE-OP: 0x6502 not readable (abort 0x06020000)" \
        "[ $RC != 0 ] && no_safeop && grep -q 'cannot read 0x6502:00 supported drive modes (SDO abort 0x06020000)' $E"
    run k07b 1 3 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-modes csp --axis-no-6502"
    chk "K-07 --axis-no-6502: OP with a WARNING" "[ $RC = 0 ] && op && grep -q 'WARNING axis 1:0: 0x6502 not readable' $E"
    ;;
k08)
    echo "=== K-08 axes file"
    printf '# test axes\naxis 1:2 modes csv name conveyor\n' > "$LOG/axes.cfg"
    run k08 1 3 "--profile 1=$AX4 --cia402 1:4" "--pdo-scan --axes-cfg $LOG/axes.cfg"
    chk "K-08 named axis from the file: OP, 'conveyor' = slave 1 axis 2, CSV" \
        "[ $RC = 0 ] && op && grep -q 'axis conveyor: slave 1 axis 2, modes CSV' $E"
    run k08n 1 2 "--profile 1=$AX4 --cia402 1:4" "--pdo-scan --axes-cfg $LOG/axes.cfg --axis 1:0"
    chk "K-08n --axis and --axes-cfg together refused" "[ $RC != 0 ] && grep -q 'exclude each other' $E"
    ;;
k05)
    echo "=== K-05 no CiA402 object index in libecmaster"
    chk "K-05 self-test (negative control)" "python3 $HERE/check_k05.py --self-test > $LOG/k05_self.txt"
    chk "K-05 libecmaster clean" "python3 $HERE/check_k05.py > $LOG/k05.txt"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
