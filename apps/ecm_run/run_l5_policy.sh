#!/usr/bin/env bash
# ==========================================================================
# run_l5_policy.sh — Giai doan 7.3: L5-01, L5-02, L5-03/04, L5-05, L5-11b,
# L5-13 against soft_bus, each with its negative control where one exists.
# Policy: docs/fault_policy.md. One ecm_run session per case.
#
#   sudo -E ./run_l5_policy.sh                 all cases
#   sudo -E CASES="l505 l513" ./run_l5_policy.sh
#   sudo -E VALGRIND=1 CASES=l503 ./run_l5_policy.sh
#
# Needs root: L5-03/04 takes veth_s down/up, and chrt (SB_PRIO) needs it.
# soft_bus node k = SOEM slave k+1; slaves 1..M are GROUP_MOTION (DC).
#
# Env: SOFT_BUS ECM_RUN SBCTL IF_M IF_S N M
#      SB_PRIO / SB_CPU  soft_bus SCHED_FIFO priority + core (as run_l6_tests.sh)
#      SB_ARGS           extra soft_bus args for every case except the L5-13
#                        positive run, which needs the SM watchdog ON. On a
#                        non-RT host use "--no-sm-wd": the 3 ms watchdog trips
#                        on scheduling hiccups and adds unrelated recoveries.
#      ECM_ARGS          extra ecm_run args (e.g. --no-tx-ts)
#      OVR_MAX           overruns tolerated in the mute case (default 10)
#      VALGRIND=1        run L5-03/04 under valgrind --leak-check=full
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SOFT_BUS=${SOFT_BUS:-$HERE/../../tools/soft_bus/soft_bus}
SBCTL=${SBCTL:-$HERE/../../tools/soft_bus/sbctl.sh}
ECM_RUN=${ECM_RUN:-$HERE/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
N=${N:-8}; M=${M:-4}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
SB_ARGS=${SB_ARGS:-}; ECM_ARGS=${ECM_ARGS:-}
OVR_MAX=${OVR_MAX:-10}; VALGRIND=${VALGRIND:-0}
CASES=${CASES:-"l501 l502 l502neg l503 l505 l505neg l511b l513 l513neg"}
CTL=/tmp/soft_bus_l5pol.ctl
SNAP=/tmp/ecm_diag_l5pol.txt

LOG=log_l5_policy_$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }   # chk "name" "test expression"

for f in "$SOFT_BUS" "$ECM_RUN" "$SBCTL"; do
    [ -x "$f" ] || { echo "missing $f (build soft_bus and ecm_run first)"; exit 2; }
done
[ "$(id -u)" = 0 ] || echo "WARNING: not root -- L5-03/04 (ip link) and SB_PRIO will fail. Use sudo -E."

# run_case TAG DURATION "SOFT_BUS ARGS" "ECM_RUN ARGS" "t:cmd" ...
#   cmd = a soft_bus control command, or "sh <shell>"
#   In "sh" commands use $(ecm_pid): $E is the pid of `timeout`, which does
#   NOT pass SIGSTOP/SIGCONT on to ecm_run (found on the Jetson 25/9: the
#   first L5-13 never stopped the master; the sandbox "passed" only because
#   its jitter tripped the watchdog anyway).
run_case () {
    local tag=$1 dur=$2 sbargs=$3 eargs=$4; shift 4
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    rm -f "$CTL" "$SNAP"
    "${sb[@]}" --iface "$IF_S" --n "$N" --pdo-size 4 --dc 32 --dc-report-s 100 --ctl "$CTL" $sbargs \
        > "$LOG/sb_$tag.log" 2>&1 &
    local P=$!
    sleep 0.5
    kill -0 $P 2>/dev/null || { echo "  soft_bus did not start ($LOG/sb_$tag.log)"; return 1; }
    local pre=() bin=$ECM_RUN
    if [ "$VALGRIND" = 1 ] && [ "$tag" = l503 ]; then
        # valgrind refuses setcap binaries ("Can't execute setuid/setgid/
        # setcap executable", found on the Jetson 25/9). We run as root, so
        # capabilities are not needed: use a copy (cp drops the xattr).
        bin=$LOG/ecm_run.nocap; cp "$ECM_RUN" "$bin"
        pre=(valgrind --leak-check=full --suppressions="$HERE/soem.supp" --log-file="$LOG/valgrind_$tag.txt")
    fi
    timeout --foreground $((dur + 60)) "${pre[@]}" "$bin" --iface "$IF_M" --n "$N" --motion-slaves "$M" \
        --duration-sec "$dur" --diag-file "$SNAP" $ECM_ARGS $eargs > "$LOG/er_$tag.log" 2>&1 &
    E=$!
    local t=0
    for step in "$@"; do
        local at=${step%%:*} cmd=${step#*:}
        sleep "$(awk -v a="$at" -v b="$t" 'BEGIN { print a - b }')"; t=$at
        if [ "${cmd#sh }" != "$cmd" ]; then eval "${cmd#sh }"; else SB_CTL=$CTL "$SBCTL" $cmd; fi
    done
    wait $E
    SB_CTL=$CTL "$SBCTL" status >/dev/null 2>&1; sleep 0.3
    kill -INT $P 2>/dev/null
    for _ in $(seq 50); do kill -0 $P 2>/dev/null || break; sleep 0.1; done
    ip link set "$IF_S" up 2>/dev/null
}

# value of KEY=<n> on the "[POLICY] final ..." line, or on "[WKC <group>]"
pol () { local v; v=$(grep "\[POLICY\] final" "$LOG/er_$1.log" | grep -o "$2=[0-9]*" | head -1 | cut -d= -f2); echo "${v:--1}"; }
ecm_pid () { local c; c=$(pgrep -P "$E" | head -1); echo "${c:-$E}"; }   # ecm_run (or valgrind) under timeout
wkc () { grep "\[WKC GROUP_$2\]" "$LOG/er_$1.log" | grep -o "$3=[0-9]*" | cut -d= -f2; }
ovr () { grep "\[GROUP_$2\] cycles" "$LOG/er_$1.log" | grep -o "overrun=[0-9]*" | cut -d= -f2; }
final_state () { local v; v=$(grep "\[POLICY\] final bus state" "$LOG/er_$1.log" | awk '{print $5}' | tr -d ';'); echo "${v:-none}"; }
has () { grep -q -- "$2" "$LOG/er_$1.log"; }

for c in $CASES; do
case $c in
l501)
    echo "=== L5-01: wkc_short on slave 3 for 50 frames"
    run_case l501 8 "$SB_ARGS" "" "3:wkc_short 2 50"
    p=$(wkc l501 MOTION partial); pio=$(wkc l501 IO partial)
    echo "  motion PARTIAL=$p io PARTIAL=$pio (50 frames with logical datagrams: motion + the IO frames among them)"
    chk "L5-01 motion PARTIAL counted 40..50" "[ ${p:-0} -ge 40 ] && [ ${p:-0} -le 50 ]"
    chk "L5-01 IO not affected (slave 3 is motion)" "[ ${pio:-1} = 0 ]"
    chk "L5-01 bus went DEGRADED (motion PARTIAL)" "has l501 'RUN -> DEGRADED (motion PARTIAL)'"
    chk "L5-01 never LOST" "[ $(pol l501 LOST) = 0 ]"
    chk "L5-01 back to RUN" "[ $(final_state l501) = RUN ]"
    ;;
l502)
    echo "=== L5-02: mute 50 (DEGRADED only), then mute 200 (LOST -> RECOVER -> RUN)"
    run_case l502a 6 "$SB_ARGS" "" "2:mute 50"
    chk "L5-02a mute 50: DEGRADED, never LOST" "[ $(pol l502a LOST) = 0 ] && [ $(pol l502a DEGRADED) -ge 1 ]"
    run_case l502 8 "$SB_ARGS" "" "2:mute 200"
    chk "L5-02 LOST entered once" "[ $(pol l502 LOST) = 1 ]"
    chk "L5-02 at exactly 100 motion NOFRAME in a row" "[ $(wkc l502 MOTION max_run_bad) = 100 ]"
    chk "L5-02 RECOVER -> RUN" "has l502 'RECOVER -> RUN' && [ $(final_state l502) = RUN ]"
    chk "L5-02 every slave back in OP" "has l502 '$N/$N in OP'"
    o=$(ovr l502 MOTION)
    chk "L5-02 no overrun chain: motion overrun=$o <= $OVR_MAX" "[ ${o:-999} -le $OVR_MAX ]"
    ;;
l502neg)
    echo "=== L5-02 negative control: legacy receive timeout (EC_TIMEOUTRET 2 ms)"
    run_case l502neg 8 "$SB_ARGS" "--rx-timeout-legacy" "2:mute 200"
    o=$(ovr l502neg MOTION)
    chk "L5-02neg legacy timeout DOES chain overruns: $o >= 50" "[ ${o:-0} -ge 50 ]"
    ;;
l503)
    echo "=== L5-03/04: $IF_S down 2 s, then up"
    if [ "$VALGRIND" = 1 ] && ! command -v valgrind >/dev/null; then
        bad "L5-03/04 VALGRIND=1 but valgrind is not installed (sudo apt install valgrind)"; continue
    fi
    sb503=$SB_ARGS
    if [ "$VALGRIND" = 1 ]; then
        # Under valgrind (tens of times slower on arm64) ecm_run cannot feed
        # the 3 ms SM watchdog while it waits for OP: "Failed to reach
        # OPERATIONAL" (Jetson 25/9). This run grades leaks and memory errors;
        # recovery after watchdog trips is graded by the normal l503 run.
        echo "  NOTE: VALGRIND=1 -> soft_bus --no-sm-wd for this case"
        sb503="$SB_ARGS --no-sm-wd"
    fi
    run_case l503 10 "$sb503" "" "3:sh ip link set $IF_S down" "5:sh ip link set $IF_S up"
    chk "L5-03 LOST while the link is down" "[ $(pol l503 LOST) -ge 1 ]"
    chk "L5-04 RECOVER -> RUN after link up" "has l503 'RECOVER -> RUN' && [ $(final_state l503) = RUN ]"
    chk "L5-04 every slave back in OP" "has l503 '$N/$N in OP'"
    chk "L5-03/04 ecm_run exited normally" "has l503 '\[POLICY\] final'"
    if [ "$VALGRIND" = 1 ]; then
        v=$LOG/valgrind_l503.txt
        chk "L5-03/04 valgrind: definitely lost 0" "grep -q 'definitely lost: 0 bytes' $v || grep -q 'no leaks are possible' $v"
        chk "L5-03/04 valgrind: 0 errors" "grep -q 'ERROR SUMMARY: 0 errors' $v"
    fi
    ;;
l505)
    echo "=== L5-05: safeop slave 3 with code 0x001A"
    run_case l505 8 "$SB_ARGS" "" "2:safeop 2 0x001A"
    chk "L5-05 monitor names slave 3 and the code" "has l505 'slave 3: AL 0x14 code 0x001a'"
    chk "L5-05 path A: ack" "has l505 'slave 3: .* -> ack'"
    chk "L5-05 path A: request OP" "has l505 'slave 3: .* -> request OP'"
    chk "L5-05 slave 3 back in OP" "has l505 'slave 3 back in OP'"
    chk "L5-05 bus never LOST" "[ $(pol l505 LOST) = 0 ]"
    ;;
l505neg)
    echo "=== L5-05 negative control: --no-recover"
    run_case l505neg 6 "$SB_ARGS" "--no-recover" "2:safeop 2 0x001A"
    chk "L5-05neg without recovery slave 3 stays SAFEOP+ERR" "has l505neg '\[DIAG\] slave 3: AL state SAFEOP+ERR'"
    ;;
l511b)
    echo "=== L5-11b: drop/restore an IO slave (6) and a DC motion slave (2): path B"
    run_case l511b 12 "$SB_ARGS" "" "2:drop_node 5" "3:restore_node 5" "6:drop_node 1" "7:restore_node 1"
    chk "L5-11b slave 6 re-addressed + reconfigured to SAFE-OP" "has l511b 'slave 6: recover+reconfigure: address ok.*SAFE-OP'"
    chk "L5-11b slave 6 back in OP" "has l511b 'slave 6 back in OP'"
    chk "L5-11b slave 2 DC restored, SYNC0 re-armed" "has l511b 'slave 2: DC restored'"
    chk "L5-11b slave 2 back in OP" "has l511b 'slave 2 back in OP'"
    chk "L5-11b bus ends in RUN" "[ $(final_state l511b) = RUN ]"
    ;;
l513)
    echo "=== L5-13: master stopped 20 ms (SIGSTOP), SM watchdog ON"
    run_case l513 8 "" "" "3:sh X=\$(ecm_pid); kill -STOP \$X; sleep 0.02; kill -CONT \$X; echo \"  stopped pid \$X (\$(cat /proc/\$X/comm)) 20 ms\""
    # the trip must be the one we caused: soft_bus logs every expiry with the gap
    ngap=$(grep -c "SM watchdog expired ([0-9][0-9]\.[0-9]* ms" "$LOG/sb_l513.log")
    echo "  soft_bus: $(grep -c 'SM watchdog expired' "$LOG/sb_l513.log") watchdog expiries, $ngap of them with a gap >= 10 ms (the SIGSTOP)"
    chk "L5-13 soft_bus saw the >= 10 ms gap on every motion slave" "[ $ngap -ge $M ]"
    chk "L5-13 master reports 'Sync manager watchdog' (0x001B)" "has l513 'code 0x001b (Sync manager watchdog)'"
    nb=0; for s in $(seq 1 "$M"); do has l513 "slave $s back in OP" && nb=$((nb+1)); done
    chk "L5-13 all $M motion slaves back in OP ($nb)" "[ $nb = $M ]"
    ;;
l513neg)
    echo "=== L5-13 negative control: soft_bus --no-sm-wd"
    run_case l513neg 6 "--no-sm-wd" "" "3:sh X=\$(ecm_pid); kill -STOP \$X; sleep 0.02; kill -CONT \$X"
    chk "L5-13neg no watchdog -> no 0x001B" "! has l513neg 'code 0x001b'"
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

[ -n "$SB_ARGS" ] && echo "NOTE: soft_bus ran with extra args: $SB_ARGS"
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ "$FAIL" = 0 ]
