#!/bin/bash
# ==========================================================================
# run_l1_tests.sh — Runs and AUTO-GRADES L1-01 .. L1-06
#
# Each run creates its own timestamped log directory: log_l1_YYYYmmdd_HHMMSS/
# Requires: soft_bus, l1_probe (make; make l1_probe), slaveinfo, tshark
# ==========================================================================

set -u

SOEM_DIR=${SOEM_DIR:-/home/khaian/projects/SOEM}
SLAVEINFO=${SLAVEINFO:-$SOEM_DIR/build/samples/slaveinfo/slaveinfo}
HERE=$(cd "$(dirname "$0")" && pwd)
SOFTBUS=$HERE/soft_bus
PROBE=$HERE/l1_probe

LOGDIR=$HERE/log_l1_$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOGDIR"

PASS=0; FAIL=0
RESULTS=()

ok   () { echo "  [PASS] $1"; PASS=$((PASS+1)); RESULTS+=("PASS|$1"); }
bad  () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); RESULTS+=("FAIL|$1"); }
note () { echo "  [ .. ] $1"; }

echo "=== Checking prerequisites ==="
for f in "$SOFTBUS" "$PROBE"; do
    [ -x "$f" ] || { echo "Missing $f — run: make && make l1_probe"; exit 1; }
done
[ -x "$SLAVEINFO" ] || { echo "slaveinfo not found at $SLAVEINFO"; exit 1; }
command -v tshark >/dev/null || { echo "Missing tshark: sudo apt install tshark"; exit 1; }
echo "OK. Logs will be saved to: $LOGDIR"

if ! ip link show veth_m >/dev/null 2>&1; then
    echo ""
    echo "=== Setting up veth rig ==="
    sudo ip link add veth_m type veth peer name veth_s || exit 1
    sudo ip link set veth_m up && sudo ip link set veth_s up
    sudo ethtool -K veth_m gro off gso off tso off 2>/dev/null
    sudo ethtool -K veth_s gro off gso off tso off 2>/dev/null
else
    echo "=== veth rig already exists, reusing it ==="
fi

SB_PID=""
cleanup () {
    [ -n "$SB_PID" ] && kill "$SB_PID" 2>/dev/null
    wait "$SB_PID" 2>/dev/null
    SB_PID=""
}
trap 'cleanup; exit 130' INT TERM

start_softbus () {   # $1=N $2=pdo $3=tag
    "$SOFTBUS" --iface veth_s --n "$1" --pdo-size "$2" > "$LOGDIR/softbus_$3.log" 2>&1 &
    SB_PID=$!
    sleep 1
    if ! kill -0 "$SB_PID" 2>/dev/null; then
        echo "soft_bus died on startup — see $LOGDIR/softbus_$3.log"
        cat "$LOGDIR/softbus_$3.log"
        return 1
    fi
    return 0
}

run_probe () { "$PROBE" veth_m > "$LOGDIR/probe_$1.log" 2>&1; echo "$LOGDIR/probe_$1.log"; }

# ======================= CASE A: N=1 — L1-01/02/03 =======================
echo ""
echo "############ CASE A: N=1  (L1-01, L1-02, L1-03) ############"
start_softbus 1 4 "n1" || exit 1
timeout 12 tshark -i veth_m -f "ether proto 0x88a4" -w "$LOGDIR/cap_n1.pcap" >/dev/null 2>&1 &
TS_PID=$!
sleep 1
LOG=$(run_probe n1)
sleep 1
kill $TS_PID 2>/dev/null; wait $TS_PID 2>/dev/null
cleanup
cat "$LOG"

CNT=$(grep -oP '^L1:SLAVECOUNT=\K\d+' "$LOG" | head -1)
[ "${CNT:-0}" = "1" ] && ok "L1-01  N=1 -> counted exactly 1 slave" \
                      || bad "L1-01  counted '${CNT:-?}', expected 1"

MAN=$(grep -oP '^L1:SLAVE=1 .*MAN=\K0x[0-9a-f]+' "$LOG" | head -1)
PIDC=$(grep -oP '^L1:SLAVE=1 .*ID=\K0x[0-9a-f]+' "$LOG" | head -1)
[ "${MAN:-}" = "0x00000499" ] && ok "L1-02  Vendor ID = $MAN (matches esc_sii.h)" \
                              || bad "L1-02  Vendor ID = '${MAN:-?}', expected 0x00000499"
[ "${PIDC:-}" = "0x00000001" ] && ok "L1-02  Product Code = $PIDC (matches esc_sii.h)" \
                               || bad "L1-02  Product Code = '${PIDC:-?}', expected 0x00000001"

ADDR=$(grep -oP '^L1:SLAVE=1 ADDR=\K0x[0-9a-f]+' "$LOG" | head -1)
[ "${ADDR:-}" = "0x1001" ] && ok "L1-03  Station address = $ADDR" \
                           || bad "L1-03  Station address = '${ADDR:-?}', expected 0x1001"

# Corroborating evidence: APWR writing to Ado 0x10 (REG_STATION_ADDR).
# tshark text format actually observed: 'APWR': Len: 2, Adp 0x0, Ado 0x10, Wc 1
if [ -f "$LOGDIR/cap_n1.pcap" ]; then
    tshark -r "$LOGDIR/cap_n1.pcap" -Y ecat > "$LOGDIR/cap_n1.txt" 2>/dev/null
    if grep "'APWR'" "$LOGDIR/cap_n1.txt" 2>/dev/null | grep -q "Ado 0x10,"; then
        ok "L1-03  Captured APWR -> Ado 0x10 on the wire"
    else
        bad "L1-03  No APWR -> Ado 0x10 found (see $LOGDIR/cap_n1.txt)"
    fi
else
    note "L1-03  No pcap available to cross-check"
fi

# ======================= CASE B: N=8 — L1-04/05 =======================
echo ""
echo "############ CASE B: N=8  (L1-04, L1-05) ############"
start_softbus 8 4 "n8" || exit 1
LOG=$(run_probe n8)
cleanup
cat "$LOG"

CNT=$(grep -oP '^L1:SLAVECOUNT=\K\d+' "$LOG" | head -1)
[ "${CNT:-0}" = "8" ] && ok "L1-04  N=8 -> counted exactly 8 slaves" \
                      || bad "L1-04  counted '${CNT:-?}', expected 8"

ADDRS=$(grep -oP '^L1:SLAVE=\d+ ADDR=\K0x[0-9a-f]+' "$LOG" | tr '\n' ' ')
EXPECT="0x1001 0x1002 0x1003 0x1004 0x1005 0x1006 0x1007 0x1008 "
[ "$ADDRS" = "$EXPECT" ] && ok "L1-04  Addresses increment 0x1001..0x1008" \
                         || bad "L1-04  Addresses = [$ADDRS]"

python3 - "$LOG" << 'PYEOF'
import re, sys
log = open(sys.argv[1]).read()
rows = re.findall(r'^L1:IOMAP=(\d+) OUT_OFF=(-?\d+) OUT_BITS=(\d+) IN_OFF=(-?\d+) IN_BITS=(\d+)',
                  log, re.M)
if not rows:
    print("  [FAIL] L1-05  Could not read any L1:IOMAP line"); sys.exit(1)
spans = []
for sl, oo, ob, io_, ib in rows:
    oo, ob, io_, ib = int(oo), int(ob), int(io_), int(ib)
    if oo >= 0 and ob > 0: spans.append((oo, oo + (ob+7)//8, f"slave{sl}.out"))
    if io_ >= 0 and ib > 0: spans.append((io_, io_ + (ib+7)//8, f"slave{sl}.in"))
spans.sort()
ovl = [f"{spans[i][2]}[{spans[i][0]}:{spans[i][1]}) overlaps {spans[i+1][2]}[{spans[i+1][0]}:{spans[i+1][1]})"
       for i in range(len(spans)-1) if spans[i][1] > spans[i+1][0]]
if not spans:
    print("  [FAIL] L1-05  No IO regions at all (Obits/Ibits all 0?)"); sys.exit(1)
if ovl:
    print("  [FAIL] L1-05  IOmap OVERLAPS:")
    for o in ovl: print("           " + o)
    sys.exit(1)
print(f"  [PASS] L1-05  {len(spans)} IOmap region(s), no overlap")
for s, e, n in spans: print(f"           {n:>14}  [{s:4d} : {e:4d})")
PYEOF
if [ $? -eq 0 ]; then PASS=$((PASS+1)); RESULTS+=("PASS|L1-05  IOmap has no overlap")
else FAIL=$((FAIL+1)); RESULTS+=("FAIL|L1-05  IOmap overlap or unreadable"); fi

# ======================= CASE C: N=32 — L1-06 part 1 =======================
echo ""
echo "############ CASE C: N=32, pdo=4  (L1-06 part 1) ############"
start_softbus 32 4 "n32" || exit 1
LOG=$(run_probe n32)
cleanup
grep -E '^L1:(SLAVECOUNT|GROUP|SEGMENT)' "$LOG"

CNT=$(grep -oP '^L1:SLAVECOUNT=\K\d+' "$LOG" | head -1)
[ "${CNT:-0}" = "32" ] && ok "L1-06  N=32 -> counted exactly 32 slaves" \
                       || bad "L1-06  counted '${CNT:-?}', expected 32"

# ============ CASE D: N=32 large pdo — L1-06 part 2 (force segmentation) ============
echo ""
echo "############ CASE D: N=32, pdo=64  (L1-06 part 2: forced frame split) ############"
start_softbus 32 64 "n32big" || exit 1
timeout 12 tshark -i veth_m -f "ether proto 0x88a4" -w "$LOGDIR/cap_n32big.pcap" >/dev/null 2>&1 &
TS_PID=$!
sleep 1
LOG=$(run_probe n32big)
sleep 1
kill $TS_PID 2>/dev/null; wait $TS_PID 2>/dev/null
cleanup
grep -E '^L1:(SLAVECOUNT|GROUP|SEGMENT|PROCESSDATA)' "$LOG"

NSEG=$(grep -oP '^L1:GROUP .*NSEGMENTS=\K\d+' "$LOG" | head -1)
OB=$(grep -oP '^L1:GROUP OBYTES=\K\d+' "$LOG" | head -1)
IB=$(grep -oP '^L1:GROUP .*IBYTES=\K\d+' "$LOG" | head -1)
TOT=$(( ${OB:-0} + ${IB:-0} ))
echo "  IOmap: ${OB:-?} out + ${IB:-?} in = $TOT byte"

if [ "${NSEG:-0}" -gt 1 ]; then
    ok "L1-06  IOmap $TOT byte > 1486 -> SOEM split into $NSEG segment(s)"
elif [ "$TOT" -le 1486 ]; then
    note "IOmap is only $TOT byte, under 1486, so SOEM had no need to split."
    note "Bump --pdo-size in CASE D and re-run to force a split."
    bad "L1-06  Did not create a frame-splitting situation"
else
    bad "L1-06  IOmap $TOT byte > 1486 but NSEGMENTS=${NSEG:-?}"
fi

if [ -f "$LOGDIR/cap_n32big.pcap" ]; then
    tshark -r "$LOGDIR/cap_n32big.pcap" -Y ecat > "$LOGDIR/cap_n32big.txt" 2>/dev/null
    NL=$(grep -cE "'(LRD|LWR|LRW)'" "$LOGDIR/cap_n32big.txt" 2>/dev/null || true)
    note "In pcap: ${NL:-0} logical datagram(s). See $LOGDIR/cap_n32big.txt"
fi

# ======================= Summary =======================
echo ""
echo "========================================================="
printf " L1 RESULT: %d pass, %d fail\n" "$PASS" "$FAIL"
echo " Full logs: $LOGDIR"
echo "========================================================="
{
    echo "# L1 results — $(date '+%F %T')"
    echo ""
    for r in "${RESULTS[@]}"; do echo "- [${r%%|*}] ${r#*|}"; done
    echo ""
    echo "Total: $PASS pass, $FAIL fail"
} > "$LOGDIR/RESULTS.md"
echo "Summary: $LOGDIR/RESULTS.md"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
