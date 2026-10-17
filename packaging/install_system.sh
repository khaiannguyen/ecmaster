#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# install_system.sh -- Phase 12.2: the boot-time integration of ecmaster
# (systemd units, helpers, NetworkManager, /etc/ecmaster). Nothing is
# enabled unless asked: after a plain install the machine boots as before.
#
#   sudo packaging/install_system.sh --iface enP1p1s0 --enable-net
#   sudo packaging/install_system.sh --dry-run --iface enP1p1s0
#   sudo packaging/install_system.sh --uninstall
#   packaging/install_system.sh --destdir /tmp/pkg --unitdir /lib/systemd/system   (package build)
#
#   --prefix DIR     where ecmaster lives (default /opt/ecmaster): helpers go
#                    to DIR/libexec, ecmaster-bootprofile to DIR/sbin; the
#                    binaries (DIR/bin) are installed by install.sh (Phase 12.3)
#   --iface IF       EtherCAT port (repeatable): NetworkManager leaves it alone
#                    (/etc/NetworkManager/conf.d/99-ecmaster-unmanaged.conf)
#   --enable-net     also systemctl enable --now ecmaster-net@IF for each --iface
#   --unitdir DIR    default /etc/systemd/system
#   --destdir DIR    install into DIR (no systemctl / nmcli calls)
#   --dry-run        print what would be done
#   --uninstall      stop and disable every ecm_run@ / ecmaster-clock@ /
#                    ecmaster-net@ instance (net: settings restored), remove
#                    exactly the files of the manifest; /etc/ecmaster/*.conf
#                    written by the user are kept
#
# Manifest: /var/lib/ecmaster/system.list (one installed path per line).
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/opt/ecmaster; DESTDIR=; UNITDIR=/etc/systemd/system
IFACES=(); ENABLE_NET=0; DRY=0; UNINSTALL=0
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift ;;
        --destdir) DESTDIR=$2; shift ;;
        --unitdir) UNITDIR=$2; shift ;;
        --iface) IFACES+=("$2"); shift ;;
        --enable-net) ENABLE_NET=1 ;;
        --dry-run) DRY=1 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) sed -n '4,30p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)"; exit 2 ;;
    esac
    shift
done
case $PREFIX in /*) ;; *) echo "--prefix must be absolute"; exit 2 ;; esac
for i in "${IFACES[@]}"; do
    [[ "$i" =~ ^[A-Za-z0-9_.:-]{1,15}$ ]] || { echo "bad interface name '$i'"; exit 2; }
done
[ $ENABLE_NET = 1 ] && [ ${#IFACES[@]} = 0 ] && { echo "--enable-net needs --iface"; exit 2; }
[ -z "$DESTDIR" ] && [ $DRY = 0 ] && [ "$(id -u)" != 0 ] && { echo "run as root (or --destdir / --dry-run)"; exit 2; }

MANIFEST=/var/lib/ecmaster/system.list
NMCONF=/etc/NetworkManager/conf.d/99-ecmaster-unmanaged.conf
live () { [ -z "$DESTDIR" ] && [ -d /run/systemd/system ] && command -v systemctl >/dev/null; }
do_ () { if [ $DRY = 1 ]; then echo "  would: $*"; else "$@"; fi; }

if [ $UNINSTALL = 1 ]; then
    echo "=== uninstall"
    if live; then
        for t in ecm_run ecmaster-clock ecmaster-net; do        # masters first, the port last
            for u in $( { systemctl list-units --all --plain --no-legend "$t@*" 2>/dev/null | awk '{print $1}'
                          systemctl list-unit-files --plain --no-legend "$t@*" 2>/dev/null | awk '{print $1}'
                          ls /etc/systemd/system/*.wants/ 2>/dev/null | grep "^$t@." ; } | grep "^$t@..*\.service$" | sort -u); do
                echo "  disable --now $u"
                do_ systemctl disable --now "$u" >/dev/null 2>&1
            done
        done
    fi
    if [ -s "$DESTDIR$MANIFEST" ]; then
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            echo "  rm $f"
            do_ rm -f "$DESTDIR$f"
        done < "$DESTDIR$MANIFEST"
        do_ rm -f "$DESTDIR$MANIFEST"
        # Phase 12.3: a port declared later with ecmaster-port is not in the manifest
        if [ -f "$DESTDIR$NMCONF" ] && head -1 "$DESTDIR$NMCONF" | grep -q '^# ecmaster'; then
            echo "  rm $NMCONF"; do_ rm -f "$DESTDIR$NMCONF"
        fi
        # our own directories only, each only when empty (never /opt, /etc...)
        for d in "$PREFIX/libexec" "$PREFIX/sbin" "$PREFIX" /etc/ecmaster /var/lib/ecmaster; do
            [ -d "$DESTDIR$d" ] && do_ rmdir --ignore-fail-on-non-empty "$DESTDIR$d"
        done
    else
        echo "  no manifest ($MANIFEST): nothing installed by this script"
    fi
    if live; then
        do_ systemctl daemon-reload
        command -v nmcli >/dev/null && do_ nmcli general reload conf 2>/dev/null
    fi
    [ -d "$DESTDIR/etc/ecmaster" ] && ls "$DESTDIR/etc/ecmaster" | grep -q . && echo "  kept: $(ls "$DESTDIR/etc/ecmaster" | tr '\n' ' ')(your configuration, in /etc/ecmaster)"
    echo "done"
    exit 0
fi

echo "=== install (prefix $PREFIX, units $UNITDIR${DESTDIR:+, destdir $DESTDIR})"
LIST=()
put () {   # SRC DST MODE: copy with @PREFIX@ / @LIBEXEC@ substituted
    local src=$1 dst=$2 mode=$3
    echo "  $dst"
    LIST+=("$dst")
    [ $DRY = 1 ] && return 0
    mkdir -p "$DESTDIR$(dirname "$dst")"
    sed -e "s#@PREFIX@#$PREFIX#g" -e "s#@LIBEXEC@#$PREFIX/libexec#g" "$src" > "$DESTDIR$dst.tmp" &&
        chmod "$mode" "$DESTDIR$dst.tmp" && mv "$DESTDIR$dst.tmp" "$DESTDIR$dst"
}
put "$HERE/libexec/ecmaster-net.sh"   "$PREFIX/libexec/ecmaster-net.sh"   755
put "$HERE/libexec/ecmaster-clock.sh" "$PREFIX/libexec/ecmaster-clock.sh" 755
put "$HERE/libexec/ecmaster-run"      "$PREFIX/libexec/ecmaster-run"      755
put "$HERE/sbin/ecmaster-bootprofile" "$PREFIX/sbin/ecmaster-bootprofile" 755
put "$HERE/sbin/ecmaster-port"        "$PREFIX/sbin/ecmaster-port"        755   # Phase 12.3
for u in ecmaster-net@.service ecmaster-clock@.service ecm_run@.service; do
    put "$HERE/systemd/$u" "$UNITDIR/$u" 644
done
put "$HERE/etc/ecmaster/example.conf" /etc/ecmaster/example.conf 644
if [ ${#IFACES[@]} -gt 0 ]; then
    devs=$(printf 'interface-name:%s;' "${IFACES[@]}"); devs=${devs%;}
    echo "  $NMCONF (unmanaged: ${IFACES[*]})"
    LIST+=("$NMCONF")
    if [ $DRY = 0 ]; then
        mkdir -p "$DESTDIR$(dirname "$NMCONF")"
        printf '# ecmaster (Phase 12.2): NetworkManager leaves the EtherCAT port(s) alone\n# (it would reset the qdisc and the link). Managed by ecmaster-port / install_system.sh.\n[keyfile]\nunmanaged-devices=%s\n' \
            "$devs" > "$DESTDIR$NMCONF"
    fi
fi
if [ $DRY = 0 ]; then
    mkdir -p "$DESTDIR$(dirname "$MANIFEST")"
    old=$( [ -s "$DESTDIR$MANIFEST" ] && cat "$DESTDIR$MANIFEST")
    printf '%s\n' "$old" "${LIST[@]}" | grep . | sort -u > "$DESTDIR$MANIFEST"
fi
if live; then
    do_ systemctl daemon-reload
    [ ${#IFACES[@]} -gt 0 ] && command -v nmcli >/dev/null && do_ nmcli general reload conf 2>/dev/null
    if [ $ENABLE_NET = 1 ]; then
        for i in "${IFACES[@]}"; do
            echo "  enable --now ecmaster-net@$i"
            do_ systemctl enable --now "ecmaster-net@$i" || echo "  ecmaster-net@$i failed: journalctl -u ecmaster-net@$i"
        done
    fi
fi
echo "done. Nothing else is enabled; see docs/systemd.md (ecm_run@NAME, ecmaster-clock@IF, ecmaster-bootprofile)."
