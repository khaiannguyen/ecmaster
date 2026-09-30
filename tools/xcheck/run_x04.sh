#!/usr/bin/env bash
# ==========================================================================
# run_x04.sh — Giai doan 9.3 X-04: IgH (an independent master) drives the
# GD9.3 CoE features of soft_bus before SOEM is trusted with them.
#
# Rig (GD8 8.1): veth_ig (MAC 02:00:00:00:00:10, IgH generic driver) <->
# veth_igs (soft_bus); /opt/etherlab/etc/ethercat.conf MASTER0_DEVICE set to
# that MAC; `sudo /opt/etherlab/sbin/ethercatctl start` BEFORE this script,
# `ethercatctl stop` + `lsmod | grep ^ec_` empty AFTER it (IgH's idle thread
# floods the bus otherwise).
#
#   sudo -E ./run_x04.sh
#
#   x04a  soft_bus --coe-ca, igh_x01b --complete-sdo --sdo8002 250: IgH writes
#         0x1C12/0x1C13 with Complete Access and 250 byte into 0x8002
#         (segmented download) in PREOP -> all 8 slaves OP, WC COMPLETE
#   x04b  while x04a runs: `ethercat upload` 0x1C12:00/01, 0x1C13:01 match
#         what was written; 0x8002 reads back the 250 byte (segmented
#         upload); `ethercat download` of a 16 byte string into 0x8002 of
#         slave 3 (normal download) reads back identical, and a 200 byte one
#         into slave 5 (segmented)
#   x04n  negative: default soft_bus (no --coe-ca), igh_x01b --complete-sdo
#         -> IgH fails the configuration (abort 0x06010000), slaves stay
#         below OP, igh_x01b returns non-zero
#
# Env: SOFT_BUS IGH_APP ETHERCAT IF_S N SECONDS SB_PRIO SB_CPU
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
IGH_APP=${IGH_APP:-$HERE/igh_x01b}
ETHERCAT=${ETHERCAT:-/opt/etherlab/bin/ethercat}
IF_S=${IF_S:-veth_igs}
N=${N:-8}
SECONDS_RUN=${SECONDS:-25}
SB_PRIO=${SB_PRIO:-79}; SB_CPU=${SB_CPU:-2}
LOG=${LOG:-log_x04_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

for f in "$SOFT_BUS" "$IGH_APP" "$ETHERCAT"; do
    [ -x "$f" ] || { echo "missing $f (igh_x01b: see the build line in igh_x01b.c)"; exit 2; }
done
lsmod | grep -q '^ec_master' || { echo "IgH not started: sudo /opt/etherlab/sbin/ethercatctl start"; exit 2; }

sb_start () {  # name, args...
    local name=$1; shift
    chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS" --iface "$IF_S" --n "$N" --dc 32 --ctl none "$@" \
        > "$LOG/sb_$name.log" 2>&1 &
    SBP=$!
    sleep 1
}
sb_stop () { kill -INT "$SBP" 2>/dev/null; wait "$SBP" 2>/dev/null; }
up () { "$ETHERCAT" upload -p "$1" "${@:2}" 2>&1; }

# Expected 0x8002 content written by igh_x01b --sdo8002 L: 'A' + k % 26.
blob () { python3 -c "import sys; print(''.join(chr(65 + k % 26) for k in range($1)))"; }

echo "=== X-04a/b: soft_bus --coe-ca, IgH complete SDO + segmented config SDO"
sb_start x04a --coe-ca
"$IGH_APP" --n "$N" --seconds "$SECONDS_RUN" --complete-sdo --sdo8002 250 > "$LOG/igh_x04a.log" 2>&1 &
IGP=$!
# wait for OP, then use the IgH command line tool while the app cycles
for _ in $(seq 100); do grep -q "all $N slaves in OP" "$LOG/igh_x04a.log" && break; sleep 0.2; done
{
    echo "--- slaves";                    "$ETHERCAT" slaves
    echo "--- 1C12:00";                   up 0 -t uint8  0x1C12 0
    echo "--- 1C12:01";                   up 0 -t uint16 0x1C12 1
    echo "--- 1C13:01 (slave $((N-1)))";  up $((N-1)) -t uint16 0x1C13 1
    echo "--- 1600:01";                   up 0 -t uint32 0x1600 1
    echo "--- 8002 slave 0";              up 0 -t string 0x8002 0
} > "$LOG/cli_x04b.txt"
S16="GD9.3-X04-normal"                     # 16 byte -> normal download
S200=$(python3 -c "print(''.join(chr(97 + k % 26) for k in range(200)))")
"$ETHERCAT" download -p 3 -t string 0x8002 0 "$S16"  > "$LOG/dl16.txt" 2>&1; DL16=$?
R16=$(up 3 -t string 0x8002 0)
"$ETHERCAT" download -p 5 -t string 0x8002 0 "$S200" > "$LOG/dl200.txt" 2>&1; DL200=$?
R200=$(up 5 -t string 0x8002 0)
wait $IGP; IGRC=$?
sb_stop
sed 's/^/    /' "$LOG/igh_x04a.log" | grep -E "PASS|FAIL|config SDOs|OP"
chk "X-04a igh_x01b rc 0 (all $N OP, WC COMPLETE, no overrun)" "[ $IGRC = 0 ]"
chk "X-04a all slaves reached OP" "grep -q '\[PASS\] X-01b-1' $LOG/igh_x04a.log"
chk "X-04b 0x1C12:00 = 1" "sed -n '/--- 1C12:00/{n;p}' $LOG/cli_x04b.txt | grep -qE '^0x01 1'"
chk "X-04b 0x1C12:01 = 0x1600" "sed -n '/--- 1C12:01/{n;p}' $LOG/cli_x04b.txt | grep -qi '^0x1600'"
chk "X-04b 0x1C13:01 = 0x1A00 (last slave)" "sed -n '/--- 1C13:01/{n;p}' $LOG/cli_x04b.txt | grep -qi '^0x1a00'"
chk "X-04b 0x1600:01 = 0x70000120" "sed -n '/--- 1600:01/{n;p}' $LOG/cli_x04b.txt | grep -qi '^0x70000120'"
chk "X-04b 0x8002 slave 0 = the 250 byte IgH configured (segmented both ways)" \
    "[ \"\$(sed -n '/--- 8002 slave 0/{n;p}' $LOG/cli_x04b.txt)\" = \"$(blob 250)\" ]"
chk "X-04b CLI download 16 byte (normal) + read back identical" "[ $DL16 = 0 ] && [ \"$R16\" = \"$S16\" ]"
chk "X-04b CLI download 200 byte (segmented) + read back identical" "[ $DL200 = 0 ] && [ \"$R200\" = \"$S200\" ]"

echo "=== X-04n (negative): default soft_bus, IgH complete SDO"
sb_start x04n
timeout 30 "$IGH_APP" --n "$N" --seconds 10 --complete-sdo > "$LOG/igh_x04n.log" 2>&1; NRC=$?
"$ETHERCAT" slaves > "$LOG/slaves_x04n.txt" 2>&1
sb_stop
dmesg | tail -200 | grep -iE "1c12|1c13|abort|complete" | tail -8 > "$LOG/dmesg_x04n.txt"
sed 's/^/    /' "$LOG/dmesg_x04n.txt"
chk "X-04n igh_x01b fails (rc != 0)" "[ $NRC != 0 ]"
chk "X-04n not all slaves in OP" "! grep -q 'all $N slaves in OP' $LOG/igh_x04n.log"
chk "X-04n IgH log shows the abort 0x06010000 (dmesg)" "grep -qi '06010000' $LOG/dmesg_x04n.txt"

echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
echo "Afterwards: sudo /opt/etherlab/sbin/ethercatctl stop; lsmod | grep ^ec_  (must be empty)"
[ "$FAIL" = 0 ]
