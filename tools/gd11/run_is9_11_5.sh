#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_is9_11_5.sh -- Phase 11.5 IS-09: the i226 receive path, configuration
# only (claude/giai_doan_11_ke_hoach.md §6.3; docs/rt_profiles.md §10).
#
# What 12/10 showed on the Jetson (/proc/interrupts with the link up):
#   - every i226 MSI (IRQ 193 + TxRx-0..3 = 194..197) is counted on CPU0:
#     the Tegra PCIe controller delivers its MSIs on one GIC line, so the
#     HARD interrupt cannot leave CPU0 (hence P1 helps: CPU0 never idles);
#   - under PREEMPT_RT each vector has a thread irq/NNN-enP1p1s0-TxRx-Q,
#     SCHED_FIFO 50, which does the NAPI poll + NET_RX (the real work); they
#     ran on CPU1, 0, 2, 4. CPU0 also carries ~15 other FIFO 50 irq threads
#     (memory controller, hsp, ...): the EtherCAT reply can queue behind them.
# IS-07 at 250 us: 144 NOFRAME / 10 min under P0, 64 under P1, each a single
# cycle whose reply passed the ~92 us receive budget.
#
# Steps, each an af_packet 250 us run of DUR s on the two IS620N (no axis
# enabled, as run_is7_11_5.sh), every change undone at the end:
#   map   which RX queue carries the EtherCAT frames (rx_queue_N_packets before
#         and after a 10 s ecm_run) -> Q and its IRQ / thread
#   base  nothing changed (reference of the same session)
#   prio  the irq threads of all i226 vectors -> SCHED_FIFO PRIO (85): above
#         every other irq thread (50). ecm_run's RT thread is FIFO 80 on the
#         isolated core 3, where no irq thread runs, so they do not compete
#   cpu   prio + the irq thread of queue Q moved to CPU THREAD_CPU (1) via
#         /proc/irq/N/smp_affinity_list (on Tegra the hard IRQ may stay on
#         CPU0; the thread follows the mask). Refused by the kernel -> SKIP
#
# Verdict per run (is_lib.sh) with NOFRAME_MAX (default 2); the point is the
# comparison table (NOFRAME, turnaround tail) against base.
#
#   sudo -E PROFILE=P0 tools/gd11/run_is9_11_5.sh                 ~35 min
#   sudo -E PROFILE=P0 STEPS="map prio" DUR=120 tools/gd11/run_is9_11_5.sh
#
# Env: PROFILE STEPS IFACE N ENI CYCLE DUR PRIO THREAD_CPU NOFRAME_MAX LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROFILE=${PROFILE:-}
STEPS=${STEPS:-"map base prio cpu"}
IFACE=${IFACE:-enP1p1s0}; N=${N:-2}
ENI=${ENI:-$ROOT/config/eni/eni_2servo.enicfg}
CYCLE=${CYCLE:-250}; DUR=${DUR:-600}; PRIO=${PRIO:-85}; THREAD_CPU=${THREAD_CPU:-1}
NOFRAME_MAX=${NOFRAME_MAX:-2}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
EMCY_MAP=$ROOT/config/emcy/is620n.emcy
case "$PROFILE" in P0|P1) ;; *) echo "PROFILE=P0 or PROFILE=P1 (as booted)"; exit 3 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
[ "$(cat /sys/class/net/$IFACE/carrier 2>/dev/null)" = 1 ] || { echo "$IFACE has no link (sudo ip link set $IFACE up; drives powered?)"; exit 3; }
[ "$THREAD_CPU" = 3 ] && { echo "THREAD_CPU 3 is the RT core"; exit 3; }
# rig (12/10: the first run went without it, with the igc defaults back after
# a reboot -- rx-usecs 3 + GRO -- and gave 31 936 NOFRAME instead of ~100)
ethtool -K "$IFACE" gro off gso off tso off 2>/dev/null
ethtool -C "$IFACE" rx-usecs 0 2>/dev/null
rxu=$(ethtool -c "$IFACE" 2>/dev/null | awk -F': *' '/^rx-usecs:/{print $2}')
[ "$rxu" = 0 ] || { echo "$IFACE rx-usecs is '$rxu', could not set 0"; exit 3; }
"$HERE/rt_profile.sh" --expect "$PROFILE" --iface "$IFACE" > /tmp/is9_rt_profile.$$ 2>&1 \
    || { [ $? -le 1 ] || { cat /tmp/is9_rt_profile.$$; echo "rt_profile.sh FAIL: fix the lines above"; exit 3; }; }
LOG=${LOG:-$ROOT/log_gd11_is9_${PROFILE}_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
CSV=$LOG/results.csv; REP=$LOG/report.md
echo "case,profile,variant,load,metric,n,min,avg,p50,p99,p999,p9999,max,unit,extra" > "$CSV"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
. "$HERE/is_lib.sh"

# ---- the i226 vectors and their threads ----
IRQS=$(awk -v d="$IFACE" '$NF ~ "^"d {sub(":","",$1); print $1}' /proc/interrupts)
[ -n "$IRQS" ] || { echo "no $IFACE interrupt in /proc/interrupts"; exit 3; }
tpid () { ps -eo pid,comm | awk -v p="irq/$1-" 'index($2,p)==1 {print $1; exit}'; }
declare -A PRIO0 AFF0
for i in $IRQS; do
    p=$(tpid "$i"); PRIO0[$i]=$(chrt -p "$p" 2>/dev/null | awk -F': ' '/priority/{print $2}')
    AFF0[$i]=$(cat /proc/irq/$i/smp_affinity_list)
done
restore () {
    for i in $IRQS; do
        p=$(tpid "$i"); [ -n "$p" ] && chrt -f -p "${PRIO0[$i]:-50}" "$p" 2>/dev/null
        echo "${AFF0[$i]}" > /proc/irq/$i/smp_affinity_list 2>/dev/null
    done
    echo "restored: irq thread priorities and affinities of $IFACE"
}
trap restore EXIT
show () {
    for i in $IRQS; do
        p=$(tpid "$i")
        printf "    IRQ %s %-24s thread pid %-6s %s, affinity %s, ran on CPU %s, count CPU0..5: %s\n" "$i" \
            "$(awk -v n="$i:" '$1==n {print $NF}' /proc/interrupts)" "$p" \
            "$(chrt -p "$p" 2>/dev/null | awk -F': ' '/policy/{print $2}' | head -1)/$(chrt -p "$p" 2>/dev/null | awk -F': ' '/priority/{print $2}')" \
            "$(cat /proc/irq/$i/smp_affinity_list)" "$(ps -o psr= -p "$p" | tr -d ' ')" \
            "$(awk -v n="$i:" '$1==n {print $2,$3,$4,$5,$6,$7}' /proc/interrupts)"
    done
}

{
    echo "# Phase 11.5 IS-09 (i226 receive path), profile $PROFILE — $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "- host $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- af_packet ${CYCLE} us, $N IS620N, ${DUR} s per step, no axis enabled"
    echo
} > "$REP"
echo "=== i226 vectors before"; show | tee -a "$LOG/irq_before.txt"

GETS=""
for s in $(seq 1 "$N"); do GETS="$GETS${GETS:+,}$s:0x603F:0"; done
ENIC=$LOG/$(basename "$ENI" .enicfg)_${CYCLE}us.enicfg
python3 "$ROOT/tools/eni/eni_cycle.py" "$ENI" "$CYCLE" -o "$ENIC" > "$LOG/eni.txt" 2>&1 || { echo "eni_cycle.py failed"; exit 3; }
run () {   # tag duration
    local tag=$1 d=$2 E=$LOG/er_$1.log
    "$ECM_RUN" --iface "$IFACE" --n "$N" --eni "$ENIC" --pdo-scan --emcy-map "$EMCY_MAP" \
        --motion-cycle-us "$CYCLE" --duration-sec "$d" --pdo-get "$GETS" \
        --diag-file "$LOG/diag_$tag.txt" > "$E" 2>&1
    echo $? > "$E.rc"
}
qpk () { ethtool -S "$IFACE" | awk -F': ' '/rx_queue_[0-9]+_packets/ {gsub(/ /,"",$1); print $1, $2}'; }

Q=0
for st in $STEPS; do
case $st in
map)
    echo "=== map: which RX queue carries EtherCAT (10 s run)"
    qpk > "$LOG/q0.txt"; run map 10; qpk > "$LOG/q1.txt"
    join "$LOG/q0.txt" "$LOG/q1.txt" | awk '{printf "    %s +%d\n", $1, $3-$2}' | tee "$LOG/qdiff.txt"
    Q=$(join "$LOG/q0.txt" "$LOG/q1.txt" | awk '{d=$3-$2; if (d>m) {m=d; q=$1}} END {gsub(/[^0-9]/,"",q); print q+0}')
    echo "    -> EtherCAT on queue $Q (IRQ $(awk -v d="$IFACE-TxRx-$Q" '$NF==d {sub(":","",$1); print $1}' /proc/interrupts))"
    echo "- EtherCAT frames arrive on RX queue $Q" >> "$REP"
    ;;
base)
    echo "=== base: no change"; run base "$DUR"
    python3 "$HERE/is_parse.py" ecm_run "$LOG/er_base.log" is09 "$PROFILE" base idle >> "$CSV"
    er_verdict base "$LOG/er_base.log" "$NOFRAME_MAX"
    ;;
prio)
    echo "=== prio: irq threads of $IFACE -> SCHED_FIFO $PRIO"
    for i in $IRQS; do chrt -f -p "$PRIO" "$(tpid "$i")" || bad "chrt irq $i"; done
    show | tee "$LOG/irq_prio.txt"
    run prio "$DUR"
    python3 "$HERE/is_parse.py" ecm_run "$LOG/er_prio.log" is09 "$PROFILE" "prio$PRIO" idle >> "$CSV"
    er_verdict prio "$LOG/er_prio.log" "$NOFRAME_MAX"
    ;;
cpu)
    IQ=$(awk -v d="$IFACE-TxRx-$Q" '$NF==d {sub(":","",$1); print $1}' /proc/interrupts)
    echo "=== cpu: prio $PRIO + IRQ $IQ (queue $Q) affinity -> CPU $THREAD_CPU"
    for i in $IRQS; do chrt -f -p "$PRIO" "$(tpid "$i")" 2>/dev/null; done
    if echo "$THREAD_CPU" > /proc/irq/$IQ/smp_affinity_list 2>"$LOG/aff_err.txt"; then
        sleep 1; show | tee "$LOG/irq_cpu.txt"
        run cpu "$DUR"
        python3 "$HERE/is_parse.py" ecm_run "$LOG/er_cpu.log" is09 "$PROFILE" "prio${PRIO}_cpu$THREAD_CPU" idle >> "$CSV"
        er_verdict cpu "$LOG/er_cpu.log" "$NOFRAME_MAX"
        ps -o psr= -p "$(tpid "$IQ")" | sed "s/^/    thread of IRQ $IQ last ran on CPU /"
    else
        echo "  [SKIP] the kernel refused smp_affinity for IRQ $IQ: $(cat "$LOG/aff_err.txt")"
        echo "- cpu step skipped: smp_affinity refused ($(cat "$LOG/aff_err.txt"))" >> "$REP"
    fi
    ;;
*) echo "unknown step $st"; FAIL=$((FAIL+1)) ;;
esac
done

echo "=== i226 vectors after"; show | tee "$LOG/irq_after.txt"
{ echo; echo '```'; cat "$LOG/irq_before.txt"; echo '```'; echo; } >> "$REP"
python3 "$HERE/is_report.py" "$CSV" >> "$REP"
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo; cat "$REP"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
