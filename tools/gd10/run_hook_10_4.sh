#!/usr/bin/env bash
# ==========================================================================
# run_hook_10_4.sh — Phase 10.4: RT hook + app <-> RT exchange in ecm_run.
# Plan: claude/giai_doan_10_ke_hoach.md §6, design docs/app_rt_exchange.md.
# Offline part: libecmaster/xchg (test_xchg_offline X-01o..X-04o, Q-04o;
# run_xchg_tsan.sh Q-05 with two negative controls).
#
#   sudo -E tools/gd10/run_hook_10_4.sh                  functional (~2 min)
#   sudo -E CASES="q02 q03" Q03_SEC=1800 STRICT=1 SB_PRIO=79 tools/gd10/run_hook_10_4.sh
#                                                        Jetson timing (Q-02, Q-03 30 min)
#
#   q01   golden 4+4 and N=1 without a hook: structure unchanged; and WITH
#         --hook empty: still unchanged (the hook adds no datagram)
#   h01   --hook empty: one call per tick, histogram printed
#   h02   --hook xchg, P1: 0x607A setpoints (sine, lead 4) + 0x6064 state:
#         setpoints used, echo of the output = the sine of tick+1 (0
#         mismatch); soft_bus sees 0x607A change. h02n: the echo checked
#         against tick+2 must mismatch (negative control of the echo)
#   h03   Q-04: producer stops 100 ms: underrun ~ 100 - lead, the output
#         holds its last value meanwhile (echo still 0 mismatch)
#   h04   negative control: --xchg-lead 0 (setpoint for the CURRENT tick):
#         every setpoint late, none used
#   h05   4 axes' 0x607A + 0x6064 on the 4-axis drive: 8 slots, all used
#   q02   occupancy p99.99 with --hook empty minus without hook < 5 us
#         (STRICT=1: a failure fails the run; else reported only)
#   q03   4 output slots CSP-like sine for Q03_SEC (default 30): hook p99.99
#         <= 20 us, 0 overrun, 0 echo mismatch (timing STRICT=1 only)
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU STRICT Q03_SEC Q02_SEC
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
STRICT=${STRICT:-0}
Q03_SEC=${Q03_SEC:-30}; Q02_SEC=${Q02_SEC:-30}
CASES=${CASES:-"q01 h01 h02 h03 h04 h05 q02 q03"}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof; AX4=$PROF/cia402_4ax.prof
CTL=/tmp/soft_bus_10_4.ctl

LOG=${LOG:-log_gd10_hook_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0; INFO=0
ok   () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad  () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
info () { echo "  [INFO] $1"; INFO=$((INFO+1)); }
chk  () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
tchk () { if eval "$2"; then ok "$1"; elif [ "$STRICT" = 1 ]; then bad "$1"; else info "$1 -- not met (timing, STRICT=0)"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done

run () {   # run TAG N DURATION "soft_bus args" "ecm_run args" STEP...
    local tag=$1 n=$2 dur=$3 sbargs=$4 eargs=$5; shift 5
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n "$n" --dc 32 --no-sm-wd --ctl "$CTL" $sbargs > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "$ECM_RUN" --iface "$IF_M" --n "$n" --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" $eargs > "$LOG/er_$tag.log" 2>&1 &
    local ep=$! t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        echo "$cmd" > "$CTL"
    done
    wait $ep; RC=$?
    sleep 0.3
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log; S=$LOG/sb_$tag.log
}
fld () { grep -o "$1" "$E" | head -1 | grep -o '[0-9]*$'; }       # first number after a pattern
slot () { grep "\[XCHG\] slot $1 " "$E" | grep -o "$2=[0-9]*" | cut -d= -f2; }
app () { grep '\[XCHG-APP\]' "$E" | grep -o " $1=[0-9]*" | cut -d= -f2; }
hookp () { grep '\[HOOK\]' "$E" | grep -o "$1=[0-9]*" | cut -d= -f2; }
cycles () { grep -o '\[GROUP_MOTION\] cycles=[0-9]*' "$E" | grep -o '[0-9]*$'; }
occ9999 () { grep '^occupancy ' "$E" | tail -1 | grep -o 'p99.99= *[0-9]*' | grep -o '[0-9]*$'; }
overrun () { grep '\[GROUP_MOTION\] cycles=' "$E" | grep -o 'overrun=[0-9]*' | cut -d= -f2; }

for c in $CASES; do
case $c in
q01)
    echo "=== Q-01 golden without a hook, and with --hook empty"
    G=$ROOT/tools/golden
    chk "Q-01 golden 4+4, no hook" "$G/check_golden.sh > $LOG/q01_a.log 2>&1"
    chk "Q-01 golden N=1, no hook" "env N=1 GROUP_ARGS= GOLDEN=$G/golden_ecm_run_n1.txt $G/check_golden.sh > $LOG/q01_b.log 2>&1"
    chk "Q-01 golden 4+4 WITH --hook empty: structure identical" \
        "env GROUP_ARGS='--motion-slaves 4 --hook empty' $G/check_golden.sh > $LOG/q01_c.log 2>&1"
    ;;
h01)
    echo "=== H-01 empty hook"
    run h01 1 3 "--profile 1=$P1" "--pdo-scan --hook empty"
    chk "H-01 rc 0, hook registered" "[ $RC = 0 ] && grep -q \"RT hook 'empty' registered\" $E"
    chk "H-01 one call per motion tick ($(hookp calls) vs $(cycles))" \
        "[ -n \"\$(hookp calls)\" ] && [ \$(( \$(cycles) - \$(hookp calls) )) -le 2 ] && [ \$(( \$(cycles) - \$(hookp calls) )) -ge -2 ]"
    ;;
h02)
    echo "=== H-02 exchange: setpoints + state, P1"
    run h02 1 4 "--profile 1=$P1" \
        "--pdo-scan --hook xchg --xchg-out 1:0x607A:0 --xchg-in 1:0x6064:0 --xchg-sine 10000:1" "3:pdo_out 0" "3.5:pdo_out 0"
    chk "H-02 rc 0, 2 slots" "[ $RC = 0 ] && grep -q 'xchg slot 1 = in  slave 1 0x6064:00' $E"
    chk "H-02 setpoints used ($(slot 0 used)) for most ticks of $(cycles)" "[ \$(slot 0 used) -gt \$(( \$(cycles) * 9 / 10 )) ]"
    chk "H-02 echo: output = sine(tick+1), $(app echo_checked) checked, 0 mismatch" \
        "[ \$(app echo_checked) -gt 100 ] && [ \$(app echo_mismatch) = 0 ]"
    chk "H-02 no full ring" "[ \$(app full) = 0 ]"
    tchk "H-02 no reader give-up (torn $(app torn))" "[ \$(app torn) = 0 ]"
    chk "H-02 soft_bus sees 0x607A (bytes 3..6) change between two samples" \
        "[ \$(grep -o 'pdo_out: node 0 .* byte: .*' $S | sed 's/.*byte: //' | cut -c10-20 | sort -u | wc -l) = 2 ]"
    run h02n 1 3 "--profile 1=$P1" "--pdo-scan --hook xchg --xchg-out 1:0x607A:0 --xchg-sine 10000:1 --xchg-echo-off 1"
    chk "H-02n negative control: echo against tick+2 mismatches > 90 % ($(app echo_mismatch)/$(app echo_checked))" \
        "[ \$(app echo_checked) -gt 100 ] && [ \$(app echo_mismatch) -gt \$(( \$(app echo_checked) * 9 / 10 )) ]"
    ;;
h03)
    echo "=== H-03 (Q-04) producer stops for 100 ms"
    run h03 1 4 "--profile 1=$P1" "--pdo-scan --hook xchg --xchg-out 1:0x607A:0 --xchg-sine 10000:1 --xchg-starve 2:100"
    U=$(slot 0 underrun)
    chk "H-03 underrun $U in [100 - lead 4 - 3, 100 + 3]" "[ -n \"$U\" ] && [ $U -ge 93 ] && [ $U -le 103 ]"
    chk "H-03 held value is consistent: echo 0 mismatch ($(app echo_checked) checked)" "[ \$(app echo_mismatch) = 0 ]"
    ;;
h04)
    echo "=== H-04 negative control: lead 0 -> every setpoint late"
    run h04 1 3 "--profile 1=$P1" "--pdo-scan --hook xchg --xchg-out 1:0x607A:0 --xchg-sine 10000:1 --xchg-lead 0"
    chk "H-04 none used, late ~ pushed (used $(slot 0 used), late $(slot 0 late), pushed $(app pushed))" \
        "[ \$(slot 0 used) -le 5 ] && [ \$(slot 0 late) -ge \$(( \$(app pushed) * 9 / 10 )) ]"
    ;;
h05)
    echo "=== H-05 four axes' targets + actuals"
    run h05 1 4 "--profile 1=$AX4 --cia402 1:4" \
        "--pdo-scan --hook xchg --xchg-out 1:0x607A:0,1:0x687A:0,1:0x707A:0,1:0x787A:0 --xchg-in 1:0x6064:0,1:0x6864:0,1:0x7064:0,1:0x7864:0 --xchg-sine 10000:2"
    chk "H-05 8 slots" "grep -q 'xchg slot 7 = in  slave 1 0x7864:00' $E"
    chk "H-05 every output slot used > 90 % of ticks" \
        "(for k in 0 1 2 3; do [ \$(slot \$k used) -gt \$(( \$(cycles) * 9 / 10 )) ] || exit 1; done)"
    chk "H-05 echo 0 mismatch" "[ \$(app echo_mismatch) = 0 ] && [ \$(app echo_checked) -gt 100 ]"
    ;;
q02)
    echo "=== Q-02 occupancy cost of an empty hook (${Q02_SEC} s each, 4+4 bus)"
    run q02a 8 "$Q02_SEC" "" "--motion-slaves 4"
    A=$(occ9999)
    run q02b 8 "$Q02_SEC" "" "--motion-slaves 4 --hook empty"
    B=$(occ9999)
    tchk "Q-02 occupancy p99.99 without $A ns, with empty hook $B ns: +$(( ${B:-0} - ${A:-0} )) ns < 5000" \
        "[ -n \"$A\" ] && [ -n \"$B\" ] && [ \$(( $B - $A )) -lt 5000 ]"
    ;;
q03)
    echo "=== Q-03 four output slots for ${Q03_SEC} s"
    run q03 1 "$Q03_SEC" "--profile 1=$AX4 --cia402 1:4" \
        "--pdo-scan --hook xchg --xchg-out 1:0x607A:0,1:0x687A:0,1:0x707A:0,1:0x787A:0 --xchg-in 1:0x6064:0,1:0x6864:0,1:0x7064:0,1:0x7864:0 --xchg-sine 10000:1"
    chk "Q-03 rc 0, echo 0 mismatch ($(app echo_checked) checked)" "[ $RC = 0 ] && [ \$(app echo_mismatch) = 0 ] && [ \$(app echo_checked) -gt 100 ]"
    tchk "Q-03 no reader give-up (torn $(app torn))" "[ \$(app torn) = 0 ]"
    tchk "Q-03 hook p99.99 $(hookp p99.99) ns <= 20000" "[ \$(hookp p99.99) -le 20000 ]"
    tchk "Q-03 0 overrun ($(overrun))" "[ \"\$(overrun)\" = 0 ]"
    tchk "Q-03 underrun 0 on every slot" "(for k in 0 1 2 3; do [ \"\$(slot \$k underrun)\" = 0 ] || exit 1; done)"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail, $INFO info   (logs: $LOG)"
[ $FAIL = 0 ]
