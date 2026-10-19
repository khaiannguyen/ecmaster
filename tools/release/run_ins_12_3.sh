#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_ins_12_3.sh -- Phase 12.3 on the Jetson: LIC-02, INS-01, INS-02.
# No drive is enabled (SYS-02 runs on veth).
#
#   sudo -E tools/release/run_ins_12_3.sh lic02     (~10 min, needs github.com)
#   sudo -E tools/release/run_ins_12_3.sh ins01     (needs make all with a SOEM)
#   sudo -E tools/release/run_ins_12_3.sh ins02     (needs make deb; ends with the
#                                                   package purged)
#
#   LIC-02  make dist; the tarball in an empty directory (/tmp/ecm_lic02, no
#           ~/projects/SOEM): build_soem.sh (clone of v2.0.0 + the 2 patches),
#           make all, the binary says VERSION; make test-offline and the golden
#           steps of the Phase 9 regression from that tree
#           negative: the tarball without soem-txtime.patch -> build_soem.sh FAILS
#   INS-01  uninstall.sh, install.sh, uninstall.sh, install.sh, uninstall.sh on the
#           machine: the two installed trees identical (files, modes, capabilities),
#           the two empty states identical, ecmaster-net@IFACE active when installed
#   INS-02  the .deb: apt install -> capabilities, units present and not enabled,
#           example.conf; ecmaster-port add IFACE -> rx-usecs 0; SYS-02 with the
#           installed ecm_run; apt purge -> no file of the package left, no NM
#           file, the port back to its settings before
#           (negative control of the capability check: test_release_offline K-02)
#
# Env: IFACE (default enP1p1s0) LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IFACE=${IFACE:-enP1p1s0}
LOG=${LOG:-$ROOT/log_gd12_ins_$(date +%Y%m%d_%H%M%S)}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
chown "${SUDO_USER:-root}:" "$LOG"     # LIC-02 writes into it as that user (make_dist)
[ $# -gt 0 ] || { sed -n '4,30p' "$0"; exit 2; }
PASS=0; FAIL=0
chk () { local n=$1; shift; if eval "$*"; then echo "  [PASS] $n"; PASS=$((PASS + 1)); else echo "  [FAIL] $n"; FAIL=$((FAIL + 1)); fi; }
V=$(cat "$ROOT/VERSION")
U=${SUDO_USER:-root}
rxu () { ethtool -c "$IFACE" 2>/dev/null | awk -F':' '/^rx-usecs:/{v = $2; gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit}'; }
snap () {   # what an install puts on the machine, with modes and capabilities
    { for d in /opt/ecmaster /etc/ecmaster /lib/systemd/system /etc/systemd/system /etc/profile.d /etc/NetworkManager/conf.d /var/lib/ecmaster; do
          [ -d "$d" ] && find "$d" \( -name '*ecm*' -o -path '/opt/ecmaster*' -o -path '/etc/ecmaster*' -o -path '/var/lib/ecmaster*' \) -printf '%p %m %y\n' 2>/dev/null
      done | sort
      getcap -r /opt/ecmaster 2>/dev/null | sort
      echo "net: $(systemctl is-enabled ecmaster-net@$IFACE 2>/dev/null)/$(systemctl is-active ecmaster-net@$IFACE 2>/dev/null)"; } > "$1"
}

for c in "$@"; do
case $c in
lic02)
    echo "=== LIC-02 rebuild from the source tarball in an empty directory"
    sudo -u "$U" "$ROOT/tools/release/make_dist.sh" --out "$LOG/dist" > "$LOG/dist.txt" 2>&1 || { cat "$LOG/dist.txt"; FAIL=$((FAIL + 1)); continue; }
    TGZ=$LOG/dist/ecmaster-$V.tar.gz
    W=/tmp/ecm_lic02; rm -rf "$W"; mkdir -p "$W"; chown "$U:" "$W"
    sudo -u "$U" tar xzf "$TGZ" -C "$W"
    S=$W/ecmaster-$V
    sudo -u "$U" env -u SOEM_DIR "$S/tools/release/build_soem.sh" "$S/build/SOEM" > "$LOG/build_soem.txt" 2>&1; r1=$?
    sudo -u "$U" make -C "$S" all SOEM_DIR="$S/build/SOEM" > "$LOG/make_all.txt" 2>&1; r2=$?
    chk "LIC-02 build_soem (rc $r1) + make all (rc $r2) from the tarball alone; the binary reports $V" \
        "[ $r1 = 0 ] && [ $r2 = 0 ] && grep -q 'built $S/build/SOEM' $LOG/build_soem.txt && grep -aq '$V' $S/apps/ecm_run/ecm_run"
    sudo -u "$U" make -C "$S" test-offline > "$LOG/test_offline.txt" 2>&1; r=$?
    chk "LIC-02 make test-offline in the clean tree (rc $r)" "[ $r = 0 ]"
    ip link show veth_m >/dev/null 2>&1 || { ip link add veth_m type veth peer name veth_s; ip link set veth_m up; ip link set veth_s up; }
    env STEPS="golden" SOEM_DIR="$S/build/SOEM" RLOG="$LOG/gd9_clean" "$S/tools/gd9/run_regression_gd9.sh" > "$LOG/gd9_clean.txt" 2>&1; r=$?
    chk "LIC-02 golden of the clean tree's binaries: 4+4, ENI, N=1, G-01b, CA + negative controls (rc $r)" \
        "[ $r = 0 ] && grep -q '| golden | .* | ok |' $LOG/gd9_clean/summary.md"
    N=$W/neg; mkdir -p "$N"; tar xzf "$TGZ" -C "$N"; rm "$N/ecmaster-$V/patches/soem-txtime.patch"
    "$N/ecmaster-$V/tools/release/build_soem.sh" "$N/SOEM" > "$LOG/build_soem_neg.txt" 2>&1; r=$?
    chk "LIC-02 negative: tarball without soem-txtime.patch -> build_soem FAILS (rc $r), nothing cloned" \
        "[ $r = 1 ] && grep -q 'missing' $LOG/build_soem_neg.txt && [ ! -e $N/SOEM ]"
    cp "$TGZ" "$TGZ.sha256" "$LOG/" 2>/dev/null
    ;;
ins01)
    echo "=== INS-01 install.sh / uninstall.sh twice"
    [ -x "$ROOT/apps/ecm_run/ecm_run" ] || { echo "  make all first"; FAIL=$((FAIL + 1)); continue; }
    dpkg-query -W -f='${Status}' ecmaster 2>/dev/null | grep -q 'install ok' && { echo "  the .deb is installed: apt purge ecmaster first"; FAIL=$((FAIL + 1)); continue; }
    "$ROOT/uninstall.sh" > "$LOG/u0.txt" 2>&1; snap "$LOG/s_empty1.txt"
    "$ROOT/install.sh" --iface "$IFACE" --enable-net > "$LOG/i1.txt" 2>&1; r1=$?; snap "$LOG/s_inst1.txt"; rx1=$(rxu)
    "$ROOT/uninstall.sh" > "$LOG/u1.txt" 2>&1; r2=$?; snap "$LOG/s_empty2.txt"; rx0=$(rxu)
    "$ROOT/install.sh" --iface "$IFACE" --enable-net > "$LOG/i2.txt" 2>&1; r3=$?; snap "$LOG/s_inst2.txt"
    "$ROOT/uninstall.sh" > "$LOG/u2.txt" 2>&1; r4=$?; snap "$LOG/s_empty3.txt"
    chk "INS-01 rc $r1/$r2/$r3/$r4; installed: 4 binaries with capabilities, ecmaster-net@$IFACE enabled/active, rx-usecs $rx1" \
        "[ $r1$r2$r3$r4 = 0000 ] && [ \$(grep -c '^/opt/ecmaster/bin/.* cap_' $LOG/s_inst1.txt) = 4 ] && grep -qx 'net: enabled/active' $LOG/s_inst1.txt && [ '$rx1' = 0 ]"
    chk "INS-01 the two installs identical, the three empty states identical, port settings restored (rx-usecs $rx0)" \
        "cmp -s $LOG/s_inst1.txt $LOG/s_inst2.txt && cmp -s $LOG/s_empty1.txt $LOG/s_empty2.txt && cmp -s $LOG/s_empty2.txt $LOG/s_empty3.txt && [ '$rx0' != 0 ]"
    chk "INS-01 empty state: no /opt/ecmaster, no unit, no manifest" "! grep -qE '^/opt/ecmaster|ecm_run@|ecmaster-net@|/var/lib/ecmaster' $LOG/s_empty3.txt"
    ;;
ins02)
    echo "=== INS-02 the .deb"
    DEB=$(ls -t "$ROOT"/dist/ecmaster_"$V"-1_*.deb 2>/dev/null | head -1)
    [ -n "$DEB" ] || { echo "  no package: make deb (after make all)"; FAIL=$((FAIL + 1)); continue; }
    [ -s /var/lib/ecmaster/files.list ] || [ -s /var/lib/ecmaster/system.list ] && "$ROOT/uninstall.sh" > "$LOG/u_before.txt" 2>&1
    snap "$LOG/d_before.txt"; rx_before=$(rxu)
    apt-get install -y "$DEB" > "$LOG/apt_install.txt" 2>&1; r=$?
    dpkg -L ecmaster > "$LOG/dpkg_L.txt" 2>/dev/null
    en=$(systemctl list-unit-files 'ecm*' 'ecmaster*' --state=enabled --no-legend 2>/dev/null | wc -l)
    chk "INS-02 apt install rc $r: 4 capabilities, units in /lib/systemd/system, none enabled ($en), example.conf, ecmaster-run --print example" \
        "[ $r = 0 ] && [ \$(getcap /opt/ecmaster/bin/* | wc -l) = 4 ] && [ -f /lib/systemd/system/ecm_run@.service ] && [ $en = 0 ] \
         && [ -f /etc/ecmaster/example.conf ] && /opt/ecmaster/libexec/ecmaster-run --print example | grep -q -- '--eni /opt/ecmaster/config/eni/eni_2servo.enicfg'"
    /opt/ecmaster/sbin/ecmaster-port add "$IFACE" > "$LOG/port_add.txt" 2>&1; r=$?
    chk "INS-02 ecmaster-port add $IFACE (rc $r): ecmaster-net@ active, rx-usecs $(rxu), NM file" \
        "[ $r = 0 ] && systemctl is-active --quiet ecmaster-net@$IFACE && [ \"\$(rxu)\" = 0 ] && grep -q 'interface-name:$IFACE' /etc/NetworkManager/conf.d/99-ecmaster-unmanaged.conf"
    ip link show veth_m >/dev/null 2>&1 || { ip link add veth_m type veth peer name veth_s; ip link set veth_m up; ip link set veth_s up; }
    SYS02_BIN=/opt/ecmaster/bin/ecm_run LOG="$LOG/sys02" "$HERE/run_sys_12_2.sh" sys02 > "$LOG/sys02.txt" 2>&1; r=$?
    grep -E 'PASS|FAIL' "$LOG/sys02.txt" | sed 's/^/    /'
    chk "INS-02 SYS-02 with the packaged ecm_run (rc $r)" "[ $r = 0 ] && grep -q '/opt/ecmaster/bin/ecm_run' $LOG/sys02/sys02_cmd.txt"
    apt-get purge -y ecmaster > "$LOG/apt_purge.txt" 2>&1; r=$?
    left=$(while read -r f; do [ -f "$f" ] && echo "$f"; done < "$LOG/dpkg_L.txt")
    snap "$LOG/d_after.txt"
    chk "INS-02 apt purge rc $r: no file of the package left, no NM file, rx-usecs back to $rx_before ($(rxu)), same state as before" \
        "[ $r = 0 ] && [ -z \"$left\" ] && [ ! -e /etc/NetworkManager/conf.d/99-ecmaster-unmanaged.conf ] && [ \"\$(rxu)\" = '$rx_before' ] && cmp -s $LOG/d_before.txt $LOG/d_after.txt"
    [ -z "$left" ] || echo "$left" | sed 's/^/    left: /'
    echo "  the package is purged. For daily use: sudo apt install $DEB && sudo /opt/ecmaster/sbin/ecmaster-port add $IFACE"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL + 1)) ;;
esac
done
chown -R "$U:" "$LOG" 2>/dev/null
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
