#!/usr/bin/env bash
# ==========================================================================
# run_regression_gd10.sh — Phase 10.10: the whole Phase 10 regression in one
# run on the Jetson (plan claude/giai_doan_10_ke_hoach.md §12): the Phase 9
# regression (offline incl. the CiA402 negative controls, golden, G, C, DC,
# E, M, P, F, V, B, L5) + the CiA402 layer 10.2 .. 10.8 with the timing
# checks counted (STRICT=1).
#
#   make all caps                                          (as your user, first)
#   export IS620N_ESI=$HOME/esi/IS620N-Ecat_v2.6.9.xml
#   sudo -E tools/gd10/run_regression_gd10.sh              (~25 min)
#   sudo -E STEPS="servo cia402 safety" tools/gd10/run_regression_gd10.sh
#   sudo -E GD9_STEPS="offline golden" tools/gd10/run_regression_gd10.sh
#   STRICT=0 SB_PRIO= SB_CPU=1 sudo -E ...                 (sandbox: timing as INFO)
#
# Each step runs one existing script with its own log in $RLOG; this script
# only collects the exit code and the RESULT line(s) and writes the table to
# $RLOG/summary.md (for giai_doan_10_tong_hop.md). Steps are independent: a
# failing step does not stop the others. The checks that bit on the Jetson
# (3/10) are done BEFORE anything runs: veth pair, IS620N_ESI a real file,
# tshark, no soft_bus / ecm_run left over.
# Not included (own rigs, run them alone): soaks (run_soak_n1.sh,
# run_soak_cia402.sh), Q-03 30 min (Q03_SEC=1800), 9.2 run_100m.sh, X-04/X-05.
#
# Env: STEPS GD9_STEPS STRICT (default 1) SB_PRIO (default 79, empty =
#      SCHED_OTHER) SB_CPU (default 2) IS620N_ESI Q02_SEC Q03_SEC RLOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}; STRICT=${STRICT:-1}
STEPS=${STEPS:-"gd9 servo axes hook cia402 safety profile diag recover"}
GD9_STEPS=${GD9_STEPS:-}
IS620N_ESI=${IS620N_ESI:-}
RLOG=${RLOG:-$ROOT/log_gd10_regression_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$RLOG"; RLOG=$(cd "$RLOG" && pwd)
export SB_PRIO SB_CPU STRICT IS620N_ESI
unset CASES LOG

# ---- pre-checks (fail fast, before 25 min of running) -------------------
bad=0
for i in veth_m veth_s; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing (veth pairs do not survive a reboot):"
        echo "  sudo ip link add veth_m type veth peer name veth_s && sudo ip link set veth_m up && sudo ip link set veth_s up"
        bad=1; break; }
done
if [ -n "$IS620N_ESI" ] && [ ! -f "$IS620N_ESI" ]; then
    echo "IS620N_ESI=$IS620N_ESI is not a file (export IS620N_ESI=\$HOME/esi/IS620N-Ecat_v2.6.9.xml)"; bad=1
fi
[ -z "$IS620N_ESI" ] && echo "note: IS620N_ESI not set -- 10.3 and 9.9 use the reduced IS620N profile (V-01 skipped)"
if echo " $STEPS " | grep -q ' diag ' && ! command -v tshark >/dev/null; then
    echo "tshark missing (step diag, X-05): sudo apt install tshark"; bad=1
fi
LEFT=$(pgrep -x 'soft_bus|ecm_run|ecm_run_neg6|tshark')     # process names only (-f would match this shell)
if [ -n "$LEFT" ]; then
    echo "still running (a soak? an interrupted test?) -- stop them first:"
    ps -o pid=,stat=,args= -p "$(echo $LEFT | tr ' ' ',')"
    echo "  (stat T = stopped by Ctrl+Z: sudo kill -CONT <pid>; sudo kill -INT <pid>)"
    bad=1
fi
if lsmod 2>/dev/null | grep -q '^ec_'; then
    echo "IgH modules loaded: sudo /opt/etherlab/sbin/ethercatctl stop"; bad=1
fi
[ $bad = 0 ] || exit 2

ROWS=()
# step NAME DESCRIPTION -- command...   (run from $ROOT)
step () {
    local name=$1 desc=$2; shift 3
    local f="$RLOG/$name.log" t0 t1 rc res
    echo "=== [$name] $desc"
    t0=$(date +%s)
    ( cd "$ROOT" && "$@" ) > "$f" 2>&1
    rc=$?
    t1=$(date +%s)
    if [ "$name" = gd9 ]; then
        local s="$RLOG/gd9/summary.md" ok nf
        ok=$(grep -c '^| .* | ok |' "$s" 2>/dev/null); nf=$(grep -c '\*\*FAIL' "$s" 2>/dev/null)
        res="$ok step(s) ok, $nf FAIL (gd9/summary.md)"
        [ "$nf" -gt 0 ] 2>/dev/null && res="$res: $(grep '\*\*FAIL' "$s" | cut -d'|' -f2 | tr -d ' ' | tr '\n' ' ')"
    else
        res=$(grep -E '^ ?RESULT:' "$f" | sed 's/(logs:.*//; s/ *$//' | tr '\n' ';' | sed 's/;$//; s/;/; /g')
        [ -n "$res" ] || res="(no RESULT line, see $name.log)"
    fi
    if [ $rc = 0 ]; then echo "    ok  ($((t1 - t0)) s) $res"; else echo "    FAIL rc=$rc ($((t1 - t0)) s) $res"; fi
    ROWS+=("| $name | $desc | $([ $rc = 0 ] && echo ok || echo "**FAIL rc=$rc**") | $res | $((t1 - t0)) s |")
}

G=$ROOT/tools/gd10
for s in $STEPS; do
case $s in
gd9)     step gd9     "Phase 9 regression (${GD9_STEPS:-12 steps: offline incl. CiA402 negctl, golden, G, C, DC, E, M, P, F, V, B, L5})" -- \
             env ${GD9_STEPS:+STEPS="$GD9_STEPS"} RLOG="$RLOG/gd9" "$ROOT/tools/gd9/run_regression_gd9.sh" ;;
servo)   step servo   "10.2 virtual CiA402 drive D-01..D-07, A-04"            -- env LOG="$RLOG/servo"   "$G/run_servo_10_2.sh" ;;
axes)    step axes    "10.3 axis configuration K-01n..K-08, K-05"              -- env LOG="$RLOG/axes"    "$G/run_axes_10_3.sh" ;;
hook)    step hook    "10.4 RT hook + exchange Q-01..Q-03 (${Q03_SEC:-30} s), H-01..H-05" -- env LOG="$RLOG/hook" "$G/run_hook_10_4.sh" ;;
cia402)  step cia402  "10.5 drive state machine + CSP/CSV T-01..T-08"          -- env LOG="$RLOG/cia402"  "$G/run_cia402_10_5.sh" ;;
safety)  step safety  "10.6 safety latches S1, S2, S4..S7 + negative controls" -- env LOG="$RLOG/safety"  "$G/run_safety_10_6.sh" ;;
profile) step profile "10.7 PP / PV / homing P2-01..P2-04"                     -- env LOG="$RLOG/profile" "$G/run_profile_10_7.sh" ;;
diag)    step diag    "10.8 axis diagnosis E2-01..E2-03, X-05 tool"            -- env LOG="$RLOG/diag"    "$G/run_diag_10_8.sh" ;;
recover) step recover "10.0 recovery re-runs the ENI InitCmds, refuses OP on failure RC-01..RC-06" -- env LOG="$RLOG/recover" "$G/run_recover_10_0.sh" ;;
*) echo "unknown step $s"; ROWS+=("| $s | unknown step | **FAIL** | | |") ;;
esac
done

{
    echo "# Phase 10 regression $(date '+%Y-%m-%d %H:%M') — $(uname -r), $(git -C "$ROOT" log --oneline -1)"
    echo
    if [ -n "$SB_PRIO" ]; then RIG="FIFO $SB_PRIO core $SB_CPU"; else RIG="SCHED_OTHER (timing not valid)"; fi
    echo "STRICT=$STRICT, soft_bus $RIG, IS620N_ESI=${IS620N_ESI:-(reduced profile)}"
    echo
    echo "| Step | Content | Status | RESULT | Time |"
    echo "|---|---|---|---|---|"
    printf '%s\n' "${ROWS[@]}"
    if [ -f "$RLOG/gd9/summary.md" ]; then echo; echo "## Phase 9 steps"; echo; sed -n '/^| Step/,$p' "$RLOG/gd9/summary.md"; fi
} > "$RLOG/summary.md"
echo
cat "$RLOG/summary.md"
! grep -q '\*\*FAIL' "$RLOG/summary.md"
