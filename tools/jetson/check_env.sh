#!/usr/bin/env bash
# ==========================================================================
# check_env.sh — Phase 9.0: read-only check of the Jetson before a
# measurement session (soak, rt_tail, golden, L-series, R-01/R-02, D-01).
# It changes nothing; every WARN/FAIL line says what to run.
#
#   tools/jetson/check_env.sh                       RT host + i226 + leftovers
#   tools/jetson/check_env.sh --iface enP1p1s0 --etf   also the ETF prerequisites
#   tools/jetson/check_env.sh --soak                leftovers are FAIL, not WARN
#   tools/jetson/check_env.sh --no-iface            host checks only (veth rigs)
#
# Collects the traps found so far: isolcpus lost (Phase 8.5 measured the
# wake tail "without isolcpus"), NetworkManager resetting the qdisc, ETF
# left on the i226 before L1/golden/soak, rebuilt binaries without
# capabilities, IgH still loaded (its idle thread scans the bus), stale
# /tmp files of another user (fs.protected_regular), EEE on a 100 Mbit/s
# EtherCAT link.
# Exit: 0 all PASS/INFO, 1 at least one WARN, 2 at least one FAIL.
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IFACE=enP1p1s0; ETF=0; SOAK=0; NO_IFACE=0; RT_CPU=3
while [ $# -gt 0 ]; do
    case $1 in
        --iface) IFACE=$2; shift ;;
        --etf) ETF=1 ;;
        --soak) SOAK=1 ;;
        --no-iface) NO_IFACE=1 ;;
        --rt-cpu) RT_CPU=$2; shift ;;
        *) echo "unknown option $1"; exit 3 ;;
    esac
    shift
done
NP=0; NW=0; NF=0
pass () { echo "  [PASS] $*"; NP=$((NP+1)); }
info () { echo "  [INFO] $*"; }
warn () { echo "  [WARN] $*"; NW=$((NW+1)); }
fail () { echo "  [FAIL] $*"; NF=$((NF+1)); }
leftover () { if [ $SOAK = 1 ]; then fail "$@"; else warn "$@"; fi; }
have () { command -v "$1" >/dev/null 2>&1; }

echo "=== host"
kr=$(uname -r)
if [ "$(cat /sys/kernel/realtime 2>/dev/null)" = 1 ] || echo "$kr" | grep -q -- '-rt'; then
    pass "PREEMPT_RT kernel ($kr)"
else
    fail "kernel $kr is not PREEMPT_RT"
fi
cmd=$(cat /proc/cmdline)
iso=$(cat /sys/devices/system/cpu/isolated 2>/dev/null)
if echo ",$iso," | grep -q ",$RT_CPU," || [ "$iso" = "$RT_CPU" ]; then
    pass "core $RT_CPU isolated (isolated=$iso)"
else
    fail "core $RT_CPU NOT isolated (isolated='${iso}'): add isolcpus=$RT_CPU to APPEND in /boot/extlinux/extlinux.conf and reboot; a JetPack/kernel update may rewrite that file"
fi
for p in irqaffinity transparent_hugepage; do
    if echo "$cmd" | grep -q "$p="; then pass "cmdline has $(echo "$cmd" | grep -o "$p=[^ ]*")"
    else warn "cmdline has no $p= (master_plan_v2 §3)"; fi
done
if have systemctl && systemctl is-active --quiet irqbalance 2>/dev/null; then
    warn "irqbalance is running: sudo systemctl disable --now irqbalance"
else
    pass "irqbalance not running"
fi
rt=$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null)
info "kernel.sched_rt_runtime_us=$rt (950000 = throttle on, the development default)"
if have nvpmodel; then
    m=$(nvpmodel -q 2>/dev/null | grep -i "power mode" | head -1)
    info "nvpmodel: ${m:-unknown} (measurements so far: MAXN_SUPER + jetson_clocks)"
fi
gov=$(cat /sys/devices/system/cpu/cpu$RT_CPU/cpufreq/scaling_governor 2>/dev/null)
[ -n "$gov" ] && { if [ "$gov" = performance ]; then pass "cpu$RT_CPU governor performance"
                   else warn "cpu$RT_CPU governor '$gov': sudo jetson_clocks (or nvpmodel -m 0 + jetson_clocks)"; fi; }

echo "=== leftovers (must be gone before a soak / golden / R-series run)"
for pn in soft_bus ecm_run tshark txtime_probe cyclictest l4_test l6_test; do
    p=$(pgrep -x "$pn" | tr '\n' ' ')
    if [ -n "$p" ]; then leftover "$pn running (pid $p)"; else pass "no $pn running"; fi
done
if lsmod 2>/dev/null | grep -q '^ec_'; then
    leftover "IgH modules loaded ($(lsmod | awk '/^ec_/{print $1}' | tr '\n' ' ')): its idle thread scans the bus -- sudo /opt/etherlab/sbin/ethercatctl stop"
else
    pass "IgH not loaded"
fi
me=$(id -un)
stale=""
for f in /tmp/soft_bus*.ctl /tmp/ecm_diag*.txt /tmp/golden_now* /tmp/ecm_run_golden.log; do
    [ -e "$f" ] || continue
    o=$(stat -c %U "$f" 2>/dev/null)
    [ "$o" != "$me" ] && stale="$stale $f($o)"
done
if [ -n "$stale" ]; then
    warn "files in /tmp owned by another user (fs.protected_regular makes scripts fail on them):$stale -- sudo rm -f them"
else
    pass "no foreign-owned /tmp files of the test scripts"
fi

echo "=== binaries (capabilities are lost on every rebuild)"
for b in tools/soft_bus/soft_bus apps/ecm_run/ecm_run apps/ecm_diag/ecm_diag apps/l4_test/l4_test; do
    [ -x "$ROOT/$b" ] || { info "$b not built"; continue; }
    if have getcap && [ -n "$(getcap "$ROOT/$b" 2>/dev/null)" ]; then pass "$b has capabilities"
    else warn "$b has NO capabilities: make caps"; fi
done

if [ $NO_IFACE = 0 ]; then
    echo "=== EtherCAT interface $IFACE"
    if [ ! -e "/sys/class/net/$IFACE" ]; then
        fail "$IFACE does not exist (ip -br link)"
    else
        drv=$(basename "$(readlink -f /sys/class/net/$IFACE/device/driver 2>/dev/null)" 2>/dev/null)
        info "driver ${drv:-?}"
        op=$(cat /sys/class/net/$IFACE/operstate)
        if [ "$op" = up ]; then pass "link up"; else warn "link $op (cable? after 'tc qdisc del' the i226 sometimes needs a replug)"; fi
        if have ethtool; then
            sp=$(ethtool "$IFACE" 2>/dev/null | awk -F': ' '/Speed:/{print $2}')
            dx=$(ethtool "$IFACE" 2>/dev/null | awk -F': ' '/Duplex:/{print $2}')
            info "speed ${sp:-?} duplex ${dx:-?} (LAN9252 and most servos: 100Mb/s Full; use --etf-ns-per-byte 80 there)"
            eee=$(ethtool --show-eee "$IFACE" 2>/dev/null | awk -F': ' '/EEE status:/{print $2}')
            case "$eee" in
                *enabled*|*active*) warn "EEE $eee: sudo ethtool --set-eee $IFACE eee off (low-power idle adds latency on an EtherCAT link)" ;;
                "") info "EEE status not reported" ;;
                *) pass "EEE $eee" ;;
            esac
            gro=$(ethtool -k "$IFACE" 2>/dev/null | awk -F': ' '/^generic-receive-offload:/{print $2}')
            case "$gro" in off*) pass "GRO off" ;; "") ;; *) warn "GRO $gro: sudo ethtool -K $IFACE gro off gso off tso off" ;; esac
            rxu=$(ethtool -c "$IFACE" 2>/dev/null | awk -F': ' '/^rx-usecs:/{print $2}')
            case "$rxu" in 0) pass "rx-usecs 0" ;; "") ;; *) warn "rx-usecs $rxu: sudo ethtool -C $IFACE rx-usecs 0 tx-usecs 0" ;; esac
        else
            warn "ethtool not installed"
        fi
        if have nmcli; then
            st=$(nmcli -g GENERAL.STATE device show "$IFACE" 2>/dev/null)
            case "$st" in
                *unmanaged*) pass "NetworkManager: unmanaged" ;;
                "") info "NetworkManager does not know $IFACE" ;;
                *) fail "NetworkManager manages $IFACE ($st): it resets the qdisc -- sudo nmcli device set $IFACE managed no" ;;
            esac
        fi
        if have tc; then
            q=$(tc qdisc show dev "$IFACE" 2>/dev/null)
            if echo "$q" | grep -q "qdisc etf"; then
                if [ $ETF = 1 ]; then
                    if echo "$q" | grep -q "offload on"; then pass "ETF with offload present"
                    else fail "ETF present WITHOUT offload: tc qdisc del dev $IFACE root, then add it again with offload (replace cannot change offload)"; fi
                else
                    fail "ETF qdisc still on $IFACE: before L1/golden/soak run  sudo tc qdisc del dev $IFACE root"
                fi
            else
                if [ $ETF = 1 ]; then fail "no ETF qdisc on $IFACE (docs/i226.md, ETF section)"
                else pass "no ETF qdisc"; fi
            fi
        fi
    fi
fi

if [ $ETF = 1 ]; then
    echo "=== ETF prerequisites (docs/i226.md)"
    if pgrep -x phc2sys >/dev/null; then pass "phc2sys running: $(tr '\0' ' ' < /proc/$(pgrep -x phc2sys | head -1)/cmdline)"
    else fail "phc2sys not running: sudo phc2sys -s CLOCK_REALTIME -c $IFACE -O 0 -m --step_threshold=1"; fi
    if have systemctl && systemctl is-active --quiet systemd-timesyncd 2>/dev/null; then
        warn "systemd-timesyncd active: an NTP step moves TAI during the run -- sudo systemctl stop systemd-timesyncd"
    else
        pass "systemd-timesyncd stopped"
    fi
fi

echo "RESULT: $NP pass, $NW warn, $NF fail"
[ $NF -gt 0 ] && exit 2
[ $NW -gt 0 ] && exit 1
exit 0
