#!/usr/bin/env bash
# ==========================================================================
# run_xchg_tsan.sh — Phase 10.4 Q-05: the app <-> RT exchange under TSan,
# with negative controls (same shape as tests/tsan/run_tsan.sh, Phase 7.6).
#
#   ./run_xchg_tsan.sh [ticks]      (default 200000; no root needed)
#
#   1. TSan build: no report, content consistent, 0 torn seqlock reads
#   2. ECM_XRING_BROKEN (producer publishes head relaxed): TSan MUST report
#   3. ECM_XST_BROKEN (reader skips the seq check): torn reads MUST be seen
# Env: CC (default gcc)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-gcc}
N=${1:-200000}
B=$(mktemp -d /tmp/xchg_tsan_XXXXXX)
export TSAN_OPTIONS="halt_on_error=1 exitcode=66 ${TSAN_OPTIONS:-}"
SRC="$HERE/test_xchg_tsan.c $HERE/ecm_xchg.c $HERE/../pdo/ecm_pdo.c"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "=== 1. exchange + seqlock under TSan ($N ticks)"
$CC -fsanitize=thread -O1 -g -std=gnu11 -Wall -Wextra -o "$B/ok" $SRC -lpthread || { bad "build"; exit 1; }
"$B/ok" "$N" > "$B/ok.log" 2>&1; rc=$?
sed 's/^/    /' "$B/ok.log" | grep -v "^    $" | head -8
if [ $rc = 0 ]; then ok "no data race, records consistent, 0 torn"; else bad "rc=$rc (66 = TSan report)"; fi

echo "=== 2. negative control: ring head published with memory_order_relaxed"
$CC -fsanitize=thread -O1 -g -std=gnu11 -DECM_XRING_BROKEN -o "$B/ring_broken" $SRC -lpthread
"$B/ring_broken" "$N" > "$B/ring_broken.log" 2>&1; rc=$?
if [ $rc = 66 ] && grep -q "WARNING: ThreadSanitizer: data race" "$B/ring_broken.log"; then
    ok "TSan reports the missing release (exit 66)"
else
    bad "TSan did NOT report the broken ring (rc=$rc)"
fi

echo "=== 3. negative control: seqlock reader without the seq check"
$CC -O2 -std=gnu11 -DECM_XST_BROKEN -o "$B/st_broken" $SRC -lpthread
"$B/st_broken" "$N" > "$B/st_broken.log" 2>&1; rc=$?
grep "seqlock:" "$B/st_broken.log" | sed 's/^/    /'
if [ $rc != 0 ] && grep -qE "seqlock: [0-9]+ consistent reads, [1-9][0-9]* torn" "$B/st_broken.log"; then
    ok "torn records seen without the seq check"
else
    bad "no torn record seen without the seq check (rc=$rc): the test cannot tell a broken seqlock"
fi

echo "RESULT: $PASS pass, $FAIL fail   (work dir: $B)"
[ $FAIL = 0 ]
