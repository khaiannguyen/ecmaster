#!/usr/bin/env bash
# ==========================================================================
# run_bm_11_7.sh -- Phase 11.7 BM-02: benchmark on the two REAL IS620N in
# motion, laid out against the acontis Jetson Orin figures
# (claude/giai_doan_11_ke_hoach.md §8). 1 ms, {af_packet, etf} x {idle,
# load}, DUR s each (default 3600, as acontis), API on (the numbers come from
# the final stats.json), no web UI.
#
# THIS ENABLES THE DRIVES for 4 x DUR s: the motion and every safety rule are
# those of Phase 10.9 W-08 (tools/gd10/run_w_10_9.sh: drive limits written and
# read back by w00 before each run, torque 15 %, ~60 rpm, motors clamped,
# both axes CSP (1 - cos) 1/2 rev at 0.2 Hz, S6 walk-down at the end).
#
#   sudo -E PROFILE=P0 tools/bench/run_bm_11_7.sh                     ~4 h 10 min, in tmux
#   sudo -E PROFILE=P0 DUR=120 tools/bench/run_bm_11_7.sh             short pass (~12 min)
#   sudo -E PROFILE=P0 CASES="etf_idle etf_load" tools/bench/run_bm_11_7.sh
#   sudo -E PROFILE=P0 SIM=1 DUR=30 CASES="af_idle af_load" tools/bench/run_bm_11_7.sh   rehearsal on veth
#
#   af_idle   --link af_packet, nothing else running
#   af_load   + stress-ng --cpu 3 --vm 1 --vm-bytes 512M on CPUs 0-2 (as IS-02)
#   etf_idle  --link etf --etf-lead-us 50 --etf-asap-us 25 (IS-04), qdisc
#             mqprio + etf delta 100 us offload installed here, removed at the end
#   etf_load  the same + the load
#
# Each case: run_w_10_9.sh w00 + w08 (its own checks: 0 overrun, NOFRAME <= 2,
# DC LOCKED, no axis error, 0x603F 0, S6), then the final stats.json is kept.
# At the end tools/bench/ecm_bench.py makes LOG/benchmark.md from the four.
# A case that does not reach w08 (w00 failed, no link...) stops the whole run.
#
# Needs for etf: phc2sys running, systemd-timesyncd stopped (checked).
# Env: PROFILE DUR CASES IFACE API_DIR ETF_DELTA LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROFILE=${PROFILE:-}
DUR=${DUR:-3600}
CASES=${CASES:-"af_idle af_load etf_idle etf_load"}
SIM=${SIM:-0}; export SIM          # SIM=1: rehearsal on veth with virtual drives (af cases only, no rig)
if [ "$SIM" = 1 ]; then IFACE=${IFACE:-veth_m}; else IFACE=${IFACE:-enP1p1s0}; fi
API_DIR=${API_DIR:-/run/ecmaster_bm}
ETF_DELTA=${ETF_DELTA:-100}
ETF_ARGS="--link etf --etf-lead-us 50 --etf-asap-us 25"
case "$PROFILE" in P0|P1) ;; *) echo "PROFILE=P0 or PROFILE=P1 (as booted)"; exit 3 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
for p in ecm_run soft_bus; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 3; }; done
command -v stress-ng >/dev/null || { echo "stress-ng missing: sudo apt install stress-ng"; exit 3; }
LOG=${LOG:-$ROOT/log_gd11_bm_${PROFILE}_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
want_etf=0; case " $CASES " in *" etf_"*) want_etf=1;; esac

n=$(echo $CASES | wc -w)
cat <<EOT
=== Phase 11.7 BM-02 on $IFACE: THE DRIVES WILL BE ENABLED, $n run(s) x ${DUR} s ($CASES) ===
  [ ] Motors clamped, nothing on the shafts, nobody near them
  [ ] The breaker is the emergency stop; drive limits: torque 15 %, ~60 rpm
  [ ] Cable: $IFACE -> drive 1 IN, drive 1 OUT -> drive 2 IN
EOT
if [ "$SIM" = 1 ]; then ans=yes; else read -r -p "Type yes to start: " ans; fi
if [ "$(printf '%s' "$ans" | tr -cd '[:alpha:]' | tr '[:upper:]' '[:lower:]')" != yes ]; then
    echo "stopped: the answer was not yes (bytes: $(printf '%s' "$ans" | od -An -c | tr -s ' '))"; exit 3
fi

restore () { [ "$want_etf" = 1 ] && tc qdisc del dev "$IFACE" root 2>/dev/null; pkill -x stress-ng 2>/dev/null; return 0; }
trap restore EXIT

if [ "$SIM" = 1 ]; then
    [ "$want_etf" = 1 ] && { echo "SIM=1: af cases only"; exit 3; }
    echo "=== SIM=1: rehearsal on veth, no rig"
else
echo "=== rig"
PROFILE=$PROFILE CASES=rig LOG=$LOG/rig "$ROOT/tools/gd11/run_is7_11_5.sh" > "$LOG/rig.txt" 2>&1
R=$?; grep -E 'PASS|FAIL|INFO' "$LOG/rig.txt"
[ $R = 0 ] || { echo "rig not ready: fix the FAIL lines above ($LOG/rig.txt)"; exit 2; }
fi
if [ "$want_etf" = 1 ]; then
    pgrep -x phc2sys >/dev/null || { echo "etf cases: phc2sys is not running (sudo phc2sys -s CLOCK_REALTIME -c $IFACE -O 0 -m --step_threshold=1, in another tmux window)"; exit 2; }
    systemctl is-active --quiet systemd-timesyncd && { echo "etf cases: stop systemd-timesyncd first (sudo systemctl stop systemd-timesyncd)"; exit 2; }
fi

etf_up () {
    tc qdisc show dev "$IFACE" | grep -q "etf.*offload on" && return 0
    tc qdisc del dev "$IFACE" root 2>/dev/null
    tc qdisc replace dev "$IFACE" parent root handle 100 mqprio num_tc 3 \
        map 2 2 1 0 2 2 2 2 2 2 2 2 2 2 2 2 queues 1@0 1@1 2@2 hw 0 &&
    tc qdisc add dev "$IFACE" parent 100:1 etf clockid CLOCK_TAI delta $((ETF_DELTA * 1000)) offload || return 1
    echo "    qdisc etf delta $ETF_DELTA us installed, waiting 90 s (igc reset, phc2sys relock, link back)"
    sleep 90
    for _ in $(seq 30); do [ "$(cat /sys/class/net/$IFACE/speed 2>/dev/null)" = 100 ] && break; sleep 1; done
    ethtool -K "$IFACE" gro off gso off tso off 2>/dev/null; ethtool -C "$IFACE" rx-usecs 0 2>/dev/null
    tc qdisc show dev "$IFACE" | grep -q "etf.*offload on"
}

LABELS=(); FAILS=0
for c in $CASES; do
    echo; echo "=== $c (${DUR} s)"
    X="--api --api-dir $API_DIR --api-cpus 1-2"
    case $c in etf_*) etf_up || { echo "$c: qdisc setup failed -- stopping"; exit 2; }; X="$X $ETF_ARGS";; esac
    case $c in
        af_idle|af_load|etf_idle|etf_load) ;;
        *) echo "unknown case $c"; exit 2 ;;
    esac
    DUR_LOAD=""
    case $c in *_load) DUR_LOAD="taskset -c 0-2 stress-ng --cpu 3 --vm 1 --vm-bytes 512M --timeout $((DUR + 30))s";; esac
    rm -rf "$API_DIR"
    c0=$(cat /sys/class/net/$IFACE/statistics/rx_crc_errors)
    YES=1 EXTRA_ARGS="$X" DURING="$DUR_LOAD" CASES=w08 DUR8=$DUR LOG=$LOG/$c "$ROOT/tools/gd10/run_w_10_9.sh"
    r=$?
    pkill -x stress-ng 2>/dev/null
    c1=$(cat /sys/class/net/$IFACE/statistics/rx_crc_errors)
    [ -s "$LOG/$c/er_w08.log" ] || { echo "$c did not reach w08 (rc $r): stopping, no further run is started"; exit 2; }
    [ $r = 0 ] || FAILS=$((FAILS + 1))
    if [ -s "$API_DIR/stats.json" ]; then
        cp "$API_DIR/stats.json" "$LOG/$c/stats.json"
        LABELS+=(--label "$c=$LOG/$c/stats.json")
    else
        echo "$c: no stats.json in $API_DIR"; FAILS=$((FAILS + 1))
    fi
    echo "$c: W-08 rc $r, NIC rx_crc_errors +$((c1 - c0))" | tee -a "$LOG/summary.txt"
done
restore

{
    echo "# Phase 11.7 BM-02 — 2 IS620N, CSP 2 axes, 1 ms — $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "- host $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?'); profile $PROFILE; ${DUR} s per run"
    echo "- load = stress-ng --cpu 3 --vm 1 --vm-bytes 512M on CPUs 0-2; etf = delta $ETF_DELTA / lead 50 / asap 25"
    echo
    sed 's/^/- /' "$LOG/summary.txt"
    echo
    python3 "$HERE/ecm_bench.py" "${LABELS[@]}"
} > "$LOG/benchmark.md" 2>&1
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo; cat "$LOG/benchmark.md"
echo "RESULT: $((n - FAILS)) of $n run(s) passed W-08   (log: $LOG, table: $LOG/benchmark.md)"
[ $FAILS = 0 ]
