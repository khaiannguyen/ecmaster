#!/usr/bin/env bash
# ==========================================================================
# run_groups.sh — Phase 9.1: group assignment per slave, empty
# GROUP_IO, one-slave bus (P1/P2/P4: one LAN9252) and a motion-only bus
# (own slave + commercial servo). Plan: claude/giai_doan_9_ke_hoach.md §3.
#
#   sudo -E ./run_groups.sh                  all cases
#   sudo -E CASES="g03 g06" ./run_groups.sh
#
#   g01b  --io-slaves 5-8 on 8 nodes == --motion-slaves 4 (golden 547, line by line)
#   g03   N=1, default groups: OP, GROUP_IO never sent (wire), no IO-caused
#         DEGRADED, DC locked on slave 1
#   g04   N=2, both motion: expected WKC 6, SYNC0 on 2 motion slaves, IO never sent
#   g05   IO slaves between motion slaves (--io-slaves 2,4): map, WKC, SYNC0 only
#         on the motion slaves
#   g06   E-03d: --eni eni_1node_1pdo on a one-node bus -> OP;
#         the same ENI on a two-node bus -> refused (negative)
#   g07   argument checks (negative): every slave IO, both options,
#         position out of range, --motion-slaves k >= n -> refused, rc 1
#   (G-01/G-02/G-08 are the golden checks: tools/golden/check_golden.sh with
#    the defaults, ENI=..., and N=1 GROUP_ARGS= GOLDEN=.../golden_ecm_run_n1.txt)
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S DUR (default 10)
#      SB_ARGS  extra soft_bus args (non-RT host: "--no-sm-wd")
#      ECM_ARGS extra ecm_run args (e.g. --no-tx-ts)
#      NOFRAME_PCT  motion NOFRAME tolerated, percent (default 2; the
#               reference is the Jetson, where it is 0)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
DUR=${DUR:-10}
SB_ARGS=${SB_ARGS:-}; ECM_ARGS=${ECM_ARGS:-}
NOFRAME_PCT=${NOFRAME_PCT:-2}
CASES=${CASES:-"g01b g03 g04 g05 g06 g07"}

LOG=${LOG:-log_gd9_groups_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
has () { grep -q -- "$2" "$LOG/er_$1.log"; }
val () { local v; v=$(grep -- "$2" "$LOG/er_$1.log" | grep -oE "$3=[0-9]+" | head -1 | cut -d= -f2); echo "${v:--1}"; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done

# logical datagrams per group in a pcap: prints "g1=<n> g2=<n>"
lcount () {
    python3 - "$1" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb').read(); off = 24; c = {1: 0, 2: 0}
while off + 16 <= len(f):
    incl = struct.unpack_from('<I', f, off + 8)[0]; p = f[off + 16:off + 16 + incl]; off += 16 + incl
    if len(p) < 16 or p[12:14] != b'\x88\xa4': continue
    q = 16
    while q + 10 <= len(p):
        cmd = p[q]; adr = struct.unpack_from('<I', p, q + 2)[0]; ln = struct.unpack_from('<H', p, q + 6)[0]
        if cmd in (10, 11, 12): c[adr >> 16] = c.get(adr >> 16, 0) + 1
        q += 10 + (ln & 0x7ff) + 2
        if not ln & 0x8000: break
print("g1=%d g2=%d" % (c.get(1, 0), c.get(2, 0)))
PY
}

# run TAG NODES "ecm_run args" -> $LOG/er_TAG.log, $LOG/sb_TAG.log, $LOG/TAG.pcap; RC
run () {
    local tag=$1 nodes=$2 eargs=$3
    "$SOFT_BUS" --iface "$IF_S" --n "$nodes" --pdo-size 4 --dc 32 --dc-report-s 100 $SB_ARGS \
        > "$LOG/sb_$tag.log" 2>&1 &
    local SB=$! TS=
    if command -v tshark >/dev/null; then
        rm -f "$LOG/$tag.pcap"
        tshark -q -i "$IF_M" -F pcap -w "$LOG/$tag.pcap" -f "ether proto 0x88a4" >/dev/null 2>&1 &
        TS=$!
        for _ in $(seq 50); do [ -s "$LOG/$tag.pcap" ] && break; sleep 0.1; done
    fi
    sleep 0.5
    # shellcheck disable=SC2086
    timeout --foreground $((DUR + 60)) "$ECM_RUN" --iface "$IF_M" --duration-sec "$DUR" \
        --diag-file "$LOG/diag_$tag.txt" $ECM_ARGS $eargs > "$LOG/er_$tag.log" 2>&1
    RC=$?
    sleep 0.3
    [ -n "$TS" ] && { kill -INT "$TS"; wait "$TS" 2>/dev/null; }
    kill -INT $SB; wait $SB 2>/dev/null
}

noframe_ok () {   # motion NOFRAME <= NOFRAME_PCT % of the motion cycles
    local nf cy; nf=$(val "$1" "\[WKC GROUP_MOTION\]" noframe); cy=$(val "$1" "\[GROUP_MOTION\] cycles" cycles)
    echo "  motion cycles=$cy noframe=$nf"
    [ "$cy" -gt 0 ] && [ $((nf * 100)) -le $((cy * NOFRAME_PCT)) ]
}

for c in $CASES; do
case $c in
g01b)
    echo "=== G-01b: --io-slaves 5-8 on 8 nodes is the old --motion-slaves 4 (golden, line by line)"
    out=$(GROUP_ARGS="--io-slaves 5-8" "$ROOT/tools/golden/check_golden.sh" 2>&1 | tail -1)
    echo "  $out"
    chk "G-01b structure identical to golden_ecm_run.txt" "echo '$out' | grep -q '^golden OK'"
    ;;
g03)
    echo "=== G-03: one-slave bus, default groups (every slave GROUP_MOTION)"
    run g03 1 "--n 1"
    chk "G-03 ecm_run exited 0" "[ $RC = 0 ]"
    chk "G-03 GROUP_IO reported empty" "has g03 'GROUP_IO (0 slave, 0 byte IOmap) -- GROUP_IO empty'"
    chk "G-03 expected WKC motion=3 io=0" "has g03 'expected WKC -- motion=3 io=0'"
    chk "G-03 reached OP and ran the loop" "has g03 'Starting cyclic loop (base tick = 1000 us, no IO group)'"
    chk "G-03 DC on, ref slave 1, SYNC0 on 1 slave" "has g03 'DC on -- ref=slave 1, SYNC0 1000000 ns on 1 motion slave'"
    chk "G-03 DC locked, 0 unlocks" "has g03 '\[DC\] state=LOCKED' && [ $(val g03 '\[DC\] state' unlocks) = 0 ]"
    chk "G-03 no DEGRADED caused by the IO group" "! has g03 'DEGRADED (io'"
    chk "G-03 never LOST" "[ $(val g03 '\[POLICY\] final' LOST) = 0 ]"
    chk "G-03 motion NOFRAME <= $NOFRAME_PCT %" "noframe_ok g03"
    if [ -s "$LOG/g03.pcap" ]; then
        lc=$(lcount "$LOG/g03.pcap"); echo "  wire: logical datagrams $lc"
        chk "G-03 wire: no GROUP_IO datagram (g2=0), GROUP_MOTION present" \
            "echo '$lc' | grep -q ' g2=0' && ! echo '$lc' | grep -q 'g1=0 '"
    fi
    ;;
g04)
    echo "=== G-04: two-slave bus, both motion (own slave + servo shape)"
    run g04 2 "--n 2"
    chk "G-04 ecm_run exited 0" "[ $RC = 0 ]"
    chk "G-04 GROUP_MOTION 2 slaves, GROUP_IO empty" "has g04 'GROUP_MOTION (2 slave, 18 byte IOmap), GROUP_IO (0 slave'"
    chk "G-04 expected WKC motion=6 io=0" "has g04 'expected WKC -- motion=6 io=0'"
    chk "G-04 SYNC0 on 2 motion slaves" "has g04 'SYNC0 1000000 ns on 2 motion slave'"
    chk "G-04 DC locked, 0 unlocks" "has g04 '\[DC\] state=LOCKED' && [ $(val g04 '\[DC\] state' unlocks) = 0 ]"
    chk "G-04 no DEGRADED caused by the IO group" "! has g04 'DEGRADED (io'"
    chk "G-04 motion NOFRAME <= $NOFRAME_PCT %" "noframe_ok g04"
    if [ -s "$LOG/g04.pcap" ]; then
        lc=$(lcount "$LOG/g04.pcap"); echo "  wire: logical datagrams $lc"
        chk "G-04 wire: no GROUP_IO datagram" "echo '$lc' | grep -q ' g2=0'"
    fi
    ;;
g05)
    echo "=== G-05: IO slaves between motion slaves (--io-slaves 2,4 on 8 nodes)"
    run g05 8 "--n 8 --io-slaves 2,4"
    chk "G-05 ecm_run exited 0" "[ $RC = 0 ]"
    chk "G-05 map: motion 6 slaves 54 byte, IO 2 slaves 18 byte" \
        "has g05 'GROUP_MOTION (6 slave, 54 byte IOmap), GROUP_IO (2 slave, 18 byte IOmap)'"
    chk "G-05 expected WKC motion=18 io=6" "has g05 'expected WKC -- motion=18 io=6'"
    s0=$(grep -E '^ecm_run: dc slave [0-9]+ .* SYNC0$' "$LOG/er_g05.log" | awk '{print $4}' | tr '\n' ' ')
    chk "G-05 SYNC0 exactly on the motion slaves (1 3 5 6 7 8): '$s0'" "[ '$s0' = '1 3 5 6 7 8 ' ]"
    chk "G-05 no PARTIAL/OVER in either group (mapping consistent)" \
        "[ $(val g05 '\[WKC GROUP_MOTION\]' partial) = 0 ] && [ $(val g05 '\[WKC GROUP_MOTION\]' over) = 0 ] && [ $(val g05 '\[WKC GROUP_IO\]' partial) = 0 ] && [ $(val g05 '\[WKC GROUP_IO\]' over) = 0 ]"
    chk "G-05 motion NOFRAME <= $NOFRAME_PCT %" "noframe_ok g05"
    ;;
g06)
    echo "=== G-06 (E-03d): ENI of one node on a one-node bus"
    run g06 1 "--eni $ROOT/config/eni/eni_1node_1pdo.enicfg"
    chk "G-06 ecm_run exited 0" "[ $RC = 0 ]"
    chk "G-06 ENI loaded (1 slave)" "has g06 'eni_1node_1pdo.xml): 1 slaves'"
    chk "G-06 reached OP and ran the loop" "has g06 'Starting cyclic loop'"
    chk "G-06 ENI has no DC slave -> DC off" "has g06 'ENI has no DC slave -> DC off'"
    chk "G-06 motion NOFRAME <= $NOFRAME_PCT %" "noframe_ok g06"
    echo "=== G-06neg: the same one-node ENI on a two-node bus must be refused"
    run g06neg 2 "--eni $ROOT/config/eni/eni_1node_1pdo.enicfg"
    chk "G-06neg refused (rc != 0)" "[ $RC != 0 ]"
    chk "G-06neg reason: slave count" "has g06neg 'slave count: ENI 1, bus 2'"
    chk "G-06neg never reached the cyclic loop" "! has g06neg 'Starting cyclic loop'"
    ;;
g07)
    echo "=== G-07 (negative): argument checks, no bus needed"
    a () {   # a TAG EXPECTED_TEXT ARGS...
        local tag=$1 want=$2; shift 2
        "$ECM_RUN" --iface "$IF_M" "$@" > "$LOG/er_$tag.log" 2>&1; local rc=$?
        chk "G-07 $tag refused with rc 1: $want" "[ $rc = 1 ] && has $tag '$want'"
    }
    a allio   "needs at least one slave"       --n 2 --io-slaves 1-2
    a both    "exclude each other"             --n 4 --motion-slaves 2 --io-slaves 3-4
    a range   "outside 1..4"                   --n 4 --io-slaves 3-5
    a syntax  "unexpected"                     --n 4 --io-slaves 3x
    a kge     "needs 0 < k < n"                --n 1 --motion-slaves 1
    ;;
*) echo "unknown case $c"; FAIL=$((FAIL+1));;
esac
done

[ -n "$SB_ARGS" ] && echo "NOTE: soft_bus ran with extra args: $SB_ARGS"
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ "$FAIL" = 0 ]
