#!/usr/bin/env bash
# ==========================================================================
# run_api_tsan.sh -- Phase 11.1: the api handoff under TSan + negative control
# (same shape as libecmaster/xchg/run_xchg_tsan.sh).
#
#   ./run_api_tsan.sh [puts]       (default 200000; no root needed)
#
#   1. TSan build: no report, payloads whole and in order, nothing lost
#   2. ECM_API_HO_BROKEN (slot state published relaxed): TSan MUST report
# Env: CC (default gcc)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-gcc}
N=${1:-200000}
B=$(mktemp -d /tmp/api_tsan_XXXXXX)
export TSAN_OPTIONS="halt_on_error=1 exitcode=66 ${TSAN_OPTIONS:-}"
SRC="$HERE/test_api_tsan.c $HERE/ecm_api.c $HERE/../telemetry/histogram.c $HERE/../diag/ecm_diag.c"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "=== 1. two-slot handoff under TSan ($N puts)"
$CC -fsanitize=thread -O1 -g -std=gnu11 -Wall -Wextra -o "$B/ok" $SRC -lpthread -lm || { bad "build"; exit 1; }
"$B/ok" "$N" > "$B/ok.log" 2>&1; rc=$?
sed 's/^/    /' "$B/ok.log" | grep -v "^    $" | head -8
if [ $rc = 0 ]; then ok "no data race, payloads whole and in order, puts = got"; else bad "rc=$rc (66 = TSan report)"; fi

echo "=== 2. negative control: slot state published with memory_order_relaxed"
$CC -fsanitize=thread -O1 -g -std=gnu11 -DECM_API_HO_BROKEN -o "$B/broken" $SRC -lpthread -lm
"$B/broken" "$N" > "$B/broken.log" 2>&1; rc=$?
if [ $rc = 66 ] && grep -q "WARNING: ThreadSanitizer: data race" "$B/broken.log"; then
    ok "TSan reports the missing release (exit 66)"
else
    bad "TSan did NOT report the broken handoff (rc=$rc)"
fi

echo "RESULT: $PASS pass, $FAIL fail   (work dir: $B)"
[ $FAIL = 0 ]
