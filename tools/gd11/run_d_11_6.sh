#!/usr/bin/env bash
# ==========================================================================
# run_d_11_6.sh -- Phase 11.6: D tests of the remote diagnostics API
# (claude/giai_doan_11_ke_hoach.md §7). The web UI (11.4) is not built yet:
# its place in D-01 is taken by two stats.json readers at 2 Hz (what a UI
# tab does) and D-09 waits for 11.4.
#
#   sudo -E tools/gd11/run_d_11_6.sh                    all, D-01 1800 s per case (~3 h 15 min)
#   sudo -E CASES="d03 d04 d12 d13" tools/gd11/run_d_11_6.sh      the short ones (~5 min)
#   sudo -E DUR=300 CASES=d01 tools/gd11/run_d_11_6.sh             D-01 dry run
#
# Rig (as run_api_11_1.sh): two virtual IS620N on soft_bus (veth, FIFO 79
# core 2), calibration is620n.cal, ENI eni_2servo_mm, hook cia402, CSP sine on
# both axes, ecm_run RT thread on core 3. API: --api --api-cpus 1-2.
#
#   d01   impact on the RT thread, one session, same binary, DUR s each:
#           OFF-a, ON, OFF-b, ON-stress, OFF-load, ON-load
#         ON        = --api + 2 readers of stats.json at 2 Hz (cores 1-2)
#         ON-stress = ON + sdo_read 20 Hz + a client that connects, holds
#                     5 s and disconnects, in a loop
#         *-load    = stress-ng --cpu 2 --vm 1 --vm-bytes 512M on cores 0-1
#         Pass: ON (both metrics) and ON-stress (wake_jitter) within [min(OFF), max(OFF) + max(2 us, 20 %)]
#         at p99.99; ON-stress occupancy (SDO mailbox cost) max <= 50 %
#         of the cycle; ON-load against OFF-load
#         the same; every run 0 overrun, NOFRAME <= 2, never LOST, DC locked,
#         0 CSP underrun.
#         Negative control d01neg (DUR_NEG s): ON + a FIFO 81 burst (60 us / 2 ms)
#         on the RT core 3 -> the same comparison MUST fail (the check can see harm)
#   d03   client SIGSTOP 60 s while connected, a client killed mid-sdo_read:
#         0 overrun; an sdo_read afterwards is served
#   d04   ecm_run killed (-9): a client sees STALE (exit 2) within 2 s of the
#         kill; stopped normally: FINAL at once; started again: the same
#         client sees it live again without being restarted (ecm_diag --watch)
#   d11   CPU time of the api thread over the ON run of d01 (or a 60 s run):
#         percent of one core, and of the readers / spam clients
#   d12   EMCY injected on slave 2 (soft_bus ctl "emcy 1 0x7500 0x01"): in
#         stats.json slaves[1].emcy (code 0x7500), emcy_count, a finding;
#         ecm_diag --api shows it; the 0x603F-without-EMCY branch is G-02
#   d13   forward compatibility: tools/gd11/api_client_example.py reads a
#         live stats.json and a synthetic "v1.1" file (2 ports, path
#         ring_open, unknown keys); negative control: a naive client that
#         assumes one port mis-reads the v1.1 file
#
# Covered by earlier steps, listed in the report, not re-run: D-02, D-05
# (C-05), D-06 (A-01), D-08 (C-04), D-10 (A-04), D-12 0x603F branch (G-02).
# D-14 (real IS620N in motion) is run_d14_11_6.sh.
#
# Env: CASES DUR DUR_NEG SB_PRIO SB_CPU IF_M IF_S API_DIR LOG SOFT_BUS ECM_RUN
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
ECM_DIAG=${ECM_DIAG:-$ROOT/apps/ecm_diag/ecm_diag}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"d03 d04 d12 d13 d01 d11"}
DUR=${DUR:-1800}; DUR_NEG=${DUR_NEG:-300}
API_DIR=${API_DIR:-/run/ecmaster_d}
ENI=$ROOT/config/eni/eni_2servo_mm.enicfg
CAL=$ROOT/config/profiles/is620n.cal
CTL=/tmp/soft_bus_11_6.ctl
VMAX=8388608
LOG=${LOG:-$ROOT/log_gd11_d_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
CSV=$LOG/results.csv; REP=$LOG/report.md
echo "case,profile,variant,load,metric,n,min,avg,p50,p99,p999,p9999,max,unit,extra" > "$CSV"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
want () { case " $CASES " in *" $1 "*) return 0;; esac; return 1; }
. "$HERE/is_lib.sh"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
for f in "$SOFT_BUS" "$ECM_RUN" "$ECM_DIAG"; do [ -x "$f" ] || { echo "missing $f (make all caps)"; exit 3; }; done
for i in "$IF_M" "$IF_S"; do ip link show "$i" >/dev/null 2>&1 || {
    echo "missing $i: sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"; exit 3; }; done
for p in ecm_run soft_bus; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 3; }; done

IS=$LOG/is620n_sim.prof
cp "$ROOT/config/profiles/is620n_min.prof" "$IS"
grep -q '^obj 0x6502 ' "$IS" || { echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro ad030000"; } >> "$IS"
for o in "0x6072 16 b80b" "0x6065 32 e76aa301" "0x607F 32 00000080" "0x6081 32 a0860100" "0x6083 32 40420f00" \
         "0x6084 32 40420f00" "0x6098 8 21" "0x609A 32 40420f00" "0x607C 32 00000000"; do
    set -- $o
    grep -q "^obj $1 " "$IS" || { echo "obj $1 var 1"; echo "sub $1 0 bits $2 rw $3"; } >> "$IS"
done
SBP=""; EP=""; BG=()
sb_start () {
    rm -f "$CTL"
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    "${sb[@]}" --iface "$IF_S" --n 2 --dc 32 --ctl "$CTL" \
        --profile 1="$IS" --cia402 1 --profile 2="$IS" --cia402 2 \
        --cia402-cal 1="$CAL" --cia402-cal 2="$CAL" ${SB_ARGS:-} > "$LOG/sb_$1.log" 2>&1 &
    SBP=$!; sleep 0.5
}
sb_stop () { [ -n "$SBP" ] && { kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null; SBP=""; }; return 0; }
bg_stop () { for p in "${BG[@]}"; do kill -TERM "$p" 2>/dev/null; done; for p in "${BG[@]}"; do wait "$p" 2>/dev/null; done; BG=(); pkill -x stress-ng 2>/dev/null; }
cleanup () { bg_stop; [ -n "$EP" ] && kill -INT "$EP" 2>/dev/null; sb_stop; }
trap cleanup EXIT
sbctl () { printf '%s\n' "$1" > "$CTL"; }

BASE=(--iface "$IF_M" --n 2 --eni "$ENI" --pdo-scan --emcy-map "$ROOT/config/emcy/is620n.emcy"
      --pdo-set "1:0x607F:0=$VMAX,2:0x607F:0=$VMAX"
      --hook cia402 --cia402-lead 20 --axis 1:0,2:0 --axis-modes csp
      --cia402-script "0.5 enable all; 1.5 sine all 100000 0.5")
API=(--api --api-dir "$API_DIR" --api-cpus 1-2)
start () {   # tag duration extra args...
    local tag=$1 dur=$2; shift 2
    rm -rf "$API_DIR"
    sb_start "$tag"
    "$ECM_RUN" "${BASE[@]}" --duration-sec "$dur" --diag-file "$LOG/diag_$tag.txt" "$@" > "$LOG/er_$tag.log" 2>&1 &
    EP=$!
}
finish () { wait "$EP"; echo $? > "$LOG/er_$1.log.rc"; EP=""; bg_stop; sb_stop; }
api_tid () { local t; for t in /proc/"$1"/task/*; do [ "$(cat "$t/comm" 2>/dev/null)" = ecm_api ] && { basename "$t"; return; }; done; }
cpu_ticks () { awk '{print $14 + $15}' "$1" 2>/dev/null || echo 0; }   # utime + stime of a /proc stat file

reader () {   # what a UI tab does: read stats.json at 2 Hz
    taskset -c 1-2 python3 - "$API_DIR/stats.json" <<'EOF' &
import json, sys, time
p = sys.argv[1]
while True:
    try:
        json.load(open(p))
    except Exception:
        pass
    time.sleep(0.5)
EOF
    BG+=($!)
}
churn () {    # connect, hold 5 s, disconnect, forever
    ( while true; do taskset -c 1-2 python3 "$HERE/ecm_ctl.py" --sock "$API_DIR/ctl.sock" hold 5 >/dev/null 2>&1; sleep 0.2; done ) &
    BG+=($!)
}
spam () {     # sdo_read 0x1018:1 at 20 Hz for $1 s
    taskset -c 1-2 python3 "$HERE/ecm_ctl.py" --sock "$API_DIR/ctl.sock" spam "$1" 20 1 0x1018 1 u32 > "$LOG/spam_$2.txt" 2>&1 &
    BG+=($!)
}
load () { taskset -c 0-1 stress-ng --cpu 2 --vm 1 --vm-bytes 512M --timeout "$1"s > /dev/null 2>&1 & BG+=($!); }

{
    echo "# Phase 11.6 D tests — $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "- host $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- rig: 2 virtual IS620N (is620n.cal), eni_2servo_mm, hook cia402, CSP sine; soft_bus ${SB_PRIO:+FIFO $SB_PRIO }core $SB_CPU"
    echo
    echo "| D | Covered by | Result |"
    echo "|---|---|---|"
    echo "| D-02 sdo_write in allowlist, read back | C-05 (11.2) | Jetson 10/10: PASS |"
    echo "| D-05 parameter write at OP, WKC unchanged | C-05 (11.2) | Jetson 10/10: PASS |"
    echo "| D-06 --api off: no file / thread, golden | A-01, A-09 (11.1) | Jetson: PASS |"
    echo "| D-08 refused writes | C-04 (11.2) | Jetson: PASS |"
    echo "| D-10 stats.json = end-of-run report | A-04 (11.1) | Jetson: PASS |"
    echo "| D-12 0x603F without EMCY | G-02 (11.3) | Jetson: PASS |"
    echo "| D-09 web UI token / Origin | 11.4 | not built |"
    echo "| D-14 real IS620N in motion | run_d14_11_6.sh | separate |"
    echo
} > "$REP"

# ---------------------------------------------------------------- d03
if want d03; then
    echo "=== D-03 stopped / killed clients"
    start d03 40 "${API[@]}"
    sleep 4
    python3 "$HERE/ecm_ctl.py" --sock "$API_DIR/ctl.sock" hold 70 > /dev/null 2>&1 & HP=$!
    sleep 0.5; kill -STOP $HP
    python3 "$HERE/ecm_ctl.py" --sock "$API_DIR/ctl.sock" spam 30 20 1 0x1018 1 u32 > "$LOG/spam_d03.txt" 2>&1 & SP=$!
    sleep 3; kill -9 $SP 2>/dev/null               # killed in the middle of its requests
    sleep 30
    python3 "$HERE/ecm_ctl.py" --sock "$API_DIR/ctl.sock" read 1 0x1018 1 u32 > "$LOG/read_d03.txt" 2>&1; R=$?
    kill -CONT $HP; kill $HP 2>/dev/null; wait $HP 2>/dev/null
    finish d03
    chk "D-03 sdo_read served after a stopped client (33 s) and a killed one" "[ $R = 0 ] && grep -q '\"ok\": *true' $LOG/read_d03.txt"
    er_verdict D-03 "$LOG/er_d03.log" 2
fi

# ---------------------------------------------------------------- d04
if want d04; then
    echo "=== D-04 STALE / FINAL / reconnect"
    start d04a 60 "${API[@]}"
    sleep 4
    "$ECM_DIAG" --api "$API_DIR" > /dev/null 2>&1; R0=$?
    kill -9 "$EP"; T0=$(date +%s.%N); wait "$EP" 2>/dev/null; EP=""
    R=0; while [ "$R" != 2 ]; do "$ECM_DIAG" --api "$API_DIR" --max-age 1 > "$LOG/d04_stale.txt" 2>&1; R=$?; sleep 0.05
        [ "$(echo "$(date +%s.%N) - $T0 > 5" | bc)" = 1 ] && break; done
    DT=$(echo "$(date +%s.%N) - $T0" | bc)
    sb_stop
    chk "D-04 live before the kill (exit $R0: 0 ok, 1 findings, not 2)" "[ $R0 != 2 ]"
    chk "D-04 killed: STALE seen after ${DT} s (<= 2 s)" "[ $R = 2 ] && [ \"\$(echo '$DT <= 2' | bc)\" = 1 ] && grep -qi stale $LOG/d04_stale.txt"
    # a watching client across a normal stop and a restart
    start d04b 6 "${API[@]}"
    taskset -c 1-2 "$ECM_DIAG" --api "$API_DIR" --watch 0.5 > "$LOG/d04_watch.txt" 2>&1 & WP=$!
    finish d04b
    "$ECM_DIAG" --api "$API_DIR" > "$LOG/d04_final.txt" 2>&1; RF=$?
    chk "D-04 stopped normally: FINAL at once (exit $RF)" "[ $RF = 2 ] && grep -q FINAL $LOG/d04_final.txt"
    start d04c 6 "${API[@]}"
    sleep 4
    PIDS=$(grep -o 'ecm_run pid [0-9]*' "$LOG/d04_watch.txt" | sort -u | wc -l)
    finish d04c
    kill $WP 2>/dev/null; wait $WP 2>/dev/null
    chk "D-04 the same --watch client saw the restarted ecm_run ($PIDS pids)" "[ $PIDS -ge 2 ]"
fi

# ---------------------------------------------------------------- d12
if want d12; then
    echo "=== D-12 EMCY on slave 2"
    start d12 12 "${API[@]}"
    sleep 5
    sbctl "emcy 1 0x7500 0x01"
    sleep 2
    cp "$API_DIR/stats.json" "$LOG/stats_d12.json"
    "$ECM_DIAG" --api "$API_DIR" > "$LOG/d12_view.txt" 2>&1
    finish d12
    chk "D-12 stats.json slave 2: emcy code 0x7500, emcy_count >= 1" \
        "python3 -c \"import json,sys; s=json.load(open('$LOG/stats_d12.json'))['slaves'][1]; sys.exit(not (s['emcy_count']>=1 and any(e['code'].upper()=='0X7500' for e in s['emcy'])))\""
    chk "D-12 a finding of type emcy" "python3 -c \"import json,sys; sys.exit(not any(f['type']=='emcy' for f in json.load(open('$LOG/stats_d12.json'))['findings']))\""
    chk "D-12 ecm_diag --api shows the EMCY" "grep -qi 'EMCY 0x7500' $LOG/d12_view.txt"
    er_verdict D-12 "$LOG/er_d12.log" 2
fi

# ---------------------------------------------------------------- d13
if want d13; then
    echo "=== D-13 forward compatibility"
    start d13 8 "${API[@]}"
    sleep 4; cp "$API_DIR/stats.json" "$LOG/stats_d13.json"
    finish d13
    python3 "$HERE/api_client_example.py" "$LOG/stats_d13.json" > "$LOG/d13_live.txt" 2>&1; R1=$?
    python3 - "$LOG/stats_d13.json" "$LOG/stats_v11.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
p2 = dict(d["bus"]["ports"][0]); p2.update(name="secondary", iface="enP8p1s0", link="down")
d["bus"]["ports"].append(p2)
d["bus"]["path"] = "ring_open"
d["bus"]["future_key"] = {"x": 1}
d["rt"]["future_rt_key"] = [1, 2]
for s in d["slaves"]:
    s["future_slave_key"] = "v1.1"
d["findings"].append({"type": "ring_open_between", "sev": "warn", "text": "ring open between 1 and 2", "a": 1, "b": 2})
json.dump(d, open(sys.argv[2], "w"))
EOF
    python3 "$HERE/api_client_example.py" "$LOG/stats_v11.json" > "$LOG/d13_v11.txt" 2>&1; R2=$?
    python3 "$HERE/api_client_example.py" --naive "$LOG/stats_v11.json" > "$LOG/d13_naive.txt" 2>&1; R3=$?
    chk "D-13 example client reads the live file (exit $R1)" "[ $R1 = 0 ] && grep -q 'ports: 1' $LOG/d13_live.txt"
    chk "D-13 example client reads the v1.1 file: 2 ports, path ring_open, unknown finding listed (exit $R2)" \
        "[ $R2 = 0 ] && grep -q 'ports: 2' $LOG/d13_v11.txt && grep -q 'path ring_open' $LOG/d13_v11.txt && grep -q 'ring_open_between' $LOG/d13_v11.txt"
    chk "D-13 negative control: the naive client mis-reports the v1.1 file" "! diff -q <(grep -v '^naive' $LOG/d13_naive.txt) <(grep -v '^naive' $LOG/d13_v11.txt) >/dev/null"
fi

# ---------------------------------------------------------------- d01 / d11
row () { python3 "$HERE/is_parse.py" ecm_run "$LOG/er_$1.log" d01 - "$1" "$2" >> "$CSV"; }
p9999 () { awk -F, -v v="$1" -v m="$2" '$3==v && $5==m {print $12}' "$CSV" | head -1; }
if want d01; then
    echo "=== D-01 impact on the RT thread (${DUR} s per case)"
    for c in offa on offb onstress offload onload; do
        case $c in on*) A=("${API[@]}");; *) A=();; esac
        echo "--- $c"
        start "$c" "$DUR" "${A[@]}"
        sleep 3
        case $c in on|onstress|onload) reader; reader;; esac
        [ $c = onstress ] && { spam "$((DUR - 10))" onstress; churn; }
        case $c in *load) load "$DUR";; esac
        if [ $c = on ]; then     # D-11: CPU ticks of the api thread between t+8 s and DUR-5 s
            ( sleep 5; T=$(api_tid "$EP"); a0=$(cpu_ticks "/proc/$EP/task/$T/stat"); t0=$(date +%s.%N)
              sleep $((DUR - 13)); a1=$(cpu_ticks "/proc/$EP/task/$T/stat"); t1=$(date +%s.%N)
              echo "$T $a0 $a1 $t0 $t1 $(getconf CLK_TCK)" > "$LOG/d11_ticks.txt" ) &
            BG+=($!)
        fi
        wait "$EP"; echo $? > "$LOG/er_$c.log.rc"
        EP=""; bg_stop; sb_stop
        case $c in *load) l=load;; *) l=idle;; esac
        row "$c" "$l"
        er_verdict "D-01 $c" "$LOG/er_$c.log" 2
        u=$(grep -o 'cia402_underrun=[0-9]*' <(grep "^d01,-,$c," "$CSV") | head -1 | cut -d= -f2)
        chk "D-01 $c CSP underrun 0 (${u:-?})" "[ '${u:-x}' = 0 ]"
    done
    within () {   # case base1 base2 metric
        python3 - "$(p9999 "$1" "$4")" "$(p9999 "$2" "$4")" "$(p9999 "$3" "$4")" <<'EOF'
import sys
v, a, b = (float(x) for x in sys.argv[1:4])
hi = max(a, b); lim = hi + max(2.0, 0.2 * hi)
print("%.1f vs OFF %.1f/%.1f, limit %.1f" % (v, a, b, lim)); sys.exit(0 if v <= lim else 1)
EOF
    }
    for m in wake_jitter occupancy; do
        s=$(within on offa offb "$m"); r=$?
        chk "D-01 on $m p99.99 $s" "[ $r = 0 ]"
        if [ $m = wake_jitter ]; then
            s=$(within onstress offa offb "$m"); r=$?
            chk "D-01 onstress $m p99.99 $s" "[ $r = 0 ]"
        else
            # sdo_read at 20 Hz puts mailbox datagrams into some cycles: their
            # occupancy is the cost of the SDO itself, not of the API thread
            # (that would show in wake_jitter first). Measured, bounded by an
            # absolute limit of 50 % of the cycle, not compared with OFF.
            s=$(within onstress offa offb "$m"); r=$?
            echo "    D-01 onstress $m p99.99 $s (SDO cost, info)"
            cyc=$(grep -oE 'cycle [0-9]+ us' "$LOG/er_onstress.log" | head -1 | grep -oE '[0-9]+'); cyc=${cyc:-1000}
            mx=$(awk -F, '$3=="onstress" && $5=="occupancy" {print $13}' "$CSV" | head -1)
            chk "D-01 onstress occupancy max ${mx:-?} us <= 50 % of the ${cyc} us cycle" \
                "python3 -c 'import sys; sys.exit(not ${mx:-1e9} <= 0.5 * $cyc)'"
        fi
        s=$(within onload offload offload "$m"); r=$?
        chk "D-01 onload $m p99.99 $s" "[ $r = 0 ]"
    done
    echo "--- d01neg: ON + a FIFO 81 burst on the RT core 3 (${DUR_NEG} s)"
    start d01neg "$DUR_NEG" "${API[@]}"
    sleep 3; reader
    # FIFO 81 (above the RT thread): 60 us busy every ~2 ms -- harm the check must see
    chrt -f 81 taskset -c 3 python3 -c '
import time
while True:
    t = time.perf_counter() + 60e-6
    while time.perf_counter() < t: pass
    time.sleep(0.002)' & BG+=($!)
    wait "$EP"; echo $? > "$LOG/er_d01neg.log.rc"; EP=""; bg_stop; sb_stop
    row d01neg idle
    s=$(within d01neg offa offb wake_jitter); r1=$?
    s2=$(within d01neg offa offb occupancy); r2=$?
    chk "D-01 negative control: a load on core 3 is seen (wake $s; occupancy $s2)" "[ $r1 = 1 ] || [ $r2 = 1 ]"
    grep -h '\[API\]' "$LOG/er_onstress.log" | sed 's/^/    /' | head -12
    cat "$LOG"/spam_onstress.txt 2>/dev/null | tail -1 | sed 's/^/    /'
fi
if want d11; then
    echo "=== D-11 CPU of the api thread"
    if [ ! -s "$LOG/d11_ticks.txt" ]; then
        DUR=60; start d11 60 "${API[@]}"; sleep 3; reader; reader
        ( sleep 5; T=$(api_tid "$EP"); a0=$(cpu_ticks "/proc/$EP/task/$T/stat"); t0=$(date +%s.%N)
          sleep 47; a1=$(cpu_ticks "/proc/$EP/task/$T/stat"); t1=$(date +%s.%N)
          echo "$T $a0 $a1 $t0 $t1 $(getconf CLK_TCK)" > "$LOG/d11_ticks.txt" ) &
        BG+=($!); wait "$EP"; EP=""; bg_stop; sb_stop
    fi
    read -r T a0 a1 t0 t1 hz < "$LOG/d11_ticks.txt"
    PCT=$(python3 -c "print('%.3f' % (100.0 * ($a1 - $a0) / $hz / ($t1 - $t0)))" 2>/dev/null)
    echo "    api thread tid $T: $((a1 - a0)) ticks in $(python3 -c "print('%.0f' % ($t1 - $t0))") s = ${PCT:-?} % of one core"
    echo "d11,-,on,idle,api_cpu,1,,,,,,,${PCT:-},%,tid=$T" >> "$CSV"
    chk "D-11 api thread CPU measured and < 5 % of one core (${PCT:-?} %)" "[ -n '${PCT:-}' ] && python3 -c 'import sys; sys.exit(not $PCT < 5)'"
fi

python3 "$HERE/is_report.py" "$CSV" >> "$REP" 2>/dev/null
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo; cat "$REP"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
