#!/usr/bin/env bash
# ==========================================================================
# run_regression_gd9.sh — Giai doan 9.11: the whole GD9 regression in one
# run on the Jetson (plan claude/giai_doan_9_ke_hoach.md §13): GD8 §4
# (offline, golden, L4, L5, L6) + G, C, DC, E, M, P, F, V, B.
#
#   make all SOEM_DIR=~/projects/SOEM && make caps      (as your user, first)
#   sudo -E tools/gd9/run_regression_gd9.sh
#   sudo -E STEPS="offline golden mixed" tools/gd9/run_regression_gd9.sh
#   IS620N_ESI=$HOME/esi/IS620N-Ecat_v2.6.9.xml sudo -E tools/gd9/run_regression_gd9.sh
#
# Each step runs one existing script with its own log; this script only
# collects the exit code and the RESULT line of each, and writes the table
# to $RLOG/summary.md (for giai_doan_9_tong_hop.md). Steps are independent:
# a failing step does not stop the others. soft_bus runs SCHED_FIFO
# $SB_PRIO on core $SB_CPU (default 79 / 2) where the script supports it.
# Not included (own rigs): 9.2 run_100m.sh (loopback cable), X-04 IgH,
# L1 (slaveinfo), soaks.
#
# Env: STEPS SB_PRIO (empty = SCHED_OTHER) SB_CPU IS620N_ESI RLOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}
STEPS=${STEPS:-"offline golden groups coe eni dc emcy fresh pdo mixed l5 l5n1"}
RLOG=${RLOG:-$ROOT/log_gd9_regression_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$RLOG"; RLOG=$(cd "$RLOG" && pwd)
export SB_PRIO SB_CPU

for i in veth_m veth_s; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing (veth pairs do not survive a reboot):"
        echo "  sudo ip link add veth_m type veth peer name veth_s && sudo ip link set veth_m up && sudo ip link set veth_s up"
        exit 2; }
done
if lsmod 2>/dev/null | grep -q '^ec_'; then
    echo "IgH modules loaded (lsmod | grep ^ec_): stop them first: sudo /opt/etherlab/sbin/ethercatctl stop"
    exit 2
fi

ROWS=()
# step NAME DESCRIPTION -- command...   (run from $ROOT unless the command cds)
step () {
    local name=$1 desc=$2; shift 3
    local f="$RLOG/$name.log" t0 t1 rc res
    echo "=== [$name] $desc"
    t0=$(date +%s)
    ( cd "$ROOT" && "$@" ) > "$f" 2>&1
    rc=$?
    t1=$(date +%s)
    res=$(grep -E '^ ?RESULT:|golden OK|negative control OK|GOLDEN MISMATCH|NEGATIVE CONTROL FAILED' "$f" \
          | sed 's/(logs:.*//; s/ *$//' | tr '\n' ';' | sed 's/;$//; s/;/; /g')
    [ -n "$res" ] || res="(no RESULT line, see $name.log)"
    if [ $rc = 0 ]; then echo "    ok  ($((t1 - t0)) s) $res"; else echo "    FAIL rc=$rc ($((t1 - t0)) s) $res"; fi
    ROWS+=("| $name | $desc | $([ $rc = 0 ] && echo ok || echo "**FAIL rc=$rc**") | $res | $((t1 - t0)) s |")
}

golden_all () {
    local g=tools/golden r=0
    sudo -E "$g/check_golden.sh" || r=1
    sudo -E "$g/negative_control.sh" || r=1
    sudo -E env ENI=config/eni/eni_8node_dc_sdo.enicfg "$g/check_golden.sh" || r=1
    sudo -E env N=1 GROUP_ARGS= GOLDEN=$g/golden_ecm_run_n1.txt "$g/check_golden.sh" || r=1
    sudo -E env GROUP_ARGS="--io-slaves 5-8" "$g/check_golden.sh" || r=1
    sudo -E env SB_ARGS=--coe-ca GOLDEN=$g/golden_ecm_run_ca.txt "$g/check_golden.sh" || r=1
    sudo -E env GOLDEN=$g/golden_ecm_run_ca.txt EXPECT_DIFF=1 "$g/check_golden.sh" || r=1
    return $r
}
l5_all () {
    local r=0
    (cd apps/ecm_run && sudo -E ./run_l5_policy.sh) || r=1
    (cd apps/ecm_run && sudo -E ./run_l5_io.sh) || r=1
    (cd apps/ecm_diag && sudo -E SB_ARGS=--no-sm-wd ./run_l5_diag.sh) || r=1
    return $r
}
l5n1_all () {
    local r=0
    (cd apps/ecm_run && sudo -E N=1 M=0 L505_SLAVE=1 CASES="l502 l502neg l503 l505 l505neg l513 l513neg" ./run_l5_policy.sh) || r=1
    (cd apps/ecm_run && sudo -E N=1 M=0 L509_SLAVE=1 CASES="l507 l507neg l508 l509 l509neg l512" ./run_l5_io.sh) || r=1
    return $r
}

for s in $STEPS; do
case $s in
offline) step offline "make test-offline (soft_bus incl. test_profile, libecmaster, TSan, ESI/ENI)" -- make test-offline ;;
golden)  step golden  "golden 4+4 / ENI / N=1 / G-01b / CA + negative controls" -- golden_all ;;
groups)  step groups  "9.1 G-01b, G-03..G-07"                   -- sudo -E env SB_ARGS=--no-sm-wd tools/gd9/run_groups.sh ;;
coe)     step coe     "9.3 C-02..C-05, L4 (+ --ca)"             -- sudo -E tools/gd9/run_coe.sh ;;
eni)     step eni     "9.6 E-01..E-10"                          -- sudo -E tools/gd9/run_eni_9_6.sh ;;
dc)      step dc      "9.5 DC-01..DC-05 (L6 40 s)"              -- sudo -E tools/gd9/run_dc_9_5.sh ;;
emcy)    step emcy    "9.7 M-01..M-03, P-01..P-03"              -- sudo -E tools/gd9/run_emcy_9_7.sh ;;
fresh)   step fresh   "9.8 F-01, F-02"                          -- sudo -E tools/gd9/run_fresh_9_8.sh ;;
pdo)     step pdo     "9.10 B-01..B-04"                         -- sudo -E tools/gd9/run_pdo_9_10.sh ;;
mixed)   step mixed   "9.9 V-01..V-03, DC-02, B-02m, B-05 (10.1), N-01..N-03c" -- sudo -E env IS620N_ESI="${IS620N_ESI:-}" tools/gd9/run_mixed_9_9.sh ;;
l5)      step l5      "L5 policy / io / diag, 4+4"              -- l5_all ;;
l5n1)    step l5n1    "L5 policy / io, N=1"                     -- l5n1_all ;;
*) echo "unknown step $s"; ROWS+=("| $s | unknown step | **FAIL** | | |") ;;
esac
done

{
    echo "# GD9 regression $(date '+%Y-%m-%d %H:%M') — $(uname -r), $(git -C "$ROOT" log --oneline -1)"
    echo
    echo "| Step | Content | Status | RESULT | Time |"
    echo "|---|---|---|---|---|"
    printf '%s\n' "${ROWS[@]}"
} > "$RLOG/summary.md"
echo
cat "$RLOG/summary.md"
! grep -q '\*\*FAIL' "$RLOG/summary.md"
