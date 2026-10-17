#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# test_sys_offline.sh -- Phase 12.2: the boot-time integration without a
# Jetson, without systemd and without root (part of make test-offline, CI).
# ethtool / ip / systemctl / phc2sys are fakes; the real-machine checks are
# tools/release/run_sys_12_2.sh (SYS-01..04).
#
#   N-01..N-05  ecmaster-net.sh up / check / down, first state kept, refusals;
#               negative control: a NIC that ignores -C -> up FAILS
#   C-01..C-03  ecmaster-clock.sh: timesyncd stopped and started again only
#               if it ran before (negative control: it did not -> not started)
#   R-01..R-06  ecmaster-run: conf -> command line, refusals, default route,
#               port not set up, exec, quoted words
#   B-01..B-06  ecmaster-bootprofile (SYS-05): add-p1 + remove-p1 = same bytes,
#               DEFAULT untouched by add-p1, set-default P1 / P0, refusals,
#               dry run, a backup per write
#   I-01..I-04  install_system.sh --destdir: placeholders, manifest,
#               idempotent, uninstall removes exactly the manifest and keeps
#               the user's .conf; units carry Restart=no / ExecStop
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
P=$ROOT/packaging
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PASS=0; FAIL=0
chk () { local n=$1; shift; if eval "$*"; then echo "  PASS $n"; PASS=$((PASS + 1)); else echo "  FAIL $n"; FAIL=$((FAIL + 1)); fi; }

# ---- fakes -----------------------------------------------------------------
F=$T/fake; mkdir -p "$F" "$T/sys/eth9"
echo 0 > "$T/sys/eth9/carrier"; echo 100 > "$T/sys/eth9/speed"
cat > "$F/ethtool" <<'EOF'
#!/usr/bin/env bash
# fake ethtool: state in $FAKE_DIR/<key>
d=$FAKE_DIR; echo "ethtool $*" >> "$d/calls"
g () { cat "$d/$1" 2>/dev/null || echo "$2"; }
case $1 in
-c) echo "Coalesce parameters for $2:"; printf 'rx-usecs:\t\t%s\n' "$(g rx 3)"; printf 'tx-usecs:\t\t3\n' ;;   # tabs as ethtool 6.x
-k) echo "Features for $2:"
    echo "tcp-segmentation-offload: $(g tso on)"
    echo "generic-segmentation-offload: $(g gso on)"
    echo "generic-receive-offload: $(g gro on)" ;;
--show-eee) echo "EEE settings for $2:"; printf '\tEEE status:\t%s\n' "$(g eee 'enabled - active')" ;;
-C) [ -n "${FAKE_STUCK:-}" ] && exit 0; [ "$3" = rx-usecs ] && echo "$4" > "$d/rx" ;;
-K) shift 2; while [ $# -ge 2 ]; do echo "$2" > "$d/$1"; shift 2; done ;;
--set-eee) [ "$4" = off ] && echo disabled > "$d/eee" || echo 'enabled - active' > "$d/eee" ;;
esac
EOF
cat > "$F/ip" <<'EOF'
#!/usr/bin/env bash
echo "ip $*" >> "$FAKE_DIR/calls"
case "$*" in
"link set dev "*" up") [ -d "$FAKE_SYS/$4" ] && echo 1 > "$FAKE_SYS/$4/carrier" ;;
"-o route show default dev "*) [ "${FAKE_DEFAULT:-}" = "$6" ] && echo "default via 192.168.137.1 dev $6 proto dhcp" ;;
esac
exit 0
EOF
cat > "$F/systemctl" <<'EOF'
#!/usr/bin/env bash
echo "systemctl $*" >> "$FAKE_DIR/calls"
case "$1 $2" in
"is-active --quiet") [ "$(cat "$FAKE_DIR/timesyncd" 2>/dev/null)" = active ] ;;
"stop systemd-timesyncd") echo inactive > "$FAKE_DIR/timesyncd" ;;
"start systemd-timesyncd") echo active > "$FAKE_DIR/timesyncd" ;;
esac
EOF
chmod +x "$F"/*
export FAKE_DIR=$T/state FAKE_SYS=$T/sys; mkdir -p "$FAKE_DIR"
NET=(env PATH="$F:$PATH" ECM_SYSNET="$T/sys" ECM_STATE_DIR="$T/run" ECM_LINK_WAIT=1 "$P/libexec/ecmaster-net.sh")
reset_nic () { rm -f "$FAKE_DIR"/{rx,gro,gso,tso,eee,calls}; rm -rf "$T/run"; echo 0 > "$T/sys/eth9/carrier"; }

echo "=== ecmaster-net.sh"
reset_nic
"${NET[@]}" up eth9 > "$T/n1.txt" 2>&1; r=$?
chk "N-01 up: rc 0, rx-usecs 3 -> 0, GRO/GSO/TSO off, EEE off, link up, state saved" \
    "[ $r = 0 ] && [ \$(cat $FAKE_DIR/rx) = 0 ] && [ \$(cat $FAKE_DIR/gro) = off ] && [ \$(cat $FAKE_DIR/tso) = off ] \
     && [ \$(cat $FAKE_DIR/eee) = disabled ] && [ \$(cat $T/sys/eth9/carrier) = 1 ] && grep -qx 'rx_usecs=3' $T/run/net-eth9.saved && grep -qx 'eee=on' $T/run/net-eth9.saved"
"${NET[@]}" up eth9 > /dev/null 2>&1
chk "N-02 up a second time keeps the first saved state (rx_usecs=3, not 0)" "grep -qx 'rx_usecs=3' $T/run/net-eth9.saved"
"${NET[@]}" check eth9 > /dev/null 2>&1; r=$?
chk "N-03 check: rc 0 when set up" "[ $r = 0 ]"
"${NET[@]}" down eth9 > "$T/n3.txt" 2>&1; r=$?
chk "N-03 down: settings restored (rx-usecs 3, GRO/GSO/TSO on, EEE on), state removed" \
    "[ $r = 0 ] && [ \$(cat $FAKE_DIR/rx) = 3 ] && [ \$(cat $FAKE_DIR/gro) = on ] && [ \$(cat $FAKE_DIR/tso) = on ] && grep -q enabled $FAKE_DIR/eee && [ ! -e $T/run/net-eth9.saved ]"
"${NET[@]}" check eth9 > /dev/null 2>&1; r=$?
chk "N-03 check after down: rc 1 (rx-usecs 3)" "[ $r = 1 ]"
reset_nic
FAKE_STUCK=1 "${NET[@]}" up eth9 > "$T/n4.txt" 2>&1; r=$?
chk "N-04 negative: NIC ignores -C rx-usecs 0 -> up rc 1, says NOT configured" "[ $r = 1 ] && grep -q 'NOT configured' $T/n4.txt"
"${NET[@]}" up 'eth9;reboot' > /dev/null 2>&1; r1=$?
"${NET[@]}" up eth7 > /dev/null 2>&1; r2=$?
"${NET[@]}" sideways eth9 > /dev/null 2>&1; r3=$?
chk "N-05 refused: bad name (rc $r1), no interface (rc $r2), bad command (rc $r3)" "[ $r1 = 2 ] && [ $r2 = 1 ] && [ $r3 = 2 ]"
reset_nic; echo 0 > "$T/sys/eth9/carrier"
cat > "$F/ip.nocarrier" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "$F/ip.nocarrier"
IP="$F/ip.nocarrier" "${NET[@]}" up eth9 > "$T/n5.txt" 2>&1; r=$?
chk "N-05 no carrier is reported, not an error of the unit (rc 0)" "[ $r = 0 ] && grep -q 'no carrier' $T/n5.txt"

echo "=== ecmaster-clock.sh"
CLK=(env PATH="$F:$PATH" ECM_STATE_DIR="$T/run" "$P/libexec/ecmaster-clock.sh")
rm -f "$FAKE_DIR/calls"; echo active > "$FAKE_DIR/timesyncd"
"${CLK[@]}" pre eth9 > /dev/null; s1=$(cat "$FAKE_DIR/timesyncd")
"${CLK[@]}" post eth9 > /dev/null; s2=$(cat "$FAKE_DIR/timesyncd")
chk "C-01 timesyncd active: pre stops it ($s1), post starts it again ($s2), state removed" \
    "[ $s1 = inactive ] && [ $s2 = active ] && [ ! -e $T/run/clock-eth9.timesyncd ]"
echo inactive > "$FAKE_DIR/timesyncd"; rm -f "$FAKE_DIR/calls"
"${CLK[@]}" pre eth9 > /dev/null; "${CLK[@]}" post eth9 > /dev/null
chk "C-02 negative: timesyncd was stopped -> post does not start it" \
    "[ \$(cat $FAKE_DIR/timesyncd) = inactive ] && ! grep -q 'start systemd-timesyncd' $FAKE_DIR/calls"
out=$(env PHC2SYS=echo "${CLK[@]}" run eth9)
out2=$(env PHC2SYS=echo PHC2SYS_ARGS="-a -r" "${CLK[@]}" run eth9)
chk "C-03 run: phc2sys -s CLOCK_REALTIME -c eth9 -O 0 -m --step_threshold=1 (PHC2SYS_ARGS replaces it)" \
    "[ \"$out\" = '-s CLOCK_REALTIME -c eth9 -O 0 -m --step_threshold=1' ] && [ \"$out2\" = '-a -r' ]"

echo "=== ecmaster-run"
CD=$T/etc; mkdir -p "$CD"
RUN=(env PATH="$F:$PATH" ETHTOOL="$F/ethtool" "$P/libexec/ecmaster-run" --conf-dir "$CD")
cat > "$CD/demo.conf" <<'EOF'
# comment
ECM_IFACE=enP1p1s0
ECM_ARGS="--n 2 --eni /opt/ecmaster/config/eni/eni_2servo.enicfg --pdo-scan"
ECM_API=yes
EOF
out=$("${RUN[@]}" --print demo 2>&1)
chk "R-01 conf -> ecm_run --iface enP1p1s0 --n 2 ... --api --api-dir /run/ecmaster/demo" \
    "[ \"$out\" = '$P/bin/ecm_run --iface enP1p1s0 --n 2 --eni /opt/ecmaster/config/eni/eni_2servo.enicfg --pdo-scan --api --api-dir /run/ecmaster/demo' ]"
printf 'ECM_IFACE=x0\nECM_ARGS=--n 1 --api --api-dir /tmp/a\n' > "$CD/own.conf"
printf 'ECM_IFACE=x0\nECM_ARGS=--n 1\nECM_API=no\n' > "$CD/noapi.conf"
o1=$("${RUN[@]}" --print own); o2=$("${RUN[@]}" --print noapi)
chk "R-02 ECM_ARGS with --api-dir: not added twice; ECM_API=no: no --api" \
    "[ \"$o1\" = '$P/bin/ecm_run --iface x0 --n 1 --api --api-dir /tmp/a' ] && [ \"$o2\" = '$P/bin/ecm_run --iface x0 --n 1' ]"
bad=0; n=0
for c in 'ECM_IFACE=x0\nECM_ARGS=--n 1\nECM_FOO=1' 'ECM_IFACE=x0\nECM_ARGS=--n 1; reboot' 'ECM_IFACE=x0\nECM_ARGS=--n $(id)' \
         'ECM_IFACE=x0\nECM_ARGS=--n 1 --iface y0' 'ECM_IFACE=x0' 'ECM_ARGS=--n 1' 'ECM_IFACE=x0\nECM_ARGS=--n 1\nECM_API=maybe' \
         'ECM_IFACE=x0\nECM_ARGS=--n 1 | tee /x' 'ECM_IFACE=x0\nexport ECM_ARGS=--n 1'; do
    n=$((n + 1)); printf "$c\n" > "$CD/bad$n.conf"
    "${RUN[@]}" --print "bad$n" > /dev/null 2>&1; [ $? = 2 ] || { bad=$((bad + 1)); echo "    not refused: $c"; }
done
"${RUN[@]}" --print '../demo' > /dev/null 2>&1; [ $? = 2 ] || bad=$((bad + 1))
"${RUN[@]}" --print nosuch > /dev/null 2>&1; [ $? = 2 ] || bad=$((bad + 1))
chk "R-03 refused (exit 2): unknown key, ';', '\$(', --iface, missing key x2, bad yes/no, '|', export, bad name, no file ($bad not refused)" "[ $bad = 0 ]"
printf 'ECM_IFACE=lo\nECM_ARGS=--n 1\nECM_NET_CHECK=no\nECM_BIN=/bin/echo\n' > "$CD/lo.conf"
FAKE_DEFAULT=lo "${RUN[@]}" lo > "$T/r4.txt" 2>&1; r=$?
chk "R-04 refused: the port carries a default route (rc $r)" "[ $r = 2 ] && grep -q 'default route' $T/r4.txt"
printf 'ECM_IFACE=lo\nECM_ARGS=--n 1\nECM_BIN=/bin/echo\n' > "$CD/lonet.conf"
reset_nic; FAKE_STUCK=1 "${RUN[@]}" lonet > "$T/r5a.txt" 2>&1; r1=$?
printf 'ECM_IFACE=lo\nECM_ARGS=--n 1 --link etf\nECM_NET_CHECK=no\nECM_BIN=/bin/echo\n' > "$CD/etf.conf"
cat > "$F/pgrep" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod +x "$F/pgrep"
"${RUN[@]}" etf > "$T/r5b.txt" 2>&1; r2=$?
"${RUN[@]}" lo > "$T/r5c.txt" 2>&1; r3=$?
chk "R-05 refused: port not set up (rc $r1), --link etf without phc2sys (rc $r2); accepted: exec ecm_run (rc $r3)" \
    "[ $r1 = 2 ] && grep -q 'not set up' $T/r5a.txt && [ $r2 = 2 ] && grep -q phc2sys $T/r5b.txt \
     && [ $r3 = 0 ] && grep -qx -- '--iface lo --n 1 --api --api-dir /run/ecmaster/lo' $T/r5c.txt"
rm -f "$F/pgrep"
printf "ECM_IFACE=x0\nECM_ARGS=\"--n 2 --cia402-script '0.5 enable all; 1.5 cos 1 100000 1' --no-tx-ts\"\n" > "$CD/q.conf"
o=$("${RUN[@]}" --print q)
chk "R-06 quoted words: --cia402-script '0.5 enable all; 1.5 cos 1 100000 1' stays one argument" \
    "[ \"\$o\" = \"$P/bin/ecm_run --iface x0 --n 2 --cia402-script '0.5 enable all; 1.5 cos 1 100000 1' --no-tx-ts --api --api-dir /run/ecmaster/q\" ]"

echo "=== ecmaster-bootprofile (SYS-05, on a copy)"
BP="$P/sbin/ecmaster-bootprofile"
E=$T/boot/extlinux.conf; mkdir -p "$T/boot"; cp "$ROOT/tools/gd11/testdata/extlinux.conf" "$E"; cp "$E" "$T/orig.conf"
python3 "$BP" add-p1 --file "$E" > "$T/b1.txt"; r=$?
chk "B-01 add-p1: P1 entry = DEFAULT + cpuidle.off=1, DEFAULT unchanged" \
    "[ $r = 0 ] && grep -q '^LABEL real-time-noidle' $E && grep -q 'cpuidle.off=1' $E && grep -q '^DEFAULT real-time\$' $E"
python3 "$BP" add-p1 --file "$E" > /dev/null 2>&1; r1=$?
python3 "$BP" set-default P1 --file "$E" > /dev/null; d1=$(awk '/^DEFAULT/{print $2}' "$E")
python3 "$BP" remove-p1 --file "$E" > /dev/null 2>&1; r2=$?
python3 "$BP" set-default P0 --file "$E" > /dev/null; d2=$(awk '/^DEFAULT/{print $2}' "$E")
chk "B-02 refused: add-p1 twice (rc $r1), remove-p1 while DEFAULT is P1 (rc $r2); set-default P1 ($d1), P0 ($d2)" \
    "[ $r1 = 2 ] && [ $r2 = 2 ] && [ $d1 = real-time-noidle ] && [ $d2 = real-time ]"
python3 "$BP" remove-p1 --file "$E" > /dev/null
chk "B-03 add-p1 ... remove-p1: the file is the original byte for byte" "cmp -s $E $T/orig.conf"
chk "B-04 one backup per write, none overwritten (4 writes)" "[ \$(ls $T/boot | grep -c 'extlinux.conf.bak-') = 4 ] && cmp -s $T/orig.conf \$(ls $T/boot/extlinux.conf.bak-* | head -1)"
printf 'DEFAULT a\nLABEL a\n  LINUX /x\n  APPEND root=/dev/x isolcpus=3' > "$T/nonl.conf"; cp "$T/nonl.conf" "$T/nonl.orig"
python3 "$BP" add-p1 --file "$T/nonl.conf" > /dev/null && python3 "$BP" remove-p1 --file "$T/nonl.conf" > /dev/null
python3 "$BP" add-p1 --dry-run --file "$T/orig.conf" > "$T/dry.txt"
chk "B-05 file without a final newline: back byte for byte; --dry-run writes nothing" \
    "cmp -s $T/nonl.conf $T/nonl.orig && cmp -s $T/orig.conf $ROOT/tools/gd11/testdata/extlinux.conf && grep -q 'dry run' $T/dry.txt"
printf 'LABEL a\n  LINUX /x\n' > "$T/noapp.conf"; printf 'DEFAULT b\nLABEL b\n  APPEND x cpuidle.off=1\n' > "$T/has.conf"
python3 "$BP" add-p1 --file "$T/noapp.conf" > /dev/null 2>&1; r1=$?
python3 "$BP" add-p1 --file "$T/has.conf" > /dev/null 2>&1; r2=$?
python3 "$BP" set-default nosuch --file "$E" > /dev/null 2>&1; r3=$?
chk "B-06 refused: no DEFAULT/APPEND (rc $r1), cpuidle.off already (rc $r2), unknown label (rc $r3); no file changed" \
    "[ $r1 = 2 ] && [ $r2 = 2 ] && [ $r3 = 2 ] && ! ls $T/noapp.conf.bak-* $T/has.conf.bak-* >/dev/null 2>&1 && [ \$(ls $T/boot | grep -c 'extlinux.conf.bak-') = 4 ]"

echo "=== install_system.sh --destdir"
D=$T/pkg
"$P/install_system.sh" --destdir "$D" --iface enP1p1s0 > "$T/i1.txt" 2>&1; r=$?
M=$D/var/lib/ecmaster/system.list
chk "I-01 installed: 4 helpers, 3 units, example.conf, NM conf, manifest of 9" \
    "[ $r = 0 ] && [ \$(wc -l < $M) = 9 ] && while read -r f; do [ -f \"$D\$f\" ] || exit 1; done < $M"
chk "I-02 placeholders replaced (/opt/ecmaster/libexec in the units), helpers executable, NM conf names the port" \
    "! grep -rq '@PREFIX@\|@LIBEXEC@' $D && grep -q 'ExecStart=/opt/ecmaster/libexec/ecmaster-run %i' $D/etc/systemd/system/ecm_run@.service \
     && [ -x $D/opt/ecmaster/libexec/ecmaster-net.sh ] && grep -qx 'unmanaged-devices=interface-name:enP1p1s0' $D/etc/NetworkManager/conf.d/99-ecmaster-unmanaged.conf"
chk "I-02 units: ecm_run@ Restart=no + SIGTERM, net@ oneshot with ExecStop, clock@ restores timesyncd (ExecStopPost)" \
    "grep -qx 'Restart=no' $D/etc/systemd/system/ecm_run@.service && grep -qx 'KillSignal=SIGTERM' $D/etc/systemd/system/ecm_run@.service \
     && grep -q '^ExecStop=.*ecmaster-net.sh down' $D/etc/systemd/system/ecmaster-net@.service \
     && grep -q '^ExecStopPost=.*ecmaster-clock.sh post' $D/etc/systemd/system/ecmaster-clock@.service"
cp "$M" "$T/m1"; "$P/install_system.sh" --destdir "$D" --iface enP1p1s0 > /dev/null 2>&1
echo 'ECM_IFACE=enP1p1s0' > "$D/etc/ecmaster/demo.conf"
chk "I-03 second install: same manifest (idempotent)" "cmp -s $M $T/m1"
"$P/install_system.sh" --destdir "$D" --uninstall > "$T/i4.txt" 2>&1; r=$?
chk "I-04 uninstall: every manifest file gone, the user's demo.conf kept, nothing else left" \
    "[ $r = 0 ] && [ \"\$(cd $D && find . -type f)\" = './etc/ecmaster/demo.conf' ] && [ ! -d $D/opt/ecmaster ]"
"$P/install_system.sh" --dry-run --destdir "$T/dry" > /dev/null 2>&1
chk "I-04 --dry-run writes nothing" "[ ! -e $T/dry ]"

echo
echo "RESULT: $PASS pass, $FAIL fail"
[ $FAIL = 0 ]
