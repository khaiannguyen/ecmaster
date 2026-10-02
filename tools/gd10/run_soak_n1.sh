#!/usr/bin/env bash
# ==========================================================================
# run_soak_n1.sh — GD10.1: long soak of a one-slave bus (open item of GD9.1:
# the 30 min soak saw 1 isolated NOFRAME in 1.8 million cycles; the 8 h
# soak says whether that is a rate or a one-off, and WHEN each one hit).
# Plan: claude/giai_doan_10_ke_hoach.md §3 item 2.
#
#   sudo -E tools/gd10/run_soak_n1.sh                    8 h (28800 s)
#   DUR=600 sudo -E tools/gd10/run_soak_n1.sh            10 min dry run
#   SKIP_ENV=1 ... (sandbox, no Jetson checks)
#
# Rig = the 9.1 30 min soak: soft_bus --n 1 --pdo-size 4 --dc 32 at
# SCHED_FIFO 79 on core 2 (SM watchdog ON), ecm_run --n 1 on veth_m (TX
# timestamps on), RT thread on the isolated core 3 as always.
# Run it inside tmux: an SSH drop must not end the soak.
#
# Verdict (exit 0 = PASS):
#   PASS  0 overrun, DC LOCKED with 0 unlocks, policy LOST=0 RECOVER=0,
#         NOFRAME <= NOFRAME_MAX (default 2: the GD9.1 rate, 1 per 1.8 M,
#         is ~16 in 8 h; a handful of isolated ones is "rate", a burst or
#         a LOST is a finding)
#   The report always lists every second in which a mismatch happened.
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU DUR NOFRAME_MAX SKIP_ENV LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}   # SB_PRIO= (empty): plain SCHED_OTHER, sandbox
DUR=${DUR:-28800}
NOFRAME_MAX=${NOFRAME_MAX:-2}
SKIP_ENV=${SKIP_ENV:-0}
LOG=${LOG:-log_gd10_soak_n1_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
REP=$LOG/report.md

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f (make all caps)"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done
if [ "$SKIP_ENV" != 1 ]; then
    "$ROOT/tools/jetson/check_env.sh" --soak --no-iface > "$LOG/check_env.txt" 2>&1
    rc=$?
    cat "$LOG/check_env.txt"
    [ $rc -ge 2 ] && { echo "check_env: FAIL -- fix the lines above before an 8 h soak"; exit 2; }
fi

{
    echo "# Soak N=1 — $(date '+%Y-%m-%d %H:%M:%S')"
    echo
    echo "- host: $(uname -n) $(uname -r)"
    echo "- commit: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- duration: ${DUR} s; soft_bus ${SB_PRIO:+FIFO $SB_PRIO core $SB_CPU}${SB_PRIO:-SCHED_OTHER (not a valid soak rig)}"
} > "$REP"

sb=("$SOFT_BUS")
[ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
"${sb[@]}" --iface "$IF_S" --n 1 --pdo-size 4 --dc 32 > "$LOG/soft_bus.log" 2>&1 &
SBP=$!
sleep 0.5
kill -0 $SBP 2>/dev/null || { echo "soft_bus did not start:"; cat "$LOG/soft_bus.log"; exit 2; }

echo "soak: $DUR s, logs in $LOG (started $(date '+%H:%M:%S'))"
"$ECM_RUN" --iface "$IF_M" --n 1 --duration-sec "$DUR" --diag-file "$LOG/diag.txt" > "$LOG/ecm_run.log" 2>&1
RC=$?
sleep 0.3
kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null
E=$LOG/ecm_run.log

num () { grep -o "$1" "$E" | head -1 | grep -o '[0-9]\+$'; }
CYC=$(grep -o '\[GROUP_MOTION\] cycles=[0-9]*' "$E" | grep -o '[0-9]*$')
OVR=$(grep '\[GROUP_MOTION\] cycles=' "$E" | grep -o 'overrun=[0-9]*' | grep -o '[0-9]*$')
NOF=$(num 'noframe=[0-9]*')
ZER=$(num ' zero=[0-9]*'); PAR=$(num 'partial=[0-9]*'); OVE=$(num ' over=[0-9]*')
UNL=$(grep '\[DC\] state=' "$E" | grep -o 'unlocks=[0-9]*' | grep -o '[0-9]*$')
DCS=$(grep -o '\[DC\] state=[A-Z_]*' "$E" | cut -d= -f2)
LOST=$(grep '\[POLICY\] final' "$E" | grep -o 'LOST=[0-9]*' | head -1 | grep -o '[0-9]*$')
REC=$(grep '\[POLICY\] final' "$E" | grep -o 'RECOVER=[0-9]*' | head -1 | grep -o '[0-9]*$')

VERDICT=PASS; WHY=""
[ "$RC" = 0 ]                      || { VERDICT=FAIL; WHY="$WHY ecm_run rc=$RC;"; }
[ "${OVR:-x}" = 0 ]                || { VERDICT=FAIL; WHY="$WHY overrun=${OVR:-?};"; }
[ "${DCS:-x}" = LOCKED ]           || { VERDICT=FAIL; WHY="$WHY DC state ${DCS:-?};"; }
[ "${UNL:-x}" = 0 ]                || { VERDICT=FAIL; WHY="$WHY DC unlocks=${UNL:-?};"; }
[ "${LOST:-x}" = 0 ] && [ "${REC:-x}" = 0 ] || { VERDICT=FAIL; WHY="$WHY policy LOST=${LOST:-?} RECOVER=${REC:-?};"; }
[ "${ZER:-0}" = 0 ] && [ "${PAR:-0}" = 0 ] && [ "${OVE:-0}" = 0 ] || { VERDICT=FAIL; WHY="$WHY WKC zero/partial/over=$ZER/$PAR/$OVE;"; }
[ -n "$NOF" ] && [ "$NOF" -le "$NOFRAME_MAX" ] || { VERDICT=FAIL; WHY="$WHY noframe=${NOF:-?} > $NOFRAME_MAX;"; }

{
    echo
    echo "## Result: **$VERDICT**${WHY:+ —$WHY}"
    echo
    echo "| Quantity | Value |"
    echo "|---|---|"
    echo "| motion cycles | ${CYC:-?} |"
    echo "| overrun | ${OVR:-?} |"
    echo "| WKC noframe / zero / partial / over | ${NOF:-?} / ${ZER:-?} / ${PAR:-?} / ${OVE:-?} |"
    if [ -n "${CYC:-}" ] && [ -n "${NOF:-}" ] && [ "$NOF" -gt 0 ]; then
        echo "| noframe rate | 1 per $((CYC / NOF)) cycles |"
    fi
    echo "| DC | ${DCS:-?}, unlocks ${UNL:-?} |"
    echo "| policy LOST / RECOVER | ${LOST:-?} / ${REC:-?} |"
    echo
    echo "### Seconds with a WKC mismatch (from the per-second snapshots)"
    echo
    awk '/^-- tick=/ {
            for (i = 1; i <= NF; i++) if ($i ~ /^mismatch=/) { split($i, a, "="); m = a[2]; break }
            t = $2; sub("tick=", "", t)
            if (m + 0 > prev + 0) printf("- t = %d s: +%d (total %d)\n", t / 1000, m - prev, m)
            prev = m
         }' "$E"
    echo
    grep -E '^\s*\[(GROUP_MOTION|DC|WKC GROUP_MOTION|POLICY|LATE|DIAG)\]' "$E" | sed 's/^/    /'
    echo
    grep -E '^(wake_jitter|prep_send|occupancy|turnaround) ' "$E" | tail -4 | sed 's/^/    /'
} >> "$REP"

cat "$REP"
[ "$VERDICT" = PASS ]
