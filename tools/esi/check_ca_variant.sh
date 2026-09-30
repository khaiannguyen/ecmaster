#!/usr/bin/env bash
# check_ca_variant.sh -- GD9.3: the Complete Access ESI variant
# (config/esi/softbus_esi_ca.xml) is up to date with softbus_esi.xml, valid
# against the schema, and matches the SII of `soft_bus --coe-ca`; the default
# ESI still matches the default SII. Negative control: the CA ESI against the
# default SII (no General category, CoE details 0) must FAIL.
#
# Usage: tools/esi/check_ca_variant.sh     (offline: gcc, python3, xmllint)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

make -s -C "$ROOT/tools/soft_bus" sii_dump >/dev/null || { echo "sii_dump build failed"; exit 2; }
"$ROOT/tools/soft_bus/sii_dump" 4 > "$TMP/sii.bin" 2>/dev/null
"$ROOT/tools/soft_bus/sii_dump" 4 --coe-ca > "$TMP/sii_ca.bin" 2>/dev/null

if python3 "$HERE/make_ca_variant.py" --check >/dev/null; then ok "softbus_esi_ca.xml up to date"
else bad "softbus_esi_ca.xml out of date (run tools/esi/make_ca_variant.py)"; fi
if "$HERE/check_xsd.sh" "$ROOT/config/esi/softbus_esi_ca.xml" >/dev/null 2>&1; then ok "softbus_esi_ca.xml validates"
else bad "softbus_esi_ca.xml does not validate"; fi
if python3 "$HERE/esi_check.py" "$ROOT/config/esi/softbus_esi.xml" "$TMP/sii.bin" > "$TMP/d.txt"; then
    ok "softbus_esi.xml == default SII"; else bad "softbus_esi.xml vs default SII"; grep FAIL "$TMP/d.txt"; fi
if python3 "$HERE/esi_check.py" "$ROOT/config/esi/softbus_esi_ca.xml" "$TMP/sii_ca.bin" > "$TMP/c.txt"; then
    ok "softbus_esi_ca.xml == SII of --coe-ca (CoE details 0x25)"; else bad "softbus_esi_ca.xml vs --coe-ca SII"; grep FAIL "$TMP/c.txt"; fi
if python3 "$HERE/esi_check.py" "$ROOT/config/esi/softbus_esi_ca.xml" "$TMP/sii.bin" > "$TMP/n.txt"; then
    bad "negative control: CA ESI accepted the default SII"
else ok "negative control: CA ESI against the default SII is rejected"; fi
echo "RESULT: $PASS pass, $FAIL fail"
[ "$FAIL" = 0 ]
