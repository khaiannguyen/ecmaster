#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# uninstall.sh -- Phase 12.3: remove what install.sh installed.
#
#   sudo ./uninstall.sh [--prefix DIR] [--destdir DIR] [--dry-run]
#
# First the boot-time integration (packaging/install_system.sh --uninstall:
# every ecm_run@ / ecmaster-clock@ / ecmaster-net@ instance stopped and
# disabled, port settings restored), then exactly the files of
# /var/lib/ecmaster/files.list, then the empty directories of PREFIX.
# /etc/ecmaster/*.conf written by the user are kept. A .deb install is
# removed with apt instead (apt remove ecmaster / apt purge ecmaster).
# ==========================================================================
set -u
ROOT=$(cd "$(dirname "$0")" && pwd)
PREFIX=/opt/ecmaster; DESTDIR=; DRY=0
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift ;;
        --destdir) DESTDIR=$2; shift ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '4,16p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)"; exit 2 ;;
    esac
    shift
done
[ -z "$DESTDIR" ] && [ $DRY = 0 ] && [ "$(id -u)" != 0 ] && { echo "run as root (or --destdir / --dry-run)"; exit 2; }
if [ -z "$DESTDIR" ] && command -v dpkg-query >/dev/null && dpkg-query -W -f='${Status}' ecmaster 2>/dev/null | grep -q 'install ok'; then
    echo "ecmaster is installed as a .deb: sudo apt remove ecmaster (or purge)"; exit 2
fi
a=(--prefix "$PREFIX" --uninstall); [ -n "$DESTDIR" ] && a+=(--destdir "$DESTDIR"); [ $DRY = 1 ] && a+=(--dry-run)
"$ROOT/packaging/install_system.sh" "${a[@]}"
MANIFEST=/var/lib/ecmaster/files.list
if [ ! -s "$DESTDIR$MANIFEST" ]; then echo "no $MANIFEST: nothing installed by install.sh"; exit 0; fi
echo "=== files ($MANIFEST)"
n=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    if [ $DRY = 1 ]; then echo "  would rm $f"; else rm -f "$DESTDIR$f"; fi
    n=$((n + 1))
done < "$DESTDIR$MANIFEST"
if [ $DRY = 0 ]; then
    rm -f "$DESTDIR$MANIFEST"
    # empty directories under PREFIX, deepest first; PREFIX itself last; never above it
    [ -d "$DESTDIR$PREFIX" ] && find "$DESTDIR$PREFIX" -depth -type d -empty -delete 2>/dev/null
    for d in /var/lib/ecmaster /etc/ecmaster; do [ -d "$DESTDIR$d" ] && rmdir --ignore-fail-on-non-empty "$DESTDIR$d"; done
fi
echo "removed $n files"
[ -d "$DESTDIR/etc/ecmaster" ] && echo "kept /etc/ecmaster: $(ls "$DESTDIR/etc/ecmaster" | tr '\n' ' ')"
exit 0
