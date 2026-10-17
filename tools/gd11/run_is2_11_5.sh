#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_is2_11_5.sh -- Phase 11.5 part 2a: ETF sweep and short cycles on the
# real i226 (claude/giai_doan_11_ke_hoach.md §6.3, docs/rt_profiles.md §6).
#
# Rig (Phase 8.5 / 9.2): loopback cable i226 enP1p1s0 (ecm_run, txtime_probe)
# <-> onboard enP8p1s0 (soft_bus), forced to 100 Mbit/s Full (LAN9252 and
# most servos). SSH must not run over either port (USB-C 192.168.55.1 or
# Wi-Fi). Run inside tmux: about 2 h 40 min with the defaults.
#
#   sudo -E PROFILE=P0 tools/gd11/run_is2_11_5.sh
#   sudo -E PROFILE=P0 CASES="rig is04" DELTAS="400 200 100" PROBE_S=120 ...
#   sudo -E PROFILE=P0 CASES="rig is05" ETF_DELTA=150 CYCLES="1000 250" ...
#
#   rig     preconditions (refuses what would make the numbers meaningless):
#           NetworkManager off both ports, SSH not over them, GRO/GSO/TSO off,
#           rx-usecs 0, phc2sys running, timesyncd stopped, both ends
#           100 Mbit/s Full after forcing the onboard port
#   is04    txtime_probe on the i226, mqprio + etf offload, for each delta in
#           DELTAS: 90 s settle, clock_check T-01/T-02, PROBE_S s at 1 ms with
#           lead = delta - 50 us (lead < delta, Phase 8.5). The smallest delta
#           with missed = invalid = 0 is used by is05 (unless ETF_DELTA is set);
#           it should then be re-run for 1 h (PROBE_S=3600 DELTAS=<it>):
#           0 / 600 000 only bounds the rate at ~5e-6
#   is05    ecm_run N=8 (4 motion + 4 IO) against soft_bus on the onboard
#           port, DUR s per cycle in CYCLES, first --link etf (lead, asap from
#           the chosen delta) then af_packet (qdisc removed): wake / prep_send /
#           occupancy / turnaround, overrun, WKC, DC, ETF counters
#   is05n   af_packet at NCYCLE (500 us) with --no-dma-latency: the c7 question
#           (i226 RX IRQ on CPU0) that the veth rig of part 1 cannot see.
#           Profile P0 -> "P0n", P1 -> "P1n"; the is05 af_packet row at the
#           same cycle is the reference
#   is05p4  Platform 4 profile: soft_bus 1 node cia402_4ax.prof (4 axes),
#           ecm_run --hook cia402, CSP sine on the 4 axes, 250 us, etf then
#           af_packet: adds CiA402 underruns and hook p99.99
#
# Verdict per run (is_lib.sh): rc 0, overrun 0, NOFRAME <= NOFRAME_MAX (2),
# policy never ENTERED LOST, DC LOCKED 0 unlocks; P4 also every axis used.
# (Part 2a only checked overrun/DC and matched a bare 'LOST=0': its 36/0
# passed 250 us runs with 6 % NOFRAME and a P4 run with 18 293 LOST.)
# A run that FAILs is kept in the table: the histograms say why (wake,
# prep_send, turnaround, or the cycle budget: at 250 us the receive deadline
# is next + cycle - asap - 150 us, RX_GUARD_NS, see IS-10).
#
# Before: PROFILE as booted (rt_profile.sh --expect), sudo jetson_clocks,
#   sudo systemctl stop systemd-timesyncd,
#   sudo phc2sys -s CLOCK_REALTIME -c enP1p1s0 -O 0 -m --step_threshold=1 &
# After (RESTORE=1): qdisc removed, onboard port back to autoneg; the i226
#   may need a replug if it stays NO-CARRIER (i226.md).
#
# Env: PROFILE CASES I226 ONB RT_CPU SB_CPU SB_PRIO DELTAS PROBE_S DUR CYCLES
#      NOFRAME_MAX NCYCLE ETF_DELTA ETF_LEAD ETF_ASAP P4_AMP P4_HZ P4_LEAD RESTORE LOG
#      SOFT_BUS ECM_RUN PROBE CLOCK_CHECK
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROFILE=${PROFILE:-}
CASES=${CASES:-"rig is04 is05 is05n is05p4"}
I226=${I226:-enP1p1s0}; ONB=${ONB:-enP8p1s0}
RT_CPU=${RT_CPU:-3}; SB_CPU=${SB_CPU:-4}; SB_PRIO=${SB_PRIO:-79}
DELTAS=${DELTAS:-"400 300 200 150 100"}; PROBE_S=${PROBE_S:-600}
DUR=${DUR:-600}; CYCLES=${CYCLES:-"1000 500 250"}; NCYCLE=${NCYCLE:-500}
ETF_DELTA=${ETF_DELTA:-}; ETF_LEAD=${ETF_LEAD:-}; ETF_ASAP=${ETF_ASAP:-}
P4_AMP=${P4_AMP:-10000}; P4_HZ=${P4_HZ:-0.5}; P4_LEAD=${P4_LEAD:-40}
RESTORE=${RESTORE:-1}
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
PROBE=${PROBE:-$ROOT/tools/etf/txtime_probe}
CLOCK_CHECK=${CLOCK_CHECK:-$ROOT/tools/etf/clock_check}
PROF4=$ROOT/config/profiles/cia402_4ax.prof
case "$PROFILE" in P0|P1) ;; *) echo "PROFILE=P0 or PROFILE=P1 (as booted; tools/gd11/rt_profile.sh)"; exit 3 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
LOG=${LOG:-$ROOT/log_gd11_is2_${PROFILE}_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
CSV=$LOG/results.csv; REP=$LOG/report.md
echo "case,profile,variant,load,metric,n,min,avg,p50,p99,p999,p9999,max,unit,extra" > "$CSV"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
. "$HERE/is_lib.sh"
NOFRAME_MAX=${NOFRAME_MAX:-2}
for f in "$SOFT_BUS" "$ECM_RUN" "$PROF4"; do [ -e "$f" ] || { echo "missing $f (make all caps)"; exit 3; }; done
for t in txtime_probe clock_check; do
    [ -x "$ROOT/tools/etf/$t" ] || gcc -O2 -Wall -Wextra -o "$ROOT/tools/etf/$t" "$ROOT/tools/etf/$t.c" \
        || { echo "build of tools/etf/$t failed"; exit 3; }
done
speed () { cat /sys/class/net/$1/speed 2>/dev/null || echo -1; }
duplex () { cat /sys/class/net/$1/duplex 2>/dev/null || echo ?; }
offload_on () { tc qdisc show dev "$I226" | grep -q "etf.*offload on"; }
qdisc_del () { tc qdisc del dev "$I226" root 2>/dev/null; sleep 5; }
qdisc_etf () {   # delta_us
    tc qdisc del dev "$I226" root 2>/dev/null
    tc qdisc replace dev "$I226" parent root handle 100 mqprio num_tc 3 \
        map 2 2 1 0 2 2 2 2 2 2 2 2 2 2 2 2 queues 1@0 1@1 2@2 hw 0 || return 1
    tc qdisc add dev "$I226" parent 100:1 etf clockid CLOCK_TAI delta $(( $1 * 1000 )) offload || return 1
    echo "    qdisc etf delta $1 us installed, waiting 90 s for the igc reset + phc2sys relock"
    sleep 90
    for _ in $(seq 30); do [ "$(speed "$I226")" = 100 ] && break; sleep 1; done
    offload_on
}
lead_of () { echo $(( $1 - 50 )); }                                    # lead < delta
asap_of () { local a=$(( $1 / 2 )); [ $a -lt 25 ] && a=25; echo $a; }   # half the lead covers the wake tail (part 1: <= 20 us)
restore () {
    [ "$RESTORE" = 1 ] || return
    tc qdisc del dev "$I226" root 2>/dev/null
    ethtool -s "$ONB" autoneg on advertise 0x02f 2>/dev/null
    pkill -INT -x soft_bus 2>/dev/null
    echo "restored: qdisc removed, $ONB autoneg on (replug the i226 if it stays NO-CARRIER)"
}
trap restore EXIT

{
    echo "# Phase 11.5 part 2a (IS-04, IS-05), profile $PROFILE — $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "- host $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- rig: $I226 (ecm_run, core $RT_CPU) <-> $ONB (soft_bus, core $SB_CPU FIFO $SB_PRIO), 100 Mbit/s"
    echo "- probe ${PROBE_S} s per delta; ecm_run ${DUR} s per run"
    echo
} > "$REP"

SBP=""
sb_start () {   # tag, extra soft_bus args...
    local tag=$1; shift
    chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS" --iface "$ONB" --dc 32 --ctl none "$@" \
        > "$LOG/sb_$tag.log" 2>&1 &
    SBP=$!; sleep 1
    kill -0 $SBP 2>/dev/null
}
sb_stop () { [ -n "$SBP" ] && { kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null; }; SBP=""; }

run_n8 () {   # tag cycle_us variant(csv) extra ecm_run args...
    local tag=$1 cyc=$2 var=$3; shift 3
    echo "=== $tag: ecm_run N=8 ${cyc} us $* (${DUR} s)"
    sb_start "$tag" --n 8 --pdo-size 4 || { bad "$tag soft_bus did not start ($LOG/sb_$tag.log)"; return; }
    "$ECM_RUN" --iface "$I226" --n 8 --motion-slaves 4 --motion-cycle-us "$cyc" --duration-sec "$DUR" \
        --diag-file "$LOG/diag_$tag.txt" "$@" > "$LOG/er_$tag.log" 2>&1
    echo $? > "$LOG/er_$tag.log.rc"; sb_stop
    python3 "$HERE/is_parse.py" ecm_run "$LOG/er_$tag.log" "${tag%%_*}" "$PROFILE" "$var" idle >> "$CSV"
    grep ",wake_jitter," "$CSV" | tail -1 | sed 's/^/    /'
    er_verdict "$tag" "$LOG/er_$tag.log" "$NOFRAME_MAX"
}

run_p4 () {   # tag variant extra ecm_run args...
    local tag=$1 var=$2; shift 2
    echo "=== $tag: P4 4 axes CSP sine, 250 us $* (${DUR} s)"
    sb_start "$tag" --n 1 --profile "1=$PROF4" --cia402 1:4 || { bad "$tag soft_bus did not start"; return; }
    "$ECM_RUN" --iface "$I226" --n 1 --motion-cycle-us 250 --duration-sec "$DUR" --diag-file "$LOG/diag_$tag.txt" \
        --pdo-scan --axis 1:0,1:1,1:2,1:3 --axis-modes csp --hook cia402 --cia402-lead "$P4_LEAD" \
        --cia402-script "1 enable all; 2 sine all $P4_AMP $P4_HZ" "$@" > "$LOG/er_$tag.log" 2>&1
    echo $? > "$LOG/er_$tag.log.rc"; sb_stop
    python3 "$HERE/is_parse.py" ecm_run "$LOG/er_$tag.log" is05p4 "$PROFILE" "$var" idle >> "$CSV"
    grep ",wake_jitter," "$CSV" | tail -1 | sed 's/^/    /'
    er_verdict "$tag" "$LOG/er_$tag.log" "$NOFRAME_MAX"
    er_axes_used "$tag" "$LOG/er_$tag.log"
}

etf_args () {   # delta -> ecm_run args for --link etf
    local l=${ETF_LEAD:-$(lead_of "$1")} a=${ETF_ASAP:-$(asap_of "${ETF_LEAD:-$(lead_of "$1")}")}
    echo "--link etf --etf-lead-us $l --etf-asap-us $a"
}

CHOSEN=""
for c in $CASES; do
case $c in
rig)
    echo "=== rig preconditions"
    if command -v nmcli >/dev/null; then
        for i in $I226 $ONB; do
            st=$(nmcli -t -f DEVICE,STATE device 2>/dev/null | awk -F: -v d=$i '$1==d{print $2}')
            chk "$i not managed by NetworkManager (${st:-unknown})" "[ '${st:-unmanaged}' = unmanaged ]"
        done
    fi
    srv=$(echo "${SSH_CONNECTION:-}" | awk '{print $3}')
    for i in $I226 $ONB; do
        chk "SSH not over $i" "! { [ -n '$srv' ] && ip -4 -o addr show dev $i 2>/dev/null | grep -q ' $srv/'; }"
    done
    ethtool -K "$I226" gro off gso off tso off 2>/dev/null
    ethtool -C "$I226" rx-usecs 0 2>/dev/null
    "$HERE/rt_profile.sh" --expect "$PROFILE" --iface "$I226" > "$LOG/rt_profile.txt" 2>&1
    chk "rt_profile.sh --expect $PROFILE --iface $I226 (see $LOG/rt_profile.txt)" "[ $? -le 1 ]"
    chk "phc2sys running" "pgrep -x phc2sys >/dev/null"
    chk "systemd-timesyncd stopped" "! systemctl is-active --quiet systemd-timesyncd"
    ethtool -s "$ONB" autoneg on advertise 0x008     # 100baseT/Full only; the i226 negotiates down
    for _ in $(seq 30); do [ "$(speed "$I226")" = 100 ] && [ "$(speed "$ONB")" = 100 ] && break; sleep 1; done
    chk "both ends 100 Mbit/s Full ($I226 $(speed $I226) $(duplex $I226), $ONB $(speed $ONB) $(duplex $ONB))" \
        "[ '$(speed $I226)$(duplex $I226)' = 100full ] && [ '$(speed $ONB)$(duplex $ONB)' = 100full ]"
    [ $FAIL = 0 ] || { echo "rig not ready: fix the FAIL lines above"; exit 2; }
    ;;
is04)
    for d in $DELTAS; do
        l=$(lead_of "$d")
        echo "=== IS-04 etf delta $d us, lead $l us, ${PROBE_S} s"
        qdisc_etf "$d" || { bad "IS-04 d$d qdisc setup / offload"; continue; }
        "$CLOCK_CHECK" -s 20 > "$LOG/clock_d$d.txt" 2>&1
        chk "IS-04 d$d T-01/T-02 clock base" "grep -E '^T-0' $LOG/clock_d$d.txt | grep -q . && ! grep -E '^T-0' $LOG/clock_d$d.txt | grep -q FAIL"
        chrt -f 70 taskset -c "$RT_CPU" "$PROBE" -i "$I226" -s "$PROBE_S" -l "$l" > "$LOG/probe_d$d.txt" 2>&1
        chk "IS-04 d$d offload still on" "offload_on"
        tc -s qdisc show dev "$I226" > "$LOG/qdisc_d$d.txt"
        python3 "$HERE/is_parse.py" probe "$LOG/probe_d$d.txt" is04 "$PROFILE" "d$d" idle >> "$CSV"
        r=$(grep ",launch_err," "$CSV" | tail -1); echo "    $r"
        m=$(echo "$r" | grep -o 'missed=[0-9]*' | cut -d= -f2); i=$(echo "$r" | grep -o 'invalid=[0-9]*' | cut -d= -f2)
        if [ "${m:-x}" = 0 ] && [ "${i:-x}" = 0 ]; then CHOSEN=$d; fi   # DELTAS run large -> small
    done
    echo "    smallest delta with 0 missed / 0 invalid: ${CHOSEN:-none}"
    echo "- IS-04: smallest delta with 0 missed / 0 invalid in ${PROBE_S} s: **${CHOSEN:-none}** µs" >> "$REP"
    ;;
is05)
    D=${ETF_DELTA:-${CHOSEN:-400}}
    echo "- IS-05: etf delta $D µs, $(etf_args "$D")" >> "$REP"
    if qdisc_etf "$D"; then
        for cyc in $CYCLES; do run_n8 "is05_etf_$cyc" "$cyc" "etf_d${D}_${cyc}us" $(etf_args "$D"); done
    else bad "IS-05 qdisc etf delta $D"; fi
    qdisc_del
    for cyc in $CYCLES; do run_n8 "is05_af_$cyc" "$cyc" "af_${cyc}us"; done
    ;;
is05n)
    qdisc_del
    run_n8 "is05n_af_$NCYCLE" "$NCYCLE" "af_${NCYCLE}us_nodma" --no-dma-latency
    ;;
is05p4)
    D=${ETF_DELTA:-${CHOSEN:-400}}
    if qdisc_etf "$D"; then run_p4 is05p4_etf "etf_d${D}_250us" $(etf_args "$D")
    else bad "IS-05 P4 qdisc etf delta $D"; fi
    qdisc_del
    run_p4 is05p4_af "af_250us"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

python3 "$HERE/is_report.py" "$CSV" >> "$REP"
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo; cat "$REP"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
