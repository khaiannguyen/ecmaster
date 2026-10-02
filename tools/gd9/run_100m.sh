#!/usr/bin/env bash
# ==========================================================================
# run_100m.sh — Phase 9.2: the i226 at 100 Mbit/s (LAN9252 and most
# servos only speak 100BASE-TX). Plan: claude/giai_doan_9_ke_hoach.md §4.
#
# Rig (Phase 8.5): loopback cable i226 enP1p1s0 (ecm_run) <-> onboard enP8p1s0
# (soft_bus). The onboard port is forced to 100/Full, the i226 negotiates
# down. SSH must NOT go over either port (use USB-C 192.168.55.1 or Wi-Fi).
#
#   sudo -E tools/gd9/run_100m.sh                 all cases
#   sudo -E CASES="h01 h02" tools/gd9/run_100m.sh
#
#   h01  link 100/Full on both ends, stable for H01_S s (default 600): no
#        carrier change, no rx/tx errors, no igc/r8168 link messages
#   h02  EEE state of both ports; FIX_EEE=1 turns it off on the i226
#   h03  txtime_probe on the i226 with mqprio + etf offload (delta 400 us),
#        H03_S s (default 600): missed / invalid / launch_err at 100 Mbit/s,
#        plus the af_packet baseline (-n, 60 s). Numbers are NOT derived
#        from the 1 Gbit/s run (Phase 8.5)
#   h04  R-02 short: ecm_run N=8 (4 motion + 4 IO) against soft_bus on the
#        onboard port, R04_S s (default 600) per backend: af_packet, then
#        --link etf (lead 350 / asap 150, ns/byte from the link speed = 80)
#
# Before: phc2sys running (-s CLOCK_REALTIME -c enP1p1s0 -O 0),
#         systemd-timesyncd stopped, NetworkManager not managing either port,
#         jetson_clocks, GRO/GSO/TSO off on the i226. The script checks these
#         and refuses what would make the numbers meaningless.
# After:  RESTORE=1 (default) deletes the qdisc and puts the onboard port
#         back to autoneg; the i226 then renegotiates 1000 Mbit/s.
#
# Env: I226 ONB SOFT_BUS ECM_RUN PROBE CLOCK_CHECK SB_CPU SB_PRIO RT_CPU
#      H01_S H03_S R04_S FIX_EEE RESTORE
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
I226=${I226:-enP1p1s0}; ONB=${ONB:-enP8p1s0}
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
PROBE=${PROBE:-$ROOT/tools/etf/txtime_probe}
CLOCK_CHECK=${CLOCK_CHECK:-$ROOT/tools/etf/clock_check}
SB_CPU=${SB_CPU:-4}; SB_PRIO=${SB_PRIO:-79}; RT_CPU=${RT_CPU:-3}
H01_S=${H01_S:-600}; H03_S=${H03_S:-600}; R04_S=${R04_S:-600}
FIX_EEE=${FIX_EEE:-0}; RESTORE=${RESTORE:-1}
CASES=${CASES:-"h01 h02 h03 h04"}
LOG=${LOG:-log_gd9_100m_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"
PASS=0; FAIL=0; INFO=()
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
note () { echo "  [INFO] $1"; INFO+=("$1"); }

[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 2; }
for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f (make all)"; exit 2; }; done
for t in txtime_probe clock_check; do
    [ -x "$ROOT/tools/etf/$t" ] || gcc -O2 -Wall -Wextra -o "$ROOT/tools/etf/$t" "$ROOT/tools/etf/$t.c" \
        || { echo "build of tools/etf/$t failed"; exit 2; }
done

speed () { cat /sys/class/net/$1/speed 2>/dev/null || echo -1; }
duplex () { cat /sys/class/net/$1/duplex 2>/dev/null || echo ?; }
carrier_changes () { cat /sys/class/net/$1/carrier_changes 2>/dev/null || echo -1; }
errs () { local d=/sys/class/net/$1/statistics
          echo $(( $(cat $d/rx_errors) + $(cat $d/tx_errors) + $(cat $d/rx_crc_errors) + $(cat $d/rx_dropped) )); }

# ---- preconditions (fail early: a wrong setup gives numbers that look fine)
echo "=== preconditions"
if command -v nmcli >/dev/null; then
    for i in $I226 $ONB; do
        st=$(nmcli -t -f DEVICE,STATE device 2>/dev/null | awk -F: -v d=$i '$1==d{print $2}')
        case "$st" in unmanaged|"") ok "$i not managed by NetworkManager";;
                      *) bad "$i managed by NetworkManager ($st): nmcli device set $i managed no";; esac
    done
fi
# SSH over a port under test disturbs the measurement (i226.md condition 7)
srv=$(echo "${SSH_CONNECTION:-}" | awk '{print $3}')
for i in $I226 $ONB; do
    if [ -n "$srv" ] && ip -4 -o addr show dev $i 2>/dev/null | grep -q " $srv/"; then
        bad "this SSH session runs over $i: use USB-C 192.168.55.1 or Wi-Fi"
    fi
done
gro=$(ethtool -k $I226 2>/dev/null | awk '/^generic-receive-offload:/{print $2}')
[ "$gro" = off ] && ok "$I226 GRO off" || { note "$I226 GRO $gro -> turning GRO/GSO/TSO off"; ethtool -K $I226 gro off gso off tso off; }

# ---- force 100 Mbit/s
echo "=== force $ONB to 100/Full (the i226 negotiates down)"
ethtool -s $ONB autoneg on advertise 0x008   # 100baseT/Full only: autoneg stays on, the i226 gets Full (autoneg off -> i226 falls back to half)
for _ in $(seq 30); do [ "$(speed $I226)" = 100 ] && [ "$(speed $ONB)" = 100 ] && break; sleep 1; done
echo "    $I226: $(speed $I226) Mb/s $(duplex $I226)   $ONB: $(speed $ONB) Mb/s $(duplex $ONB)"
chk "both ends 100 Mbit/s Full" "[ '$(speed $I226)$(duplex $I226)' = 100full ] && [ '$(speed $ONB)$(duplex $ONB)' = 100full ]"
sleep 3

qdisc_etf () {
    tc qdisc del dev $I226 root 2>/dev/null
    tc qdisc replace dev $I226 parent root handle 100 mqprio num_tc 3 \
        map 2 2 1 0 2 2 2 2 2 2 2 2 2 2 2 2 queues 1@0 1@1 2@2 hw 0 || return 1
    tc qdisc add dev $I226 parent 100:1 etf clockid CLOCK_TAI delta 400000 offload || return 1
    echo "    qdisc installed, waiting 90 s for the igc reset + phc2sys relock"
    sleep 90
    for _ in $(seq 30); do [ "$(speed $I226)" = 100 ] && break; sleep 1; done
}
offload_on () { tc qdisc show dev $I226 | grep -q "etf.*offload on"; }

for c in $CASES; do
case $c in
h01)
    echo "=== H-01 link stable ${H01_S} s at 100 Mbit/s"
    c0i=$(carrier_changes $I226); c0o=$(carrier_changes $ONB); e0i=$(errs $I226); e0o=$(errs $ONB)
    t0=$(date +%s); dmesg --time-format iso > "$LOG/dmesg_h01_before.txt"
    sleep "$H01_S"
    c1i=$(carrier_changes $I226); c1o=$(carrier_changes $ONB); e1i=$(errs $I226); e1o=$(errs $ONB)
    dmesg --time-format iso | tail -n +"$(( $(wc -l < "$LOG/dmesg_h01_before.txt") + 1 ))" \
        | grep -iE "igc|r8169|r8168|link" > "$LOG/dmesg_h01.txt"
    echo "    carrier changes $I226 +$((c1i-c0i)) $ONB +$((c1o-c0o)); errors $I226 +$((e1i-e0i)) $ONB +$((e1o-e0o))"
    chk "H-01 no carrier change on either end" "[ $((c1i-c0i)) = 0 ] && [ $((c1o-c0o)) = 0 ]"
    chk "H-01 no rx/tx/crc error or drop" "[ $((e1i-e0i)) = 0 ] && [ $((e1o-e0o)) = 0 ]"
    chk "H-01 no link message in dmesg" "[ ! -s $LOG/dmesg_h01.txt ]"
    chk "H-01 still 100 Mbit/s Full" "[ '$(speed $I226)$(duplex $I226)' = 100full ]"
    ;;
h02)
    echo "=== H-02 EEE"
    for i in $I226 $ONB; do ethtool --show-eee $i > "$LOG/eee_$i.txt" 2>&1; done
    e=$(grep -iE "^EEE status" "$LOG/eee_$I226.txt" | head -1)
    note "$I226 ${e:-EEE status not reported}"; note "$ONB $(grep -iE '^EEE status' "$LOG/eee_$ONB.txt" | head -1)"
    if echo "$e" | grep -qi "enabled - active"; then
        if [ "$FIX_EEE" = 1 ]; then ethtool --set-eee $I226 eee off; sleep 3
             chk "H-02 EEE turned off on $I226" "ethtool --show-eee $I226 | grep -qiE 'EEE status: *(disabled|not supported)'"
        else bad "H-02 EEE active on $I226 (FIX_EEE=1 turns it off; add to the boot checklist)"; fi
    else ok "H-02 EEE not active on $I226"; fi
    ;;
h03)
    echo "=== H-03 txtime_probe at 100 Mbit/s (etf offload, delta 400 us, lead 200 us)"
    pgrep -x phc2sys >/dev/null || { bad "H-03 phc2sys not running (see i226.md)"; continue; }
    systemctl is-active --quiet systemd-timesyncd && bad "H-03 systemd-timesyncd active (TAI may step)"
    chrt -f 70 taskset -c $RT_CPU "$PROBE" -i $I226 -s 60 -n > "$LOG/probe_af.txt" 2>&1
    grep -E "launch_err|late wake" "$LOG/probe_af.txt" | sed 's/^/    af_packet /'
    qdisc_etf || { bad "H-03 qdisc setup failed"; continue; }
    chk "H-03 etf offload on (before)" "offload_on"
    "$CLOCK_CHECK" -s 20 > "$LOG/clock_h03.txt" 2>&1; grep -E "^T-0" "$LOG/clock_h03.txt" | sed 's/^/    /'
    chk "H-03 T-01/T-02 clock base PASS" "! grep -E '^T-0' $LOG/clock_h03.txt | grep -q FAIL"
    chrt -f 70 taskset -c $RT_CPU "$PROBE" -i $I226 -s "$H03_S" -l 200 -c "$LOG/probe_etf.csv" > "$LOG/probe_etf.txt" 2>&1
    grep -E "ETF drops|tx timestamps|late wake|launch_err" "$LOG/probe_etf.txt" | sed 's/^/    /'
    chk "H-03 etf offload on (after)" "offload_on"
    miss=$(grep "ETF drops:" "$LOG/probe_etf.txt" | head -1 | grep -oE "missed [0-9]+" | awk "{print \$2}"); inv=$(grep "ETF drops:" "$LOG/probe_etf.txt" | head -1 | grep -oE "invalid_param [0-9]+" | awk "{print \$2}")
    chk "H-03 ETF missed = 0 (got ${miss:-?})" "[ '${miss:-x}' = 0 ]"
    chk "H-03 ETF invalid <= 1 (got ${inv:-?}; 1 = a wake-tail > lead, Phase 8)" "[ '${inv:-9}' -le 1 ]"
    tc -s qdisc show dev $I226 > "$LOG/qdisc_h03.txt"
    ;;
h04)
    echo "=== H-04 R-02 short at 100 Mbit/s: N=8, ${R04_S} s per backend"
    for be in af etf; do
        if [ $be = etf ]; then offload_on || qdisc_etf || { bad "H-04 qdisc setup failed"; continue; }
                              A=(--link etf --etf-lead-us 350 --etf-asap-us 150)
        else tc qdisc del dev $I226 root 2>/dev/null; sleep 5; A=(); fi
        chrt -f $SB_PRIO taskset -c $SB_CPU "$SOFT_BUS" --iface $ONB --n 8 --pdo-size 4 --dc 32 --ctl none \
            > "$LOG/sb_h04_$be.log" 2>&1 &
        SBP=$!; sleep 1
        "$ECM_RUN" --iface $I226 --n 8 --motion-slaves 4 --duration-sec "$R04_S" "${A[@]}" \
            > "$LOG/er_h04_$be.log" 2>&1
        RC=$?; kill -INT $SBP; wait $SBP 2>/dev/null
        E=$LOG/er_h04_$be.log
        grep -E "ns/byte|\[WKC\]|\[DC\]|\[POLICY\]|\[LINK\]|overrun" "$E" | head -12 | sed "s/^/    $be /"
        chk "H-04 $be ecm_run rc 0" "[ $RC = 0 ]"
        chk "H-04 $be never LOST" "grep -q 'LOST=0' $E"
        [ $be = etf ] && chk "H-04 etf 80 ns/byte picked from the link speed" "grep -q '100 Mbit/s -> 80 ns/byte' $E"
        [ $be = etf ] && chk "H-04 etf missed=0 invalid=0" "grep -q 'missed=0 invalid=0' $E"
    done
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

if [ "$RESTORE" = 1 ]; then
    tc qdisc del dev $I226 root 2>/dev/null
    ethtool -s $ONB autoneg on advertise 0x02f
    echo "restored: qdisc removed, $ONB autoneg on (i226 may need a replug if NO-CARRIER, i226.md)"
fi
for i in "${INFO[@]}"; do echo "INFO: $i"; done
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ "$FAIL" = 0 ]
