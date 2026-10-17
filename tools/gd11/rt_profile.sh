#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# rt_profile.sh -- Phase 11.5 IS-01: which RT boot profile is this Jetson in,
# and is everything a measurement depends on in place?
# (claude/giai_doan_11_ke_hoach.md §6.2, docs/rt_profiles.md)
#
#   tools/gd11/rt_profile.sh                     print the profile (P0 / P1 / ?)
#   tools/gd11/rt_profile.sh --expect P1         exit 2 unless this is P1
#   sudo tools/gd11/rt_profile.sh --expect P0 --iface enP1p1s0
#                                                also the i226 offloads/coalescing
#
# Profiles (cmdline only; whether ecm_run holds /dev/cpu_dma_latency is a
# run option, P0n / P1n = --no-dma-latency, not a boot profile):
#   P0  isolcpus=3 irqaffinity=0-2 transparent_hugepage=never, cpuidle on
#   P1  P0 + cpuidle.off=1 (cpuidle current_driver = none)
#
# Checked: kernel PREEMPT_RT, isolated = RT core, cpuidle state matches the
# profile, cpufreq governor performance on the RT core (jetson_clocks),
# nvpmodel (info), and with --iface: GRO/GSO/TSO off, rx-usecs 0.
# SYSROOT (default /) and CMDLINE (default /proc/cmdline) are for the
# offline test (fake sysfs tree).
#
# Output: one "PROFILE=Px" line, one line per check, "RESULT: ...".
# Exit: 0 = profile as expected and every check passes, 1 = only warnings,
#       2 = wrong profile or a failed check, 3 = usage.
# ==========================================================================
set -u
SYSROOT=${SYSROOT:-}
CMDLINE=${CMDLINE:-$SYSROOT/proc/cmdline}
RT_CPU=3; EXPECT=""; IFACE=""
while [ $# -gt 0 ]; do
    case $1 in
        --expect) EXPECT=$2; shift ;;
        --iface)  IFACE=$2; shift ;;
        --rt-cpu) RT_CPU=$2; shift ;;
        *) echo "usage: $0 [--expect P0|P1] [--iface IF] [--rt-cpu N]"; exit 3 ;;
    esac
    shift
done
case "$EXPECT" in ""|P0|P1) ;; *) echo "--expect P0 or P1"; exit 3 ;; esac
NP=0; NW=0; NF=0
pass () { echo "  [PASS] $*"; NP=$((NP+1)); }
warn () { echo "  [WARN] $*"; NW=$((NW+1)); }
fail () { echo "  [FAIL] $*"; NF=$((NF+1)); }
info () { echo "  [INFO] $*"; }
S=$SYSROOT/sys/devices/system/cpu

cmd=$(cat "$CMDLINE" 2>/dev/null)
drv=$(cat "$S/cpuidle/current_driver" 2>/dev/null || echo none)
off=0
case " $cmd " in *" cpuidle.off=1 "*) off=1 ;; esac
[ "$drv" = none ] && off=1
if [ $off = 1 ]; then PROF=P1; else PROF=P0; fi
echo "PROFILE=$PROF"

kr=${KERNEL:-$(uname -r)}
if echo "$kr" | grep -q -- '-rt' || [ "$(cat "$SYSROOT/sys/kernel/realtime" 2>/dev/null)" = 1 ]; then
    pass "PREEMPT_RT kernel ($kr)"
else
    fail "kernel $kr is not PREEMPT_RT"
fi
iso=$(cat "$S/isolated" 2>/dev/null)
if [ "$iso" = "$RT_CPU" ] || echo ",$iso," | grep -q ",$RT_CPU,"; then pass "core $RT_CPU isolated (isolated=$iso)"
else fail "core $RT_CPU not isolated (isolated='$iso')"; fi
for p in isolcpus irqaffinity transparent_hugepage; do
    v=$(echo " $cmd " | grep -o " $p=[^ ]*" | head -1 | tr -d ' ')
    if [ -n "$v" ]; then info "cmdline $v"; else warn "cmdline has no $p="; fi
done

# cpuidle, as the profile says
if [ $PROF = P1 ]; then
    if [ "$drv" = none ]; then pass "cpuidle off (current_driver none)"
    else fail "cmdline has cpuidle.off=1 but current_driver is '$drv'"; fi
else
    deep=""
    for st in "$S/cpu$RT_CPU"/cpuidle/state*; do
        [ -r "$st/name" ] || continue
        [ "$(cat "$st/disable" 2>/dev/null)" = 0 ] && deep="$deep $(cat "$st/name")"
    done
    info "cpuidle driver $drv, states enabled on cpu$RT_CPU:${deep:- none} (ecm_run holds cpu_dma_latency=0 unless --no-dma-latency)"
fi
for p in nohz_full rcu_nocbs idle processor.max_cstate; do
    echo " $cmd " | grep -q " $p=" && info "cmdline has $p= (no effect here: see docs/rt_profiles.md)"
done

gov=$(cat "$S/cpu$RT_CPU/cpufreq/scaling_governor" 2>/dev/null)
case "$gov" in
    performance) pass "cpu$RT_CPU governor performance" ;;
    "") info "cpu$RT_CPU has no cpufreq governor (cpufreq.off?)" ;;
    *) fail "cpu$RT_CPU governor '$gov': sudo jetson_clocks" ;;
esac
if [ -z "$SYSROOT" ] && command -v nvpmodel >/dev/null 2>&1; then
    m=$(nvpmodel -q 2>/dev/null | grep -i "power mode" | head -1)
    case "$m" in *MAXN*) pass "nvpmodel: $m" ;; *) warn "nvpmodel: ${m:-unknown} (measurements so far: MAXN_SUPER)" ;; esac
fi

if [ -n "$IFACE" ]; then
    if [ ! -e "/sys/class/net/$IFACE" ]; then fail "$IFACE does not exist"
    else
        k=$(ethtool -k "$IFACE" 2>/dev/null)
        for f in generic-receive-offload generic-segmentation-offload tcp-segmentation-offload; do
            v=$(echo "$k" | awk -F': ' -v f="$f" '$1==f{print $2}')
            case "$v" in off*) pass "$IFACE $f off" ;; "") warn "$IFACE $f not readable" ;;
                *) fail "$IFACE $f $v: sudo ethtool -K $IFACE gro off gso off tso off" ;; esac
        done
        rxu=$(ethtool -c "$IFACE" 2>/dev/null | awk -F': ' '/^rx-usecs:/{print $2}')
        case "$rxu" in 0) pass "$IFACE rx-usecs 0" ;; "") warn "$IFACE coalescing not readable (sudo)" ;;
            *) fail "$IFACE rx-usecs $rxu: sudo ethtool -C $IFACE rx-usecs 0" ;; esac
    fi
fi

if [ -n "$EXPECT" ]; then
    if [ "$EXPECT" = "$PROF" ]; then pass "profile $PROF as expected"
    else fail "profile is $PROF, expected $EXPECT (boot entry? docs/rt_profiles.md §2)"; fi
fi
echo "RESULT: $NP pass, $NW warn, $NF fail"
[ $NF -gt 0 ] && exit 2
[ $NW -gt 0 ] && exit 1
exit 0
