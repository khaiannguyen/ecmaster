#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# test_release_offline.sh -- Phase 12.3: release tooling without SOEM,
# network or root (make test-offline, CI). Works on a copy of the tree with
# stand-in binaries; the real builds are LIC-02 / INS-01 / INS-02 on the
# Jetson (tools/release/run_ins_12_3.sh).
#
#   D-01  make_dist: ecmaster-VERSION/ prefix, LICENSE, VERSION, patches +
#         SHA256SUMS, build_soem.sh, install.sh, packaging; no log, capture,
#         ELF binary, .git, vendor ESI profile
#   D-02  make_dist refuses uncommitted changes (negative control)
#   S-01  build_soem.sh --check-patches ok; negative: a changed patch, a
#         missing patch -> exit 1
#   I-01  install.sh --destdir: binaries, config (no is620n.prof, no TwinCAT
#         project), docs, VERSION, profile.d, system part, manifests
#   I-02  uninstall.sh --destdir: nothing left but the user's .conf;
#         install + uninstall twice give the same trees (INS-01 offline)
#   I-03  install.sh refuses when a binary is missing
#   P-01  ecmaster-port add / remove / status (fakes): NM file, idempotent,
#         two ports, file removed with the last one, bad name refused
#   K-01  build_deb.sh (when dpkg-deb exists): control fields, files,
#         conffiles, no manifests, maintainer scripts parse (sh -n)
#   K-02  negative control: --no-postinst -> the package check finds no setcap
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PASS=0; FAIL=0; SKIP=0
chk () { local n=$1; shift; if eval "$*"; then echo "  PASS $n"; PASS=$((PASS + 1)); else echo "  FAIL $n"; FAIL=$((FAIL + 1)); fi; }
V=$(cat "$ROOT/VERSION")

# a copy of the tree (tracked + new files) with stand-in binaries
C=$T/tree; mkdir -p "$C"
INGIT=0; git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 && [ -z "$(git -C "$ROOT" rev-parse --show-prefix)" ] && INGIT=1
if [ $INGIT = 1 ]; then
    git -C "$ROOT" ls-files -co --exclude-standard -z | (cd "$ROOT" && xargs -0 cp --parents -t "$C" 2>/dev/null)
else    # a source tarball (LIC-02): every file but the build products
    (cd "$ROOT" && find . -type f -not -path './build/*' -not -path './dist/*' -not -name '*.o' -print0 | xargs -0 cp --parents -t "$C")
fi
for b in apps/ecm_run/ecm_run apps/ecm_diag/ecm_diag apps/ecm_peek/ecm_peek tools/soft_bus/soft_bus; do
    printf '#!/bin/sh\necho "%s stand-in $*"\n' "$(basename $b)" > "$C/$b"; chmod 755 "$C/$b"
done
echo "vendor 0x00100000" > "$C/config/profiles/is620n.prof"      # a local vendor-ESI profile: must not be installed

echo "=== make_dist"
if [ $INGIT = 0 ]; then
    echo "  SKIP D-01/D-02: not a git tree (a source tarball makes no tarball)"; SKIP=$((SKIP + 2))
else
G=$T/git; git clone -q "$ROOT" "$G" 2>/dev/null
# the clone is at the last commit; bring the working state of this tree into it as one commit
( cd "$ROOT" && git ls-files -co --exclude-standard -z | xargs -0 cp --parents -t "$G" 2>/dev/null )
git -C "$G" add -A >/dev/null 2>&1; git -C "$G" -c user.name=t -c user.email=t@t commit -qm test >/dev/null 2>&1
"$G/tools/release/make_dist.sh" --out "$T/dist" > "$T/d1.txt" 2>&1; r=$?
TGZ=$T/dist/ecmaster-$V.tar.gz
tar tzf "$TGZ" > "$T/tlist.txt" 2>/dev/null
mkdir -p "$T/x"; tar xzf "$TGZ" -C "$T/x" 2>/dev/null
elf=$(find "$T/x" -type f -exec sh -c 'head -c4 "$1" | grep -q "ELF" && echo "$1"' _ {} \; | wc -l)
chk "D-01 tarball: prefix ecmaster-$V/, LICENSE VERSION SHA256SUMS build_soem install.sh packaging, sha256 file (rc $r)" \
    "[ $r = 0 ] && ! grep -qv '^ecmaster-$V/' $T/tlist.txt && for f in LICENSE VERSION THIRD_PARTY.md patches/SHA256SUMS patches/soem-txtime.patch tools/release/build_soem.sh install.sh uninstall.sh packaging/install_system.sh; do grep -qx \"ecmaster-$V/\$f\" $T/tlist.txt || exit 1; done && (cd $T/dist && sha256sum -c --quiet ecmaster-$V.tar.gz.sha256)"
chk "D-01 tarball: no log / capture ($(grep -cE '\.log$|\.pcapng?$|/log_' "$T/tlist.txt")), no ELF ($elf), no .git, no is620n.prof" \
    "! grep -qE '\.log\$|\.pcapng?\$|/log_|/\.git/|is620n\.prof\$' $T/tlist.txt && [ $elf = 0 ]"
echo "change" >> "$G/README.md"
"$G/tools/release/make_dist.sh" --out "$T/dist2" > "$T/d2.txt" 2>&1; r=$?
chk "D-02 negative: uncommitted change -> refused (rc $r), no tarball" "[ $r = 2 ] && [ ! -e $T/dist2/ecmaster-$V.tar.gz ]"
fi

echo "=== build_soem.sh (patches only, no network)"
"$C/tools/release/build_soem.sh" --check-patches > /dev/null 2>&1; r0=$?
mkdir -p "$T/p1" "$T/p2"; cp "$C"/patches/* "$T/p1/"; cp "$C"/patches/* "$T/p2/"
sed -i '1s/^/ /' "$T/p1/soem-txtime.patch"; rm "$T/p2/soem-mbx-cnt.patch"
PATCH_DIR=$T/p1 "$C/tools/release/build_soem.sh" --check-patches > "$T/s1.txt" 2>&1; r1=$?
PATCH_DIR=$T/p2 "$C/tools/release/build_soem.sh" --check-patches > "$T/s2.txt" 2>&1; r2=$?
PATCH_DIR=$T/p1 "$C/tools/release/build_soem.sh" "$T/never" > /dev/null 2>&1; r3=$?
chk "S-01 patches ok (rc $r0); negative: changed patch (rc $r1), missing patch (rc $r2), and no clone attempted with a bad patch" \
    "[ $r0 = 0 ] && [ $r1 = 1 ] && grep -q 'sha256' $T/s1.txt && [ $r2 = 1 ] && grep -q 'missing' $T/s2.txt && [ $r3 = 1 ] && [ ! -e $T/never ]"

echo "=== install.sh / uninstall.sh --destdir"
D=$T/root
"$C/install.sh" --destdir "$D" --iface enP1p1s0 > "$T/i1.txt" 2>&1; r=$?
P=$D/opt/ecmaster
chk "I-01 install rc $r: 6 programs, ENI/ESI/profiles, docs, VERSION, profile.d, units, helpers, manifests" \
    "[ $r = 0 ] && for b in ecm_run ecm_diag ecm_peek soft_bus eni2cfg.py ecm_ctl.py; do [ -x $P/bin/\$b ] || exit 1; done \
     && [ -f $P/config/eni/eni_2servo.enicfg ] && [ -f $P/config/profiles/is620n.cal ] && [ -f $P/share/doc/ecmaster/LICENSE ] \
     && [ -f $P/share/doc/ecmaster/docs/systemd.md ] && [ \$(cat $P/VERSION) = $V ] && grep -q '/opt/ecmaster/bin' $D/etc/profile.d/ecmaster.sh \
     && [ -f $D/etc/systemd/system/ecm_run@.service ] && [ -x $P/sbin/ecmaster-port ] && [ -s $D/var/lib/ecmaster/files.list ] && [ -s $D/var/lib/ecmaster/system.list ]"
chk "I-01 not installed: the vendor-ESI profile, the TwinCAT project; every manifest path exists" \
    "[ ! -e $P/config/profiles/is620n.prof ] && ! find $P/config -path '*twincat*' | grep -q . && cat $D/var/lib/ecmaster/*.list | while read -r f; do [ -e \"$D\$f\" ] || exit 1; done"
(cd "$D" && find . | sort) > "$T/tree1.txt"
echo 'ECM_IFACE=enP1p1s0' > "$D/etc/ecmaster/demo.conf"
"$C/uninstall.sh" --destdir "$D" > "$T/u1.txt" 2>&1; r1=$?
left1=$(cd "$D" && find . -type f | sort | tr '\n' ' ')
rm -f "$D/etc/ecmaster/demo.conf"
"$C/install.sh" --destdir "$D" --iface enP1p1s0 > /dev/null 2>&1; (cd "$D" && find . | sort) > "$T/tree2.txt"
"$C/uninstall.sh" --destdir "$D" > /dev/null 2>&1; r2=$?
left2=$(cd "$D" && find . -type f | wc -l)
chk "I-02 uninstall rc $r1/$r2: only the user's demo.conf left ($left1); second cycle: same tree, nothing left ($left2 files)" \
    "[ $r1 = 0 ] && [ $r2 = 0 ] && [ \"$left1\" = './etc/ecmaster/demo.conf ' ] && cmp -s $T/tree1.txt $T/tree2.txt && [ $left2 = 0 ] && [ ! -e $P ]"
mv "$C/apps/ecm_diag/ecm_diag" "$T/ecm_diag.bak"
"$C/install.sh" --destdir "$T/root3" > "$T/i3.txt" 2>&1; r=$?
mv "$T/ecm_diag.bak" "$C/apps/ecm_diag/ecm_diag"
chk "I-03 negative: ecm_diag not built -> refused (rc $r), nothing written" "[ $r = 2 ] && grep -q 'missing apps/ecm_diag' $T/i3.txt && [ ! -e $T/root3 ]"

echo "=== ecmaster-port (fakes)"
F=$T/fake; mkdir -p "$F"
printf '#!/bin/sh\necho "systemctl $*" >> %s/calls\n' "$T" > "$F/systemctl"
printf '#!/bin/sh\necho "nmcli $*" >> %s/calls\n' "$T" > "$F/nmcli"
chmod +x "$F"/*
NM=$T/nm/99-ecmaster-unmanaged.conf
PORT=(env ECM_NMCONF="$NM" SYSTEMCTL="$F/systemctl" NMCLI="$F/nmcli" "$C/packaging/sbin/ecmaster-port")
"${PORT[@]}" add eth9 > /dev/null; "${PORT[@]}" add eth9 > /dev/null; "${PORT[@]}" add eth8 > /dev/null
l1=$(sed -n 's/^unmanaged-devices=//p' "$NM")
"${PORT[@]}" remove eth9 > /dev/null; l2=$(sed -n 's/^unmanaged-devices=//p' "$NM")
"${PORT[@]}" remove eth8 > /dev/null
"${PORT[@]}" add 'eth9;x' > /dev/null 2>&1; r=$?
chk "P-01 add twice + second port ($l1), remove one ($l2), last removed -> no file, enable/disable --now called, bad name (rc $r)" \
    "[ '$l1' = 'interface-name:eth8;interface-name:eth9' ] && [ '$l2' = 'interface-name:eth8' ] && [ ! -e $NM ] \
     && grep -q 'enable --now ecmaster-net@eth9' $T/calls && grep -q 'disable --now ecmaster-net@eth8' $T/calls && [ $r = 2 ]"

echo "=== build_deb.sh"
if command -v dpkg-deb >/dev/null; then
    "$C/tools/release/build_deb.sh" --out "$T/deb" > "$T/k1.txt" 2>&1; r=$?
    K=$(ls "$T"/deb/ecmaster_*.deb 2>/dev/null | head -1)
    dpkg-deb -c "$K" 2>/dev/null | awk '{print $6}' > "$T/klist.txt"
    mkdir -p "$T/kc"; dpkg-deb -e "$K" "$T/kc" 2>/dev/null
    has_caps () { grep -q 'setcap cap_net_raw,cap_net_admin,cap_ipc_lock,cap_sys_nice+ep $B/ecm_run' "$1/postinst" 2>/dev/null; }
    chk "K-01 deb rc $r: Package/Version $V-1/Depends, ecm_run + units in /lib/systemd/system + copyright, conffile example.conf" \
        "[ $r = 0 ] && grep -qx 'Package: ecmaster' $T/kc/control && grep -qx 'Version: $V-1' $T/kc/control && grep -q '^Depends: .*ethtool' $T/kc/control \
         && grep -qx './opt/ecmaster/bin/ecm_run' $T/klist.txt && grep -qx './lib/systemd/system/ecm_run@.service' $T/klist.txt \
         && grep -qx './usr/share/doc/ecmaster/copyright' $T/klist.txt && grep -qx '/etc/ecmaster/example.conf' $T/kc/conffiles"
    chk "K-01 no manifest in the deb, maintainer scripts parse, postinst sets the capabilities" \
        "! grep -q 'var/lib/ecmaster' $T/klist.txt && for s in postinst prerm postrm; do sh -n $T/kc/\$s || exit 1; done && has_caps $T/kc"
    "$C/tools/release/build_deb.sh" --no-postinst --out "$T/deb2" > /dev/null 2>&1
    mkdir -p "$T/kc2"; dpkg-deb -e "$(ls "$T"/deb2/*.deb)" "$T/kc2" 2>/dev/null
    chk "K-02 negative: package without postinst -> the capability check fails" "! has_caps $T/kc2"
else
    echo "  SKIP K-01/K-02 (no dpkg-deb)"; SKIP=$((SKIP + 2))
fi

echo
echo "RESULT: $PASS pass, $FAIL fail, $SKIP skip"
[ $FAIL = 0 ]
