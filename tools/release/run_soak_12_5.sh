#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_soak_12_5.sh -- Phase 12.5: the 8 h soak of master-v1.0 at 1 ms on the
# two REAL IS620N, both axes MOVING (decision of 17/10/2026).
#
# The motion and every limit are those of Phase 10.9 W-08 (run_w_10_9.sh):
# drive limits written by the ENI and read back by SDO before anything is
# enabled (w00: torque 15 %, ~60 rpm, following error window 1/8 rev), both
# axes CSP (1 - cos) 1/2 rev at 0.2 Hz, S6 walk-down at the end. Added here:
#   - a checklist answered item by item (letters only compared, as 0046)
#   - tools/release/soak_watch.py beside the run, outside the RT path: reads
#     stats.json (--api) and the machine every 10 s, STOPS THE RUN AT THE
#     FIRST FAULT (axis error, an axis leaving Operation enabled, DC not
#     LOCKED / unlock, overrun, wrong WKC, a NOFRAME burst) with SIGTERM =
#     ecm_run's normal end (S6); records CPU 3 frequency (throttling),
#     temperatures and ecm_run VmRSS
#   - NOFRAME_MAX = 2 per started hour, the W-08 rate
#   - check_env --soak on the i226 before, the NIC CRC counters before/after
#
#   sudo -E tools/release/run_soak_12_5.sh                 8 h, in tmux
#   sudo -E DUR=600 tools/release/run_soak_12_5.sh         10 min trial first
#   sudo -E SIM=1 DUR=120 tools/release/run_soak_12_5.sh   rehearsal on veth, no drive
#
# Report: LOG/summary.md (criteria, W-08 report, watch verdict, NIC counters).
# Env: DUR (default 28800) IFACE SIM LOG API_DIR SKIP_ENV
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DUR=${DUR:-28800}
SIM=${SIM:-0}
if [ "$SIM" = 1 ]; then IFACE=${IFACE:-veth_m}; else IFACE=${IFACE:-enP1p1s0}; fi
API_DIR=${API_DIR:-/run/ecmaster/soak}
SKIP_ENV=${SKIP_ENV:-0}
HOURS=$(( (DUR + 3599) / 3600 ))
NOFRAME_MAX=${NOFRAME_MAX:-$((2 * HOURS))}
LOG=${LOG:-$ROOT/log_gd12_soak_$(date +%Y%m%d_%H%M%S)}
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
[ "$DUR" -ge 60 ] 2>/dev/null || { echo "DUR >= 60 s"; exit 3; }
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd); chown "${SUDO_USER:-root}:" "$LOG"
for p in ecm_run soft_bus; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 3; }; done
[ -x "$ROOT/apps/ecm_run/ecm_run" ] || { echo "make all first"; exit 3; }
[ -n "${TMUX:-}${STY:-}" ] || [ "$DUR" -le 600 ] || echo "WARNING: not inside tmux/screen -- an SSH drop would end the soak"

yes_or_stop () {   # PROMPT
    local ans
    if [ "$SIM" = 1 ]; then echo "  $1  (SIM=1: no drive)"; return 0; fi
    read -r -p "  $1  -- type yes: " ans
    if [ "$(printf '%s' "$ans" | tr -cd '[:alpha:]' | tr '[:upper:]' '[:lower:]')" != yes ]; then
        echo "stopped: the answer was not yes (bytes: $(printf '%s' "$ans" | od -An -c | tr -s ' '))"; exit 3
    fi
}
cat <<EOF
=== Phase 12.5 soak: $IFACE, ${DUR} s ($HOURS h), BOTH IS620N AXES MOVING ===
Limits (written and read back before enabling): torque 15 %, ~60 rpm, following error 1/8 rev.
Motion: CSP (1 - cos) 1/2 rev, 0.2 Hz, both axes. The run stops itself at the first fault.
EOF
yes_or_stop "Motors clamped, nothing on the shafts, nobody will be near them during the run"
yes_or_stop "The breaker is the emergency stop and someone in the house can reach it"
yes_or_stop "Someone is at home and knows the drives are running for the whole ${HOURS} h (asleep is fine, away is not)"
yes_or_stop "Nothing flammable next to the drives, drives ventilated"
yes_or_stop "Cable: $IFACE -> drive 1 IN, drive 1 OUT -> drive 2 IN; this is the commit to be tagged ($(git -C "$ROOT" describe --always --dirty))"

{
    echo "# Phase 12.5 soak — $(date '+%Y-%m-%d %H:%M'), $(uname -r), $(git -C "$ROOT" describe --always --dirty)"
    echo
    echo "- ${DUR} s, motion W-08 on $IFACE$([ "$SIM" = 1 ] && echo ' (SIM=1: virtual drives on veth, not a soak result)')"
    echo "- criteria: W-08 checks with NOFRAME <= $NOFRAME_MAX (2 per hour), soak_watch.py no fault, no throttling, no temperature above 95 degC, VmRSS growth <= 1 MiB"
} > "$LOG/summary.md"

if [ "$SIM" != 1 ] && [ "$SKIP_ENV" != 1 ]; then
    "$ROOT/tools/jetson/check_env.sh" --soak --iface "$IFACE" > "$LOG/check_env.txt" 2>&1; r=$?
    grep -E 'FAIL|WARN|RESULT' "$LOG/check_env.txt"
    [ $r = 2 ] && { echo "check_env: FAIL lines above -- fix them first (or SKIP_ENV=1 for a trial)"; exit 2; }
    ethtool -S "$IFACE" > "$LOG/ethtool_S_before.txt" 2>/dev/null
fi
crc0=$(cat /sys/class/net/$IFACE/statistics/rx_crc_errors 2>/dev/null || echo 0)

rm -rf "$API_DIR"; mkdir -p "$(dirname "$API_DIR")"
python3 "$HERE/soak_watch.py" --stats "$API_DIR/stats.json" --log "$LOG/watch" --period 10 --warmup 60 --duration "$DUR" \
    > "$LOG/watch.txt" 2>&1 &
WP=$!
trap 'kill $WP 2>/dev/null' EXIT

YES=1 SIM=$SIM IFACE=$IFACE CASES=w08 DUR8=$DUR NOFRAME_MAX=$NOFRAME_MAX \
    EXTRA_ARGS="--api --api-dir $API_DIR --api-cpus 1-2" LOG="$LOG/w" \
    "$ROOT/tools/gd10/run_w_10_9.sh" 2>&1 | tee "$LOG/w08.txt"
R=${PIPESTATUS[0]}
for _ in $(seq 90); do kill -0 $WP 2>/dev/null || break; sleep 1; done
kill $WP 2>/dev/null; wait $WP 2>/dev/null
cp "$API_DIR/stats.json" "$LOG/stats_final.json" 2>/dev/null
crc1=$(cat /sys/class/net/$IFACE/statistics/rx_crc_errors 2>/dev/null || echo 0)
[ "$SIM" != 1 ] && ethtool -S "$IFACE" > "$LOG/ethtool_S_after.txt" 2>/dev/null

wv=$(grep -h '^VERDICT' "$LOG/watch/watch_verdict.txt" 2>/dev/null | awk '{print $2}')
{
    echo
    echo "## Result"
    echo
    echo "| Check | Result |"
    echo "|---|---|"
    echo "| W-08 checks (w00 limits, bus, DC, axes, 0x603F, S6) | $([ $R = 0 ] && echo PASS || echo "**FAIL** (rc $R)") |"
    echo "| soak_watch (first-fault stop, throttling, temperature, VmRSS) | $([ "$wv" = PASS ] && echo PASS || echo "**FAIL**") |"
    echo "| NIC rx_crc_errors during the run (recorded, known +1..+5/h on drive 1 -> Jetson) | +$((crc1 - crc0)) |"
    echo
    echo "## soak_watch"; echo; sed 's/^/    /' "$LOG/watch/watch_verdict.txt" 2>/dev/null
    echo
    echo "## W-08 report"; echo; grep -E 'PASS|FAIL|INFO|^\s+\[' "$LOG/w08.txt" | sed 's/^/    /'
    echo
    echo "Machine samples every 10 s: watch/watch.csv. Final stats.json: stats_final.json."
} >> "$LOG/summary.md"
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo; cat "$LOG/summary.md"
[ $R = 0 ] && [ "$wv" = PASS ]
