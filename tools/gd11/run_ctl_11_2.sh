#!/usr/bin/env bash
# ==========================================================================
# run_ctl_11_2.sh -- Phase 11.2: ctl.sock of `ecm_run --api` on soft_bus
# (claude/giai_doan_11_ke_hoach.md §3, docs/api.md §10). Functional checks;
# timing (D-01, D-11, D-14) is step 11.5 on the Jetson.
#
#   sudo -E tools/gd11/run_ctl_11_2.sh
#
# Rig: two virtual IS620N (soft_bus --cia402, is620n.cal) with a test object
# 0x2008 (u16 rw, default 10), ENI eni_2servo_mm + the drive limits of 10.9
# (eni_limits.py: 0x6072/0x6065/0x607F as PS InitCmds), --pdo-scan, hook
# cia402, CSP (1 - cos) on both axes, an allowlist with 0x2008 (allowed),
# 0x607F (in the RxPDO 0x1702) and 0x6072 (ENI InitCmd).
#
#   C-01  --api off: no ctl.sock (D-06); negative control: with --api it exists, 0660
#   C-02  info: identity, ENI sha256 == sha256sum of the file, PDO table, InitCmds;
#         negative control: the sha256 of another ENI differs
#   C-03  sdo_read: typed, size mismatch, abort code + text of a missing object
#   C-04  sdo_write refusals (D-08): 0x1C12, 0x6040, 0x607F (allowlisted but in the
#         RxPDO), 0x6072 (allowlisted but an ENI InitCmd), not listed, out of range;
#         no allowlist; no PDO table; an allowlist naming 0x6040 refused at start;
#         and none of them reached the slave (0x6072 unchanged)
#   C-05  sdo_write of an allowlisted object while both axes run CSP (D-02, D-05):
#         read back, axes stay OPERATION_ENABLED, no ZERO/PARTIAL/OVER WKC,
#         stats.json keeps publishing during the SDO traffic
#   C-06  limits: 5th client refused, burst rate-limited, over-long line,
#         a stopped client (SIGSTOP), a client killed mid-request (D-03 part)
#   C-07  recovery fence: slave 2 power-cycled (path B) while a client reads its
#         objects at 15 Hz: every reply well-formed, slave back in OP, the fence used
#   C-08  diag_dump: files, stats copy valid; the 33rd dump refused
#   C-09  golden with --api while a client polls `info` (no SDO): datagrams identical
#
# Env: SOFT_BUS ECM_RUN IF_M IF_S CASES SB_PRIO SB_CPU API_DIR LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"c01 c02 c03 c04 c05 c06 c07 c08 c09"}
API_DIR=${API_DIR:-/run/ecmaster_test}
SOCK=$API_DIR/ctl.sock
CHECK="python3 $HERE/stats_check.py"
CTLPY="python3 $HERE/ecm_ctl.py --sock $SOCK"
ENI0=$ROOT/config/eni/eni_2servo_mm.enicfg
CAL=$ROOT/config/profiles/is620n.cal
CTL=/tmp/soft_bus_11_2.ctl
VMAX=8388608

LOG=${LOG:-log_gd11_ctl_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
want () { case " $CASES " in *" $1 "*) return 0;; esac; return 1; }
jget () { python3 -c "import json,sys; d=json.loads(sys.stdin.read()); print(eval(sys.argv[1]))" "$1"; }

for f in "$SOFT_BUS" "$ECM_RUN"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done
for p in ecm_run soft_bus; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 2; }; done

# virtual IS620N: limit/PP/HM objects as run_w_10_9.sh SIM=1, plus the test object 0x2008
IS=$LOG/is620n_sim.prof
cp "$ROOT/config/profiles/is620n_min.prof" "$IS"
grep -q '^obj 0x6502 ' "$IS" || { echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro ad030000"; } >> "$IS"
for o in "0x6072 16 b80b" "0x6065 32 e76aa301" "0x607F 32 00000080" "0x6081 32 a0860100" "0x6083 32 40420f00" \
         "0x6084 32 40420f00" "0x6098 8 21" "0x609A 32 40420f00" "0x607C 32 00000000" "0x2008 16 0a00"; do
    set -- $o
    grep -q "^obj $1 " "$IS" || { echo "obj $1 var 1"; echo "sub $1 0 bits $2 rw $3"; } >> "$IS"
done
ENI=$LOG/eni_2servo_mm_limits.enicfg
python3 "$ROOT/tools/eni/eni_limits.py" "$ENI0" -o "$ENI" --torque-permille 150 --ferr 1048576 --max-vel "$VMAX" > "$LOG/eni_limits.txt" || exit 2
ALLOW=$LOG/allow.json
cat > "$ALLOW" <<'EOF'
{"schema": 1, "allow": [
  {"vendor": "0x00100000", "product": "0x000C0108", "index": "0x2008", "sub": 0, "type": "u16", "min": 0, "max": 1000,
   "note": "virtual test parameter (soft_bus profile of this runner)"},
  {"vendor": "0x00100000", "product": "0x000C0108", "index": "0x607F", "sub": 0, "type": "u32", "min": 0, "max": 8388608,
   "note": "mapped in the RxPDO 0x1702: must be refused anyway"},
  {"vendor": "0x00100000", "product": "0x000C0108", "index": "0x6072", "sub": 0, "type": "u16", "min": 0, "max": 3000,
   "note": "written by the ENI (InitCmd): must be refused anyway"}
]}
EOF

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
sbctl () { printf '%s\n' "$*" | dd of="$CTL" conv=nocreat,notrunc oflag=nonblock status=none; }
trap 'sb_stop' EXIT

BASE=(--iface "$IF_M" --n 2 --eni "$ENI" --pdo-scan --emcy-map "$ROOT/config/emcy/is620n.emcy" --no-tx-ts
      --pdo-set "1:0x607F:0=$VMAX,2:0x607F:0=$VMAX")
MOT=(--hook cia402 --cia402-lead 20 --axis 1:0,2:0 --axis-modes csp
     --cia402-script "0.5 enable all; 1.5 cos 1 100000 1; 1.5 cos 2 100000 1")
API=(--api --api-dir "$API_DIR")

# start TAG DURATION ARGS... (background, PID in EP); waits for the cyclic loop
start () {
    local tag=$1 dur=$2; shift 2
    sb_start "$tag"
    "$ECM_RUN" --duration-sec "$dur" --diag-file "$LOG/diag_$tag.txt" "$@" > "$LOG/er_$tag.log" 2>&1 &
    EP=$!
    for _ in $(seq 1 100); do grep -q 'Starting cyclic loop' "$LOG/er_$tag.log" 2>/dev/null && break; sleep 0.1; done
}
finish () { wait "$EP"; RC=$?; sb_stop; }
fresh () { rm -rf "$API_DIR"; }
err () { jget "d.get('error')"; }

echo "=== Phase 11.2 ctl.sock on soft_bus (2 virtual IS620N, hook cia402), log $LOG"

if want c01; then
    echo "--- C-01: --api off + negative control"
    fresh; rm -f /run/ecmaster/ctl.sock
    start c01 3 "${BASE[@]}"
    sleep 1; S1=$( [ -S /run/ecmaster/ctl.sock ] && echo yes || echo no )
    finish
    chk "C-01 --api off: rc 0, no ctl.sock" "[ $RC = 0 ] && [ $S1 = no ]"
    start c01n 3 "${BASE[@]}" "${API[@]}"
    sleep 1; M=$(stat -c %a "$SOCK" 2>/dev/null); T=$(stat -c %F "$SOCK" 2>/dev/null)
    finish
    chk "C-01 negative control: with --api the socket exists, mode 0660 ('$T' $M)" "[ '$M' = 660 ] && [ '$T' = socket ]"
    chk "C-01 socket removed at exit" "[ ! -e '$SOCK' ]"
fi

if want c02 || want c03 || want c04 || want c05 || want c06 || want c08; then
    echo "--- C-02..C-06, C-08: one 20 s run, CSP on both axes, allowlist $ALLOW"
    fresh
    start main 20 "${BASE[@]}" "${MOT[@]}" "${API[@]}" --api-allow "$ALLOW"
    sleep 2
    if want c02; then
        $CTLPY info --pdo > "$LOG/info.json"; R=$?
        SHA=$(sha256sum "$ENI" | cut -d' ' -f1); SHA0=$(sha256sum "$ENI0" | cut -d' ' -f1)
        chk "C-02 info ok, 2 slaves IS620N, ENI path and source" \
            "[ $R = 0 ] && jget \"len(d['info']['slaves'])==2 and d['info']['slaves'][1]['vendor']=='0x00100000' and d['info']['eni']['source']=='eni_2servo_mm.xml' and d['info']['eni']['path']=='$ENI'\" < $LOG/info.json | grep -q True"
        GOT=$(jget "d['info']['eni']['sha256']" < "$LOG/info.json")
        chk "C-02 ENI sha256 == sha256sum ($GOT)" "[ '$GOT' = '$SHA' ]"
        chk "C-02 negative control: another ENI gives another sha256" "[ '$GOT' != '$SHA0' ]"
        chk "C-02 PDO table from the bus (--pdo-scan), RxPDO of slave 1 has 0x607F" \
            "jget \"d['info']['pdo']['source']=='bus' and any(e['slave']==1 and e['dir']=='out' and e['index']=='0x607F' for e in d['info']['pdo']['entries'])\" < $LOG/info.json | grep -q True"
        chk "C-02 InitCmds counted (14 of the ENI + 6 limits = 20)" \
            "jget \"d['info']['eni']['coe_initcmds']\" < $LOG/info.json | grep -qx 20"
        chk "C-02 allowlist listed (3 entries)" "jget \"len(d['info']['allow']['entries'])\" < $LOG/info.json | grep -qx 3"
    fi
    if want c03; then
        R=$($CTLPY read 1 0x1018 1 u32); echo "$R" > "$LOG/c03_vendor.json"
        chk "C-03 0x1018:1 u32 = 0x00100000" "echo '$R' | jget \"d['value']\" | grep -qx 1048576"
        R=$($CTLPY read 2 0x2008 0 u16)
        chk "C-03 0x2008 u16 = 10 (profile default)" "echo '$R' | jget \"d['value']\" | grep -qx 10"
        R=$($CTLPY read 2 0x2008 0 u8)
        chk "C-03 wrong type -> size_mismatch with the raw bytes" "echo '$R' | jget \"d['error']=='size_mismatch' and d['raw']=='0A00'\" | grep -q True"
        R=$($CTLPY read 1 0x5555 0)
        chk "C-03 missing object -> sdo_failed, abort 0x06020000 + text" \
            "echo '$R' | jget \"d['error']=='sdo_failed' and d['abort']=='0x06020000' and 'does not exist' in d['msg']\" | grep -q True"
    fi
    if want c04; then
        B6072=$($CTLPY read 1 0x6072 0 u16 | jget "d['value']")
        for x in "0x1C12 u8 1 denied_comm_area" "0x6040 u16 15 denied_hard" "0x6860 i8 8 denied_hard" "0x607F u32 1000 denied_rxpdo" \
                 "0x6072 u16 100 denied_initcmd" "0x2009 u16 1 not_allowed" "0x2008 u16 1001 out_of_range" "0x2008 u32 5 type_mismatch"; do
            set -- $x
            E=$($CTLPY write 1 "$1" 0 "$2" "$3" | err)
            chk "C-04 write $1 = $3 -> $4" "[ '$E' = '$4' ]"
        done
        A6072=$($CTLPY read 1 0x6072 0 u16 | jget "d['value']")
        chk "C-04 refused writes never reached the slave: 0x6072 still $B6072 (ENI limit 150 = 15.0 %)" "[ '$A6072' = '$B6072' ] && [ '$A6072' = 150 ]"
    fi
    if want c05; then
        S0=$(jget "d['pub_seq']" < "$API_DIR/stats.json")
        R1=$($CTLPY write 1 0x2008 0 u16 42); R2=$($CTLPY write 2 0x2008 0 u16 "0x3E8")
        for k in $(seq 10); do $CTLPY read 1 0x2008 0 u16 > /dev/null; $CTLPY read 2 0x2008 0 u16 > /dev/null; done
        S1=$(jget "d['pub_seq']" < "$API_DIR/stats.json")
        R3=$($CTLPY read 1 0x2008 0 u16)
        echo "$R1" > "$LOG/c05_w1.json"; echo "$R2" > "$LOG/c05_w2.json"
        chk "C-05 write 42 / 1000 at OP, read back equal (D-02)" \
            "echo '$R1' | jget \"d['readback_ok']\" | grep -q True && echo '$R2' | jget \"d['readback']==1000\" | grep -q True && echo '$R3' | jget \"d['value']\" | grep -qx 42"
        chk "C-05 stats.json kept publishing during 22 SDO (pub_seq $S0 -> $S1)" "[ \$(( $S1 - $S0 )) -ge 5 ]"
        chk "C-05 both axes still OPERATION_ENABLED, no axis error" \
            "jget \"all(a['state']=='OPERATION_ENABLED' and a['err']=='none' for a in d['axes'])\" < $API_DIR/stats.json | grep -q True"
    fi
    if want c06; then
        HPS=; for k in 1 2 3 4; do $CTLPY hold 3 & HPS="$HPS $!"; done
        sleep 0.5
        R=$($CTLPY info 2>&1)
        wait $HPS
        chk "C-06 5th client refused: too_many_clients" "echo '$R' | grep -q too_many_clients"
        python3 - "$SOCK" > "$LOG/c06_burst.txt" <<'EOF'
import socket, sys, json
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.settimeout(5)
s.sendall(b"".join(b'{"id":%d,"cmd":"info"}\n' % i for i in range(40)))
buf = b""
while buf.count(b"\n") < 40:
    d = s.recv(1 << 20)
    if not d: break
    buf += d
r = [json.loads(l) for l in buf.split(b"\n") if l]
print("served=%d limited=%d" % (sum(x["ok"] for x in r), sum(x.get("error") == "rate_limited" for x in r)))
EOF
        chk "C-06 burst of 40: rate limited ($(cat $LOG/c06_burst.txt))" "grep -qE 'served=(19|20|21|22) limited=(18|19|20|21)' $LOG/c06_burst.txt"
        sleep 1.2
        R=$(python3 -c "print('x'*3000)" | (cat; echo '{"id":9,"cmd":"info"}') | python3 -c "
import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect('$SOCK'); s.settimeout(5); s.sendall(sys.stdin.buffer.read())
b=b''
while b.count(b'\n')<2:
    d=s.recv(1<<20)
    if not d: break
    b+=d
print(b.decode().replace('\n',' | ')[:200])")
        chk "C-06 over-long line -> line_too_long, next request served" "echo '$R' | grep -q 'line_too_long.*\"id\":9,\"ok\":true'"
        $CTLPY hold 30 & HP=$!; sleep 0.3; kill -STOP $HP
        $CTLPY spam 2 15 1 0x2008 0 u16 > "$LOG/c06_spam.txt"; R=$?
        kill -CONT $HP; kill $HP 2>/dev/null; wait $HP 2>/dev/null
        chk "C-06 a stopped client does not block the others ($(cat $LOG/c06_spam.txt))" "[ $R = 0 ]"
        $CTLPY spam 5 15 2 0x2008 0 u16 > /dev/null & KP=$!; sleep 0.4; kill -9 $KP; wait $KP 2>/dev/null
        R=$($CTLPY read 2 0x2008 0 u16)
        chk "C-06 client killed mid-request: the next one is served" "echo '$R' | jget \"d['ok']\" | grep -q True"
    fi
    if want c08; then
        R=$($CTLPY dump); echo "$R" > "$LOG/c08_dump.json"
        F1=$(echo "$R" | jget "d['files'][0]"); F2=$(echo "$R" | jget "d['files'][1]")
        chk "C-08 diag_dump: diag text + stats copy" "[ -s '$F1' ] && grep -q 'ecm_run' '$F1' && $CHECK schema '$F2' > /dev/null"
        n=0; for k in $(seq 31); do $CTLPY dump > /dev/null && n=$((n+1)); sleep 0.06; done
        E=$($CTLPY dump | err)
        chk "C-08 32 dumps per run, the 33rd refused ($n more ok, then '$E')" "[ $n = 31 ] && [ '$E' = dump_failed ]"
    fi
    finish
    chk "C-0x run: rc 0" "[ $RC = 0 ]"
    $CHECK schema "$API_DIR/stats.json" > /dev/null; S=$?
    chk "C-05 final stats.json: schema ok, no ZERO/PARTIAL/OVER WKC (D-05)" \
        "[ $S = 0 ] && jget \"all(g['zero']==0 and g['partial']==0 and g['over']==0 for g in d['groups'])\" < $API_DIR/stats.json | grep -q True"
    grep "\[API\] ctl.sock" "$LOG/er_main.log" | sed 's/^/    /'
    want c04 && chk "C-04 refusals counted in [API] (>= 8)" "grep -qE 'refused_writes=([89]|[1-9][0-9])' $LOG/er_main.log"
fi

if want c04; then
    echo "--- C-04: no allowlist, no PDO table, a forbidden allowlist"
    fresh
    start c04a 4 "${BASE[@]}" "${API[@]}"
    sleep 1; E=$($CTLPY write 1 0x2008 0 u16 1 | err)
    finish
    chk "C-04 no --api-allow: 0x2008 -> not_allowed" "[ '$E' = not_allowed ]"
    fresh
    sb_start c04b
    "$ECM_RUN" --iface "$IF_M" --n 2 --no-tx-ts --duration-sec 4 "${API[@]}" --api-allow "$ALLOW" > "$LOG/er_c04b.log" 2>&1 & EP=$!
    sleep 2.5; E=$($CTLPY write 1 0x2008 0 u16 1 | err)
    finish
    chk "C-04 no --eni / --pdo-scan: no PDO table -> denied_no_pdo_table" "[ '$E' = denied_no_pdo_table ]"
    sed 's/"0x2008"/"0x6040"/' "$ALLOW" > "$LOG/allow_bad.json"
    "$ECM_RUN" "${BASE[@]}" "${API[@]}" --api-allow "$LOG/allow_bad.json" > "$LOG/er_c04c.log" 2>&1; R=$?
    chk "C-04 allowlist naming 0x6040: refused at start, bus not touched (rc $R)" \
        "[ $R != 0 ] && grep -q 'never written through the API' $LOG/er_c04c.log && ! grep -q 'expected WKC' $LOG/er_c04c.log"
fi

if want c07; then
    echo "--- C-07: recovery fence (slave 2 power-cycled, path B) under SDO traffic"
    fresh
    start c07 14 "${BASE[@]}" "${API[@]}"
    sleep 2
    $CTLPY spam 8 15 2 0x2008 0 u16 > "$LOG/c07_spam.txt" &
    SP=$!; sleep 1
    sbctl "drop_node 1"; sleep 2; sbctl "restore_node 1"
    wait $SP
    finish
    sed 's/^/    /' "$LOG/c07_spam.txt"
    chk "C-07 every reply well-formed: ok / not_ready / busy / timeout / sdo_failed only" \
        "grep -qE '^spam: sent=[0-9]+( (ok|not_ready|busy|timeout|sdo_failed)=[0-9]+)+ latency' $LOG/c07_spam.txt"
    chk "C-07 slave 2 recovered (path B) and back in OP" "grep -q 'slave 2 back in OP' $LOG/er_c07.log"
    chk "C-07 the fence was taken by the recovery" "grep -qE 'recovery fence waits=[1-9]' $LOG/er_c07.log"
    chk "C-07 SDO served again after the recovery (ok > 0)" "grep -qE ' ok=[1-9]' $LOG/c07_spam.txt"
    chk "C-07 rc 0" "[ $RC = 0 ]"
fi

if want c09; then
    echo "--- C-09: golden with --api while a client polls info"
    fresh
    ECM_EXTRA="--api --api-dir $API_DIR" "$ROOT/tools/golden/check_golden.sh" > "$LOG/golden_ctl.txt" 2>&1 & GP=$!
    while kill -0 $GP 2>/dev/null; do $CTLPY info > /dev/null 2>&1; sleep 0.05; done
    wait $GP; R=$?
    tail -1 "$LOG/golden_ctl.txt" | sed 's/^/    /'
    chk "C-09 golden structure identical with --api and info polling" "[ $R = 0 ]"
    chk "C-09 the poller was served" "grep -qE 'ctl.sock clients=[1-9]' /tmp/ecm_run_golden.log"
fi

rm -rf "$API_DIR"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
