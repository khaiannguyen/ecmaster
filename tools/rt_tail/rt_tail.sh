#!/usr/bin/env bash
# rt_tail.sh - long cyclictest on the isolated RT core with ftrace armed on
# that core only. If one wake-up exceeds BREAK_US, cyclictest writes a trace
# marker, stops tracing and exits, so trace.txt holds what ran on the core
# right before the spike. Otherwise it runs for DUR and leaves a histogram.
#   sudo ./rt_tail.sh                  (defaults: CPU=3 BREAK_US=500 DUR=8h)
#   sudo CPU=3 BREAK_US=300 DUR=2h ./rt_tail.sh
set -u
CPU=${CPU:-3}; BREAK_US=${BREAK_US:-500}; DUR=${DUR:-8h}
OUT=${OUT:-log_rt_tail_$(date +%Y%m%d_%H%M%S)}
T=/sys/kernel/tracing
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
mkdir -p "$OUT"

ORIG_MASK=$(cat $T/tracing_cpumask)
# buffer_size_kb may read "7 (expanded: 1408)": keep only the last number
ORIG_BUF=$(grep -o "[0-9]\+" $T/buffer_size_kb | tail -1)
echo 0 > $T/tracing_on
echo nop > $T/current_tracer
echo > $T/trace
printf '%x' $((1 << CPU)) > $T/tracing_cpumask
echo 16384 > $T/buffer_size_kb
echo > $T/set_event
for ev in sched:sched_switch sched:sched_wakeup irq:irq_handler_entry irq:irq_handler_exit \
          irq:softirq_entry irq:softirq_exit timer:hrtimer_expire_entry \
          workqueue:workqueue_execute_start power:cpu_idle ipi:ipi_raise; do
    echo "$ev" >> $T/set_event 2>/dev/null || echo "event $ev not available" | tee -a "$OUT/notes.txt"
done
echo 1 > $T/tracing_on

date '+%F %T' > "$OUT/start.txt"
echo "rt_tail: CPU=$CPU BREAK_US=$BREAK_US DUR=$DUR -> $OUT (Ctrl+C to stop early)"
cyclictest -a "$CPU" -t1 -p 95 -m -i 1000 -h 3000 -D "$DUR" -b "$BREAK_US" --tracemark -q \
    > "$OUT/cyclictest.txt" 2>&1
echo 0 > $T/tracing_on
date '+%F %T' > "$OUT/end.txt"
cp $T/trace "$OUT/trace.txt"
journalctl --since "$(cat "$OUT/start.txt")" --no-pager > "$OUT/journal.txt" 2>/dev/null

echo > $T/set_event; echo "$ORIG_MASK" > $T/tracing_cpumask; echo "$ORIG_BUF" > $T/buffer_size_kb
chown -R "${SUDO_USER:-root}:" "$OUT"
echo "---- summary ($OUT) ----"
grep -E "Max Latencies|Break|# Total|Overflows" "$OUT/cyclictest.txt"
