#!/usr/bin/env bash
# ==========================================================================
# check_golden.sh — Giai doan 7.5: regression check of the master's frame
# structure against tools/golden/golden_ecm_run.txt.
#
#   sudo ./check_golden.sh              capture, compare; exit 1 on a difference
#   sudo UPDATE=1 ./check_golden.sh     capture and REWRITE the golden file
#                                       (only after a deliberate change, and
#                                       commit the new file with it)
#   sudo EXPECT_DIFF=1 ./check_golden.sh   negative control: exit 1 if the
#                                       structure did NOT change
# Env: ECM_RUN SOFT_BUS IF_M IF_S (passed to capture_ecm_run.sh), GOLDEN,
#      ATTEMPTS (default 2)
# Lines starting with '#' (counts) are not compared.
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
GOLDEN=${GOLDEN:-$HERE/golden_ecm_run.txt}
OUT=$(mktemp /tmp/golden_now_XXXXXX.txt)
PCAP=${PCAP:-/tmp/golden_now.pcap}
ATTEMPTS=${ATTEMPTS:-2}
capture () { "$HERE/capture_ecm_run.sh" "$PCAP" > "$OUT" || { echo "capture failed"; exit 2; }; }
capture
if [ "${UPDATE:-0}" = 1 ]; then
    cp "$OUT" "$GOLDEN"
    echo "golden rewritten: $GOLDEN ($(grep -vc '^#' "$GOLDEN") structure lines)"
    exit 0
fi
[ -f "$GOLDEN" ] || { echo "no golden file $GOLDEN (run with UPDATE=1 once)"; exit 2; }
# A frame lost on a busy non-RT host makes SOEM retry during configuration
# (one more EEPROM read, say): timing, not behaviour. A real change differs
# on every attempt, so retrying cannot hide it.
SAME=0
for n in $(seq 1 "$ATTEMPTS"); do
    if diff -u <(grep -v '^#' "$GOLDEN") <(grep -v '^#' "$OUT") > "$OUT.diff"; then SAME=1; break; fi
    [ "${EXPECT_DIFF:-0}" = 1 ] && break
    [ "$n" -lt "$ATTEMPTS" ] && { echo "attempt $n differs, capturing again"; capture; }
done
if [ "${EXPECT_DIFF:-0}" = 1 ]; then
    if [ $SAME = 1 ]; then
        echo "NEGATIVE CONTROL FAILED: the structure did not change, the golden check cannot see this change"
        exit 1
    fi
    echo "negative control OK: the change is visible in the structure:"
    head -20 "$OUT.diff"
    exit 0
fi
if [ $SAME = 1 ]; then
    echo "golden OK: structure identical to $GOLDEN ($(grep -vc '^#' "$GOLDEN") lines)"
    exit 0
fi
echo "GOLDEN MISMATCH (- golden, + this run; pcap: $PCAP):"
cat "$OUT.diff"
exit 1
