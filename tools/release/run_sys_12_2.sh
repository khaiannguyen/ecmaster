#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_sys_12_2.sh -- Phase 12.2 SYS-01..04 on the Jetson: the boot-time
# integration (packaging/install_system.sh) on the real machine. No drive is
# enabled: SYS-02 runs on veth with two virtual IS620N.
#
#   sudo -E tools/release/run_sys_12_2.sh sys01a     install, enable ecmaster-net@IFACE,
#                                                   snapshot the state before; then REBOOT
#   sudo -E tools/release/run_sys_12_2.sh sys01b     after the reboot: port set up with no
#                                                   command typed; negative control: unit
#                                                   disabled -> check_env --boot FAILS
#   sudo -E tools/release/run_sys_12_2.sh sys02 sys03
#   sudo -E tools/release/run_sys_12_2.sh sys04      uninstall: back to the snapshot of sys01a
#
#   SYS-01  after reboot: ecmaster-net@IFACE enabled + active, rx-usecs 0, GRO off,
#           IFACE unmanaged by NetworkManager (check_env --boot)
#           negative: systemctl disable --now (= the settings a boot without the
#           unit gives) -> check_env --boot reports FAIL for rx-usecs and the unit
#   SYS-02  systemctl start ecm_run@sys02 (veth, soft_bus, 2 axes CSP): OP,
#           stats.json in /run/ecmaster/sys02, axes OPERATION_ENABLED;
#           systemctl stop -> final stats.json, S6 walk-down, unit inactive (not
#           restarted); negative: a conf with --iface in ECM_ARGS -> unit failed,
#           ecm_run never started
#   SYS-03  ecmaster-clock@IFACE: phc2sys runs, timesyncd stopped; stop -> timesyncd
#           as before; negative: timesyncd stopped beforehand -> still stopped after
#   SYS-04  install_system.sh --uninstall: no unit file, no NM conf, no manifest,
#           rx-usecs / GRO as in the sys01a snapshot
#
# Env: IFACE (default enP1p1s0) IF_M IF_S SB_PRIO SB_CPU LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IFACE=${IFACE:-enP1p1s0}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}
SNAP=/var/lib/ecmaster-test/sys_12_2.pre
LOG=${LOG:-$ROOT/log_gd12_sys_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
[ $# -gt 0 ] || { sed -n '4,32p' "$0"; exit 2; }
PASS=0; FAIL=0
chk () { local n=$1; shift; if eval "$*"; then echo "  [PASS] $n"; PASS=$((PASS + 1)); else echo "  [FAIL] $n"; FAIL=$((FAIL + 1)); fi; }
rxu () { ethtool -c "$IFACE" 2>/dev/null | awk -F': *' '/^rx-usecs:/{print $2; exit}'; }
gro () { ethtool -k "$IFACE" 2>/dev/null | awk -F': *' '/^generic-receive-offload:/{split($2, a, " "); print a[1]; exit}'; }
CE () { "$ROOT/tools/jetson/check_env.sh" --iface "$IFACE" --boot > "$LOG/check_env_$1.txt" 2>&1; }

for c in "$@"; do
case $c in
sys01a)
    echo "=== SYS-01a install + enable ecmaster-net@$IFACE (snapshot first)"
    mkdir -p "$(dirname "$SNAP")"
    if [ -s "$SNAP" ]; then echo "  snapshot kept from an earlier sys01a: $SNAP"
    else
        { echo "rx_usecs=$(rxu)"; echo "gro=$(gro)"
          echo "nm=$(nmcli -g GENERAL.STATE device show "$IFACE" 2>/dev/null)"
          echo "units=$(ls /etc/systemd/system | grep -c '^ecm')"; } > "$SNAP"
    fi
    sed 's/^/  before: /' "$SNAP"
    "$ROOT/packaging/install_system.sh" --iface "$IFACE" --enable-net > "$LOG/install.txt" 2>&1; r=$?
    chk "SYS-01a install_system.sh rc $r, ecmaster-net@$IFACE active now, rx-usecs $(rxu), GRO $(gro)" \
        "[ $r = 0 ] && systemctl is-active --quiet ecmaster-net@$IFACE && [ \"\$(rxu)\" = 0 ] && [ \"\$(gro)\" = off ]"
    echo "  next: sudo reboot, then: cd ~/projects/ecmaster && sudo -E tools/release/run_sys_12_2.sh sys01b"
    ;;
sys01b)
    echo "=== SYS-01b after the reboot (nothing typed since)"
    up=$(awk '{print int($1)}' /proc/uptime)
    CE boot
    chk "SYS-01 check_env --boot: unit enabled + active, rx-usecs 0, GRO off, unmanaged at boot (uptime ${up} s)" \
        "grep -q 'PASS.*ecmaster-net@$IFACE.service enabled and active' $LOG/check_env_boot.txt && grep -q 'PASS.*rx-usecs 0' $LOG/check_env_boot.txt \
         && grep -q 'PASS.*GRO off' $LOG/check_env_boot.txt && grep -q 'PASS.*$IFACE unmanaged at boot' $LOG/check_env_boot.txt"
    chk "SYS-01 NetworkManager: $IFACE unmanaged" "nmcli -g GENERAL.STATE device show $IFACE 2>/dev/null | grep -q unmanaged"
    systemctl disable --now "ecmaster-net@$IFACE" > /dev/null 2>&1
    CE neg
    chk "SYS-01 negative: unit disabled (settings restored: rx-usecs $(rxu)) -> check_env --boot FAIL on rx-usecs and the unit" \
        "grep -q 'FAIL.*rx-usecs' $LOG/check_env_neg.txt && grep -q 'FAIL.*ecmaster-net@$IFACE.service not enabled' $LOG/check_env_neg.txt"
    systemctl enable --now "ecmaster-net@$IFACE" > /dev/null 2>&1
    chk "SYS-01 enabled again: rx-usecs $(rxu)" "[ \"\$(rxu)\" = 0 ]"
    ;;
sys02)
    echo "=== SYS-02 ecm_run@sys02 on $IF_M (soft_bus, 2 virtual IS620N, CSP)"
    [ -e /etc/systemd/system/ecm_run@.service ] || { echo "  not installed: run sys01a first"; FAIL=$((FAIL + 1)); continue; }
    for i in "$IF_M" "$IF_S"; do ip link show "$i" >/dev/null 2>&1 || { echo "  $i missing: sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"; exit 2; }; done
    pgrep -x soft_bus >/dev/null && { echo "  soft_bus already running"; exit 2; }
    IS=$LOG/is620n_sim.prof
    cp "$ROOT/config/profiles/is620n_min.prof" "$IS"
    for o in "0x6502 32 ad030000 ro" "0x607F 32 00000080 rw"; do
        set -- $o; grep -q "^obj $1 " "$IS" || { echo "obj $1 var 1"; echo "sub $1 0 bits $2 $4 $3"; } >> "$IS"
    done
    CTL=/tmp/soft_bus_12_2.ctl; rm -f "$CTL"
    sb=("$ROOT/tools/soft_bus/soft_bus"); [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "${sb[@]}")
    "${sb[@]}" --iface "$IF_S" --n 2 --dc 32 --no-sm-wd --ctl "$CTL" --profile 1="$IS" --cia402 1 --profile 2="$IS" --cia402 2 \
        --cia402-cal 1="$ROOT/config/profiles/is620n.cal" --cia402-cal 2="$ROOT/config/profiles/is620n.cal" > "$LOG/sb_sys02.log" 2>&1 &
    SBP=$!; sleep 0.5
    cat > /etc/ecmaster/sys02.conf <<EOF
# run_sys_12_2.sh SYS-02 (removed at the end)
ECM_IFACE=$IF_M
ECM_ARGS="--n 2 --eni $ROOT/config/eni/eni_2servo_mm.enicfg --pdo-scan --no-tx-ts --pdo-set 1:0x607F:0=8388608,2:0x607F:0=8388608 --hook cia402 --cia402-lead 20 --axis 1:0,2:0 --axis-modes csp --cia402-script '0.5 enable all; 1.5 cos 1 100000 1; 1.5 cos 2 100000 1'"
ECM_NET_CHECK=no
ECM_BIN=$ROOT/apps/ecm_run/ecm_run
EOF
    python3 /opt/ecmaster/libexec/ecmaster-run --print sys02 > "$LOG/sys02_cmd.txt" 2>&1
    rm -rf /run/ecmaster/sys02
    t0=$(date '+%Y-%m-%d %H:%M:%S')
    systemctl start ecm_run@sys02
    for _ in $(seq 60); do [ -s /run/ecmaster/sys02/stats.json ] && break; sleep 0.5; done
    sleep 6
    cp /run/ecmaster/sys02/stats.json "$LOG/stats_mid.json" 2>/dev/null
    chk "SYS-02 running: stats.json in /run/ecmaster/sys02, final false, both axes OPERATION_ENABLED" \
        "python3 -c \"import json,sys; d=json.load(open('$LOG/stats_mid.json')); a=d['axes']; sys.exit(not (d.get('final') is False and len(a)==2 and all(x['state']=='OPERATION_ENABLED' for x in a)))\""
    systemctl stop ecm_run@sys02
    journalctl -u ecm_run@sys02 --since "$t0" --no-pager > "$LOG/journal_sys02.txt" 2>&1
    chk "SYS-02 stop: final stats.json, S6 walk-down in the journal, unit inactive (Restart=no)" \
        "python3 -c \"import json,sys; sys.exit(json.load(open('/run/ecmaster/sys02/stats.json')).get('final') is not True)\" \
         && grep -q 'shutdown (S6): every axis walked down' $LOG/journal_sys02.txt && [ \"\$(systemctl is-active ecm_run@sys02)\" = inactive ]"
    sed -i 's/^ECM_ARGS="/ECM_ARGS="--iface veth_x /' /etc/ecmaster/sys02.conf
    rm -rf /run/ecmaster/sys02; t0=$(date '+%Y-%m-%d %H:%M:%S')
    systemctl start ecm_run@sys02; sleep 2
    journalctl -u ecm_run@sys02 --since "$t0" --no-pager > "$LOG/journal_sys02_neg.txt" 2>&1
    chk "SYS-02 negative: --iface in ECM_ARGS -> refused, unit failed, ecm_run never started" \
        "grep -q 'refused' $LOG/journal_sys02_neg.txt && [ \"\$(systemctl is-active ecm_run@sys02)\" = failed ] && [ ! -e /run/ecmaster/sys02 ]"
    systemctl reset-failed ecm_run@sys02 2>/dev/null
    rm -f /etc/ecmaster/sys02.conf
    kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null
    ;;
sys03)
    echo "=== SYS-03 ecmaster-clock@$IFACE"
    command -v phc2sys >/dev/null || { echo "  phc2sys missing: sudo apt install linuxptp"; FAIL=$((FAIL + 1)); continue; }
    pgrep -x phc2sys >/dev/null && { echo "  a phc2sys runs already: stop it first"; FAIL=$((FAIL + 1)); continue; }
    ts0=$(systemctl is-active systemd-timesyncd)
    systemctl start systemd-timesyncd
    systemctl start "ecmaster-clock@$IFACE"; sleep 5
    chk "SYS-03 running: phc2sys -c $IFACE, timesyncd $(systemctl is-active systemd-timesyncd)" \
        "pgrep -af phc2sys | grep -q -- '-c $IFACE' && ! systemctl is-active --quiet systemd-timesyncd"
    journalctl -u "ecmaster-clock@$IFACE" -n 20 --no-pager > "$LOG/journal_clock.txt" 2>&1
    systemctl stop "ecmaster-clock@$IFACE"; sleep 1
    chk "SYS-03 stop: phc2sys gone, timesyncd active again (was active before start)" \
        "! pgrep -x phc2sys >/dev/null && systemctl is-active --quiet systemd-timesyncd"
    systemctl stop systemd-timesyncd
    systemctl start "ecmaster-clock@$IFACE"; sleep 2; systemctl stop "ecmaster-clock@$IFACE"; sleep 1
    chk "SYS-03 negative: timesyncd stopped beforehand -> still stopped after the unit" "! systemctl is-active --quiet systemd-timesyncd"
    [ "$ts0" = active ] && systemctl start systemd-timesyncd
    echo "  timesyncd back to its state at the start: $(systemctl is-active systemd-timesyncd) (was $ts0)"
    ;;
sys04)
    echo "=== SYS-04 uninstall"
    "$ROOT/packaging/install_system.sh" --uninstall > "$LOG/uninstall.txt" 2>&1; r=$?
    left=$(ls /etc/systemd/system | grep -E '^(ecm_run|ecmaster-)' ; ls /etc/systemd/system/*.wants 2>/dev/null | grep -E '^(ecm_run|ecmaster-)')
    chk "SYS-04 rc $r: no unit, no NM conf, no manifest, no /opt/ecmaster/libexec" \
        "[ $r = 0 ] && [ -z \"$left\" ] && [ ! -e /etc/NetworkManager/conf.d/99-ecmaster-unmanaged.conf ] && [ ! -e /var/lib/ecmaster/system.list ] && [ ! -e /opt/ecmaster/libexec ]"
    if [ -s "$SNAP" ]; then
        want_rx=$(sed -n 's/^rx_usecs=//p' "$SNAP"); want_gro=$(sed -n 's/^gro=//p' "$SNAP")
        chk "SYS-04 port as before sys01a: rx-usecs $(rxu) (was $want_rx), GRO $(gro) (was $want_gro)" \
            "[ \"\$(rxu)\" = '$want_rx' ] && [ \"\$(gro)\" = '$want_gro' ]"
        rm -f "$SNAP"
    else
        echo "  no snapshot of sys01a: port state not compared"
    fi
    echo "  NetworkManager after the next reboot manages $IFACE again unless something else says otherwise (nmcli device status)"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL + 1)) ;;
esac
done
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
