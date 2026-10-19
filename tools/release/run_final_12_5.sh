#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_final_12_5.sh -- Phase 12.5: the final regression of the commit to be
# tagged master-v1.0, on the Jetson, no drive (veth):
#
#   commit  the tree must be clean (what runs is what gets tagged)
#   env     tools/jetson/check_env.sh --soak --no-iface (FAIL stops)
#   reg     tools/gd11/run_regression_gd11.sh (offline, Phase 9/10 incl.
#           golden + negative controls, Phase 11 API / ctl / diag / D tests)
#   lic     make test-license (reuse >= 3.0)
#   l0      L0: tools/rt_tail/rt_tail.sh 10 min on CPU 3 (cyclictest + ftrace):
#           no wake-up above BREAK_US (default 100 us)
#   vg      valgrind: self-check first (a program leaking 4 KiB once MUST be
#           reported "definitely lost"), then ecm_run under valgrind through
#           L5-03 (cable out / in): "definitely lost: 0", "0 errors"
#
#   sudo -E tools/release/run_final_12_5.sh                (~70 min)
#   sudo -E STEPS="vg l0" tools/release/run_final_12_5.sh
#
# Report: LOG/summary.md. Env: STEPS L0_DUR (10m) BREAK_US (100) SB_PRIO SB_CPU LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
STEPS=${STEPS:-"commit env reg lic l0 vg"}
L0_DUR=${L0_DUR:-10m}; BREAK_US=${BREAK_US:-100}
LOG=${LOG:-$ROOT/log_gd12_final_$(date +%Y%m%d_%H%M%S)}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd); chown "${SUDO_USER:-root}:" "$LOG"
U=${SUDO_USER:-root}
ROWS=(); FAILS=0
row () { ROWS+=("| $1 | $2 | $3 |"); [ "$2" = ok ] || FAILS=$((FAILS + 1)); echo "=== [$1] $2  $3"; }

for s in $STEPS; do
case $s in
commit)
    d=$(git -C "$ROOT" status --porcelain --untracked-files=no)
    c=$(git -C "$ROOT" describe --always --dirty)
    if [ -z "$d" ]; then row commit ok "$c ($(git -C "$ROOT" log -1 --format=%s | cut -c1-70))"
    else row commit "**FAIL**" "uncommitted changes: the result would not belong to a commit"; fi
    ;;
env)
    "$ROOT/tools/jetson/check_env.sh" --soak --no-iface > "$LOG/check_env.txt" 2>&1; r=$?
    if [ $r = 2 ]; then row env "**FAIL**" "$(grep -c FAIL "$LOG/check_env.txt") FAIL (check_env.txt)"
    else row env ok "$(tail -1 "$LOG/check_env.txt")"; fi
    ;;
reg)
    RLOG="$LOG/regression" "$ROOT/tools/gd11/run_regression_gd11.sh" > "$LOG/regression.txt" 2>&1; r=$?
    n_ok=$(grep -c '| ok |' "$LOG/regression/summary.md" 2>/dev/null)
    n_bad=$(grep -c '\*\*FAIL' "$LOG/regression/summary.md" 2>/dev/null)
    if [ $r = 0 ]; then row reg ok "$n_ok step rows ok, 0 FAIL (regression/summary.md)"
    else row reg "**FAIL**" "$n_bad FAIL row(s) (regression/summary.md)"; fi
    ;;
lic)
    if sudo -u "$U" -i bash -c "cd '$ROOT' && export PATH=\$HOME/.local/bin:\$PATH && make -s test-license" > "$LOG/license.txt" 2>&1
    then row lic ok "$(grep RESULT "$LOG/license.txt")"
    else row lic "**FAIL**" "$(grep -E 'RESULT|too old|missing' "$LOG/license.txt" | head -1)"; fi
    ;;
l0)
    command -v cyclictest >/dev/null || { row l0 "**FAIL**" "cyclictest missing (sudo apt install rt-tests)"; continue; }
    OUT="$LOG/rt_tail" DUR=$L0_DUR BREAK_US=$BREAK_US CPU=3 "$ROOT/tools/rt_tail/rt_tail.sh" > "$LOG/rt_tail.txt" 2>&1
    mx=$(grep -h 'Max Latencies' "$LOG/rt_tail/cyclictest.txt" | grep -o '[0-9]\+' | sort -n | tail -1)
    if grep -qiE '# Break|break thread|break value' "$LOG/rt_tail/cyclictest.txt"; then
        row l0 "**FAIL**" "a wake-up above $BREAK_US us stopped cyclictest: rt_tail/trace.txt shows what ran on CPU 3"
    elif [ -n "$mx" ] && [ "$mx" -le "$BREAK_US" ]; then row l0 ok "cyclictest CPU 3, $L0_DUR: max $mx us (<= $BREAK_US)"
    else row l0 "**FAIL**" "max ${mx:-?} us (rt_tail.txt)"; fi
    ;;
vg)
    command -v valgrind >/dev/null || { row vg "**FAIL**" "valgrind missing (sudo apt install valgrind)"; continue; }
    V=$LOG/valgrind; mkdir -p "$V"
    printf '#include <stdlib.h>\n#include <string.h>\nint main(void){char*p=malloc(4096);memset(p,1,4096);p=0;return 0;}\n' > "$V/leak.c"
    gcc -g -O0 -o "$V/leak" "$V/leak.c"
    valgrind --leak-check=full --log-file="$V/leak.txt" "$V/leak" >/dev/null 2>&1
    if ! grep -q 'definitely lost: 4,096 bytes' "$V/leak.txt"; then
        row vg "**FAIL**" "self-check: valgrind did not report the planted 4 KiB leak: nothing below would mean anything"; continue
    fi
    # run_l5_policy.sh writes its log_l5_policy_* directory into the current directory
    (cd "$V" && VALGRIND=1 CASES=l503 "$ROOT/apps/ecm_run/run_l5_policy.sh") > "$V/l503.txt" 2>&1; r=$?
    f=$(ls -t "$V"/log_l5_policy_*/valgrind_l503.txt 2>/dev/null | head -1)
    dl=$(grep -o 'definitely lost: [0-9,]* bytes' "$f" 2>/dev/null | head -1)
    es=$(grep -o 'ERROR SUMMARY: [0-9]* errors' "$f" 2>/dev/null | head -1)
    if [ $r = 0 ] && [ -n "$f" ] && ! grep -q 'definitely lost: [1-9]' "$f" && grep -q 'ERROR SUMMARY: 0 errors' "$f"; then
        row vg ok "self-check caught 4 096 B; ecm_run L5-03 (cable out/in) under valgrind: ${dl:-no leak summary (all freed)}, $es"
    else row vg "**FAIL**" "L5-03 rc $r; ${dl:-?}; ${es:-?} ($f)"; fi
    ;;
*) row "$s" "**FAIL**" "unknown step" ;;
esac
done

{
    echo "# Phase 12.5 final regression — $(date '+%Y-%m-%d %H:%M'), $(uname -r), $(git -C "$ROOT" describe --always --dirty)"
    echo
    echo "| Step | Status | Result |"
    echo "|---|---|---|"
    printf '%s\n' "${ROWS[@]}"
    if [ -f "$LOG/regression/summary.md" ]; then echo; echo "## Regression"; echo; sed -n '/^| Step/,$p' "$LOG/regression/summary.md"; fi
} > "$LOG/summary.md"
chown -R "$U:" "$LOG" 2>/dev/null
echo; cat "$LOG/summary.md"
[ $FAILS = 0 ]
