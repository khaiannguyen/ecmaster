#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# test_is_offline.sh -- Phase 11.5: offline tests of the IS tools (no root,
# no Jetson; runs in CI).
#
#   tools/gd11/test_is_offline.sh
#
#   O-01 rt_profile.sh on fake sysfs trees: P0 / P1 detected, --expect of
#        the right profile exit 0, of the wrong one exit 2 (negative
#        control); cpuidle.off=1 with a driver still loaded is a FAIL;
#        non-RT kernel / core not isolated / governor not performance FAIL
#   O-02 extlinux_p1.py: dry run leaves the file alone; --apply adds one
#        LABEL real-time-noidle (copy of DEFAULT + cpuidle.off=1), keeps
#        every old line, writes a backup, DEFAULT unchanged; a second
#        --apply is refused; --set-default switches and back; unknown label
#        refused
#   O-03 is_parse.py on fixtures: cyclictest (percentiles, overflow ->
#        ">HIST"), ecm_run (last summary block, clamp to max), tegrastats;
#        a log without the lines gives n=0 + reason (negative control)
#        txtime_probe summary (IS-04)
#   O-04 is_report.py: tables from two profiles and IS-04; a foreign CSV -> exit 2
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
PASS=0; FAIL=0
chk () { if eval "$2"; then echo "  [PASS] $1"; PASS=$((PASS+1)); else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi; }

mk_sys () {   # dir driver(or "") isolated governor cmdline
    local d=$1
    mkdir -p "$d/sys/devices/system/cpu/cpuidle" "$d/sys/devices/system/cpu/cpu3/cpufreq" "$d/proc"
    for s in 0 1; do mkdir -p "$d/sys/devices/system/cpu/cpu3/cpuidle/state$s"
        echo 0 > "$d/sys/devices/system/cpu/cpu3/cpuidle/state$s/disable"; done
    echo wfi > "$d/sys/devices/system/cpu/cpu3/cpuidle/state0/name"
    echo c7  > "$d/sys/devices/system/cpu/cpu3/cpuidle/state1/name"
    [ -n "$2" ] && echo "$2" > "$d/sys/devices/system/cpu/cpuidle/current_driver"
    echo "$3" > "$d/sys/devices/system/cpu/isolated"
    echo "$4" > "$d/sys/devices/system/cpu/cpu3/cpufreq/scaling_governor"
    echo "$5" > "$d/proc/cmdline"
}
rp () { SYSROOT=$1 KERNEL=${KERNEL:-6.8.12-1021-rt-tegra} "$HERE/rt_profile.sh" "${@:2}" > "$T/rp.txt" 2>&1; echo $?; }
BASE="root=/dev/x isolcpus=3 irqaffinity=0-2 transparent_hugepage=never"

echo "=== O-01 rt_profile.sh"
mk_sys "$T/p0" psci_idle 3 performance "$BASE"
mk_sys "$T/p1" none 3 performance "$BASE cpuidle.off=1"
mk_sys "$T/bad" psci_idle 3 performance "$BASE cpuidle.off=1"
mk_sys "$T/noiso" psci_idle "" schedutil "$BASE"
r=$(rp "$T/p0"); chk "O-01 P0 tree -> PROFILE=P0, exit 0 (got $r)" "grep -q PROFILE=P0 $T/rp.txt && [ $r = 0 ]"
r=$(rp "$T/p0" --expect P0); chk "O-01 P0 --expect P0 exit 0 (got $r)" "[ $r = 0 ]"
r=$(rp "$T/p0" --expect P1); chk "O-01 negative control: P0 --expect P1 exit 2 (got $r)" "[ $r = 2 ] && grep -q 'profile is P0, expected P1' $T/rp.txt"
r=$(rp "$T/p1" --expect P1); chk "O-01 P1 tree --expect P1 exit 0 (got $r)" "[ $r = 0 ] && grep -q PROFILE=P1 $T/rp.txt"
r=$(rp "$T/p1" --expect P0); chk "O-01 negative control: P1 --expect P0 exit 2 (got $r)" "[ $r = 2 ]"
r=$(rp "$T/bad"); chk "O-01 cpuidle.off=1 but a driver loaded -> FAIL (got $r)" "[ $r = 2 ] && grep -q 'current_driver is .psci_idle.' $T/rp.txt"
r=$(rp "$T/noiso"); chk "O-01 core not isolated + governor schedutil -> 2 FAIL (got $r)" "[ $r = 2 ] && [ \$(grep -c FAIL\] $T/rp.txt) = 2 ]"
r=$(KERNEL=6.8.12-tegra rp "$T/p0"); chk "O-01 non-RT kernel -> FAIL (got $r)" "[ $r = 2 ] && grep -q 'not PREEMPT_RT' $T/rp.txt"
r=$(rp "$T/p0" --expect P2); chk "O-01 --expect P2 -> usage exit 3 (got $r)" "[ $r = 3 ]"

echo "=== O-02 extlinux_p1.py"
F=$T/extlinux.conf; cp "$HERE/testdata/extlinux.conf" "$F"; S0=$(sha256sum < "$F")
python3 "$HERE/extlinux_p1.py" --file "$F" > "$T/dry.txt" 2>&1; r=$?
chk "O-02 dry run: exit 0, file unchanged, diff shows the new APPEND" \
    "[ $r = 0 ] && [ \"\$(sha256sum < $F)\" = \"$S0\" ] && grep -q '^+.*isolcpus=3.*cpuidle.off=1' $T/dry.txt"
python3 "$HERE/extlinux_p1.py" --file "$F" --apply > "$T/apply.txt" 2>&1; r=$?
chk "O-02 --apply: exit 0, backup written" "[ $r = 0 ] && ls $F.bak-* >/dev/null 2>&1"
chk "O-02 every old line kept, in order" "diff <(cat $HERE/testdata/extlinux.conf) <(head -n \$(wc -l < $HERE/testdata/extlinux.conf) $F) >/dev/null"
chk "O-02 one new LABEL real-time-noidle with Image.rt and cpuidle.off=1" \
    "[ \$(grep -c 'LABEL real-time-noidle' $F) = 1 ] && awk '/LABEL real-time-noidle/{f=1} f' $F | grep -q 'Image.rt' && awk '/LABEL real-time-noidle/{f=1} f' $F | grep -q 'isolcpus=3 irqaffinity=0-2 transparent_hugepage=never cpuidle.off=1'"
chk "O-02 DEFAULT still real-time" "grep -q '^DEFAULT real-time$' $F"
python3 "$HERE/extlinux_p1.py" --file "$F" --apply > "$T/again.txt" 2>&1; r=$?
chk "O-02 negative control: second --apply refused (exit 2)" "[ $r = 2 ] && grep -q 'exists already' $T/again.txt"
python3 "$HERE/extlinux_p1.py" --file "$F" --set-default real-time-noidle > /dev/null 2>&1; r1=$?
python3 "$HERE/extlinux_p1.py" --file "$F" --set-default real-time > /dev/null 2>&1; r2=$?
chk "O-02 --set-default there and back" "[ $r1 = 0 ] && [ $r2 = 0 ] && grep -q '^DEFAULT real-time$' $F"
python3 "$HERE/extlinux_p1.py" --file "$F" --set-default nosuch > "$T/ns.txt" 2>&1; r=$?
chk "O-02 unknown label refused (exit 2)" "[ $r = 2 ]"
python3 "$HERE/extlinux_p1.py" --file "$F" --from real-time-noidle > "$T/dup.txt" 2>&1; r=$?
chk "O-02 a source that already has cpuidle.off refused" "[ $r = 2 ]"

echo "=== O-03 is_parse.py"
{ echo "# /dev/cpu_dma_latency set to 0us"; echo "# Histogram"
  for i in $(seq 0 9); do printf "%06d %06d\n" $i 0; done
  printf "%06d %06d\n" 3 9000; printf "%06d %06d\n" 5 980; printf "%06d %06d\n" 8 18
  echo "# Total: 000010000"; echo "# Min Latencies: 00002"; echo "# Avg Latencies: 00003"
  echo "# Max Latencies: 00055"; echo "# Histogram Overflows: 00002"; } > "$T/cyc.txt"
out=$(python3 "$HERE/is_parse.py" cyclictest "$T/cyc.txt" is02 P0 dma idle 10)
chk "O-03 cyclictest: p50 3, p99 5, p99.9 8, p99.99 >10, max 55 ($out)" "[ '$out' = 'is02,P0,dma,idle,cyclictest,10000,2,3,3,5,8,>10,55,us,overflow=2' ]"
{ echo "wake_jitter          n=100 p50=  1000ns p99=  2000ns p99.9=  3000ns p99.99=  4000ns max=  3500ns overflow=0"
  echo "  [GROUP_MOTION] cycles=600000 wkc_mismatch=2 (0.0003%) overrun=0 (0.0000%)"
  echo "  [WKC GROUP_MOTION] ok=599998 noframe=2 zero=0 partial=0 over=0 max_run_bad=1"
  echo "  [DC] state=LOCKED locks=1 unlocks=0 clamps=0 stale=0 wraps=1"
  echo "wake_jitter          n=600000 p50=  2818ns p99=  7079ns p99.9= 11220ns p99.99= 25118ns max= 20500ns overflow=0"
  echo "prep_send            n=600000 p50=  5011ns p99=  6309ns p99.9=  7079ns p99.99=  7943ns max=  9100ns overflow=0"; } > "$T/er.log"
out=$(python3 "$HERE/is_parse.py" ecm_run "$T/er.log" is03 P1 nodma load)
chk "O-03 ecm_run: last block, p99.99 clamped to max 20.5" "echo '$out' | grep -q '^is03,P1,nodma,load,wake_jitter,600000,,,2.8,7.1,11.2,20.5,20.5,us,overrun=0 wkc_mismatch=2 noframe=2 dc_unlocks=0$'"
chk "O-03 ecm_run: missing occupancy -> n=0 with the reason (negative control)" "echo '$out' | grep -q '^is03,P1,nodma,load,occupancy,0,.*no occupancy line'"
printf 'RAM x cpu@47.5C VDD_IN 6000mW/6000mW\nRAM x cpu@49C VDD_IN 7000mW/6500mW\n' > "$T/tg.txt"
out=$(python3 "$HERE/is_parse.py" tegrastats "$T/tg.txt" is06 P1 bus idle)
chk "O-03 tegrastats: mean 6500 mW, max temp 49" "[ '$out' = 'is06,P1,bus,idle,vdd_in,2,6000,6500,,,,,7000,mW,cpu_temp_max=49.0C' ]"
out=$(python3 "$HERE/is_parse.py" tegrastats /dev/null is06 P1 bus idle)
chk "O-03 tegrastats without VDD_IN -> n=0 + reason" "echo '$out' | grep -q ',vdd_in,0,.*no VDD_IN'"

{ echo "[counts]"; echo "  attempts 600000  sent 600000  send_err 0"
  echo "  ETF drops: missed 0  invalid_param 1  other 0"; echo "  late wake (woke after t_target) 1"
  echo "  launch_err = tx_ts - target  min      303  p50      314  p99      328  p99.9      332  p99.99      336  max      344 ns (n=599999)"
  echo "  wake jitter (MONO)           min     1200  p50     2800  p99     5000  p99.9     8000  p99.99    14000  max    20500 ns (n=600000)"; } > "$T/probe.txt"
python3 "$HERE/is_parse.py" probe "$T/probe.txt" is04 P0 d150 idle > "$T/probe.csv"; out=$(grep -v probe_send "$T/probe.csv")
chk "O-03 probe: launch_err row with missed/invalid, wake in us" \
    "echo '$out' | grep -q '^is04,P0,d150,idle,launch_err,599999,303,,314,328,332,336,344,ns,sent=600000 missed=0 invalid=1 late_wake=1$' && echo '$out' | grep -q '^is04,P0,d150,idle,probe_wake,600000,1.2,,2.8,5.0,8.0,14.0,20.5,us,'"
chk "O-03 probe: missing send line -> n=0 + reason (negative control)" "grep -q ',probe_send,0,.*no .send call duration. line' $T/probe.csv"

echo "=== O-04 is_report.py"
H="case,profile,variant,load,metric,n,min,avg,p50,p99,p999,p9999,max,unit,extra"
{ echo "$H"; python3 "$HERE/is_parse.py" ecm_run "$T/er.log" is03 P0 dma idle; } > "$T/a.csv"
{ echo "$H"; python3 "$HERE/is_parse.py" ecm_run "$T/er.log" is03 P1 nodma load; python3 "$HERE/is_parse.py" cyclictest "$T/cyc.txt" is02 P1 dma idle 10; } > "$T/b.csv"
{ echo "$H"; python3 "$HERE/is_parse.py" probe "$T/probe.txt" is04 P0 d150 idle; } > "$T/c.csv"
python3 "$HERE/is_report.py" "$T/a.csv" "$T/b.csv" "$T/c.csv" > "$T/rep.md"; r=$?
chk "O-04 report: exit 0, P0 and P1n rows, 8.5 reference, IS-02 table" \
    "[ $r = 0 ] && grep -q '^| P0 | idle | 600000' $T/rep.md && grep -q '^| P1n | load | 600000' $T/rep.md && grep -q 'GĐ8 8.5' $T/rep.md && grep -q 'IS-02' $T/rep.md && grep -q '^| P0 | d150 | 599999' $T/rep.md"
echo "a,b,c" > "$T/x.csv"
python3 "$HERE/is_report.py" "$T/x.csv" > /dev/null 2>&1; r=$?
chk "O-04 negative control: foreign CSV -> exit 2" "[ $r = 2 ]"

echo "=== O-05 is_lib.sh er_verdict / er_axes_used (lines from the 12/10 Jetson logs)"
ok () { :; }; bad () { echo "      bad: $1"; NB=$((NB+1)); }
. "$HERE/is_lib.sh"
{ echo "  [GROUP_MOTION] cycles=1200001 wkc_mismatch=0 (0.0000%) overrun=0 (0.0000%)"
  echo "  [DC] state=LOCKED locks=1 unlocks=0 clamps=0 stale=0 wraps=3"
  echo "  [WKC GROUP_MOTION] ok=1200001 noframe=0 zero=0 partial=0 over=0 max_run_bad=0"
  echo "  [POLICY] final bus state RUN; entered: DEGRADED=0 LOST=0 RECOVER=0 RUN=0; ticks: RUN=1200001 DEGRADED=0 LOST=0 RECOVER=0"; } > "$T/good.log"
echo 0 > "$T/good.log.rc"
{ echo "  [GROUP_MOTION] cycles=2400001 wkc_mismatch=1944132 (81.0000%) overrun=0 (0.0000%)"
  echo "  [DC] state=LOCKED locks=1 unlocks=0 clamps=0 stale=0 wraps=3"
  echo "  [WKC GROUP_MOTION] ok=2527 noframe=1944132 zero=0 partial=0 over=0 max_run_bad=32845"
  echo "  [POLICY] final bus state DEGRADED; entered: DEGRADED=20821 LOST=18293 RECOVER=18293 RUN=20820; ticks: RUN=20821 DEGRADED=1925838 LOST=18733 RECOVER=434609"
  echo "  [CIA402] axis 0 (1:0): Switch on disabled sw=0x0250 cw=0x0000 mode=8 pos=0 vel=0 err=bus lost 0x603F=0x0000 used=0 late=470075 underrun=0 dropped=1921921 track_max=0 (n=0)"
  echo "  [CIA402] axis 1 (1:1): Switch on disabled sw=0x0250 cw=0x0000 mode=8 pos=0 vel=0 err=bus lost 0x603F=0x0000 used=0 late=470075 underrun=0 dropped=1921921 track_max=0 (n=0)"; } > "$T/p4.log"
echo 0 > "$T/p4.log.rc"
NB=0; er_verdict good "$T/good.log" 2; chk "O-05 clean 500 us log: 0 FAIL" "[ $NB = 0 ]"
NB=0; er_verdict p4 "$T/p4.log" 2 > "$T/v.txt"; cat "$T/v.txt"
chk "O-05 negative control: the P4 250 us log (36/0 in part 2a) now FAILs NOFRAME + LOST=18293 (2)" "[ $NB = 2 ] && grep -q 'entered LOST 18293 times' $T/v.txt"
NB=0; er_axes_used p4 "$T/p4.log"; chk "O-05 negative control: axes never used -> FAIL" "[ $NB = 1 ]"

echo "=== O-06 ecm_run --rx-guard-us refused before the bus is touched (needs apps/ecm_run/ecm_run)"
ER=$HERE/../../apps/ecm_run/ecm_run
if [ -x "$ER" ]; then
    for a in "10" "2000" "200 --motion-cycle-us 250" "180 --motion-cycle-us 250 --link etf --etf-lead-us 50 --etf-asap-us 25"; do
        # shellcheck disable=SC2086
        "$ER" --iface lo --n 1 --rx-guard-us $a > "$T/g.txt" 2>&1; r=$?
        chk "O-06 --rx-guard-us $a: rc 1, refused, bus not touched" "[ $r = 1 ] && grep -q 'rx-guard-us .*: need 20..1000' $T/g.txt && ! grep -q 'expected WKC' $T/g.txt"
    done
    "$ER" --iface lo --n 1 --motion-cycle-us 250 --rx-guard-us 60 > "$T/g.txt" 2>&1
    chk "O-06 negative control: --rx-guard-us 60 at 250 us passes the check" "! grep -q 'rx-guard-us .*: need' $T/g.txt"
else
    echo "  [SKIP] ecm_run not built"
fi

echo "=== O-07 deadtime.py (IS-08): 2-cycle model, margins, negative control setpoint 90 %"
DT=$HERE/deadtime.py
mk250 () {   # pct -> a 250 us etf + guard 60 log with the numbers of the 1 h run (11.5 §17)
    { echo "ecm_run: link etf: lead 50 us, asap 25 us, SO_PRIORITY 7, 80 ns/byte; qdisc on enP1p1s0: etf"
      echo "ecm_run: DC on -- ref=slave 1, SYNC0 250000 ns on 2 motion slave(s), setpoint $1% after SYNC0, first SYNC0 in 100.0 ms"
      echo "ecm_run: fault policy: rx timeout = tick deadline - 60 us - asap, LOST after 100 motion NOFRAME, recovery ON"
      echo "wake_jitter          n=14400456 p50=    2818ns p99=    3981ns p99.9=    5011ns p99.99=    7079ns max=   62000ns overflow=0"
      echo "prep_send            n=14400456 p50=    4466ns p99=    6309ns p99.9=    7079ns p99.99=   11220ns max=   21400ns overflow=0"
      echo "occupancy            n=14400456 p50=   89125ns p99=  158489ns p99.9=  177827ns p99.99=  199526ns max=  246000ns overflow=0"
      echo "turnaround           n=14400451 p50=   70794ns p99=   89125ns p99.9=  100000ns p99.99=  112201ns max=  213000ns overflow=0"
      echo "  [GROUP_MOTION] cycles=14400456 wkc_mismatch=5 (0.0000%) overrun=0 (0.0000%)"
      echo "  [DC] |e| (after 5 s) p50=0 p99=0 p99.9=0 p99.99=0 us, mean e=+0.00 us"
      echo "  [WKC GROUP_MOTION] ok=14400451 noframe=5 zero=0 partial=0 over=0 max_run_bad=1"; } > "$T/dt$1.log"
}
mk250 30; mk250 90
python3 "$DT" "$T/dt30.log" > "$T/dt30.txt" 2>&1; r=$?
chk "O-07 250 us etf, setpoint 30 %: dead time 500 us (2 cycles), output and input margins >= 0" \
    "[ $r = 0 ] && grep -q '500.0 us (2 cycles)' $T/dt30.txt && grep -q 'output margin to SYNC0 .* | 104.0 | 104.0 |' $T/dt30.txt && grep -q 'input margin .* | 75.0 | 75.0 |' $T/dt30.txt"
chk "O-07 receive margin at max 250 - 25 - 60 - 213 = -48 (the 5 NOFRAME of the 1 h run) is reported" \
    "grep -q 'receive margin .* | 52.8 | -48.0 |' $T/dt30.txt && grep -q 'negative margin at max: rx_margin_max' $T/dt30.txt"
python3 "$DT" "$T/dt90.log" > "$T/dt90.txt" 2>&1
chk "O-07 negative control: setpoint 90 % -> negative output margin" "grep -q 'negative margin at max: out_margin_max' $T/dt90.txt"
python3 "$DT" --csv "$T/dt30.log" "$T/dt90.log" > "$T/dt.csv"
chk "O-07 --csv: header + 2 rows" "[ \$(wc -l < $T/dt.csv) = 3 ]"

echo "RESULT: $PASS pass, $FAIL fail"
[ $FAIL = 0 ]
