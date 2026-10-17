#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_api_11_1.sh -- Phase 11.1: stats.json of `ecm_run --api` on soft_bus
# (claude/giai_doan_11_ke_hoach.md §2, docs/api.md). Functional checks; the
# cost on the RT thread is D-01 (step 11.5), measured on the Jetson only.
#
#   sudo -E tools/gd11/run_api_11_1.sh
#
# Rig: two virtual IS620N (soft_bus --cia402, calibration is620n.cal), the
# TwinCAT ENI eni_2servo_mm (0x1702/0x1B02, 0x607F in the RxPDO -> --pdo-set),
# hook cia402, CSP (1 - cos) on both axes.
#
#   A-01  --api off: no file, no api thread, no [API] line   (D-06)
#         negative control: the same run with --api has all three
#   A-02  schema v1 while running and at the end; one port, path single (D-13 part)
#         + the checker's own negative control (stats_check.py selftest)
#   A-03  10 Hz, a reader never sees a half file, pub_seq only goes up
#   A-04  final stats.json == end-of-run report (D-10)
#         negative control: a stats.json taken mid-run MUST differ
#   A-05  api thread pinned to --api-cpus, never CPU 3; --api-cpus 3 refused;
#         CPU 0 accepted with a warning (plan §0.5)
#   A-06  axes: OPERATION_ENABLED while the script runs, error none;
#         negative control: no --hook -> axes [] and hist.hook null
#   A-07  bad options refused before the bus is touched (--api-dir not writable,
#         --api-dir without --api)
#   A-08  kill -9 mid-run: the last stats.json is whole (final false)
#   A-09  golden: datagrams identical with --api (8 nodes, 2 groups) and that
#         run's stats.json passes the schema and D-10 with an IO group
#   A-10  --no-diag: slaves without counters, sources.monitor.diag false, schema ok
#   A-11  (Phase 11.3) rt.* = this machine: isolated CPUs, cpuidle driver, idle states
#         of core 3, kernel, RT flags of /proc/cmdline; negative control: the same
#         check on a copy with one value changed MUST fail
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S CASES SB_PRIO SB_CPU API_DIR (tmpfs!) LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"a01 a02 a03 a04 a05 a06 a07 a08 a09 a10 a11"}
API_DIR=${API_DIR:-/run/ecmaster_test}          # tmpfs: on ext4 every rename is a journal write
CHECK="python3 $HERE/stats_check.py"
ENI=$ROOT/config/eni/eni_2servo_mm.enicfg
CAL=$ROOT/config/profiles/is620n.cal
CTL=/tmp/soft_bus_11_1.ctl
VMAX=8388608                                    # 60 rpm, 2^23 inc/rev

LOG=${LOG:-log_gd11_api_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
want () { case " $CASES " in *" $1 "*) return 0;; esac; return 1; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done
for p in ecm_run soft_bus; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 2; }; done
case "$(stat -f -c %T "$(dirname "$API_DIR")" 2>/dev/null)" in
    tmpfs) ;;
    *) echo "note: $(dirname "$API_DIR") is not tmpfs -- publish times will be those of a disk" ;;
esac

# soft_bus: two virtual IS620N with the limit and PP/HM objects (as run_w_10_9.sh SIM=1)
IS=$LOG/is620n_sim.prof
cp "$ROOT/config/profiles/is620n_min.prof" "$IS"
grep -q '^obj 0x6502 ' "$IS" || { echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro ad030000"; } >> "$IS"
for o in "0x6072 16 b80b" "0x6065 32 e76aa301" "0x607F 32 00000080" "0x6081 32 a0860100" "0x6083 32 40420f00" \
         "0x6084 32 40420f00" "0x6098 8 21" "0x609A 32 40420f00" "0x607C 32 00000000"; do
    set -- $o
    grep -q "^obj $1 " "$IS" || { echo "obj $1 var 1"; echo "sub $1 0 bits $2 rw $3"; } >> "$IS"
done
SBP=
sb_start () {
    rm -f "$CTL"
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    "${sb[@]}" --iface "$IF_S" --n 2 --dc 32 --no-sm-wd --ctl "$CTL" \
        --profile 1="$IS" --cia402 1 --profile 2="$IS" --cia402 2 \
        --cia402-cal 1="$CAL" --cia402-cal 2="$CAL" > "$LOG/sb_$1.log" 2>&1 &
    SBP=$!; sleep 0.5
}
sb_stop () { [ -n "$SBP" ] && { kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null; SBP=; }; return 0; }
trap 'sb_stop' EXIT

BASE=(--iface "$IF_M" --n 2 --eni "$ENI" --pdo-scan --emcy-map "$ROOT/config/emcy/is620n.emcy" --no-tx-ts
      --pdo-set "1:0x607F:0=$VMAX,2:0x607F:0=$VMAX")
MOT=(--hook cia402 --cia402-lead 20 --axis 1:0,2:0 --axis-modes csp
     --cia402-script "0.5 enable all; 1.5 cos 1 100000 1; 1.5 cos 2 100000 1")

# start TAG DURATION ARGS...  (background, PID in EP)
start () {
    local tag=$1 dur=$2; shift 2
    sb_start "$tag"
    "$ECM_RUN" "${BASE[@]}" --duration-sec "$dur" --diag-file "$LOG/diag_$tag.txt" "$@" > "$LOG/er_$tag.log" 2>&1 &
    EP=$!
}
finish () { wait "$EP"; RC=$?; sb_stop; }
fresh () { rm -rf "$API_DIR"; }
api_tid () {   # thread id of ecm_api in process $1, empty if none
    local t
    for t in /proc/"$1"/task/*; do [ "$(cat "$t/comm" 2>/dev/null)" = ecm_api ] && { basename "$t"; return; }; done
}
expect_cpus () {   # --api-cpus list $1 restricted to the online CPUs, as the kernel prints it
    python3 - "$1" <<'EOF'
import sys
def parse(s):
    out = set()
    for part in s.strip().split(','):
        a, _, b = part.partition('-')
        out.update(range(int(a), int(b or a) + 1))
    return out
want = parse(sys.argv[1]) & parse(open('/sys/devices/system/cpu/online').read())
r, ws = [], sorted(want)
i = 0
while i < len(ws):
    j = i
    while j + 1 < len(ws) and ws[j + 1] == ws[j] + 1: j += 1
    r.append(str(ws[i]) if i == j else "%d-%d" % (ws[i], ws[j])); i = j + 1
print(",".join(r))
EOF
}

echo "=== Phase 11.1 stats.json on soft_bus (2 virtual IS620N, hook cia402), log $LOG"

if want a01; then
    echo "--- A-01: --api off (D-06) + negative control"
    rm -rf /run/ecmaster
    start a01 4 "${MOT[@]}"
    sleep 2.5; T=$(api_tid $EP)
    finish
    chk "A-01 rc 0" "[ $RC = 0 ]"
    chk "A-01 no /run/ecmaster/stats.json" "[ ! -e /run/ecmaster/stats.json ]"
    chk "A-01 no api thread" "[ -z '$T' ]"
    chk "A-01 no API line in the log" "! grep -q 'API' '$LOG/er_a01.log'"
    fresh
    start a01n 4 "${MOT[@]}" --api --api-dir "$API_DIR"
    sleep 2.5; T=$(api_tid $EP)
    finish
    chk "A-01 negative control: with --api the file exists" "[ -s '$API_DIR/stats.json' ]"
    chk "A-01 negative control: with --api the thread exists" "[ -n '$T' ]"
    chk "A-01 negative control: with --api the [API] lines exist" "grep -q '\[API\] stats.json writes=' '$LOG/er_a01n.log'"
fi

if want a02 || want a03 || want a04 || want a05 || want a06; then
    echo "--- A-02..A-06: one 8 s run with --api"
    fresh
    start a02 8 "${MOT[@]}" --api --api-dir "$API_DIR"
    sleep 1.0
    $CHECK watch "$API_DIR/stats.json" 4 > "$LOG/watch_a03.txt"; WRC=$?
    cp "$API_DIR/stats.json" "$LOG/stats_mid.json"
    T=$(api_tid $EP)
    CPUS=$( [ -n "$T" ] && awk '/^Cpus_allowed_list/ {print $2}' /proc/$EP/task/$T/status)
    finish
    cp "$API_DIR/stats.json" "$LOG/stats_final.json"
    chk "A-02 rc 0" "[ $RC = 0 ]"
    if want a02; then
        $CHECK schema "$LOG/stats_mid.json" > "$LOG/schema_mid.txt"; chk "A-02 schema v1 mid-run ($(cat "$LOG/schema_mid.txt"))" "[ $? = 0 ]"
        $CHECK schema "$LOG/stats_final.json" > "$LOG/schema_final.txt"; chk "A-02 schema v1 final" "[ $? = 0 ]"
        chk "A-02 one port in a list, path single, final flag set" \
            "python3 -c \"import json,sys; d=json.load(open('$LOG/stats_final.json')); sys.exit(not (len(d['bus']['ports'])==1 and d['bus']['path']=='single' and d['final'] and not json.load(open('$LOG/stats_mid.json'))['final']))\""
        $CHECK selftest "$LOG/stats_final.json" > "$LOG/selftest.txt"; chk "A-02 negative control: the checker rejects 16 broken documents" "[ $? = 0 ]"
    fi
    if want a03; then
        cat "$LOG/watch_a03.txt" | sed 's/^/    /'
        chk "A-03 no half file read, pub_seq never backwards" "[ $WRC = 0 ]"
        chk "A-03 rate 8..12 Hz, largest gap < 300 ms" \
            "awk '{for(i=1;i<=NF;i++){split(\$i,a,\"=\"); v[a[1]]=a[2]}} END {exit !(v[\"rate\"]>=8 && v[\"rate\"]<=12 && v[\"max_gap_ms\"]<300)}' '$LOG/watch_a03.txt'"
        chk "A-03 no tmp file left" "[ ! -e '$API_DIR/.stats.json.tmp' ]"
    fi
    if want a04; then
        $CHECK compare "$LOG/stats_final.json" "$LOG/er_a02.log" > "$LOG/compare_final.txt"; R=$?
        tail -1 "$LOG/compare_final.txt" | sed 's/^/    /'
        chk "A-04 final stats.json == end-of-run report (D-10)" "[ $R = 0 ]"
        $CHECK compare "$LOG/stats_mid.json" "$LOG/er_a02.log" > "$LOG/compare_mid.txt"; R=$?
        chk "A-04 negative control: a mid-run stats.json differs ($(tail -1 "$LOG/compare_mid.txt"))" "[ $R = 1 ]"
        chk "A-04 no handoff drop" "grep -q 'handoff puts/drops: rt=[0-9]*/0 dc=[0-9]*/0 hook=[0-9]*/0 telemetry=[0-9]*/0 monitor=[0-9]*/0' '$LOG/er_a02.log'"
    fi
    if want a05; then
        E=$(expect_cpus 1-2)
        chk "A-05 api thread pinned to --api-cpus 1-2 (kernel: '$CPUS', online part '$E')" "[ -n '$CPUS' ] && [ '$CPUS' = '$E' ]"
    fi
    if want a06; then
        chk "A-06 mid-run: both axes OPERATION_ENABLED, error none, hook cia402" \
            "python3 -c \"import json,sys; d=json.load(open('$LOG/stats_mid.json')); a=d['axes']; sys.exit(not (len(a)==2 and all(x['state']=='OPERATION_ENABLED' and x['err']=='none' and x['error_src']=='none' for x in a) and d['rt']['hook']=='cia402' and d['hist']['hook']))\""
    fi
fi

if want a05; then
    echo "--- A-05: CPU options"
    "$ECM_RUN" --iface "$IF_M" --n 2 --api --api-dir "$API_DIR" --api-cpus 3 > "$LOG/er_a05_cpu3.log" 2>&1; R=$?
    chk "A-05 --api-cpus 3 refused before the bus (rc $R)" "[ $R != 0 ] && grep -q 'CPU 3 is the RT core' '$LOG/er_a05_cpu3.log'"
    "$ECM_RUN" --iface "$IF_M" --n 2 --api --api-dir "$API_DIR" --api-cpus 2-4 > "$LOG/er_a05_cpu24.log" 2>&1; R=$?
    chk "A-05 --api-cpus 2-4 refused (contains 3)" "[ $R != 0 ]"
    fresh
    start a05c0 3 --api --api-dir "$API_DIR" --api-cpus 0
    sleep 1.5; T=$(api_tid $EP); CPUS=$( [ -n "$T" ] && awk '/^Cpus_allowed_list/ {print $2}' /proc/$EP/task/$T/status)
    finish
    chk "A-05 --api-cpus 0: runs, pinned to 0 ('$CPUS'), warns about the i226 IRQ" \
        "[ $RC = 0 ] && [ '$CPUS' = 0 ] && grep -q 'CPU0 takes the i226 interrupts' '$LOG/er_a05c0.log'"
fi

if want a06; then
    echo "--- A-06 negative control: no hook"
    fresh
    start a06n 3 --api --api-dir "$API_DIR"
    finish
    $CHECK schema "$API_DIR/stats.json" > /dev/null; S=$?
    chk "A-06 no --hook: schema ok, axes [], hist.hook null, rt.hook null" \
        "[ $S = 0 ] && python3 -c \"import json,sys; d=json.load(open('$API_DIR/stats.json')); sys.exit(not (d['axes']==[] and d['hist']['hook'] is None and d['rt']['hook'] is None))\""
fi

if want a07; then
    echo "--- A-07: bad options refused before the bus is touched"
    "$ECM_RUN" --iface "$IF_M" --n 2 --api --api-dir /proc/ecm_api_no > "$LOG/er_a07a.log" 2>&1; R=$?
    chk "A-07 --api-dir not usable -> rc $R, message" "[ $R != 0 ] && grep -q 'cannot use /proc/ecm_api_no' '$LOG/er_a07a.log'"
    "$ECM_RUN" --iface "$IF_M" --n 2 --api-dir "$API_DIR" > "$LOG/er_a07b.log" 2>&1; R=$?
    chk "A-07 --api-dir without --api -> rc $R" "[ $R != 0 ] && grep -q 'need --api' '$LOG/er_a07b.log'"
    "$ECM_RUN" --iface "$IF_M" --n 2 --api --api-cpus 1- > "$LOG/er_a07c.log" 2>&1; R=$?
    chk "A-07 --api-cpus 1- -> rc $R" "[ $R != 0 ] && grep -q 'api-cpus LIST' '$LOG/er_a07c.log'"
    chk "A-07 none of them touched the bus (no 'expected WKC' line)" "! grep -qh 'expected WKC' '$LOG/er_a07a.log' '$LOG/er_a07b.log' '$LOG/er_a07c.log'"
fi

if want a08; then
    echo "--- A-08: kill -9 mid-run"
    fresh
    start a08 30 "${MOT[@]}" --api --api-dir "$API_DIR"
    sleep 3; kill -9 "$EP"; wait "$EP" 2>/dev/null; sb_stop
    $CHECK schema "$API_DIR/stats.json" > "$LOG/schema_a08.txt"; S=$?
    chk "A-08 last stats.json whole after kill -9, final false ($(cat "$LOG/schema_a08.txt"))" \
        "[ $S = 0 ] && python3 -c \"import json,sys; sys.exit(json.load(open('$API_DIR/stats.json'))['final'])\""
fi

if want a09; then
    echo "--- A-09: golden with --api (8 nodes, motion + io)"
    fresh
    ECM_EXTRA="--api --api-dir $API_DIR" "$ROOT/tools/golden/check_golden.sh" > "$LOG/golden_api.txt" 2>&1; R=$?
    tail -1 "$LOG/golden_api.txt" | sed 's/^/    /'
    chk "A-09 golden structure identical with --api" "[ $R = 0 ]"
    cp /tmp/ecm_run_golden.log "$LOG/er_a09_golden.log"
    $CHECK schema "$API_DIR/stats.json" > /dev/null; S=$?
    $CHECK compare "$API_DIR/stats.json" "$LOG/er_a09_golden.log" > "$LOG/compare_a09.txt"; C=$?
    chk "A-09 two groups: schema ok, D-10 ($(tail -1 "$LOG/compare_a09.txt"))" \
        "[ $S = 0 ] && [ $C = 0 ] && python3 -c \"import json,sys; sys.exit(len(json.load(open('$API_DIR/stats.json'))['groups'])!=2)\""
fi

if want a10; then
    echo "--- A-10: --no-diag"
    fresh
    start a10 3 --api --api-dir "$API_DIR" --no-diag
    finish
    $CHECK schema "$API_DIR/stats.json" > /dev/null; S=$?
    chk "A-10 --no-diag: schema ok, no slave counters, sources.monitor.diag false" \
        "[ $S = 0 ] && python3 -c \"import json,sys; d=json.load(open('$API_DIR/stats.json')); s=d['slaves'][0]; sys.exit(not ('rx_err' not in s and 'al' not in s and s['recover']=='ok' and d['sources']['monitor']['diag'] is False))\""
fi

if want a11; then
    echo "--- A-11: rt configuration in stats.json"
    fresh
    start a11 3 --api --api-dir "$API_DIR"
    finish
    cp "$API_DIR/stats.json" "$LOG/stats_a11.json"
    cat > "$LOG/rtcheck.py" <<'EOF2'
import json, os, sys
d = json.load(open(sys.argv[1]))["rt"]
def rd(p):
    try:
        return open(p).read().strip()
    except OSError:
        return None
bad = []
iso = rd("/sys/devices/system/cpu/isolated")
if d["isolated_cpus"] != iso: bad.append("isolated_cpus %r != %r" % (d["isolated_cpus"], iso))
drv = rd("/sys/devices/system/cpu/cpuidle/current_driver")
want = None if drv in (None, "none") else drv
if d["cpuidle"]["driver"] != want: bad.append("cpuidle.driver %r != %r" % (d["cpuidle"]["driver"], want))
states, dis = 0, []
while True:
    n = rd("/sys/devices/system/cpu/cpu3/cpuidle/state%d/name" % states)
    if n is None: break
    if rd("/sys/devices/system/cpu/cpu3/cpuidle/state%d/disable" % states) not in (None, "0"): dis.append(n)
    states += 1
if d["cpuidle"]["states"] != states or d["cpuidle"]["states_disabled"] != dis: bad.append("idle states of core 3")
if d["kernel"] != os.uname().release: bad.append("kernel %r" % d["kernel"])
words = open("/proc/cmdline").read().split()
if not set(d["cmdline_flags"]) <= set(words): bad.append("cmdline_flags not from /proc/cmdline")
for w in ("isolcpus=", "nohz_full=", "rcu_nocbs=", "irqaffinity=", "cpuidle.off="):
    if any(x.startswith(w) for x in words) and not any(x.startswith(w) for x in d["cmdline_flags"]): bad.append("missing " + w)
print("rtcheck: " + ("ok" if not bad else "; ".join(bad)))
sys.exit(1 if bad else 0)
EOF2
    python3 "$LOG/rtcheck.py" "$LOG/stats_a11.json" > "$LOG/rtcheck.txt"; R=$?
    chk "A-11 rt.* match this machine ($(cat $LOG/rtcheck.txt))" "[ $R = 0 ]"
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); d['rt']['kernel']+='-x'; json.dump(d,open(sys.argv[2],'w'))" \
        "$LOG/stats_a11.json" "$LOG/stats_a11_neg.json"
    python3 "$LOG/rtcheck.py" "$LOG/stats_a11_neg.json" > /dev/null; R=$?
    chk "A-11 negative control: a changed value is detected" "[ $R = 1 ]"
    chk "A-11 schema ok with the rt keys" "$CHECK schema $LOG/stats_a11.json > /dev/null"
fi

rm -rf "$API_DIR"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
