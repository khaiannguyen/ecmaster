#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_is7_11_5.sh -- Phase 11.5 IS-07: short cycles on the two real IS620N
# drives (claude/giai_doan_11_ke_hoach.md §6.3).
#
# The loopback rig of part 2a could not answer "does 250 us work": its slave
# (soft_bus on the onboard Realtek port, in user space) adds 60-300 us of
# turnaround, a real ESC well under 1 us. Here the slaves are the two IS620N
# on the i226 (ENI eni_2servo, as in Phase 10.0 R-05/R-09).
#
# NOTHING here enables an axis: no --hook, no --axis, no --cia402-script, no
# --pdo-set. Outputs stay 0 (controlword 0), the drives stay in "Switch on
# disabled". Control power (L1C/L2C) is enough; motor power may stay off.
#
#   sudo -E PROFILE=P0 tools/gd11/run_is7_11_5.sh               ~75 min
#   sudo -E PROFILE=P0 CASES="rig af500 af250" DUR=120 ...       short pass
#
#   rig       i226 link 100 Mbit/s Full, NetworkManager off, SSH not over it,
#             GRO/GSO/TSO off, rx-usecs 0, rt_profile.sh --expect PROFILE,
#             no qdisc left on the i226, onboard port unused (CPU0 then only
#             serves the i226: the c7 comparison of part 2a was blurred by the
#             onboard NIC's interrupts on CPU0)
#   af1000    af_packet 1 ms (Phase 10 reference)
#   af500     af_packet 500 us
#   af250     af_packet 250 us
#   etf250    --link etf delta 100 / lead 50 / asap 25 at 250 us (needs
#             phc2sys + timesyncd stopped; the qdisc is installed here and
#             removed at the end)
#   etf500    the same at 500 us
#   g250      af_packet 250 us --rx-guard-us GUARD (60): the receive window
#             grows from cycle - 150 to cycle - GUARD (Phase 11.5 option,
#             default 150 = previous behaviour); af250 is its control
#   getf250   the same with --link etf (window cycle - asap - GUARD)
#   nodma500  af_packet 500 us --no-dma-latency: the c7 question on a quiet
#             CPU0 (Phase 10: replies ~400 us late). Expect it to be worse
#             than af500 under P0; under P1 (cpuidle.off=1) the P1n answer
#
# Each run: ENI copy for the cycle (tools/eni/eni_cycle.py), DUR s, --pdo-get
# of 0x6041/0x603F/0x6064 at the end, NIC CRC/missed counters before/after.
# Verdict (is_lib.sh): rc 0, overrun 0, NOFRAME <= NOFRAME_MAX (2), policy
# never entered LOST, DC LOCKED 0 unlocks; plus 0x603F = 0 on both drives
# (no Er.E08 sync loss) and no CRC error.
#
# Env: PROFILE CASES IFACE N ENI DUR NOFRAME_MAX ETF_DELTA ETF_LEAD ETF_ASAP GUARD
#      ONB RT_CPU ECM_RUN LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROFILE=${PROFILE:-}
CASES=${CASES:-"rig af1000 af500 af250 nodma500 etf500 etf250"}
IFACE=${IFACE:-enP1p1s0}; ONB=${ONB:-enP8p1s0}; N=${N:-2}
ENI=${ENI:-$ROOT/config/eni/eni_2servo.enicfg}
DUR=${DUR:-600}; NOFRAME_MAX=${NOFRAME_MAX:-2}
ETF_DELTA=${ETF_DELTA:-100}; ETF_LEAD=${ETF_LEAD:-50}; ETF_ASAP=${ETF_ASAP:-25}
GUARD=${GUARD:-60}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
EMCY_MAP=$ROOT/config/emcy/is620n.emcy
case "$PROFILE" in P0|P1) ;; *) echo "PROFILE=P0 or PROFILE=P1 (as booted)"; exit 3 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
for f in "$ECM_RUN" "$ENI" "$EMCY_MAP"; do [ -e "$f" ] || { echo "missing $f"; exit 3; }; done
LOG=${LOG:-$ROOT/log_gd11_is7_${PROFILE}_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
CSV=$LOG/results.csv; REP=$LOG/report.md
echo "case,profile,variant,load,metric,n,min,avg,p50,p99,p999,p9999,max,unit,extra" > "$CSV"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
. "$HERE/is_lib.sh"
speed () { cat /sys/class/net/$1/speed 2>/dev/null || echo -1; }
duplex () { cat /sys/class/net/$1/duplex 2>/dev/null || echo ?; }
nicerr () { local d=/sys/class/net/$IFACE/statistics
            echo $(( $(cat $d/rx_crc_errors) + $(cat $d/rx_missed_errors 2>/dev/null || echo 0) + $(cat $d/rx_errors) )); }
restore () { tc qdisc del dev "$IFACE" root 2>/dev/null; }
trap restore EXIT

GETS=""
for s in $(seq 1 "$N"); do GETS="$GETS${GETS:+,}$s:0x6041:0,$s:0x603F:0,$s:0x6064:0"; done
{
    echo "# Phase 11.5 IS-07 on $N IS620N, profile $PROFILE — $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "- host $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- $IFACE, ENI $(basename "$ENI") (per-cycle copy), ${DUR} s per run, no axis enabled"
    echo
} > "$REP"

run () {   # tag cycle_us variant extra ecm_run args...
    local tag=$1 cyc=$2 var=$3; shift 3
    local eni=$LOG/$(basename "$ENI" .enicfg)_${cyc}us.enicfg E=$LOG/er_$tag.log e0 e1
    [ -s "$eni" ] || python3 "$ROOT/tools/eni/eni_cycle.py" "$ENI" "$cyc" -o "$eni" > "$LOG/eni_$cyc.txt" 2>&1
    [ -s "$eni" ] || { bad "$tag eni_cycle.py failed ($LOG/eni_$cyc.txt)"; return; }
    echo "=== $tag: ${cyc} us $* (${DUR} s)"
    e0=$(nicerr)
    "$ECM_RUN" --iface "$IFACE" --n "$N" --eni "$eni" --pdo-scan --emcy-map "$EMCY_MAP" \
        --motion-cycle-us "$cyc" --duration-sec "$DUR" --pdo-get "$GETS" \
        --diag-file "$LOG/diag_$tag.txt" "$@" > "$E" 2>&1
    echo $? > "$E.rc"
    e1=$(nicerr)
    python3 "$HERE/is_parse.py" ecm_run "$E" is07 "$PROFILE" "$var" idle >> "$CSV"
    grep ",turnaround," "$CSV" | tail -1 | sed 's/^/    /'
    er_verdict "$tag" "$E" "$NOFRAME_MAX"
    local s v
    for s in $(seq 1 "$N"); do
        v=$(sed -nE "s/.*\[PDO\] get $s:0x603F:0 = (0x[0-9A-Fa-f]+).*/\1/p" "$E" | head -1)
        if [ "${v:-x}" = 0x0 ]; then ok "$tag slave $s 0x603F = 0 (no Er.E08)"
        else bad "$tag slave $s 0x603F = ${v:-?} (config/emcy/is620n.emcy)"; fi
    done
    if [ "$e1" = "$e0" ]; then ok "$tag NIC rx crc/missed/errors +0"; else bad "$tag NIC errors +$((e1 - e0))"; fi
}

for c in $CASES; do
case $c in
rig)
    echo "=== rig preconditions"
    if command -v nmcli >/dev/null; then
        st=$(nmcli -t -f DEVICE,STATE device 2>/dev/null | awk -F: -v d="$IFACE" '$1==d{print $2}')
        chk "$IFACE not managed by NetworkManager (${st:-unknown})" "[ '${st:-unmanaged}' = unmanaged ]"
    fi
    srv=$(echo "${SSH_CONNECTION:-}" | awk '{print $3}')
    chk "SSH not over $IFACE" "! { [ -n '$srv' ] && ip -4 -o addr show dev $IFACE 2>/dev/null | grep -q ' $srv/'; }"
    ethtool -K "$IFACE" gro off gso off tso off 2>/dev/null
    ethtool -C "$IFACE" rx-usecs 0 2>/dev/null
    "$HERE/rt_profile.sh" --expect "$PROFILE" --iface "$IFACE" > "$LOG/rt_profile.txt" 2>&1
    chk "rt_profile.sh --expect $PROFILE --iface $IFACE ($LOG/rt_profile.txt)" "[ $? -le 1 ]"
    tc qdisc del dev "$IFACE" root 2>/dev/null
    chk "no ETF qdisc left on $IFACE" "! tc qdisc show dev $IFACE | grep -q etf"
    chk "$IFACE 100 Mbit/s Full ($(speed $IFACE) $(duplex $IFACE); IS620N speak 100BASE-TX)" \
        "[ '$(speed $IFACE)$(duplex $IFACE)' = 100full ]"
    oc=$(cat /sys/class/net/$ONB/carrier 2>/dev/null || echo 0)
    if [ "$oc" = 1 ]; then echo "  [INFO] $ONB has a link: its IRQs also land on CPU0 (unplug the loopback cable for the c7 comparison)"
    else echo "  [INFO] $ONB has no link: CPU0 serves the i226 only"; fi
    if echo " $CASES " | grep -q " etf"; then
        chk "phc2sys running (for etf)" "pgrep -x phc2sys >/dev/null"
        chk "systemd-timesyncd stopped (for etf)" "! systemctl is-active --quiet systemd-timesyncd"
    fi
    [ $FAIL = 0 ] || { echo "rig not ready: fix the FAIL lines above"; exit 2; }
    ;;
af1000)   run af1000 1000 af_1000us ;;
af500)    run af500 500 af_500us ;;
af250)    run af250 250 af_250us ;;
g250)     run g250 250 "af_250us_guard$GUARD" --rx-guard-us "$GUARD" ;;
nodma500) run nodma500 500 af_500us_nodma --no-dma-latency ;;
etf500|etf250|getf250)
    cyc=${c##*etf}; GX=(); var=""
    [ "${c%%etf*}" = g ] && { GX=(--rx-guard-us "$GUARD"); var="_guard$GUARD"; }
    if ! tc qdisc show dev "$IFACE" | grep -q "etf.*offload on"; then
        tc qdisc del dev "$IFACE" root 2>/dev/null
        tc qdisc replace dev "$IFACE" parent root handle 100 mqprio num_tc 3 \
            map 2 2 1 0 2 2 2 2 2 2 2 2 2 2 2 2 queues 1@0 1@1 2@2 hw 0 &&
        tc qdisc add dev "$IFACE" parent 100:1 etf clockid CLOCK_TAI delta $((ETF_DELTA * 1000)) offload ||
            { bad "$c qdisc setup"; continue; }
        echo "    qdisc etf delta $ETF_DELTA us, waiting 90 s (igc reset, phc2sys relock, drives see a link drop)"
        sleep 90
        for _ in $(seq 30); do [ "$(speed "$IFACE")" = 100 ] && break; sleep 1; done
    fi
    chk "$c etf offload on" "tc qdisc show dev $IFACE | grep -q 'etf.*offload on'"
    run "$c" "$cyc" "etf_d${ETF_DELTA}_${cyc}us$var" --link etf --etf-lead-us "$ETF_LEAD" --etf-asap-us "$ETF_ASAP" "${GX[@]}"
    grep -h '\[LINK\] etf' "$LOG/er_$c.log" | sed 's/^/    /'
    chk "$c ETF missed=0 invalid=0" "grep -q 'missed=0 invalid=0' $LOG/er_$c.log"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

python3 "$HERE/is_report.py" "$CSV" >> "$REP"
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo; cat "$REP"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
