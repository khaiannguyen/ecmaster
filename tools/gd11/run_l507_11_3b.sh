#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_l507_11_3b.sh -- Phase 11.3b: L5-07 on a one-slave bus (N=1), repeated.
#
# Before 11.3b, L5-07 N=1 failed in about 1 run out of 10: a stream of old
# replies (SOEM index reuse after the quarantine filled up) is
# self-consistent, so after 3 rejections the DC reply-age gate took one of
# them as a real clock step ("gate resync") and the next old replies reached
# the application (the input counter went backwards). 11.3b holds the resync
# back while the rejected samples lie behind the host clock by no more than
# the reuse window (ecm_dc.h, gate_reuse_ns).
#
#   sudo -E tools/gd11/run_l507_11_3b.sh            # K=20 runs, then K_NEG=10
#
#   L507-R   K runs of L5-07 (with its l507neg) on N=1: every run passes and
#            input regressions = 0 in every run (gate resyncs reported)
#   L507-N   negative control, K_NEG runs with --dc-gate-legacy (the Phase 7.4
#            rule): the gate resyncs in the late stream (resyncs > 0 in most
#            runs); runs with input regressions are reported (info: about
#            1 in 10 on the Jetson, so a short series may show none)
#
# Env: K K_NEG SB_PRIO SB_CPU SB_ARGS ECM_ARGS (passed on to run_l5_io.sh)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${K:-20}; K_NEG=${K_NEG:-10}
OUT=${OUT:-$ROOT/log_gd11_l507_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
PASS=0; FAIL=0
chk () { if eval "$2"; then echo "  [PASS] $1"; PASS=$((PASS+1)); else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi; }

# one run -> "rc regressions resyncs held logdir"
one () {
    local extra=$1 txt=$2 rc d reg rs hd
    (cd "$ROOT/apps/ecm_run" && ECM_ARGS="${ECM_ARGS:-} $extra" N=1 M=0 L509_SLAVE=1 \
        CASES="l507 l507neg" ./run_l5_io.sh) > "$txt" 2>&1
    rc=$?
    d=$(grep -oE "log_l5_io_[0-9_]+" "$txt" | tail -1)
    reg=$(grep "\[FRESH\] total" "$ROOT/apps/ecm_run/$d/er_l507.log" 2>/dev/null | grep -oE "regressions=[0-9]+" | cut -d= -f2)
    rs=$(grep -oE "gate resyncs=[0-9]+" "$ROOT/apps/ecm_run/$d/er_l507.log" 2>/dev/null | cut -d= -f2)
    hd=$(grep -oE "resyncs held back[^=]*=[0-9]+" "$ROOT/apps/ecm_run/$d/er_l507.log" 2>/dev/null | grep -oE "[0-9]+$")
    echo "$rc ${reg:--1} ${rs:--1} ${hd:-0} $d"
}

echo "=== Phase 11.3b L5-07 N=1 x $K, log $OUT"
nfail=0; nreg=0; nrs=0; held=0
for i in $(seq 1 "$K"); do
    read -r rc reg rs hd d < <(one "" "$OUT/r$i.txt")
    printf "    run %2d: rc=%s regressions=%s gate_resyncs=%s held=%s (%s)\n" "$i" "$rc" "$reg" "$rs" "$hd" "$d"
    [ "$rc" = 0 ] || nfail=$((nfail+1))
    [ "$reg" = 0 ] || nreg=$((nreg+1))
    [ "$rs" = 0 ] || nrs=$((nrs+1))
    held=$((held+hd))
done
chk "L507-R all $K runs pass (failed: $nfail)" "[ $nfail = 0 ]"
chk "L507-R input regressions 0 in every run (runs with some: $nreg)" "[ $nreg = 0 ]"
echo "    info: resyncs held back by the reuse window, all runs: $held; runs with a resync: $nrs"
echo "          (a resync is still allowed for misses that do not look like index reuse:"
echo "           forward, <= 2 cycles behind, or beyond the window; expected 0 on the Jetson)"

echo "=== L507-N negative control: --dc-gate-legacy x $K_NEG"
nrs=0; nreg=0
for i in $(seq 1 "$K_NEG"); do
    read -r rc reg rs hd d < <(one "--dc-gate-legacy" "$OUT/n$i.txt")
    printf "    run %2d: rc=%s regressions=%s gate_resyncs=%s (%s)\n" "$i" "$rc" "$reg" "$rs" "$d"
    [ "$rs" -gt 0 ] 2>/dev/null && nrs=$((nrs+1))
    [ "$reg" -gt 0 ] 2>/dev/null && nreg=$((nreg+1))
done
chk "L507-N the legacy gate resyncs on the late stream (in $nrs of $K_NEG runs)" "[ $nrs -gt 0 ]"
echo "    info: legacy runs where an old reply reached the application: $nreg of $K_NEG"

echo "RESULT: $PASS pass, $FAIL fail   (log: $OUT)"
[ $FAIL = 0 ]
