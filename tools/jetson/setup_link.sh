#!/usr/bin/env bash
# ==========================================================================
# setup_link.sh — Phase 10.0: put the i226 EtherCAT port in the state every
# measurement assumes. Run once after each boot (none of it survives a
# reboot; NetworkManager leaves the port alone, so nobody brings it up):
#
#   sudo tools/jetson/setup_link.sh [IFACE]        default enP1p1s0
#
# link up; GRO/GSO/TSO off; interrupt coalescing 0 (igc queue-pair mode:
# rx-usecs covers tx too); EEE off. Then prints the state. Deep CPU idle is
# not touched here: ecm_run holds /dev/cpu_dma_latency = 0 while it runs.
# ==========================================================================
set -u
IFACE=${1:-enP1p1s0}
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 2; }
ip link show "$IFACE" >/dev/null 2>&1 || { echo "no interface $IFACE"; exit 2; }
ip link set "$IFACE" up
ethtool -K "$IFACE" gro off gso off tso off 2>/dev/null
ethtool -C "$IFACE" rx-usecs 0 2>/dev/null || echo "WARNING: could not set rx-usecs 0"
ethtool --set-eee "$IFACE" eee off 2>/dev/null
for i in $(seq 1 50); do ethtool "$IFACE" 2>/dev/null | grep -q 'Link detected: yes' && break; sleep 0.1; done
ethtool "$IFACE" | grep -E 'Speed|Duplex|Link detected'
ethtool -c "$IFACE" | grep '^rx-usecs'
ethtool -k "$IFACE" | grep '^generic-receive-offload'
ethtool --show-eee "$IFACE" 2>/dev/null | grep -i 'EEE status'
