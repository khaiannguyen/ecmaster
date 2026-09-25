#!/usr/bin/env bash
# ==========================================================================
# run_tsan.sh — Giai doan 7.6: ThreadSanitizer on every lock-free queue.
#
#   ./run_tsan.sh [items_per_queue]      (default 2 000 000; no root needed)
#
#   1. self-check: a program with a deliberate data race MUST be reported.
#      If not, TSan does not work on this machine and nothing below means
#      anything -> FAIL (never a silent pass).
#   2. the stress test (test_spsc_stress.c), TSan + content checks: no report.
#   3. negative control: the same test with ONE memory_order_release in
#      ring_spsc.c turned into memory_order_relaxed (the producer's publish
#      of head) MUST be reported.
# Env: CC (default gcc; try CC=clang if gcc's TSan does not run)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CC=${CC:-gcc}
N=${1:-2000000}
B=$(mktemp -d /tmp/tsan_XXXXXX)
export TSAN_OPTIONS="halt_on_error=1 exitcode=66 second_deadlock_stack=1 ${TSAN_OPTIONS:-}"
CFLAGS="-fsanitize=thread -O1 -g -std=gnu11 -Wall -Wextra -I$ROOT"
SRC_LIB="$ROOT/libecmaster/telemetry/turnaround.c $ROOT/libecmaster/telemetry/histogram.c \
         $ROOT/libecmaster/policy/ecm_policy.c $ROOT/libecmaster/diag/ecm_diag.c"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "=== environment: $(uname -m), $($CC --version | head -1)"
[ -r /proc/config.gz ] && zcat /proc/config.gz 2>/dev/null | grep -E "^CONFIG_ARM64_VA_BITS=" | sed 's/^/    /'
[ -r /proc/sys/vm/mmap_rnd_bits ] && echo "    vm.mmap_rnd_bits=$(cat /proc/sys/vm/mmap_rnd_bits)"

hint () {   # explain the usual reasons TSan does not start
    if grep -qE "unexpected memory mapping|FATAL: ThreadSanitizer" "$1"; then
        echo "    TSan could not start. Usual cause: high ASLR entropy on recent kernels."
        echo "    Try:  sudo sysctl vm.mmap_rnd_bits=28   (restore the old value afterwards)"
        echo "    or:   setarch \$(uname -m) -R $0        (ASLR off for this run only)"
    fi
}

echo "=== 1. self-check: a deliberate race must be reported"
cat > "$B/race.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
static int x;
static void *f(void *a) { for (int i = 0; i < 100000; i++) x++; return a; }
int main(void)
{
    pthread_t t;
    pthread_create(&t, 0, f, 0);
    for (int i = 0; i < 100000; i++) x++;
    pthread_join(t, 0);
    printf("race program finished, x=%d\n", x);
    return 0;
}
EOF
$CC -fsanitize=thread -g -O1 "$B/race.c" -o "$B/race" -lpthread 2>"$B/race_build.txt" \
    || { cat "$B/race_build.txt"; bad "self-check: cannot build with -fsanitize=thread ($CC)"; }
if [ -x "$B/race" ]; then
    "$B/race" > "$B/race.txt" 2>&1; rc=$?
    if [ $rc = 66 ] && grep -q "WARNING: ThreadSanitizer: data race" "$B/race.txt"; then
        ok "self-check: TSan reports a deliberate race (exit 66)"
    else
        bad "self-check: deliberate race NOT reported (exit $rc) -- TSan is not working here"
        sed 's/^/    /' "$B/race.txt" | head -8; hint "$B/race.txt"
    fi
fi

echo "=== 2. stress: 5 queues x $N items, producer + consumer each"
if $CC $CFLAGS "$HERE/test_spsc_stress.c" "$ROOT/libecmaster/telemetry/ring_spsc.c" $SRC_LIB \
        -o "$B/stress" -lpthread -lm 2>"$B/stress_build.txt"; then
    t0=$(date +%s)
    "$B/stress" "$N" > "$B/stress.txt" 2>&1; rc=$?
    sed 's/^/  /' "$B/stress.txt" | grep -v "^  *$" | head -12
    echo "    ($(( $(date +%s) - t0 )) s)"
    if [ $rc = 0 ] && ! grep -q "ThreadSanitizer" "$B/stress.txt"; then
        ok "stress: no data race, every element in order and whole"
    elif [ $rc = 66 ]; then
        bad "stress: TSan reported a race"; hint "$B/stress.txt"
    else
        bad "stress: exit $rc"; hint "$B/stress.txt"
    fi
else
    cat "$B/stress_build.txt"; bad "stress: build failed"
fi

echo "=== 3. negative control: ring_spsc producer publishes head with memory_order_relaxed"
sed '0,/atomic_store_explicit(&r->head, head + 1, memory_order_release)/s//atomic_store_explicit(\&r->head, head + 1, memory_order_relaxed)/' \
    "$ROOT/libecmaster/telemetry/ring_spsc.c" > "$B/ring_spsc_broken.c"
if cmp -s "$ROOT/libecmaster/telemetry/ring_spsc.c" "$B/ring_spsc_broken.c"; then
    bad "negative control: the release store in ring_spsc.c was not found (file changed?)"
else
    $CC $CFLAGS -I"$ROOT/libecmaster/telemetry" "$HERE/test_spsc_stress.c" "$B/ring_spsc_broken.c" $SRC_LIB \
        -o "$B/stress_broken" -lpthread -lm
    # the race is timing-dependent: up to 3 attempts before calling it missed
    for try in 1 2 3; do
        "$B/stress_broken" 200000 > "$B/broken.txt" 2>&1; rc=$?
        [ $rc = 66 ] && break
    done
    if [ $rc = 66 ] && grep -q "WARNING: ThreadSanitizer: data race" "$B/broken.txt"; then
        ok "negative control: TSan reports the missing release (exit 66)"
        grep -m1 -A3 "WARNING: ThreadSanitizer" "$B/broken.txt" | sed 's/^/    /'
    else
        bad "negative control: missing release NOT reported (exit $rc)"
    fi
fi

echo "RESULT: $PASS pass, $FAIL fail   (work dir: $B)"
[ $FAIL = 0 ]
