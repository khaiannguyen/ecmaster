#!/usr/bin/env bash
# check_enicfg.sh -- Phase 9.6: every committed config/eni/X.enicfg is exactly
# what tools/eni/eni2cfg.py makes from config/eni/X.xml today. A stale
# .enicfg (old eni2cfg, edited by hand, XML re-exported) would make ecm_run
# check and run something else than the ENI says.
# Negative control: a copy with one register InitCmd removed must differ.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
for x in "$ROOT"/config/eni/*.xml; do
    b=$(basename "$x" .xml)
    c=$ROOT/config/eni/$b.enicfg
    [ -f "$c" ] || continue
    if python3 "$ROOT/tools/eni/eni2cfg.py" "$x" -o "$TMP/$b.enicfg" 2>"$TMP/err" &&
       diff -q "$c" "$TMP/$b.enicfg" >/dev/null; then
        echo "  [PASS] $b.enicfg up to date"; PASS=$((PASS+1))
    else
        echo "  [FAIL] $b.enicfg differs from eni2cfg.py $b.xml (regenerate it)"; FAIL=$((FAIL+1))
        cat "$TMP/err"
    fi
done
# negative control
c=$ROOT/config/eni/eni_8node_dc_sdo.enicfg
sed '0,/^reg 1 /{/^reg 1 /d}' "$c" > "$TMP/neg.enicfg"
if ! diff -q "$c" "$TMP/neg.enicfg" >/dev/null && [ "$(grep -c '^reg ' "$TMP/neg.enicfg")" -lt "$(grep -c '^reg ' "$c")" ]; then
    echo "  [PASS] negative control: a file with one register InitCmd less differs"; PASS=$((PASS+1))
else
    echo "  [FAIL] negative control"; FAIL=$((FAIL+1))
fi
echo "RESULT: $PASS pass, $FAIL fail"
[ "$FAIL" = 0 ]
