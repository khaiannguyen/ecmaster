#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# ecmaster-net.sh -- Phase 12.2: put an EtherCAT port in the state the
# measurements were made in, at every boot (ecmaster-net@IFACE.service).
#
#   ecmaster-net.sh up IFACE      save the current settings, then
#                                 ethtool -C rx-usecs 0, -K gro/gso/tso off,
#                                 EEE off, link up; wait for the carrier;
#                                 exit 1 if rx-usecs / GRO did not take
#   ecmaster-net.sh check IFACE   exit 0 if rx-usecs 0 and GRO off, else 1
#   ecmaster-net.sh down IFACE    restore what `up` saved (unit stop / disable)
#
# Why: rx-usecs and GRO are lost on reboot; IS-09 measured 31 936 NOFRAME
# with the driver default against 87 with rx-usecs 0 (docs/i226.md).
# The link speed is not forced: the EtherCAT slaves negotiate 100 Mbit/s.
# No cable is not an error of this unit (reported, exit 0).
#
# Env (tests): ETHTOOL IP ECM_STATE_DIR (default /run/ecmaster)
#              ECM_SYSNET (default /sys/class/net) ECM_LINK_WAIT (s, default 10)
# ==========================================================================
set -u
ETHTOOL=${ETHTOOL:-ethtool}
IP=${IP:-ip}
STATE_DIR=${ECM_STATE_DIR:-/run/ecmaster}
SYSNET=${ECM_SYSNET:-/sys/class/net}
LINK_WAIT=${ECM_LINK_WAIT:-10}

usage () { echo "usage: ecmaster-net.sh up|down|check IFACE" >&2; exit 2; }
[ $# = 2 ] || usage
CMD=$1; IF=$2
[[ "$IF" =~ ^[A-Za-z0-9_.:-]{1,15}$ ]] || { echo "ecmaster-net: bad interface name '$IF'" >&2; exit 2; }
[ -e "$SYSNET/$IF" ] || { echo "ecmaster-net: no interface $IF" >&2; exit 1; }
SAVED=$STATE_DIR/net-$IF.saved

# ethtool 6.x aligns values with tabs ("rx-usecs:\t\t0", seen on the Jetson): trim
rx_usecs () { $ETHTOOL -c "$IF" 2>/dev/null | awk -F':' '/^rx-usecs:/{v = $2; gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit}'; }
feature () { $ETHTOOL -k "$IF" 2>/dev/null | awk -F': *' -v k="$1" '$1 == k {split($2, a, " "); print a[1]; exit}'; }
eee () { $ETHTOOL --show-eee "$IF" 2>/dev/null | awk -F':' '/EEE status:/{v = $2; gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit}'; }   # line starts with a tab

check () {
    local r g c sp
    r=$(rx_usecs); g=$(feature generic-receive-offload)
    c=$(cat "$SYSNET/$IF/carrier" 2>/dev/null || echo 0)
    sp=$(cat "$SYSNET/$IF/speed" 2>/dev/null || echo ?)
    echo "ecmaster-net: $IF rx-usecs ${r:-?}, gro ${g:-?}, gso $(feature generic-segmentation-offload), tso $(feature tcp-segmentation-offload), EEE $(eee), carrier $c, speed $sp"
    [ "$c" = 1 ] || echo "ecmaster-net: $IF has no carrier (cable, slave power?) -- not an error of this unit"
    [ "$r" = 0 ] && [ "$g" = off ]
}

case $CMD in
up)
    mkdir -p "$STATE_DIR"
    if [ ! -s "$SAVED" ]; then      # keep the first state across restarts of the unit
        {
            echo "rx_usecs=$(rx_usecs)"
            echo "gro=$(feature generic-receive-offload)"
            echo "gso=$(feature generic-segmentation-offload)"
            echo "tso=$(feature tcp-segmentation-offload)"
            case "$(eee)" in *enabled*|*active*) echo "eee=on" ;; *) echo "eee=off" ;; esac
        } > "$SAVED"
    fi
    $ETHTOOL -C "$IF" rx-usecs 0 || echo "ecmaster-net: ethtool -C $IF rx-usecs 0 failed" >&2
    $ETHTOOL -K "$IF" gro off gso off tso off || echo "ecmaster-net: ethtool -K $IF gro/gso/tso off failed" >&2
    $ETHTOOL --set-eee "$IF" eee off 2>/dev/null || true
    $IP link set dev "$IF" up
    for _ in $(seq 1 $((LINK_WAIT * 10))); do
        [ "$(cat "$SYSNET/$IF/carrier" 2>/dev/null)" = 1 ] && break
        sleep 0.1
    done
    check || { echo "ecmaster-net: $IF NOT configured (rx-usecs / GRO did not take)" >&2; exit 1; }
    ;;
check)
    check
    ;;
down)
    [ -s "$SAVED" ] || { echo "ecmaster-net: nothing saved for $IF, left as it is"; exit 0; }
    rx_usecs=; gro=; gso=; tso=; eee=
    while IFS='=' read -r k v; do
        case $k in rx_usecs|gro|gso|tso|eee) [[ "$v" =~ ^[a-z0-9]*$ ]] && printf -v "$k" '%s' "$v" ;; esac
    done < "$SAVED"
    [ -n "$rx_usecs" ] && $ETHTOOL -C "$IF" rx-usecs "$rx_usecs"
    k=()
    for f in gro gso tso; do [ -n "${!f}" ] && k+=("$f" "${!f}"); done
    [ ${#k[@]} -gt 0 ] && $ETHTOOL -K "$IF" "${k[@]}"
    [ "$eee" = on ] && $ETHTOOL --set-eee "$IF" eee on 2>/dev/null
    rm -f "$SAVED"
    echo "ecmaster-net: $IF restored (rx-usecs ${rx_usecs:-?}, gro ${gro:-?}, gso ${gso:-?}, tso ${tso:-?}, EEE ${eee:-?})"
    ;;
*) usage ;;
esac
