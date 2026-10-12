#!/usr/bin/env bash
# ==========================================================================
# run_is_11_5.sh -- Phase 11.5 part 1: wake tail after the c7 fix, per boot
# profile (claude/giai_doan_11_ke_hoach.md §6, docs/rt_profiles.md).
# Run it once in each boot profile (P0, then P1 after the reboot); merge the
# two results.csv with tools/gd11/is_report.py.
#
#   sudo -E PROFILE=P0 tools/gd11/run_is_11_5.sh            all cases, 600 s each
#   sudo -E PROFILE=P1 CASES="is01 is02" DUR=60 tools/gd11/run_is_11_5.sh
#
#   is01  rt_profile.sh --expect $PROFILE passes; negative control: expecting
#         the other profile fails (exit 2)
#   is02  cyclictest on the RT core (-m -p 80 -i 1000 -h HIST), DUR s each:
#         {dma: /dev/cpu_dma_latency=0 held (cyclictest default),
#          nodma: --laptop} x {idle, load}
#   is03  ecm_run N=8 (4 motion + 4 IO) 1 ms against soft_bus on veth, DUR s
#         each: {dma (P0/P1), nodma = --no-dma-latency (P0n/P1n)} x {idle,
#         load}: wake_jitter / prep_send / occupancy / turnaround, overrun,
#         NOFRAME, DC unlocks. Phase 8.5 reference: wake p99.99 144 us,
#         max 277 us (before isolcpus was restored and before the c7 fix)
#   is06  power: tegrastats 1 s for DUR s, {rest: nothing running,
#         bus: the is03 rig at 1 ms}; mean VDD_IN, max CPU temperature
#
# Load (as the acontis article): stress-ng --cpu 3 --vm 1 --vm-bytes 512M
# --sock 1 pinned to cores 0-2; plus iperf3 to IPERF_PEER if set (e.g. the
# laptop on USB-C: IPERF_PEER=192.168.55.100, iperf3 -s running there).
#
# Nothing here changes the system: no qdisc, no boot entry, no sysctl.
# Output: $LOG/report.md, $LOG/results.csv, raw logs.
#
# Env: PROFILE (P0|P1, required) CASES DUR HIST RT_CPU SB_CPU SB_PRIO SB_ARGS
#      IF_M IF_S IPERF_PEER LOAD_CPUS (default 0-2) CYCLICTEST STRESS TEGRASTATS (paths, for tests)
#      SOFT_BUS ECM_RUN LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROFILE=${PROFILE:-}
CASES=${CASES:-"is01 is02 is03 is06"}
DUR=${DUR:-600}; HIST=${HIST:-1000}
RT_CPU=${RT_CPU:-3}; SB_CPU=${SB_CPU:-2}; SB_PRIO=${SB_PRIO-79}; SB_ARGS=${SB_ARGS:-}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
IPERF_PEER=${IPERF_PEER:-}; LOAD_CPUS=${LOAD_CPUS:-0-2}
CYCLICTEST=${CYCLICTEST:-cyclictest}; STRESS=${STRESS:-stress-ng}; TEGRASTATS=${TEGRASTATS:-tegrastats}
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
LOG=${LOG:-$ROOT/log_gd11_is_${PROFILE}_$(date +%Y%m%d_%H%M%S)}
case "$PROFILE" in P0|P1) ;; *) echo "PROFILE=P0 or PROFILE=P1 (the boot profile you expect, docs/rt_profiles.md)"; exit 3 ;; esac
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
CSV=$LOG/results.csv; REP=$LOG/report.md
echo "case,profile,variant,load,metric,n,min,avg,p50,p99,p999,p9999,max,unit,extra" > "$CSV"
PASS=0; FAIL=0
chk () { if eval "$2"; then echo "  [PASS] $1"; PASS=$((PASS+1)); else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi; }
have () { command -v "$1" >/dev/null 2>&1; }
OTHER=P1; [ "$PROFILE" = P1 ] && OTHER=P0

{
    echo "# Phase 11.5 IS-01..IS-06, profile $PROFILE — $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "- host $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- cmdline: $(cat /proc/cmdline)"
    echo "- DUR ${DUR} s per variant; RT core $RT_CPU; soft_bus core $SB_CPU ${SB_PRIO:+FIFO $SB_PRIO}"
    echo
} > "$REP"

# ---- load generator (cores 0-2) ----
LOADPIDS=()
load_start () {
    have "$STRESS" || { echo "  stress-ng missing: sudo apt install stress-ng"; return 1; }
    taskset -c "$LOAD_CPUS" "$STRESS" --cpu 3 --vm 1 --vm-bytes 512M --sock 1 --timeout $((DUR + 60))s \
        > "$LOG/stress_$1.log" 2>&1 &
    LOADPIDS+=($!)
    if [ -n "$IPERF_PEER" ] && have iperf3; then
        taskset -c "$LOAD_CPUS" iperf3 -c "$IPERF_PEER" -t $((DUR + 30)) > "$LOG/iperf_$1.log" 2>&1 &
        LOADPIDS+=($!)
    fi
    sleep 3
}
load_stop () {
    for p in "${LOADPIDS[@]}"; do kill -TERM "$p" 2>/dev/null; done
    for p in "${LOADPIDS[@]}"; do wait "$p" 2>/dev/null; done
    pkill -x stress-ng 2>/dev/null; LOADPIDS=()
}
trap 'load_stop; pkill -INT -x soft_bus 2>/dev/null' EXIT

# ---- the is03 rig: soft_bus N=8 on IF_S, ecm_run on IF_M ----
SBP=""
sb_start () {
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    "${sb[@]}" --iface "$IF_S" --n 8 --dc 32 $SB_ARGS > "$LOG/sb_$1.log" 2>&1 &
    SBP=$!; sleep 0.5
    kill -0 $SBP 2>/dev/null
}
sb_stop () { [ -n "$SBP" ] && { kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null; }; SBP=""; }

# cyclictest histogram -> csv row
cyc_row () {   # file variant load
    python3 "$HERE/is_parse.py" cyclictest "$1" is02 "$PROFILE" "$2" "$3" "$HIST" >> "$CSV"
}

# ecm_run end-of-run timing lines -> csv rows (is_parse.py, Python 3.8+)
er_rows () {   # file variant load
    python3 "$HERE/is_parse.py" ecm_run "$1" is03 "$PROFILE" "$2" "$3" >> "$CSV"
}

for c in $CASES; do
case $c in
is01)
    echo "=== IS-01 boot profile $PROFILE"
    "$HERE/rt_profile.sh" --expect "$PROFILE" --rt-cpu "$RT_CPU" > "$LOG/is01.txt" 2>&1; r=$?
    sed 's/^/    /' "$LOG/is01.txt"
    chk "IS-01 rt_profile.sh --expect $PROFILE: exit 0/1 (got $r)" "[ $r -le 1 ]"
    "$HERE/rt_profile.sh" --expect "$OTHER" --rt-cpu "$RT_CPU" > "$LOG/is01_neg.txt" 2>&1; r=$?
    chk "IS-01 negative control --expect $OTHER: exit 2 on the profile check (got $r)" "[ $r = 2 ] && grep -q 'profile is $PROFILE, expected $OTHER' $LOG/is01_neg.txt"
    { echo "## IS-01"; echo; echo '```'; cat "$LOG/is01.txt"; echo '```'; echo; } >> "$REP"
    ;;
is02)
    have "$CYCLICTEST" || { echo "cyclictest missing: sudo apt install rt-tests"; FAIL=$((FAIL+1)); continue; }
    for load in idle load; do
        [ $load = load ] && { load_start is02 || { FAIL=$((FAIL+1)); continue; }; }
        for var in dma nodma; do
            A=(); [ $var = nodma ] && A=(--laptop)
            echo "=== IS-02 cyclictest core $RT_CPU, $var, $load, ${DUR} s"
            "$CYCLICTEST" -m -p 80 -a "$RT_CPU" -t1 -i 1000 -h "$HIST" -q -D "${DUR}" "${A[@]}" \
                > "$LOG/is02_${var}_$load.txt" 2>&1
            cyc_row "$LOG/is02_${var}_$load.txt" $var $load
            tail -1 "$CSV" | sed 's/^/    /'
        done
        [ $load = load ] && load_stop
    done
    ;;
is03)
    for i in "$IF_M" "$IF_S"; do ip link show "$i" >/dev/null 2>&1 || {
        echo "missing $i: sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        FAIL=$((FAIL+1)); continue 2; }; done
    for load in idle load; do
        [ $load = load ] && { load_start is03 || { FAIL=$((FAIL+1)); continue; }; }
        for var in dma nodma; do
            A=(); [ $var = nodma ] && A=(--no-dma-latency)
            echo "=== IS-03 ecm_run N=8 1 ms, $var, $load, ${DUR} s"
            sb_start "is03_${var}_$load" || { echo "  soft_bus did not start"; FAIL=$((FAIL+1)); continue; }
            "$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 --duration-sec "$DUR" --diag-file "$LOG/diag_is03.txt" "${A[@]}" \
                > "$LOG/is03_${var}_$load.log" 2>&1
            rc=$?; sb_stop
            chk "IS-03 $var $load: ecm_run rc 0" "[ $rc = 0 ]"
            er_rows "$LOG/is03_${var}_$load.log" $var $load
            grep "^is03,.*wake_jitter" "$CSV" | tail -1 | sed 's/^/    /'
        done
        [ $load = load ] && load_stop
    done
    ;;
is06)
    have "$TEGRASTATS" || { echo "tegrastats missing (not a Jetson?)"; FAIL=$((FAIL+1)); continue; }
    # the bus run needs the veth pair too (a reboot removes it; 12/10 P1 run)
    for i in "$IF_M" "$IF_S"; do ip link show "$i" >/dev/null 2>&1 || {
        echo "missing $i: sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        FAIL=$((FAIL+1)); continue 2; }; done
    for var in rest bus; do
        echo "=== IS-06 power, $var, ${DUR} s"
        if [ $var = bus ]; then
            sb_start is06 || { echo "  soft_bus did not start ($LOG/sb_is06.log)"; FAIL=$((FAIL+1)); continue; }
            "$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 --duration-sec "$DUR" --diag-file "$LOG/diag_is06.txt" \
                > "$LOG/is06_bus.log" 2>&1 &
            ERP=$!
        fi
        "$TEGRASTATS" --interval 1000 > "$LOG/is06_$var.txt" 2>&1 &
        TP=$!
        sleep "$DUR"
        kill -TERM $TP 2>/dev/null; wait $TP 2>/dev/null   # TERM: a background job ignores SIGINT
        [ $var = bus ] && { wait $ERP; sb_stop; }
        python3 "$HERE/is_parse.py" tegrastats "$LOG/is06_$var.txt" is06 "$PROFILE" "$var" idle >> "$CSV"
        tail -1 "$CSV" | sed 's/^/    /'
    done
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

python3 "$HERE/is_report.py" "$CSV" >> "$REP"
chown -R "${SUDO_USER:-$(id -un)}:" "$LOG" 2>/dev/null
echo
cat "$REP"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
