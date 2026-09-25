#!/usr/bin/env bash
# ==========================================================================
# capture_ecm_run.sh — Giai doan 7.5: run the fixed golden scenario and
# write its structure (tools/golden/pcap2struct.py) to stdout.
#
#   sudo ./capture_ecm_run.sh [out.pcap] > structure.txt
#
# Scenario (fixed): soft_bus 8 nodes, pdo 4 bytes, DC 32-bit, SM watchdog
# reaction off (timing on a CI runner is not RT); ecm_run 4 motion + 4 IO,
# enumerate -> PREOP -> SM watchdog -> DC -> SAFEOP -> OP -> 2 s cyclic
# (with 1 s diagnostics) -> INIT.
# Env: SOFT_BUS ECM_RUN IF_M IF_S ECM_RUN_BIN_NOTE
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SOFT_BUS=${SOFT_BUS:-$HERE/../soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$HERE/../../apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
PCAP=${1:-$(mktemp /tmp/golden_XXXXXX.pcap)}
command -v tshark >/dev/null || { echo "tshark not installed" >&2; exit 2; }
rm -f "$PCAP"

"$SOFT_BUS" --iface "$IF_S" --n 8 --pdo-size 4 --dc 32 --dc-report-s 100 --no-sm-wd >/dev/null 2>&1 &
SBP=$!
tshark -q -i "$IF_M" -F pcap -w "$PCAP" -f "ether proto 0x88a4" >/dev/null 2>&1 &
TSP=$!
# tshark needs a moment before it really captures
for _ in $(seq 50); do [ -s "$PCAP" ] && break; sleep 0.1; done
sleep 1
"$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 --duration-sec 2 --no-tx-ts \
    --diag-file /tmp/ecm_diag_golden.txt >/tmp/ecm_run_golden.log 2>&1
RC=$?
sleep 0.5
kill -INT $TSP; wait $TSP 2>/dev/null
kill -INT $SBP; wait $SBP 2>/dev/null
[ $RC = 0 ] || { echo "ecm_run failed (rc=$RC), see /tmp/ecm_run_golden.log" >&2; exit 3; }
python3 "$HERE/pcap2struct.py" "$PCAP"
