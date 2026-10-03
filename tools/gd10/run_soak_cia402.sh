#!/usr/bin/env bash
# ==========================================================================
# run_soak_cia402.sh — Phase 10.10: 8 h CSP soak of the CiA402 layer on the
# virtual 4-axis drive (plan claude/giai_doan_10_ke_hoach.md §12, DoD "soak
# 8 h CSP sine: 0 overrun, 0 setpoint underrun, 0 WKC error").
#
#   sudo -E tools/gd10/run_soak_cia402.sh                 8 h (28800 s)
#   DUR=600 sudo -E tools/gd10/run_soak_cia402.sh         10 min dry run
#   SKIP_ENV=1 SB_PRIO= SB_ARGS=--no-sm-wd DUR=60 ...     sandbox dry run (not RT:
#                                                         the SM watchdog would trip)
#
# Rig: soft_bus --n 1, profile config/profiles/cia402_4ax.prof, --cia402 1:4,
# DC 32 bit, SM watchdog ON, SCHED_FIFO 79 on core 2; ecm_run --n 1 on
# veth_m, RT thread on the isolated core 3, --hook cia402, 4 axes 1:0..1:3
# in CSP, each a sine of AMP inc at HZ (different phase per axis is not
# needed: the drive axes are independent), setpoints LEAD ticks ahead
# (--cia402-lead, 20 = the Q-03 value). The run ends by --duration-sec:
# S6 walks every axis down before the slave leaves OP.
# Run it inside tmux: an SSH drop must not end the soak.
#
# Verdict (exit 0 = PASS):
#   bus   0 overrun, WKC zero/partial/over 0, NOFRAME <= NOFRAME_MAX (2),
#         DC LOCKED 0 unlocks, policy LOST = RECOVER = 0
#   hook  p99.99 <= 20 us
#   axes  each of the 4: no axis error, underrun 0, late 0, step refused 0,
#         tracking |actual(k) - setpoint(k-1)| max <= TRACK_MAX (1 inc),
#         Operation enabled once only, Switch on disabled at the end (S6)
#   no [AXIS] diagnosis line during the run
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU DUR AMP HZ LEAD
#      NOFRAME_MAX TRACK_MAX SKIP_ENV LOG SB_ARGS (extra soft_bus options)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}
DUR=${DUR:-28800}
AMP=${AMP:-10000}; HZ=${HZ:-0.5}; LEAD=${LEAD:-20}
NOFRAME_MAX=${NOFRAME_MAX:-2}; TRACK_MAX=${TRACK_MAX:-1}
SKIP_ENV=${SKIP_ENV:-0}
SB_ARGS=${SB_ARGS:-}
PROF=$ROOT/config/profiles/cia402_4ax.prof
LOG=${LOG:-log_gd10_soak_cia402_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
REP=$LOG/report.md

for f in "$SOFT_BUS" "$ECM_RUN" "$PROF"; do [ -e "$f" ] || { echo "missing $f (make all caps)"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done
[ "$DUR" -ge 60 ] || { echo "DUR >= 60 s"; exit 2; }
if [ "$SKIP_ENV" != 1 ]; then
    "$ROOT/tools/jetson/check_env.sh" --soak --no-iface > "$LOG/check_env.txt" 2>&1
    rc=$?
    cat "$LOG/check_env.txt"
    [ $rc -ge 2 ] && { echo "check_env: FAIL -- fix the lines above before an 8 h soak"; exit 2; }
fi

if [ -n "$SB_PRIO" ]; then RIG="FIFO $SB_PRIO core $SB_CPU"; else RIG="SCHED_OTHER (not a valid soak rig)"; fi
{
    echo "# Soak CiA402 CSP, 4 axes — $(date '+%Y-%m-%d %H:%M:%S')"
    echo
    echo "- host: $(uname -n) $(uname -r)"
    echo "- commit: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- duration: ${DUR} s; soft_bus $RIG${SB_ARGS:+ $SB_ARGS}; sine ${AMP} inc at ${HZ} Hz on 4 axes; lead ${LEAD} ticks"
} > "$REP"

sb=("$SOFT_BUS")
[ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
"${sb[@]}" --iface "$IF_S" --n 1 --dc 32 --profile "1=$PROF" --cia402 1:4 $SB_ARGS > "$LOG/soft_bus.log" 2>&1 &
SBP=$!
sleep 0.5
kill -0 $SBP 2>/dev/null || { echo "soft_bus did not start:"; cat "$LOG/soft_bus.log"; exit 2; }

# the sine runs from t=2 s to the end; the axes are walked down by S6 at the end
SCRIPT="1 enable all; 2 sine all $AMP $HZ"
echo "soak: $DUR s, logs in $LOG (started $(date '+%H:%M:%S'))"
"$ECM_RUN" --iface "$IF_M" --n 1 --duration-sec "$DUR" --diag-file "$LOG/diag.txt" \
    --pdo-scan --axis 1:0,1:1,1:2,1:3 --axis-modes csp \
    --hook cia402 --cia402-lead "$LEAD" --cia402-script "$SCRIPT" > "$LOG/ecm_run.log" 2>&1
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
HP=$(grep '\[HOOK\]' "$E" | grep -o 'p99.99=[0-9]*' | cut -d= -f2)
HM=$(grep '\[HOOK\]' "$E" | grep -o 'max=[0-9]*' | cut -d= -f2)
AXL=$(grep -c 'ecm_run: \[AXIS\]' "$E")

VERDICT=PASS; WHY=""
fail () { VERDICT=FAIL; WHY="$WHY $1;"; }
[ "$RC" = 0 ]                       || fail "ecm_run rc=$RC"
[ "${OVR:-x}" = 0 ]                 || fail "overrun=${OVR:-?}"
[ "${DCS:-x}" = LOCKED ]            || fail "DC state ${DCS:-?}"
[ "${UNL:-x}" = 0 ]                 || fail "DC unlocks=${UNL:-?}"
[ "${LOST:-x}" = 0 ] && [ "${REC:-x}" = 0 ] || fail "policy LOST=${LOST:-?} RECOVER=${REC:-?}"
[ "${ZER:-0}" = 0 ] && [ "${PAR:-0}" = 0 ] && [ "${OVE:-0}" = 0 ] || fail "WKC zero/partial/over=$ZER/$PAR/$OVE"
[ -n "$NOF" ] && [ "$NOF" -le "$NOFRAME_MAX" ] || fail "noframe=${NOF:-?} > $NOFRAME_MAX"
[ -n "$HP" ] && [ "$HP" -le 20000 ] || fail "hook p99.99=${HP:-?} ns > 20000"
[ "$AXL" = 0 ]                      || fail "$AXL [AXIS] diagnosis line(s)"
grep -q 'shutdown (S6): every axis walked down' "$E" || fail "S6 shutdown line missing"

AXROWS=""
for a in 0 1 2 3; do
    L=$(grep "  \[CIA402\] axis $a " "$E" | head -1)
    st=$(echo "$L" | sed 's/.*): //; s/ sw=.*//')
    g () { echo "$L" | grep -o "$1=[-0-9a-zA-Z]*" | head -1 | cut -d= -f2; }
    err=$(g err); und=$(g underrun); late=$(g late); used=$(g used); tm=$(g track_max)
    tn=$(echo "$L" | grep -o '(n=[0-9]*' | cut -d= -f2)
    sr=$(grep "  \[CIA402\] S4 axis $a " "$E" | grep -o 'step_refused=[0-9]*' | cut -d= -f2)
    oe=$(grep -c "\[CIA402\] t=.* axis $a Switched on -> Operation enabled (" "$E")
    [ -n "$L" ] || { fail "axis $a: no report line"; continue; }
    [ "$err" = none ]          || fail "axis $a err=$err"
    [ "${und:-x}" = 0 ]        || fail "axis $a underrun=$und"
    [ "${late:-x}" = 0 ]       || fail "axis $a late=$late"
    [ "${sr:-0}" = 0 ]         || fail "axis $a step_refused=$sr"
    [ -n "$tm" ] && [ "$tm" -le "$TRACK_MAX" ] || fail "axis $a track_max=${tm:-?} > $TRACK_MAX"
    [ "${tn:-0}" -gt 1000 ]    || fail "axis $a tracking measured on ${tn:-0} records"
    [ "$oe" = 1 ]              || fail "axis $a Operation enabled $oe times"
    [ "$st" = "Switch on disabled" ] || fail "axis $a ends '$st'"
    AXROWS="$AXROWS| $a | $st | $err | $used | $und | $late | ${sr:-0} | $tm (n=$tn) | $oe |
"
done

{
    echo
    echo "## Result: **$VERDICT**${WHY:+ —$WHY}"
    echo
    echo "| Quantity | Value |"
    echo "|---|---|"
    echo "| motion cycles | ${CYC:-?} |"
    echo "| overrun | ${OVR:-?} |"
    echo "| WKC noframe / zero / partial / over | ${NOF:-?} / ${ZER:-?} / ${PAR:-?} / ${OVE:-?} |"
    echo "| DC | ${DCS:-?}, unlocks ${UNL:-?} |"
    echo "| policy LOST / RECOVER | ${LOST:-?} / ${REC:-?} |"
    echo "| hook cia402 p99.99 / max | ${HP:-?} / ${HM:-?} ns |"
    echo "| [AXIS] diagnosis lines | $AXL |"
    echo
    echo "| Axis | End state | Error | Setpoints used | Underrun | Late | Step refused | Tracking max (inc) | Enabled |"
    echo "|---|---|---|---|---|---|---|---|---|"
    printf '%s' "$AXROWS"
    echo
    grep -E '^\s*\[(GROUP_MOTION|DC|WKC GROUP_MOTION|POLICY|LATE|HOOK)\]|shutdown \(S6\)|\[CIA402\] commands' "$E" | sed 's/^/    /'
    echo
    grep -E '^(wake_jitter|prep_send|occupancy|turnaround) ' "$E" | tail -4 | sed 's/^/    /'
    if [ "$AXL" != 0 ]; then echo; echo "### [AXIS] lines"; grep 'ecm_run: \[AXIS\]' "$E" | head -20 | sed 's/^/    /'; fi
} >> "$REP"

cat "$REP"
[ "$VERDICT" = PASS ]
