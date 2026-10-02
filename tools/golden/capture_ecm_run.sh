#!/usr/bin/env bash
# ==========================================================================
# capture_ecm_run.sh — Phase 7.5: run the fixed golden scenario and
# write its structure (tools/golden/pcap2struct.py) to stdout.
#
#   sudo ./capture_ecm_run.sh [out.pcap] > structure.txt
#
# Scenario (fixed): soft_bus 8 nodes, pdo 4 bytes, DC 32-bit, SM watchdog
# reaction off (timing on a CI runner is not RT); ecm_run 4 motion + 4 IO,
# enumerate -> PREOP -> SM watchdog -> DC -> SAFEOP -> OP -> 2 s cyclic
# (with 1 s diagnostics) -> INIT.
# Env: SOFT_BUS ECM_RUN IF_M IF_S ECM_RUN_BIN_NOTE
#      ENI=config/eni/X.enicfg  run the same scenario in ENI mode (Phase 8.4,
#      E-05): ecm_run --eni, identity/layout checks, CoE InitCmds, ENI DC
#      N=1 GROUP_ARGS=           Phase 9.1 (G-08): one-slave bus, every slave in
#                                GROUP_MOTION, GROUP_IO empty
#                                (defaults: N=8 GROUP_ARGS="--motion-slaves 4")
#      SB_ARGS="--coe-ca"         Phase 9.3 (C-02/C-05): extra soft_bus options
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SOFT_BUS=${SOFT_BUS:-$HERE/../soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$HERE/../../apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
PCAP=${1:-$(mktemp /tmp/golden_XXXXXX.pcap)}
ENI=${ENI:-}
N=${N:-8}
GROUP_ARGS=${GROUP_ARGS---motion-slaves 4}
# shellcheck disable=SC2206
GROUP_ARR=($GROUP_ARGS)
SB_ARGS=${SB_ARGS:-}
# shellcheck disable=SC2206
SB_ARR=($SB_ARGS)
ENI_ARGS=()
if [ -n "$ENI" ]; then
    [ -f "$ENI" ] || { echo "ENI file $ENI not found" >&2; exit 2; }
    ENI_ARGS=(--eni "$ENI")
fi
command -v tshark >/dev/null || { echo "tshark not installed" >&2; exit 2; }
rm -f "$PCAP"

"$SOFT_BUS" --iface "$IF_S" --n "$N" --pdo-size 4 --dc 32 --dc-report-s 100 --no-sm-wd ${SB_ARR[@]+"${SB_ARR[@]}"} >/dev/null 2>&1 &
SBP=$!
tshark -q -i "$IF_M" -F pcap -w "$PCAP" -f "ether proto 0x88a4" >/dev/null 2>&1 &
TSP=$!
# tshark needs a moment before it really captures
for _ in $(seq 50); do [ -s "$PCAP" ] && break; sleep 0.1; done
sleep 1
"$ECM_RUN" --iface "$IF_M" --n "$N" ${GROUP_ARR[@]+"${GROUP_ARR[@]}"} --duration-sec 2 --no-tx-ts \
    --diag-file /tmp/ecm_diag_golden.txt "${ENI_ARGS[@]}" >/tmp/ecm_run_golden.log 2>&1
RC=$?
sleep 0.5
kill -INT $TSP; wait $TSP 2>/dev/null
kill -INT $SBP; wait $SBP 2>/dev/null
[ $RC = 0 ] || { echo "ecm_run failed (rc=$RC), see /tmp/ecm_run_golden.log" >&2; exit 3; }
python3 "$HERE/pcap2struct.py" "$PCAP"
