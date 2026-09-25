#!/usr/bin/env bash
# ==========================================================================
# run_l5_io.sh — Giai doan 7.4: L5-07 (late replies), L5-08 (duplicate /
# reordered replies), L5-09 (stale inputs), L5-12 (mailbox repeat /
# duplicate), each with a negative control.
#
#   sudo -E ./run_l5_io.sh                    all cases
#   sudo -E CASES="l507 l507neg" ./run_l5_io.sh
#
# soft_bus runs with --app-seq 0: every node increments a 16-bit counter at
# byte 0 of its TxPDO (the slave's PDO contract), and ecm_run gets
# --fresh-offset 0 to check it. That counter is also the oracle for L5-07:
# an old reply taken for a new frame makes it go backwards.
#
# Env: SOFT_BUS ECM_RUN L4_TEST SBCTL IF_M IF_S N M SB_PRIO SB_CPU
#      SB_ARGS  (e.g. --no-sm-wd on a non-RT host)   ECM_ARGS (e.g. --no-tx-ts)
#      OVR_MAX  overruns tolerated in the late case (default 20)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SOFT_BUS=${SOFT_BUS:-$HERE/../../tools/soft_bus/soft_bus}
SBCTL=${SBCTL:-$HERE/../../tools/soft_bus/sbctl.sh}
ECM_RUN=${ECM_RUN:-$HERE/ecm_run}
L4_TEST=${L4_TEST:-$HERE/../l4_test/l4_test}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
N=${N:-8}; M=${M:-4}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
SB_ARGS=${SB_ARGS:-}; ECM_ARGS=${ECM_ARGS:-}
OVR_MAX=${OVR_MAX:-20}
CASES=${CASES:-"l507 l507neg l508 l509 l509neg l512"}
CTL=/tmp/soft_bus_l5io.ctl
SNAP=/tmp/ecm_diag_l5io.txt

LOG=log_l5_io_$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN" "$SBCTL"; do
    [ -x "$f" ] || { echo "missing $f (build soft_bus and ecm_run first)"; exit 2; }
done

sb_start () {   # sb_start TAG "extra args"
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    "${sb[@]}" --iface "$IF_S" --n "$N" --pdo-size 4 --dc 32 --dc-report-s 100 --ctl "$CTL" \
        --app-seq 0 $SB_ARGS $2 > "$LOG/sb_$1.log" 2>&1 &
    SBP=$!
    sleep 0.5
    kill -0 $SBP 2>/dev/null || { echo "  soft_bus did not start ($LOG/sb_$1.log)"; return 1; }
}
sb_stop () {
    SB_CTL=$CTL "$SBCTL" status >/dev/null 2>&1; sleep 0.3
    kill -INT $SBP 2>/dev/null
    for _ in $(seq 50); do kill -0 $SBP 2>/dev/null || break; sleep 0.1; done
}

# run_case TAG DURATION "ECM_RUN ARGS" "t:cmd" ...
run_case () {
    local tag=$1 dur=$2 eargs=$3; shift 3
    sb_start "$tag" "" || return 1
    rm -f "$SNAP"
    timeout --foreground $((dur + 60)) "$ECM_RUN" --iface "$IF_M" --n "$N" --motion-slaves "$M" \
        --duration-sec "$dur" --diag-file "$SNAP" --fresh-offset 0 $ECM_ARGS $eargs > "$LOG/er_$tag.log" 2>&1 &
    E=$!
    local t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        SB_CTL=$CTL "$SBCTL" $cmd
    done
    wait $E
    sb_stop
}

num () { local v; v=$(grep -oE "$2=[0-9]+" "$LOG/er_$1.log" | tail -1 | cut -d= -f2); echo "${v:--1}"; }
wkcf () { local v; v=$(grep "\[WKC GROUP_$2\]" "$LOG/er_$1.log" | grep -oE "$3=[0-9]+" | cut -d= -f2); echo "${v:--1}"; }
foreign () { local v; v=$(grep "reused index" "$LOG/er_$1.log" | grep -oE "$2=[0-9]+" | head -1 | cut -d= -f2); echo "${v:--1}"; }
final_state () { local v; v=$(grep "\[POLICY\] final bus state" "$LOG/er_$1.log" | awk '{print $5}' | tr -d ';'); echo "${v:-none}"; }
has () { grep -q -- "$2" "$LOG/er_$1.log"; }
unlocks () { local v; v=$(grep "\[DC\] state" "$LOG/er_$1.log" | grep -oE "unlocks=[0-9]+" | cut -d= -f2); echo "${v:--1}"; }
regr () { local v; v=$(grep "\[FRESH\] total" "$LOG/er_$1.log" | grep -oE "regressions=[0-9]+" | cut -d= -f2); echo "${v:--1}"; }

for c in $CASES; do
case $c in
l507)
    echo "=== L5-07: replies 14 ms and 15 ms late (~ SOEM's 16-index reuse period), 100 frames each"
    run_case l507 9 "" "2:late 14000 100" "5:late 15000 100"
    fm=$(foreign l507 motion); fi=$(foreign l507 io)
    echo "  caught: DC age gate=$(num l507 'age gate') foreign replies motion=$fm io=$fi, quarantined=$(num l507 parked)"
    chk "L5-07 no old reply reached the application (counter never went back)" "[ $(regr l507) = 0 ]"
    chk "L5-07 IO never took another frame's reply (PARTIAL/OVER = 0)" \
        "[ $(wkcf l507 IO partial) = 0 ] && [ $(wkcf l507 IO over) = 0 ]"
    chk "L5-07 DC stayed locked (0 unlocks)" "[ $(unlocks l507) = 0 ]"
    o=$(grep "\[GROUP_MOTION\] cycles" "$LOG/er_l507.log" | grep -oE "overrun=[0-9]+" | cut -d= -f2)
    chk "L5-07 no overrun chain: motion overrun=${o:-?} <= $OVR_MAX" "[ ${o:-999} -le $OVR_MAX ]"
    chk "L5-07 bus back to RUN" "[ $(final_state l507) = RUN ]"
    ;;
l507neg)
    echo "=== L5-07 negative control: --no-quarantine --no-reply-check (count, don't act)"
    run_case l507neg 9 "--no-quarantine --no-reply-check" "2:late 14000 100" "5:late 15000 100"
    fm=$(foreign l507neg motion); fi=$(foreign l507neg io); g=$(num l507neg 'age gate')
    r=$(regr l507neg); pio=$(wkcf l507neg IO partial); oio=$(wkcf l507neg IO over)
    echo "  unprotected: foreign motion=$fm io=$fi, stale motion (DC)=$g, counter regressions=$r, IO PARTIAL=$pio OVER=$oio"
    chk "L5-07neg without protection old/foreign replies ARE accepted" \
        "[ $((fm + fi + g + r + pio + oio)) -gt 0 ]"
    ;;
l508)
    echo "=== L5-08: 50 duplicated replies, then 30 reordered pairs"
    run_case l508 8 "" "2:dup 50" "4:reorder 30"
    chk "L5-08 internal state intact: bus ends in RUN" "[ $(final_state l508) = RUN ]"
    chk "L5-08 DC stayed locked (0 unlocks)" "[ $(unlocks l508) = 0 ]"
    chk "L5-08 no old reply reached the application" "[ $(regr l508) = 0 ]"
    chk "L5-08 duplicates / late ones recognised by index pairing (rx_unmatched > 0)" "[ $(num l508 rx_unmatched) -gt 0 ]"
    if ! echo "$ECM_ARGS" | grep -q -- --no-tx-ts; then
        mt=$(grep -oE "turnaround: matched=[0-9]+" "$LOG/er_l508.log" | tail -1 | cut -d= -f2); mt=${mt:-0}; mc=$(grep "\[GROUP_MOTION\] cycles" "$LOG/er_l508.log" | grep -oE "cycles=[0-9]+" | cut -d= -f2)
        chk "L5-08 turnaround pairing kept up: matched $mt of $mc motion cycles (>= 90 %)" "[ $((mt * 10)) -ge $((mc * 9)) ]"
    fi
    ;;
l509)
    echo "=== L5-09: slave 3's TxPDO frozen for 60 PD frames, WKC stays correct"
    run_case l509 6 "" "2:stale 2 60"
    chk "L5-09 detected: slave 3 inputs unchanged although WKC is correct" "has l509 'slave 3: inputs UNCHANGED'"
    chk "L5-09 reported back to normal" "has l509 'slave 3: inputs changing again'"
    chk "L5-09 no other slave flagged" "[ \$(grep -c 'inputs UNCHANGED' $LOG/er_l509.log) = 1 ]"
    ;;
l509neg)
    echo "=== L5-09 negative control: same injection, checker threshold above it (--fresh-stale 1000)"
    run_case l509neg 6 "--fresh-stale 1000" "2:stale 2 60"
    chk "L5-09neg nothing detected when the threshold is not reached" "! has l509neg 'inputs UNCHANGED'"
    ;;
l512)
    echo "=== L5-12: 100 SDO write+read rounds, mbx_dup and mbx_repeat injected every 10 rounds"
    if [ ! -x "$L4_TEST" ]; then bad "L5-12 missing $L4_TEST (make in apps/l4_test)"; continue; fi
    sb_start l512 "--no-sm-wd" || continue
    timeout --foreground 120 "$L4_TEST" --iface "$IF_M" --n "$N" --l512 100 --ctl "$CTL" > "$LOG/l4_l512.log" 2>&1
    sb_stop
    grep -E "SOEM mailbox Cnt patch|SOEM dropped|round " "$LOG/l4_l512.log" | sed 's/^/  /'
    chk "L5-12 no response processed twice" "grep -q '\[PASS\] L5-12 no response processed twice' $LOG/l4_l512.log"
    chk "L5-12 every round completes (repeat request works)" "grep -q '\[PASS\] L5-12 every round completes' $LOG/l4_l512.log"
    chk "L5-12 injections really happened (soft_bus mbx_dup/mbx_lost > 0)" \
        "grep -qE 'mbx_lost=[1-9][0-9]* mbx_dup=[1-9]' $LOG/sb_l512.log"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

[ -n "$SB_ARGS" ] && echo "NOTE: soft_bus ran with extra args: $SB_ARGS"
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ "$FAIL" = 0 ]
