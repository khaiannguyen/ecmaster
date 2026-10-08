#!/usr/bin/env bash
# ==========================================================================
# run_events_10_0.sh — Phase 10.0 R-07: what the real IS620N drives and the
# master report when something happens on the bus, with ecm_run in OP and NO
# axis enabled (no hook, outputs 0). Plan claude/giai_doan_10_ke_hoach.md 2.2.
#
#   sudo -E tools/gd10/run_events_10_0.sh                      all 3 events
#   sudo -E EVENTS="cable" tools/gd10/run_events_10_0.sh       one event
#   sudo -E SIM=1 tools/gd10/run_events_10_0.sh                rehearsal on veth:
#        the events are injected through soft_bus --ctl (stand-ins, see below)
#
# Events, in this order (EVENTS), each: DO -> hold -> RESTORE -> settle:
#   estop      press the E-stop (drive DI), release it
#   mainpower  main power MCB (L1/L2) off at OP, control power stays; back on
#   cable      unplug the cable drive 1 OUT -> drive 2 IN, plug it back
# The operator presses Enter at the moment of each action; the script stamps
# it with CLOCK_MONOTONIC, the same clock as every ecm_run log line (each line
# is stamped on arrival). Per event the report lists what the master saw
# from DO - 0.5 s to RESTORE + SETTLE s: bus state changes, EMCY codes,
# recovery, SDO/SOEM errors, the diag snapshot (L5-11 on real hardware:
# which segment), and statusword/0x603F at the end.
#
# PASS: ecm_run kept running through every event and the bus is back in RUN
# at the end. What each event produced is DATA (EMCY/AL codes for the
# virtual drive 10.2 and config/emcy/is620n.emcy), graded OBSERVED / NONE.
# The master never resets a drive fault: a fault latched by an event stays
# (S7); clear it on the drive panel / power cycle after the run.
#
# SIM stand-ins: estop = drv_quickstop 0 0, mainpower = drv_fault 0 0 0x3220,
# cable = drop_node 1 (restore: drv_clear / restore_node).
# Env: IFACE N ENI EVENTS HOLD SETTLE DUR CAP SIM SB_PRIO SB_CPU YES LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SIM=${SIM:-0}
if [ "$SIM" = 1 ]; then IFACE=${IFACE:-veth_m}; else IFACE=${IFACE:-enP1p1s0}; fi
IF_S=${IF_S:-veth_s}
N=${N:-2}
ENI=${ENI:-$ROOT/config/eni/eni_2servo.enicfg}
EVENTS=${EVENTS:-"estop mainpower cable"}
HOLD=${HOLD:-10}; SETTLE=${SETTLE:-20}; DUR=${DUR:-1800}
CAP=${CAP:-1}; YES=${YES:-0}
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
EMCY_MAP=$ROOT/config/emcy/is620n.emcy
LOG=${LOG:-log_gd10_events_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
CTL=/tmp/soft_bus_10_0e.ctl
EV=$LOG/events.txt

mono () { python3 -c 'import time; print("%.3f" % time.monotonic())'; }
die  () { echo "$*"; exit 2; }

for f in "$ECM_RUN" "$ENI"; do [ -e "$f" ] || die "missing $f (make all caps)"; done
ip link show "$IFACE" >/dev/null 2>&1 || die "interface $IFACE missing"
for p in ecm_run soft_bus ecm_peek ecm_diag; do
    pgrep -x "$p" >/dev/null && die "a $p process is running -- stop it first"
done
for e in $EVENTS; do case $e in estop|mainpower|cable) ;; *) die "unknown event $e" ;; esac; done

if [ "$SIM" != 1 ]; then
    cat <<EOF

=== Phase 10.0 R-07 on $IFACE: events with the bus in OP, no axis enabled ===
  [ ] Motors loose / clamped, nothing on the shafts; hand near the MCB
  [ ] E-stop wired to a drive DI (for 'estop')
  [ ] You can reach the main power MCB without touching live parts ('mainpower')
  [ ] You can reach the cable drive 1 OUT -> drive 2 IN ('cable')
  Events: $EVENTS  (hold ${HOLD} s, then restore, then ${SETTLE} s to settle)
The master never enables an axis and never resets a drive fault.
EOF
    if [ "$YES" != 1 ]; then read -r -p "Type yes to start: " ans; [ "$ans" = yes ] || { echo stopped; exit 3; }; fi
fi

# ---------------------------------------------------------------- start
SBP=
if [ "$SIM" = 1 ]; then
    IS=$LOG/is620n_min_6502.prof
    { cat "$ROOT/config/profiles/is620n_min.prof"; echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro a5010000"; } > "$IS"
    rm -f "$CTL"
    sb=("$SOFT_BUS"); [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    SBA=""; for s in $(seq 1 "$N"); do SBA="$SBA --profile $s=$IS --cia402 $s"; done
    "${sb[@]}" --iface "$IF_S" --n "$N" --dc 32 --no-sm-wd --ctl "$CTL" $SBA > "$LOG/soft_bus.log" 2>&1 &
    SBP=$!; sleep 0.5
    kill -0 $SBP 2>/dev/null || { cat "$LOG/soft_bus.log"; die "soft_bus did not start"; }
fi
ctl () { printf '%s\n' "$*" | dd of="$CTL" conv=nocreat,notrunc oflag=nonblock status=none; }

TP=""; CAPF=""
if [ "$CAP" = 1 ] && command -v tshark >/dev/null; then
    CAPF=/tmp/events_r07_$$.pcapng
    tshark -i "$IFACE" -w "$CAPF" -f "ether proto 0x88a4" > "$LOG/tshark.log" 2>&1 &
    TP=$!; sleep 1.5
fi

GETS=""; for s in $(seq 1 "$N"); do GETS="$GETS${GETS:+,}$s:0x6041:0,$s:0x603F:0"; done
ER=(--iface "$IFACE" --n "$N" --eni "$ENI" --pdo-scan --emcy-map "$EMCY_MAP" --pdo-get "$GETS"
    --duration-sec "$DUR" --diag-file "$LOG/diag.txt")
[ "$SIM" = 1 ] && ER+=(--no-tx-ts)
STAMP='import sys, time
for line in sys.stdin:
    sys.stdout.write("%.3f %s" % (time.monotonic(), line)); sys.stdout.flush()'
ethtool -S "$IFACE" > "$LOG/ethtool_S_before.txt" 2>/dev/null
"$ECM_RUN" "${ER[@]}" > >(python3 -u -c "$STAMP" > "$LOG/er_r07.log") 2>&1 &
ERP=$!
echo "ecm_run ${ER[*]}" > "$LOG/cmd.txt"

cleanup () {
    kill -INT $ERP 2>/dev/null; wait $ERP 2>/dev/null
    [ -n "$TP" ] && { sleep 0.5; kill -INT $TP 2>/dev/null; wait $TP 2>/dev/null; }
    [ -n "$SBP" ] && { kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null; }
}
trap 'cleanup; exit 130' INT

echo "waiting for OP ..."
for _ in $(seq 1 300); do
    grep -q 'all slaves in OPERATIONAL' "$LOG/er_r07.log" 2>/dev/null && break
    kill -0 $ERP 2>/dev/null || break
    sleep 0.1
done
if ! grep -q 'all slaves in OPERATIONAL' "$LOG/er_r07.log" 2>/dev/null; then
    cleanup; tail -20 "$LOG/er_r07.log"; die "ecm_run did not reach OP (see $LOG/er_r07.log)"
fi
echo "OP. 5 s baseline ..."; sleep 5
: > "$EV"

# ---------------------------------------------------------------- events
do_text () { case $1 in
    estop)     echo "PRESS the E-stop and keep it pressed" ;;
    mainpower) echo "Switch the MAIN power MCB (L1/L2) OFF (control power stays on)" ;;
    cable)     echo "UNPLUG the cable drive 1 OUT -> drive 2 IN" ;; esac; }
undo_text () { case $1 in
    estop)     echo "RELEASE the E-stop" ;;
    mainpower) echo "Switch the MAIN power MCB back ON" ;;
    cable)     echo "PLUG the cable back in (drive 1 OUT -> drive 2 IN)" ;; esac; }
sim_do ()   { case $1 in estop) ctl drv_quickstop 0 0 ;; mainpower) ctl drv_fault 0 0 0x3220 ;; cable) ctl drop_node 1 ;; esac; }
sim_undo () { case $1 in estop) ctl drv_clear 0 ;; mainpower) ctl drv_clear 0 ;; cable) ctl restore_node 1 ;; esac; }

for e in $EVENTS; do
    kill -0 $ERP 2>/dev/null || { echo "ecm_run is gone"; break; }
    echo; echo "=== event: $e"
    if [ "$SIM" = 1 ]; then
        t=$(mono); sim_do "$e"
    else
        echo "  NEXT: $(do_text "$e")"
        read -r -p "  press Enter at the moment you do it: " _; t=$(mono)
    fi
    echo "$e do $t" >> "$EV"
    sleep 3; cp "$LOG/diag.txt" "$LOG/diag_${e}_do.txt" 2>/dev/null
    sleep $((HOLD > 3 ? HOLD - 3 : 0))
    if [ "$SIM" = 1 ]; then
        t=$(mono); sim_undo "$e"
    else
        echo "  NEXT: $(undo_text "$e")"
        read -r -p "  press Enter at the moment you do it: " _; t=$(mono)
    fi
    echo "$e undo $t" >> "$EV"
    echo "  settling ${SETTLE} s ..."
    sleep "$SETTLE"
    cp "$LOG/diag.txt" "$LOG/diag_${e}_after.txt" 2>/dev/null
done

echo; echo "stopping ecm_run ..."
ALIVE=0; kill -0 $ERP 2>/dev/null && ALIVE=1
cleanup
sleep 0.5
ethtool -S "$IFACE" > "$LOG/ethtool_S_after.txt" 2>/dev/null
[ -n "$CAPF" ] && mv "$CAPF" "$LOG/r07.pcapng" 2>/dev/null

python3 "$HERE/events_report.py" --log "$LOG/er_r07.log" --events "$EV" --dir "$LOG" --settle "$SETTLE" \
    --alive "$ALIVE" ${SIM:+--sim "$SIM"} > "$LOG/report.md"
rc=$?
cat "$LOG/report.md"
echo; echo "(report: $LOG/report.md${CAPF:+, capture r07.pcapng})"
exit $rc
