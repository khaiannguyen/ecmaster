#!/usr/bin/env bash
# check_xsd.sh -- GD8 8.2: validate the soft_bus ESI against the EtherCAT ESI XML schema.
#
# The official schema (ETG, ESI_Schema_V1i21.zip) is for ETG members only and
# TwinCAT 4024 does not install it. We validate against schema version 1.17 as
# published in synapticon/siitool (pinned commit below); the files are fetched,
# never committed. Set XSD_DIR to a folder that already holds EtherCATInfo.xsd +
# EtherCATBase.xsd (e.g. the official 1.21 files) to use those instead.
#
# Includes a negative control: the same ESI without its <Type> element must FAIL.
#
# Usage: tools/esi/check_xsd.sh [esi.xml]      (default config/esi/softbus_esi.xml)
# Needs: xmllint (libxml2-utils); curl only when XSD_DIR is not set.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
ESI=${1:-$ROOT/config/esi/softbus_esi.xml}
SIITOOL_COMMIT=c46570adb1228eada44ae1f38bd61a81fab99c98
URL=https://raw.githubusercontent.com/synapticon/siitool/$SIITOOL_COMMIT/examples

command -v xmllint >/dev/null || { echo "xmllint not installed (apt install libxml2-utils)"; exit 2; }
[ -f "$ESI" ] || { echo "ESI $ESI not found"; exit 2; }

if [ -z "${XSD_DIR:-}" ]; then
    XSD_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/ecmaster/esi_xsd_1.17
    mkdir -p "$XSD_DIR"
    for f in EtherCATInfo.xsd EtherCATBase.xsd; do
        [ -s "$XSD_DIR/$f" ] || curl -fsSL "$URL/$f" -o "$XSD_DIR/$f" \
            || { echo "download of $f failed (offline? set XSD_DIR)"; exit 2; }
    done
fi
XSD=$XSD_DIR/EtherCATInfo.xsd
echo "schema: $XSD ($(head -3 "$XSD" | grep -o '<!--[0-9.]*-->' || echo 'version ?'))"

PASS=0; FAIL=0
if xmllint --noout --schema "$XSD" "$ESI"; then
    echo "  [PASS] $(basename "$ESI") validates"; PASS=$((PASS + 1))
else
    echo "  [FAIL] $(basename "$ESI") does not validate"; FAIL=$((FAIL + 1))
fi

# negative control: remove the first <Type ...>...</Type> of a Device -> must fail
NEG=$(mktemp /tmp/esi_neg.XXXXXX.xml)
awk '!done && /<Type ProductCode=/ { done = 1; next } { print }' "$ESI" > "$NEG"
if xmllint --noout --schema "$XSD" "$NEG" 2>/dev/null; then
    echo "  [FAIL] negative control: ESI without <Type> still validates (schema not applied?)"; FAIL=$((FAIL + 1))
else
    echo "  [PASS] negative control: ESI without <Type> is rejected"; PASS=$((PASS + 1))
fi
rm -f "$NEG"
echo "RESULT: $PASS pass, $FAIL fail"
[ $FAIL = 0 ]
