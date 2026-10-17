#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_d14_11_6.sh -- Phase 11.6 D-14: the remote diagnostics API next to the
# two REAL IS620N axes in motion (claude/giai_doan_11_ke_hoach.md §7).
#
# THIS ENABLES THE DRIVES: the motion and every safety rule are those of
# Phase 10.9 W-08 (tools/gd10/run_w_10_9.sh: motors clamped, nothing on the
# shafts, hand next to the breaker, drive limits written and read back by w00
# before anything is enabled, both axes CSP (1 - cos) 1/2 rev at 0.2 Hz).
# 1 ms cycle, af_packet, the i226 as in IS-07.
#
#   sudo -E PROFILE=P0 tools/gd11/run_d14_11_6.sh             2 x 1800 s + rig (~65 min)
#   sudo -E PROFILE=P0 DUR=120 tools/gd11/run_d14_11_6.sh     short pass (~6 min)
#   sudo -E PROFILE=P0 SIM=1 DUR=60 tools/gd11/run_d14_11_6.sh   rehearsal on veth, no drive
#
#   rig   run_is7_11_5.sh CASES=rig (NM off, GRO/GSO/TSO off, rx-usecs 0,
#         rt_profile --expect PROFILE, 100 Mbit/s Full)
#   off   run_w_10_9.sh w00 + w08 for DUR s, API off. d14_load.sh runs too
#         and must find NO stats.json (negative control of the API checks)
#   on    the same with --api --api-dir API_DIR --api-cpus 1-2 and d14_load.sh:
#         2 stats.json readers 2 Hz, sdo_read 20 Hz, sdo_read of 0x6064 on
#         both drives 1 Hz, connect/hold/disconnect loop
#
# Pass:
#   - both runs pass every W-08 check (0 overrun, NOFRAME <= 2, DC LOCKED,
#     no axis error, S6 walk-down, both axes end Switch on disabled, 0x603F 0)
#   - on: stats.json followed the motion: both axes seen in Operation
#     enabled, position span of each axis >= RES/4 in the samples
#   - on: SDO in OP: the IS620N has 2 FMMUs, no cyclic mailbox (D-15): every
#     sdo_read answered no_mailbox at once (SIM, 3 FMMUs: ok), 0 sdo_failed,
#     0 timeout; [API] 0 write error,
#     0 handoff drop
#   - on vs off: tracking max per axis <= off x 1.2 + RES/1000; wake_jitter
#     p99.99 <= off + max(2 us, 20 %); occupancy reported (SDO cost, D-01),
#     max <= 50 % of the cycle
#   - off: d14_load.sh found no stats.json (negative control). The RT-harm
#     negative control (FIFO burst on core 3) is D-01neg on veth: not done
#     with the drives enabled
#
# Env: PROFILE DUR API_DIR LOG and every run_w_10_9.sh variable (RES MAX_RPM TQ ...)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PROFILE=${PROFILE:-}
DUR=${DUR:-1800}
RES=${RES:-8388608}
API_DIR=${API_DIR:-/run/ecmaster_d14}
case "$PROFILE" in P0|P1) ;; *) echo "PROFILE=P0 or PROFILE=P1 (as booted)"; exit 3 ;; esac
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
LOG=${LOG:-$ROOT/log_gd11_d14_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
CSV=$LOG/results.csv; REP=$LOG/report.md
echo "case,profile,variant,load,metric,n,min,avg,p50,p99,p999,p9999,max,unit,extra" > "$CSV"
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

SIM=${SIM:-0}; export SIM
if [ "$SIM" != 1 ]; then
    # Asked ONCE here, before anything; both W-08 runs then get YES=1. A run
    # that does not start (rc 3, no er_w08.log) stops the whole D-14: the
    # second run must never enable the drives without this answer.
    cat <<EOT
=== Phase 11.6 D-14 on the i226: THE DRIVES WILL BE ENABLED, twice (off, then on), ${DUR} s each ===
  [ ] Motors clamped, nothing on the shafts, nobody near them
  [ ] Your hand next to the breaker (it is the emergency stop) for the whole run
  [ ] Cable: enP1p1s0 -> drive 1 IN, drive 1 OUT -> drive 2 IN
EOT
    read -r -p "Type yes to start: " ans
    # keep letters only: a terminal may add invisible bytes (CR, the bracketed-paste
    # markers ESC[200~ / ESC[201~) that print as nothing (14/10: 'yes' refused twice)
    if [ "$(printf '%s' "$ans" | tr -cd '[:alpha:]' | tr '[:upper:]' '[:lower:]')" != yes ]; then
        echo "stopped: the answer was not yes (bytes: $(printf '%s' "$ans" | od -An -c | tr -s ' '))"; exit 3
    fi
fi
if [ "$SIM" = 1 ]; then echo "=== SIM=1: rehearsal on veth (virtual drives), no rig"; else
echo "=== rig"
PROFILE=$PROFILE CASES=rig LOG=$LOG/rig "$HERE/run_is7_11_5.sh" > "$LOG/rig.txt" 2>&1
R=$?; grep -E 'PASS|FAIL|INFO' "$LOG/rig.txt"
[ $R = 0 ] || { echo "rig not ready: fix the FAIL lines above ($LOG/rig.txt)"; exit 2; }
fi

echo "=== off: W-08 ${DUR} s, API off"
rm -rf "$API_DIR"
YES=1 DURING="$HERE/d14_load.sh $API_DIR $DUR $LOG/load_off" CASES=w08 DUR8=$DUR RES=$RES LOG=$LOG/off \
    "$ROOT/tools/gd10/run_w_10_9.sh"
R_OFF=$?
[ -s "$LOG/off/er_w08.log" ] || { echo "the off run did not reach w08 (rc $R_OFF): stopping, the on run is NOT started"; exit 2; }
grep -q 'w00 FAILED' "$LOG/off/report.md" && { echo "w00 failed: limits not in the drives, stopping"; exit 1; }

echo "=== on: W-08 ${DUR} s, API on + clients"
rm -rf "$API_DIR"
YES=1 EXTRA_ARGS="--api --api-dir $API_DIR --api-cpus 1-2" DURING="$HERE/d14_load.sh $API_DIR $DUR $LOG/load_on" \
    CASES=w08 DUR8=$DUR RES=$RES LOG=$LOG/on "$ROOT/tools/gd10/run_w_10_9.sh"
R_ON=$?

echo; echo "=== D-14 verdict"
EO=$LOG/off/er_w08.log; EN=$LOG/on/er_w08.log
chk "off: every W-08 check passed ($LOG/off/report.md)" "[ $R_OFF = 0 ]"
chk "on: every W-08 check passed ($LOG/on/report.md)" "[ $R_ON = 0 ]"
chk "off: no stats.json, the API checks below cannot pass by themselves (negative control)" "[ -f $LOG/load_off/no_api ]"

python3 - "$LOG/load_on/samples.jsonl" "$RES" > "$LOG/samples_check.txt" 2>&1 <<'EOF'
import json, sys
res = int(sys.argv[2]); oe = [False, False]; pos = [[], []]; n = err = 0
for ln in open(sys.argv[1]):
    d = json.loads(ln); n += 1
    if "error" in d: err += 1; continue
    for k, a in enumerate(d["axes"][:2]):
        if a.get("state") in ("OPERATION_ENABLED", "Operation enabled"): oe[k] = True
        if a.get("pos") is not None and a.get("state") in ("OPERATION_ENABLED", "Operation enabled"): pos[k].append(a["pos"])
span = [max(p) - min(p) if p else 0 for p in pos]
print("samples %d (read errors %d), Operation enabled seen %s, position span %s inc" % (n, err, oe, span))
sys.exit(0 if all(oe) and all(s >= res // 4 for s in span) else 1)
EOF
R=$?; chk "on: stats.json followed the motion: $(cat "$LOG/samples_check.txt")" "[ $R = 0 ]"

python3 - "$LOG/load_on/sdo6064.txt" "$RES" > "$LOG/sdo_check.txt" 2>&1 <<'EOF'
import json, sys
res = int(sys.argv[2]); v = {1: [], 2: []}; tot = {1: 0, 2: 0}
for ln in open(sys.argv[1]):
    p = ln.split(None, 2)
    if len(p) < 3: continue
    s = int(p[1]); tot[s] += 1
    try:
        d = json.loads(p[2])
        if d.get("ok"): v[s].append(d["value"])
    except ValueError:
        pass
span = {s: (max(x) - min(x) if x else 0) for s, x in v.items()}
print("ok %d/%d and %d/%d, span %d / %d inc" % (len(v[1]), tot[1], len(v[2]), tot[2], span[1], span[2]))
sys.exit(0 if all(tot[s] and len(v[s]) >= 0.9 * tot[s] and span[s] >= res // 4 for s in (1, 2)) else 1)
EOF
R=$?
# rate_limited is the API's own limiter (docs/api.md §10.1: the 20 Hz spam + these
# reads exceed it), a legitimate refusal; anything else than the two is a failure
# lines after the last real answer are reads that met ecm_run stopping
# ("not_ready ... stopping", then the socket gone): not counted
F6=$LOG/load_on/sdo6064_counted.txt
awk '{l[NR]=$0} /"error":"(no_mailbox|rate_limited)"|"ok":true/ {last=NR} END {for (i=1;i<=last;i++) print l[i]}' \
    "$LOG/load_on/sdo6064.txt" > "$F6" 2>/dev/null
NOMB=$(grep -c '"error":"no_mailbox"' "$F6"); TOT=$(grep -c . "$F6"); RL=$(grep -c '"error":"rate_limited"' "$F6")
if [ "$SIM" = 1 ]; then echo "  [INFO] SIM: the virtual drives have no live 0x6064 in their object dictionary: $(cat "$LOG/sdo_check.txt")"
else
    # 11.6 D-15: the IS620N has 2 FMMUs, no cyclic mailbox in OP -> refused at once
    chk "on: SDO 0x6064 refused at once on both drives (no_mailbox $NOMB + rate_limited $RL of $TOT, D-15)" \
        "[ ${NOMB:-0} -gt 0 ] && [ \$(( ${NOMB:-0} + ${RL:-0} )) = '$TOT' ]"
fi

SP=$(tail -1 "$LOG/load_on/spam.txt" 2>/dev/null)
if [ "$SIM" = 1 ]; then WANT=ok; else WANT=no_mailbox; fi
chk "on: sdo_read 20 Hz, every answer '$WANT' or rate_limited ($SP)" "python3 -c \"
import re,sys; s='''$SP'''; g=lambda k: int((re.search(k+'=([0-9]+)', s) or [0,0])[1])
sys.exit(not (g('sent') > 0 and g('$WANT') > 0 and g('$WANT') + g('rate_limited') == g('sent') - g('not_ready') and g('sdo_failed') == 0 and g('timeout') == 0))\""
chk "on: [API] 0 stats.json write error, 0 handoff drop" \
    "grep -q '\[API\] stats.json writes=[1-9][0-9]* errors=0 ' $EN && grep '\[API\] handoff' $EN | grep -qvE '/[1-9]'"

trk () { grep "  \[CIA402\] axis $2 (" "$1" | head -1 | grep -o ' track_max=[0-9]*' | cut -d= -f2; }
for a in 0 1; do
    o=$(trk "$EO" $a); n=$(trk "$EN" $a)
    chk "axis $a tracking max on ${n:-?} <= off ${o:-?} x 1.2 + $((RES / 1000)) inc" \
        "[ -n '$o' ] && [ -n '$n' ] && [ $n -le \$(( $o * 12 / 10 + $RES / 1000 )) ]"
done

python3 "$HERE/is_parse.py" ecm_run "$EO" d14 "$PROFILE" off motion >> "$CSV"
python3 "$HERE/is_parse.py" ecm_run "$EN" d14 "$PROFILE" on motion >> "$CSV"
col () { awk -F, -v v="$1" -v m="$2" -v c="$3" '$3==v && $5==m {print $c}' "$CSV" | head -1; }
wo=$(col off wake_jitter 12); wn=$(col on wake_jitter 12)
chk "wake_jitter p99.99 on ${wn:-?} <= off ${wo:-?} + max(2 us, 20 %)" \
    "python3 -c 'import sys; o=float(\"${wo:-nan}\"); n=float(\"${wn:-nan}\"); sys.exit(not n <= o + max(2.0, 0.2 * o))'"
oo=$(col off occupancy 12); on=$(col on occupancy 12); om=$(col on occupancy 13)
echo "    occupancy p99.99 off ${oo:-?} / on ${on:-?} us (SDO cost, as D-01)"
chk "on: occupancy max ${om:-?} us <= 50 % of the 1000 us cycle" "python3 -c 'import sys; sys.exit(not float(\"${om:-1e9}\") <= 500)'"

{
    echo "# Phase 11.6 D-14 — API next to the two IS620N in motion — $(date '+%Y-%m-%d %H:%M')"
    echo
    echo "- host $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?'); profile $PROFILE; ${DUR} s per run"
    echo "- W-08 motion: both axes CSP (1 - cos) 1/2 rev 0.2 Hz, 1 ms, af_packet"
    echo
    for a in 0 1; do echo "- axis $a tracking max: off $(trk "$EO" $a) / on $(trk "$EN" $a) inc"; done
    echo "- $(cat "$LOG/samples_check.txt")"
    echo "- SDO 0x6064: $(cat "$LOG/sdo_check.txt")"
    echo "- spam: $SP"
    echo
    grep -h '\[API\]' "$EN" | sed 's/^/    /'
    echo
    python3 "$HERE/is_report.py" "$CSV"
} > "$REP" 2>/dev/null
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo; cat "$REP"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
