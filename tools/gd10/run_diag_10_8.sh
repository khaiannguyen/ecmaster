#!/usr/bin/env bash
# ==========================================================================
# run_diag_10_8.sh — Phase 10.8: axis diagnosis (EMCY / 0x603F -> axis
# error with text, slave configuration errors -> axis error, no retry) and
# the X-05 tooling (controlword / statusword trace from a capture),
# through SOEM / ecm_run against the virtual drive of soft_bus.
# Plan: claude/giai_doan_10_ke_hoach.md §10. Offline: make -C libecm_cia402
# test negctl (D-01..D-04, E2-01, E2-02).
#
#   sudo -E tools/gd10/run_diag_10_8.sh
#
#   e01  E2-01 drv_fault 0x2310 while enabled: [EMCY] from the slave, an
#        [AXIS] line "Fault ... code 0x2310 continuous over current
#        [current] (0x603F, = EMCY)", the same line in the diag file
#   e02  vendor table (--emcy-map, P1's vendor id): drv_fault 0xFF42 ->
#        the vendor text, class "device specific"; a bad map file refused
#   e03  slave refuses its configuration (safeop, AL 0x001D) while enabled:
#        no retry (FAILED(config)), axis error "slave refused its
#        configuration", a later ENABLE refused, never enabled again
#   x05  cw_trace.py on our own capture: path SOD -06-> RTSO -07-> SO -0F->
#        OE -07-> SO -06-> RTSO -00-> SOD; X-05 against itself PASS;
#        negative control: against a run that stops by quick stop -> FAIL
# Env: SOFT_BUS ECM_RUN IF_M IF_S SB_PRIO SB_CPU
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
STRICT=${STRICT:-0}
CASES=${CASES:-"e01 e02 e03 x05"}
PROF=$ROOT/config/profiles
P1=$PROF/p1_draft.prof
ENI=$ROOT/config/eni/eni_mixed.enicfg
CTL=/tmp/soft_bus_10_8.ctl

LOG=${LOG:-log_gd10_diag_$(date +%Y%m%d_%H%M%S)}
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
IS=$LOG/is620n_min_6502.prof          # reduced IS620N + 0x6502 (test assumption, see run_axes_10_3.sh)
{ cat "$PROF/is620n_min.prof"; echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro a5010000"; } > "$IS"

run () {   # run TAG N DURATION "soft_bus args" "ecm_run args" "script" STEP...
    local tag=$1 n=$2 dur=$3 sbargs=$4 eargs=$5 script=$6; shift 6
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL"
    # shellcheck disable=SC2086
    "${sb[@]}" --iface "$IF_S" --n "$n" --dc 32 --no-sm-wd --ctl "$CTL" $sbargs > "$LOG/sb_$tag.log" 2>&1 &
    local sbp=$!
    sleep 0.5
    # shellcheck disable=SC2086
    "${ER:-$ECM_RUN}" --iface "$IF_M" --n "$n" --duration-sec "$dur" --no-tx-ts \
        --diag-file "$LOG/diag_$tag.txt" $eargs --hook cia402 --cia402-script "$script" > "$LOG/er_$tag.log" 2>&1 &
    local ep=$! t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        case $cmd in
            kill:*) kill -"${cmd#kill:}" $ep ;;
            *) echo "$cmd" > "$CTL" ;;
        esac
    done
    wait $ep; RC=$?
    sleep 0.3
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    E=$LOG/er_$tag.log; S=$LOG/sb_$tag.log
}
# tick of the first state event "-> STATE" of an axis, and of a script command
evt  () { grep "\[CIA402\] t=.* axis $1 .* -> $2 (" "$E" | grep -v "axis $1 $2 -> $2 (" | head -1 | grep -o 'tick=[0-9]*' | cut -d= -f2; }
cmdt () { grep "\[CIA402-APP\] t=.* axis $1: $2$" "$E" | head -1 | grep -o 'tick=[0-9]*' | cut -d= -f2; }
fin  () { grep "  \[CIA402\] axis $1 " "$E" | grep -o "$2=[-0-9a-zA-Z]*" | head -1 | cut -d= -f2; }
final_state () { grep "  \[CIA402\] axis $1 " "$E" | sed 's/.*): //; s/ sw=.*//'; }
never () { ! grep "\[CIA402\] t=.* axis $1 .* -> $2 (" "$E" | grep -qv "axis $1 $2 -> $2 ("; }



command -v tshark >/dev/null || { echo "tshark not installed"; exit 2; }
CWT="python3 $ROOT/tools/gd10/cw_trace.py"
# capture TAG: a run (args as run) with tshark on the master side
cap () {
    local tag=$1; shift
    rm -f "$LOG/$tag.pcap"
    tshark -q -i "$IF_M" -F pcap -w "$LOG/$tag.pcap" -f "ether proto 0x88a4" >/dev/null 2>&1 &
    local tp=$!
    for _ in $(seq 50); do [ -s "$LOG/$tag.pcap" ] && break; sleep 0.1; done
    sleep 1
    run "$tag" "$@"
    sleep 0.3
    kill -INT $tp; wait $tp 2>/dev/null
}

for c in $CASES; do
case $c in
e01)
    echo "=== E2-01 drive fault -> axis diagnosis"
    run e01 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0" "2:drv_fault 0 0 0x2310" "2.2:drv_clear 0 0"
    chk "E2-01 EMCY 0x2310 received from slave 1" "grep -q '\[EMCY\] .*slave 1: code 0x2310' $E"
    chk "E2-01 [AXIS] line: Fault, code, text, class, source" \
        "grep -q '\[AXIS\] .*axis 1:0 (slave 1): Fault, mode CSP, error drive fault; code 0x2310 continuous over current \[current\] (0x603F, = EMCY)' $E"
    chk "E2-01 the diag file has the axis section with the same line" \
        "grep -q '^CiA402 axes:' $LOG/diag_e01.txt && grep -q 'axis 1:0 (slave 1): Fault.*code 0x2310 continuous over current' $LOG/diag_e01.txt"
    ;;
e02)
    echo "=== E2-02 vendor EMCY texts"
    M=$LOG/p1_test.emcy
    printf '# test table (P1 vendor id)\nvendor 0x00000499\n0xFF42 test vendor fault 42\n' > "$M"
    run e02 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502 --emcy-map $M" "1 enable 0" "2:drv_fault 0 0 0xFF42" "2.2:drv_clear 0 0"
    chk "E2-02 map loaded (1 vendor code)" "grep -q 'EMCY texts from .*p1_test.emcy (1 vendor codes loaded)' $E"
    chk "E2-02 vendor text, class device specific" "grep -q 'code 0xFF42 test vendor fault 42 \[device specific\]' $E"
    printf 'vendor 0x499\n0xFF42\n' > "$LOG/bad.emcy"
    "$ECM_RUN" --iface "$IF_M" --n 1 --emcy-map "$LOG/bad.emcy" > "$LOG/er_e02bad.log" 2>&1; RB=$?
    chk "E2-02 bad map refused before the bus, line named (rc $RB)" "[ $RB != 0 ] && grep -q 'bad.emcy:2:' $LOG/er_e02bad.log"
    ;;
e03)
    echo "=== E2-03 slave configuration refused"
    run e03 1 5 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0; 3.5 enable 0" "2:safeop 0 0x001D"
    chk "E2-03 FAILED(config) at once, no retry" "grep -q 'CONFIGURATION error -> FAILED(config) at once' $E && ! grep -q 'slave 1: .* -> ack' $E"
    chk "E2-03 axis error: slave refused its configuration" "grep -q '\[AXIS\] axis 1:0 (slave 1): slave refused its configuration -> axis disabled' $E"
    chk "E2-03 the later ENABLE refused, enabled once only" \
        "grep -q 'ERROR slave refused its configuration' $E && [ \$(grep -c 'axis 0 Switched on -> Operation enabled (' $E) = 1 ]"
    ;;
x05)
    echo "=== X-05 tooling: controlword / statusword trace from a capture"
    cap x05a 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0; 2.5 disable 0"
    $CWT "$LOG/x05a.pcap" > "$LOG/x05a.trace" 2>&1; RT=$?
    chk "X-05 trace found the words, path SOD -06-> RTSO -07-> SO -0F-> OE -07-> SO -06-> RTSO -00-> SOD (rc $RT)" \
        "grep -q 'path: .*SOD -06-> RTSO -07-> SO -0F-> OE -07-> SO -06-> RTSO -00-> SOD$' $LOG/x05a.trace"
    $CWT "$LOG/x05a.pcap" --ref "$LOG/x05a.pcap" > "$LOG/x05_self.txt" 2>&1; RS=$?
    chk "X-05 against itself: PASS" "[ $RS = 0 ] && grep -q '^X-05 PASS' $LOG/x05_self.txt"
    cap x05q 1 4 "--profile 1=$P1 --cia402 1" "--pdo-scan --axis 1 --axis-no-6502" "1 enable 0; 2.5 quickstop 0; 3 disable 0"
    $CWT "$LOG/x05a.pcap" --ref "$LOG/x05q.pcap" > "$LOG/x05_neg.txt" 2>&1; RN=$?
    chk "X-05 negative control: reference stops by quick stop -> FAIL naming OE -07-> SO (rc $RN)" \
        "[ $RN = 1 ] && grep -q 'master transition OE -07-> SO not in the reference' $LOG/x05_neg.txt"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1)) ;;
esac
done

echo
echo "RESULT: $PASS pass, $FAIL fail, $INFO info   (logs: $LOG)"
[ $FAIL = 0 ]
