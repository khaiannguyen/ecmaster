#!/usr/bin/env bash
# ==========================================================================
# run_emcy_9_7.sh — Phase 9.7: CoE Emergency (EMCY) and AL status code
# classes, against soft_bus over veth. Plan: claude/giai_doan_9_ke_hoach.md §9.
#
#   sudo -E ./run_emcy_9_7.sh             all cases
#   sudo -E CASES="m01 p01a" ./run_emcy_9_7.sh
#
#   m01   one EMCY (soft_bus ctl "emcy 2 0x2310 0x03") during OP -> ecm_run
#         logs it for slave 3 with code, class, register and the 5 data
#         bytes; the diag snapshot has it under "emcy:" and as a finding
#   m02   100 EMCY in a row from slave 6 (IO group) -> all 100 arrive, in
#         order (data[0..1] is soft_bus's sequence number), lost=0; the log
#         stops after 20 lines per slave ("only counted")
#   m03   EMCY during OP: WKC never wrong (zero/partial/over all 0 in both
#         groups; NOFRAME is the host's timing, compared in m03n without EMCY)
#   p01a  startup: slave 3 refuses every PREOP->SAFEOP with 0x0036 (DC
#         invalid sync0 cycle time, soft_bus "reject_al 2 0x0036 4 sticky")
#         -> "Failed to reach SAFEOP", slave 3 named with its code and
#         "CONFIGURATION error", rc != 0
#   p01b  runtime: slave 3 drops to SAFEOP+ERR with 0x0036 -> FAILED(config)
#         at once, 0 ack / 0 OP requests for it, the others unaffected
#   p02   transient code: L5-05 (0x001A) and its negative control, through
#         apps/ecm_run/run_l5_policy.sh (unchanged behaviour)
#   p03   offline: test_policy_offline (P8, the class table) and
#         test_diag_offline (D9, EMCY model)
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU; SB_ARGS (default
#      --no-sm-wd: the 3 ms SM watchdog is not under test here)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
SB_ARGS=${SB_ARGS---no-sm-wd}
CASES=${CASES:-"m01 m02 m03 p01a p01b p02 p03"}
CTL=/tmp/soft_bus_9_7.ctl

LOG=${LOG:-log_gd9_emcy_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing (veth pairs do not survive a reboot):"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done

# run TAG DURATION "pre-ctl" "STEP..." : soft_bus 8 nodes with a ctl FIFO;
# "pre-ctl" is sent before ecm_run starts; each STEP is "<s>:<ctl command>".
run () {
    local tag=$1 dur=$2 pre=$3; shift 3
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n 8 --pdo-size 4 --dc 32 --ctl "$CTL" $SB_ARGS > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    [ -n "$pre" ] && { echo "$pre" > "$CTL"; sleep 0.3; }
    "$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" > "$LOG/er_$tag.log" 2>&1 &
    local ep=$! t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        echo "$cmd" > "$CTL"
    done
    wait $ep; RC=$?
    sleep 0.3
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log; D=$LOG/diag_$tag.txt
}
wkcbad () {   # zero+partial+over of both groups in ecm_run's final [WKC ...] lines
    grep -E "^\s+\[WKC GROUP_(MOTION|IO)\]" "$1" | grep -oE "(zero|partial|over)=[0-9]+" |
        awk -F= '{ s += $2 } END { print s + 0 }'
}

for c in $CASES; do
case $c in
m01)
    echo "=== M-01 one EMCY from slave 3 during OP"
    run m01 4 "" "2:emcy 2 0x2310 0x03"
    chk "M-01 ecm_run rc 0" "[ $RC = 0 ]"
    chk "M-01 log: slave 3, code 0x2310 (current), reg 0x03, data 00 00 A5 5A C3" \
        "grep -q '\[EMCY\] tick=[0-9]* slave 3: code 0x2310 (current) reg 0x03 data 00 00 A5 5A C3' $E"
    chk "M-01 exactly one EMCY, from slave 3 only" "[ \$(grep -c '\[EMCY\]' $E) = 1 ]"
    chk "M-01 snapshot: emcy section, slave 3, 1 message" \
        "grep -q '^emcy: total=1 lost=0' $D && grep -q '^  slave 3: 1 message' $D && grep -q 'code=0x2310 (current) reg=0x03 data=00 00 A5 5A C3' $D"
    chk "M-01 snapshot finding names slave 3" "grep -q 'slave 3: 1 emergency message(s), last: code 0x2310' $D"
    ;;
m02)
    echo "=== M-02 100 EMCY in a row from slave 6"
    run m02 6 "" "2:emcy 5 0xFF01 0x81 100"
    chk "M-02 ecm_run rc 0" "[ $RC = 0 ]"
    chk "M-02 soft_bus posted 100" "grep -q 'node 5 posted its last EMCY (100 in total)' $LOG/sb_m02.log"
    chk "M-02 snapshot: 100 received, 0 lost" "grep -q '^emcy: total=100 lost=0' $D && grep -q '^  slave 6: 100 message' $D"
    chk "M-02 newest kept is sequence 99 (0x63), then 98, 97, 96: order kept" \
        "[ \"\$(grep -A4 '^  slave 6:' $D | grep -oE 'data=[0-9A-F]{2}' | cut -d= -f2 | tr '\n' ' ')\" = '63 62 61 60 ' ]"
    SEQ=$(grep '\[EMCY\] .* slave 6:' "$E" | grep -oE 'data [0-9A-F]{2}' | awk '{print $2}' | tr '\n' ' ')
    chk "M-02 logged lines are sequences 00..13 in order (first 20)" \
        "[ \"$SEQ\" = '00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 ' ]"
    chk "M-02 log says further EMCY are only counted" "grep -q 'further EMCY of this slave only counted' $E"
    ;;
m03)
    echo "=== M-03 EMCY during OP does not disturb process data"
    run m03 6 "" "2:emcy 0 0x8110 0x11 50" "3:emcy 4 0x4210 0x09 50"
    RC1=$RC
    run m03n 6 ""
    chk "M-03 both runs rc 0" "[ $RC1 = 0 ] && [ $RC = 0 ]"
    chk "M-03 with 100 EMCY: zero+partial+over = $(wkcbad "$LOG/er_m03.log") (must be 0)" "[ $(wkcbad "$LOG/er_m03.log") = 0 ]"
    chk "M-03 without EMCY: zero+partial+over = $(wkcbad "$LOG/er_m03n.log") (must be 0)" "[ $(wkcbad "$LOG/er_m03n.log") = 0 ]"
    chk "M-03 100 EMCY received" "grep -q '^emcy: total=100 lost=0' $LOG/diag_m03.txt"
    echo "    NOFRAME motion with EMCY / without: $(grep -oE 'GROUP_MOTION\] ok=[0-9]+ noframe=[0-9]+' $LOG/er_m03.log | grep -oE 'noframe=[0-9]+') / $(grep -oE 'GROUP_MOTION\] ok=[0-9]+ noframe=[0-9]+' $LOG/er_m03n.log | grep -oE 'noframe=[0-9]+')"
    ;;
p01a)
    echo "=== P-01a startup: slave 3 refuses SAFE-OP with 0x0036 (every time)"
    run p01a 2 "reject_al 2 0x0036 4 sticky"
    chk "P-01a ecm_run refuses (rc != 0): Failed to reach SAFEOP" "[ $RC != 0 ] && grep -q 'Failed to reach SAFEOP' $E"
    chk "P-01a slave 3 named with 0x0036 and CONFIGURATION error" \
        "grep -q 'slave 3 .*code 0x0036 (DC invalid sync0 cycle time) -- CONFIGURATION error' $E"
    chk "P-01a only slave 3 listed" "[ \$(grep -c '^ecm_run:   slave ' $E) = 1 ]"
    chk "P-01a never requested OP" "! grep -q 'Failed to reach OPERATIONAL' $E"
    ;;
p01b)
    echo "=== P-01b runtime: slave 3 drops to SAFEOP+ERR with 0x0036"
    run p01b 6 "" "2:safeop 2 0x0036"
    chk "P-01b ecm_run rc 0 (runs on, degraded)" "[ $RC = 0 ]"
    chk "P-01b FAILED(config) at once, with code and text" \
        "grep -q 'slave 3: AL 0x14 code 0x0036 (DC invalid sync0 cycle time) is a CONFIGURATION error -> FAILED(config) at once' $E"
    chk "P-01b no ack / no OP request for slave 3" "! grep -qE '\[RECOVERY\] slave 3: .* -> (ack|request OP)' $E"
    chk "P-01b summary: slave 3 ack=0 op=0 FAILED(config)" "grep -q '\[POLICY\] slave 3: recoveries=0 ack=0 op=0 reconfig=0 UNHEALTHY at exit FAILED(config)' $E"
    chk "P-01b no other slave touched" "! grep -qE '\[POLICY\] slave [124-8]:' $E"
    ;;
p02)
    echo "=== P-02 transient code (L5-05, 0x001A) still recovers"
    (cd "$ROOT/apps/ecm_run" && LOG="$LOG/p02" SB_ARGS="$SB_ARGS" ECM_ARGS="--no-tx-ts" CASES="l505 l505neg" \
        ./run_l5_policy.sh) > "$LOG/p02.log" 2>&1
    chk "P-02 L5-05 + negative control pass" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/p02.log"
    ;;
p03)
    echo "=== P-03 offline"
    make -s -C "$ROOT/libecmaster/policy" test > "$LOG/p03_policy.log" 2>&1
    chk "P-03 test_policy_offline (P8 class table)" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/p03_policy.log && grep -q 'P8s' $LOG/p03_policy.log"
    make -s -C "$ROOT/libecmaster/diag" test > "$LOG/p03_diag.log" 2>&1
    chk "P-03 test_diag_offline (D9 EMCY model)" "grep -q 'RESULT: [0-9]* pass, 0 fail' $LOG/p03_diag.log && grep -q 'D9' $LOG/p03_diag.log"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
