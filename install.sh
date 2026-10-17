#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# install.sh -- Phase 12.3: install ecmaster built from this tree into
# /opt/ecmaster (the same layout as the .deb, so either replaces the other).
#
#   tools/release/build_soem.sh && make all SOEM_DIR=$PWD/build/SOEM
#   sudo ./install.sh [--iface enP1p1s0 --enable-net]
#   sudo ./uninstall.sh
#
#   --prefix DIR     default /opt/ecmaster
#   --iface IF       EtherCAT port (repeatable), see packaging/install_system.sh
#   --enable-net     enable ecmaster-net@IF now and at boot
#   --destdir DIR    stage into DIR (package build: no setcap, no systemctl)
#   --unitdir DIR    systemd units (default /etc/systemd/system)
#   --no-system      binaries, config and docs only (no unit, helper, NM file)
#   --dry-run        print, write nothing
#
# Installs: bin/ ecm_run ecm_diag ecm_peek soft_bus eni2cfg.py ecm_ctl.py,
# config/ (ENI, ESI and profiles of the project; never a profile generated
# from a vendor ESI), share/doc/ecmaster/ (README, LICENSE, THIRD_PARTY,
# docs), VERSION, /etc/profile.d/ecmaster.sh (PATH), then the boot-time
# integration (packaging/install_system.sh: units, helpers, example.conf).
# Capabilities (setcap) as `make caps`. Nothing is enabled without
# --enable-net. Rebuild and run again to replace the binaries (GPLv3 §6).
# Manifest: /var/lib/ecmaster/files.list.
# ==========================================================================
set -u
ROOT=$(cd "$(dirname "$0")" && pwd)
PREFIX=/opt/ecmaster; DESTDIR=; UNITDIR=/etc/systemd/system; DRY=0; SYSTEM=1
SYSARGS=()
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift ;;
        --destdir) DESTDIR=$2; shift ;;
        --unitdir) UNITDIR=$2; shift ;;
        --iface) SYSARGS+=(--iface "$2"); shift ;;
        --enable-net) SYSARGS+=(--enable-net) ;;
        --no-system) SYSTEM=0 ;;
        --dry-run) DRY=1 ;;
        -h|--help) sed -n '4,30p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--help)"; exit 2 ;;
    esac
    shift
done
case $PREFIX in /*) ;; *) echo "--prefix must be absolute"; exit 2 ;; esac
[ -z "$DESTDIR" ] && [ $DRY = 0 ] && [ "$(id -u)" != 0 ] && { echo "run as root (or --destdir / --dry-run)"; exit 2; }

# name in bin/ : source in the tree : capabilities (as the Makefiles' setcap)
BINS=(
    "ecm_run:apps/ecm_run/ecm_run:cap_net_raw,cap_net_admin,cap_ipc_lock,cap_sys_nice+ep"
    "ecm_diag:apps/ecm_diag/ecm_diag:cap_net_raw,cap_net_admin+ep"
    "ecm_peek:apps/ecm_peek/ecm_peek:cap_net_raw,cap_net_admin+ep"
    "soft_bus:tools/soft_bus/soft_bus:cap_net_raw,cap_net_admin+ep"
    "eni2cfg.py:tools/eni/eni2cfg.py:"
    "ecm_ctl.py:tools/gd11/ecm_ctl.py:"
)
miss=0
for b in "${BINS[@]}"; do
    src=$(echo "$b" | cut -d: -f2)
    [ -x "$ROOT/$src" ] || { echo "missing $src: tools/release/build_soem.sh && make all SOEM_DIR=\$PWD/build/SOEM"; miss=1; }
done
[ $miss = 0 ] || exit 2

MANIFEST=/var/lib/ecmaster/files.list
LIST=()
put () {   # SRC DST MODE
    LIST+=("$2")
    if [ $DRY = 1 ]; then echo "  $2"; return 0; fi
    mkdir -p "$DESTDIR$(dirname "$2")"
    install -m "$3" "$1" "$DESTDIR$2.tmp" && mv -f "$DESTDIR$2.tmp" "$DESTDIR$2"
}
echo "=== ecmaster $(cat "$ROOT/VERSION") -> $PREFIX${DESTDIR:+ (destdir $DESTDIR)}"
for b in "${BINS[@]}"; do
    IFS=: read -r name src _ <<< "$b"
    put "$ROOT/$src" "$PREFIX/bin/$name" 755
done
# config: the project's own files; the vendor-ESI profile stays local (.gitignore, LIC-03 T4)
while IFS= read -r f; do
    case $f in config/profiles/is620n.prof|*/twincat/*) continue ;; esac
    put "$ROOT/$f" "$PREFIX/$f" 644
done < <(cd "$ROOT" && find config -type f \( -name '*.enicfg' -o -name '*.xml' -o -name '*.prof' -o -name '*.cal' \
                                            -o -name '*.emcy' -o -name '*.json' -o -name '*.txt' \) | sort)
for f in README.md LICENSE THIRD_PARTY.md ETG1500_COMPLIANCE.md; do put "$ROOT/$f" "$PREFIX/share/doc/ecmaster/$f" 644; done
for f in "$ROOT"/docs/*.md; do put "$f" "$PREFIX/share/doc/ecmaster/docs/$(basename "$f")" 644; done
put "$ROOT/VERSION" "$PREFIX/VERSION" 644
PD=$(mktemp); trap 'rm -f "$PD"' EXIT
printf '# ecmaster (Phase 12.3): commands in the PATH of login shells\ncase ":$PATH:" in *":%s/bin:"*) ;; *) PATH="$PATH:%s/bin:%s/sbin" ;; esac\n' "$PREFIX" "$PREFIX" "$PREFIX" > "$PD"
put "$PD" /etc/profile.d/ecmaster.sh 644

if [ $DRY = 0 ]; then
    mkdir -p "$DESTDIR$(dirname "$MANIFEST")"
    printf '%s\n' "${LIST[@]}" | sort -u > "$DESTDIR$MANIFEST"
    echo "  ${#LIST[@]} files, manifest $MANIFEST"
    if [ -z "$DESTDIR" ]; then
        for b in "${BINS[@]}"; do
            IFS=: read -r name _ cap <<< "$b"
            [ -n "$cap" ] && { setcap "$cap" "$PREFIX/bin/$name" || echo "  setcap $name failed (libcap2-bin?)"; }
        done
        echo "  capabilities: $(getcap "$PREFIX"/bin/* 2>/dev/null | wc -l) binaries"
    fi
fi
if [ $SYSTEM = 1 ]; then
    a=(--prefix "$PREFIX" --unitdir "$UNITDIR" ${SYSARGS[@]+"${SYSARGS[@]}"})
    [ -n "$DESTDIR" ] && a+=(--destdir "$DESTDIR")
    [ $DRY = 1 ] && a+=(--dry-run)
    "$ROOT/packaging/install_system.sh" "${a[@]}" || exit 1
fi
echo "installed. New login shells have $PREFIX/bin in PATH. Next: docs/systemd.md (ecmaster-port add IFACE, /etc/ecmaster/NAME.conf)."
