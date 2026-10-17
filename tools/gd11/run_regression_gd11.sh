#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_regression_gd11.sh -- Phase 11.9: the whole Phase 11 regression in one
# run on the Jetson, veth only (no drive, no i226 change):
#
#   offline  make test-offline (incl. libecmaster/api offline, negctl, TSan,
#            schema) + test_is_offline.sh + test_bench_offline.sh
#   gd10     tools/gd10/run_regression_gd10.sh (API off: golden, Phase 9 and
#            10 steps; the binary must behave as before Phase 11)
#   api      11.1 run_api_11_1.sh  A-01..A-11 (golden with --api off AND on)
#   ctl      11.2 run_ctl_11_2.sh  C-01..C-09
#   diag     11.3 run_diag_11_3.sh G-01..G-07
#   l507     11.3b run_l507_11_3b.sh (L5-07 N=1 x 20 + legacy negative control)
#   d        11.6 run_d_11_6.sh d03 d04 d12 d13 (the short D tests)
#   d15      11.6 run_d15_11_6.sh (no cyclic mailbox without the SM1 FMMU)
#
#   make all caps                                  (as your user, first)
#   sudo -E tools/gd11/run_regression_gd11.sh      (~45 min)
#   sudo -E STEPS="offline api ctl" tools/gd11/run_regression_gd11.sh
#
# --api on under timing is covered by D-01 (6 x 30 min) and D-14 / BM-02 (real
# drives): too long for a regression; their last results are listed in the
# summary as references, not re-run.
# Each step: own log in $RLOG, exit code and RESULT line(s) collected into
# $RLOG/summary.md. Steps are independent.
#
# Env: STEPS SB_PRIO (default 79) SB_CPU (default 2) IS620N_ESI RLOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}
STEPS=${STEPS:-"offline gd10 api ctl diag l507 d d15"}
RLOG=${RLOG:-$ROOT/log_gd11_regression_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$RLOG"; RLOG=$(cd "$RLOG" && pwd)
export SB_PRIO SB_CPU
unset CASES LOG

[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
bad=0
for i in veth_m veth_s; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing: sudo ip link add veth_m type veth peer name veth_s && sudo ip link set veth_m up && sudo ip link set veth_s up"
        bad=1; break; }
done
LEFT=$(pgrep -x 'soft_bus|ecm_run|tshark|stress-ng')
[ -n "$LEFT" ] && { echo "still running, stop them first: $(echo $LEFT)"; bad=1; }
for f in apps/ecm_run/ecm_run apps/ecm_diag/ecm_diag tools/soft_bus/soft_bus; do
    [ -x "$ROOT/$f" ] || { echo "missing $f: make all caps"; bad=1; }
done
[ $bad = 0 ] || exit 2

ROWS=()
step () {   # NAME DESCRIPTION -- command...
    local name=$1 desc=$2; shift 3
    local f="$RLOG/$name.log" t0 t1 rc res
    echo "=== [$name] $desc"
    t0=$(date +%s)
    ( cd "$ROOT" && "$@" ) > "$f" 2>&1
    rc=$?
    t1=$(date +%s)
    if [ "$name" = gd10 ]; then
        local s="$RLOG/gd10/summary.md"
        res="$(grep -c '| ok |' "$s" 2>/dev/null) step(s) ok, $(grep -c '\*\*FAIL' "$s" 2>/dev/null) FAIL (gd10/summary.md)"
    else
        res=$(grep -E '^ ?RESULT:|: [0-9]+ pass, [0-9]+ fail' "$f" | sed 's/(log.*//; s/ *$//' | tail -8 | tr '\n' ';' | sed 's/;$//; s/;/; /g')
        [ -n "$res" ] || res="(no RESULT line, see $name.log)"
    fi
    if [ $rc = 0 ]; then echo "    ok  ($((t1 - t0)) s) $res"; else echo "    FAIL rc=$rc ($((t1 - t0)) s) $res"; fi
    ROWS+=("| $name | $desc | $([ $rc = 0 ] && echo ok || echo "**FAIL rc=$rc**") | $res | $((t1 - t0)) s |")
}

for s in $STEPS; do
case $s in
offline) step offline "make test-offline + test_is_offline + test_bench_offline" -- \
             bash -c 'make test-offline && tools/gd11/test_is_offline.sh && tools/bench/test_bench_offline.sh' ;;
gd10)    step gd10 "Phase 10 regression, API off (golden, gd9, 10.2..10.9, recover)" -- \
             env RLOG="$RLOG/gd10" "$ROOT/tools/gd10/run_regression_gd10.sh" ;;
api)     step api  "11.1 stats.json A-01..A-11"            -- env LOG="$RLOG/api"  "$HERE/run_api_11_1.sh" ;;
ctl)     step ctl  "11.2 ctl.sock C-01..C-09"              -- env LOG="$RLOG/ctl"  "$HERE/run_ctl_11_2.sh" ;;
diag)    step diag "11.3 ecm_diag through the API G-01..G-07" -- env LOG="$RLOG/diag" "$HERE/run_diag_11_3.sh" ;;
l507)    step l507 "11.3b L5-07 N=1 repeated + --dc-gate-legacy negative control" -- env LOG="$RLOG/l507" "$HERE/run_l507_11_3b.sh" ;;
d)       step d    "11.6 D-03 D-04 D-12 D-13"              -- env CASES="d03 d04 d12 d13" LOG="$RLOG/d" "$HERE/run_d_11_6.sh" ;;
d15)     step d15  "11.6 D-15 2-FMMU slave: no cyclic mailbox, no_mailbox + --mbx-cyclic-force negctl" -- \
             env LOG="$RLOG/d15" "$HERE/run_d15_11_6.sh" ;;
*) echo "unknown step $s"; ROWS+=("| $s | unknown step | **FAIL** | | |") ;;
esac
done

{
    echo "# Phase 11 regression $(date '+%Y-%m-%d %H:%M') — $(uname -r), $(git -C "$ROOT" log --oneline -1)"
    echo
    if [ -n "$SB_PRIO" ]; then echo "soft_bus FIFO $SB_PRIO core $SB_CPU"; else echo "soft_bus SCHED_OTHER (timing not valid)"; fi
    echo
    echo "| Step | Content | Status | RESULT | Time |"
    echo "|---|---|---|---|---|"
    printf '%s\n' "${ROWS[@]}"
    echo
    echo "Not re-run (long, own rigs; last results): D-01 6 x 30 min 64/1 then PASS with 0041;"
    echo "D-14 real IS620N 11/11; BM-02 4 x 1 h 4/4; IS-07 250 us 1 h 5 NOFRAME (docs/rt_profiles.md, docs/benchmark.md)."
    if [ -f "$RLOG/gd10/summary.md" ]; then echo; echo "## Phase 10 steps"; echo; sed -n '/^| Step/,$p' "$RLOG/gd10/summary.md"; fi
} > "$RLOG/summary.md"
chown -R "${SUDO_USER:-root}:" "$RLOG" 2>/dev/null
echo
cat "$RLOG/summary.md"
! grep -q '\*\*FAIL' "$RLOG/summary.md"
