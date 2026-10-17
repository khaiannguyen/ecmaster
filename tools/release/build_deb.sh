#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# build_deb.sh -- Phase 12.3: the binary package (make deb), built on the
# target (arm64 Jetson, JetPack r39.2) from this tree's binaries:
# dist/ecmaster_VERSION-1_ARCH.deb, plain dpkg-deb (no debhelper).
#
#   tools/release/build_soem.sh && make all SOEM_DIR=$PWD/build/SOEM
#   tools/release/build_deb.sh [--out DIR]
#   sudo apt install ./dist/ecmaster_1.0.0-1_arm64.deb      apt remove / apt purge ecmaster
#
# Layout = install.sh --destdir (prefix /opt/ecmaster, units in
# /lib/systemd/system), without its manifests (dpkg keeps the list).
# /etc/ecmaster/example.conf is a conffile. postinst: setcap, daemon-reload,
# nothing enabled; prerm: stop / disable the instances; postrm purge: the
# NetworkManager file of ecmaster-port. --no-postinst: test only (the
# negative control of INS-02: a package without capabilities).
# ==========================================================================
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/dist; NOPOST=0
while [ $# -gt 0 ]; do
    case $1 in --out) OUT=$2; shift ;; --no-postinst) NOPOST=1 ;; *) echo "unknown option $1"; exit 2 ;; esac
    shift
done
command -v dpkg-deb >/dev/null || { echo "build_deb: dpkg-deb missing"; exit 2; }
V=$(cat "$ROOT/VERSION"); ARCH=${ARCH:-$(dpkg --print-architecture)}
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
"$ROOT/install.sh" --destdir "$S/root" --unitdir /lib/systemd/system > "$S/install.txt" 2>&1 || { cat "$S/install.txt"; exit 1; }
P=$S/root
rm -f "$P/var/lib/ecmaster/files.list" "$P/var/lib/ecmaster/system.list"
rmdir "$P/var/lib/ecmaster" "$P/var/lib" "$P/var" 2>/dev/null
mkdir -p "$P/DEBIAN" "$P/usr/share/doc/ecmaster"
install -m 644 "$ROOT/packaging/deb/copyright" "$P/usr/share/doc/ecmaster/copyright"
for f in postinst prerm postrm; do
    [ $f = postinst ] && [ $NOPOST = 1 ] && continue
    install -m 755 "$ROOT/packaging/deb/$f" "$P/DEBIAN/$f"
done
( cd "$P" && find etc -type f | sed 's#^#/#' ) > "$P/DEBIAN/conffiles"
SIZE=$(du -sk --exclude=DEBIAN "$P" | cut -f1)
sed -e "s/@VERSION@/$V-1/" -e "s/@ARCH@/$ARCH/" -e "s/@SIZE@/$SIZE/" "$ROOT/packaging/deb/control.in" > "$P/DEBIAN/control"
find "$P" -type d -exec chmod 755 {} +
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
D=$OUT/ecmaster_${V}-1_${ARCH}.deb
dpkg-deb --root-owner-group -Zxz --build "$P" "$D" > /dev/null || exit 1
(cd "$OUT" && sha256sum "$(basename "$D")" > "$(basename "$D").sha256")
echo "build_deb: $D ($(dpkg-deb -c "$D" | grep -c '^-') files, ${SIZE} KiB installed)"
command -v lintian >/dev/null && { lintian --no-tag-display-limit "$D" > "$OUT/lintian.txt" 2>&1; echo "build_deb: lintian (for reference) -> $OUT/lintian.txt: $(grep -c '^E:' "$OUT/lintian.txt") E, $(grep -c '^W:' "$OUT/lintian.txt") W"; }
exit 0
