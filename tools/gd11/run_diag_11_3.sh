#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_diag_11_3.sh -- Phase 11.3: ecm_diag as a client of ecm_run's API
# (claude/giai_doan_11_ke_hoach.md §4, docs/api.md §11).
#
#   sudo -E tools/gd11/run_diag_11_3.sh
#
# Rig: two virtual IS620N (soft_bus --cia402, is620n.cal, test object 0x2008),
# ENI eni_2servo_mm, --pdo-scan, hook cia402, CSP (1 - cos) on both axes.
#
#   G-01  ecm_diag --api while ecm_run runs: bus RUN, rt line, 2 axes
#         OPERATION_ENABLED, no finding, exit 0; --json is the file itself
#   G-02  drive fault 0x0E08 on axis 2:0 (no EMCY, like the IS620N): FAULT,
#         "code 0x0E08 Er.E08 Synchronization loss ... (from 0x603F)", exit 1;
#         the healthy G-01 view is its negative control
#   G-03  ecm_run stopped: FINAL, exit 2; killed: STALE after --max-age, exit 2;
#         no stats.json: exit 2
#   G-04  ctl.sock commands: info (sha256 = sha256sum), sdo read, sdo write
#         (refused without allowlist: exit 1; allowed: read back "same", exit 0),
#         abort code shown, --json, no socket: exit 2, bad arguments: exit 2
#   G-05  --standalone refused while the API is alive, also with --no-diag (no
#         snapshot file: the Phase 7 check alone would have let it through);
#         after ecm_run ends it reads the bus again (exit 0/1, not 3)
#   G-06  --api --watch refreshes
#   G-07  run_l5_diag.sh (the Phase 7 snapshot mode) still passes
#
# Env: SOFT_BUS ECM_RUN ECM_DIAG IF_M IF_S CASES SB_PRIO SB_CPU API_DIR LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
ECM_DIAG=${ECM_DIAG:-$ROOT/apps/ecm_diag/ecm_diag}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}
CASES=${CASES:-"g01 g02 g03 g04 g05 g06 g07"}
API_DIR=${API_DIR:-/run/ecmaster_test}
ENI=$ROOT/config/eni/eni_2servo_mm.enicfg
CAL=$ROOT/config/profiles/is620n.cal
CTL=/tmp/soft_bus_11_3.ctl
VMAX=8388608
D=("$ECM_DIAG" --api "$API_DIR")

LOG=${LOG:-log_gd11_diag_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
want () { case " $CASES " in *" $1 "*) return 0;; esac; return 1; }

for f in "$SOFT_BUS" "$ECM_RUN" "$ECM_DIAG"; do [ -x "$f" ] || { echo "missing $f"; exit 2; }; done
for i in "$IF_M" "$IF_S"; do
    ip link show "$i" >/dev/null 2>&1 || {
        echo "interface $i missing:"
        echo "  sudo ip link add $IF_M type veth peer name $IF_S && sudo ip link set $IF_M up && sudo ip link set $IF_S up"
        exit 2; }
done
for p in ecm_run soft_bus; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 2; }; done

IS=$LOG/is620n_sim.prof
cp "$ROOT/config/profiles/is620n_min.prof" "$IS"
grep -q '^obj 0x6502 ' "$IS" || { echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro ad030000"; } >> "$IS"
for o in "0x6072 16 b80b" "0x6065 32 e76aa301" "0x607F 32 00000080" "0x2008 16 0a00"; do
    set -- $o
    grep -q "^obj $1 " "$IS" || { echo "obj $1 var 1"; echo "sub $1 0 bits $2 rw $3"; } >> "$IS"
done
ALLOW=$LOG/allow.json
printf '{"schema":1,"allow":[{"vendor":"0x00100000","product":"0x000C0108","index":"0x2008","sub":0,"type":"u16","min":0,"max":1000}]}\n' > "$ALLOW"

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
      --pdo-set "1:0x607F:0=$VMAX,2:0x607F:0=$VMAX" --hook cia402 --cia402-lead 20 --axis 1:0,2:0 --axis-modes csp
      --cia402-script "0.5 enable all; 1.5 cos 1 100000 1; 1.5 cos 2 100000 1")
start () {
    local tag=$1 dur=$2; shift 2
    sb_start "$tag"
    "$ECM_RUN" "${BASE[@]}" --duration-sec "$dur" --diag-file "$LOG/diag_$tag.txt" --api --api-dir "$API_DIR" "$@" \
        > "$LOG/er_$tag.log" 2>&1 &
    EP=$!
    for _ in $(seq 1 100); do grep -q 'Starting cyclic loop' "$LOG/er_$tag.log" 2>/dev/null && break; sleep 0.1; done
}
finish () { wait "$EP"; RC=$?; sb_stop; }
dg () { local tag=$1; shift; "$@" > "$LOG/$tag.txt" 2>&1; echo $? > "$LOG/$tag.rc"; }
rc () { cat "$LOG/$1.rc"; }
has () { grep -q -- "$2" "$LOG/$1.txt"; }

echo "=== Phase 11.3 ecm_diag through the API, log $LOG"

if want g01 || want g02 || want g04 || want g05 || want g06; then
    echo "--- one 14 s run (--no-diag: no Phase 7 snapshot file, for G-05)"
    rm -rf "$API_DIR"; rm -f /tmp/ecm_diag.txt
    start main 14 --no-diag --api-allow "$ALLOW"
    sleep 2.5
    if want g01; then
        dg g01 "${D[@]}"
        chk "G-01 live view: exit 0" "[ \$(rc g01) = 0 ]"
        chk "G-01 bus RUN, rt line with core 3 and the kernel" "has g01 '^bus RUN' && has g01 '^rt: core 3, isolated' && has g01 \"kernel $(uname -r)\""
        chk "G-01 2 axes OPERATION_ENABLED, no finding" "[ \$(grep -c 'OPERATION_ENABLED' $LOG/g01.txt) = 2 ] && has g01 '^findings: none'"
        chk "G-01 timing table with percentiles" "has g01 '^  wake_jitter ' && has g01 '^  hook '"
        dg g01j "${D[@]}" --json
        chk "G-01 --json is the stats.json document" "python3 -c \"import json; d=json.load(open('$LOG/g01j.txt')); assert d['schema']==1\""
    fi
    if want g04; then
        dg g04i "${D[@]}" info
        SHA=$(sha256sum "$ENI" | cut -d' ' -f1)
        chk "G-04 info: exit 0, ENI sha256 = sha256sum, 2 slaves, allowlist 1 entry" \
            "[ \$(rc g04i) = 0 ] && has g04i 'sha256 $SHA' && [ \$(grep -c '^  slave ' $LOG/g04i.txt) = 2 ] && has g04i 'allowlist: .*, 1 entries'"
        dg g04r "${D[@]}" sdo read 1 0x1018 1 u32
        chk "G-04 sdo read 0x1018:1 = 1048576, exit 0" "[ \$(rc g04r) = 0 ] && has g04r 'u32 = 1048576 (raw 00001000'"
        dg g04n "${D[@]}" sdo write 1 0x2009 0 u16 3
        chk "G-04 sdo write not in the allowlist: refused, exit 1" "[ \$(rc g04n) = 1 ] && has g04n 'refused: not_allowed'"
        dg g04w "${D[@]}" sdo write 2 0x2008 0 u16 0x2A
        chk "G-04 sdo write 0x2008 = 0x2A: read back same, exit 0" "[ \$(rc g04w) = 0 ] && has g04w 'written 42, read back 42 (same)'"
        dg g04a "${D[@]}" sdo read 1 0x5555 0
        chk "G-04 abort shown, exit 1" "[ \$(rc g04a) = 1 ] && has g04a 'abort 0x06020000'"
        dg g04j "${D[@]}" --json sdo read 1 0x2008 0 u16
        chk "G-04 --json reply" "python3 -c \"import json; d=json.load(open('$LOG/g04j.txt')); assert d['ok'] and d['value']==10\""
        dg g04x "${D[@]}" sdo read 1
        chk "G-04 bad arguments: usage, exit 2" "[ \$(rc g04x) = 2 ] && has g04x 'usage:'"
    fi
    if want g05; then
        chk "G-05 precondition: no Phase 7 snapshot file (--no-diag)" "[ ! -e /tmp/ecm_diag.txt ]"
        dg g05 "$ECM_DIAG" --standalone "$IF_M" --api "$API_DIR"
        chk "G-05 --standalone refused while the API is alive, exit 3" "[ \$(rc g05) = 3 ] && has g05 \"API is alive in $API_DIR\""
    fi
    if want g06; then
        timeout 2.6 "${D[@]}" --watch 0.5 > "$LOG/g06.txt" 2>&1
        NF=$(grep -c 'ecm_run pid' "$LOG/g06.txt")
        chk "G-06 --watch refreshes ($NF frames in 2.6 s at 0.5 s)" "[ $NF -ge 4 ]"
    fi
    if want g02; then
        sbctl "drv_fault 1 0 0x0E08"; sleep 1
        dg g02 "${D[@]}"
        chk "G-02 axis 2:0 FAULT, exit 1" "[ \$(rc g02) = 1 ] && grep -q '^  2:0 .*FAULT' $LOG/g02.txt"
        chk "G-02 code 0x0E08 Er.E08 from 0x603F (no EMCY)" "has g02 'code 0x0E08 Er.E08 Synchronization loss.*(from 0x603F)' && ! grep -q 'EMCY 0x0E08' $LOG/g02.txt"
        chk "G-02 axis 1:0 untouched" "grep -q '^  1:0 .*OPERATION_ENABLED' $LOG/g02.txt"
    fi
    finish
    if want g03; then
        dg g03f "${D[@]}"
        chk "G-03 ecm_run stopped: FINAL, exit 2" "[ \$(rc g03f) = 2 ] && has g03f 'FINAL (ecm_run has stopped)'"
    fi
    if want g05; then
        dg g05b "$ECM_DIAG" --standalone "$IF_M" --api "$API_DIR"
        chk "G-05 after ecm_run: --standalone reads the bus again (exit $(rc g05b), not 3)" "[ \$(rc g05b) != 3 ] && has g05b 'standalone'"
    fi
fi

if want g03; then
    echo "--- G-03: killed, missing"
    rm -rf "$API_DIR"
    start g03 30
    sleep 2; kill -9 "$EP"; wait "$EP" 2>/dev/null; sb_stop
    dg g03l "${D[@]}" --max-age 3
    sleep 3.5
    dg g03s "${D[@]}" --max-age 3
    chk "G-03 killed: still read as live right after (exit $(rc g03l)), STALE after --max-age, exit 2" \
        "[ \$(rc g03l) != 2 ] && [ \$(rc g03s) = 2 ] && has g03s 'STALE'"
    rm -rf "$API_DIR"
    dg g03m "${D[@]}"
    chk "G-03 no stats.json: exit 2 with a hint" "[ \$(rc g03m) = 2 ] && has g03m 'is ecm_run running with --api'"
    dg g03c "${D[@]}" info
    chk "G-03 no ctl.sock: exit 2" "[ \$(rc g03c) = 2 ]"
fi

if want g07; then
    echo "--- G-07: Phase 7 snapshot mode (run_l5_diag.sh)"
    (cd "$ROOT/apps/ecm_diag" && SB_ARGS=--no-sm-wd ./run_l5_diag.sh --no-tx-ts) > "$LOG/g07.txt" 2>&1; R=$?
    tail -1 "$LOG/g07.txt" | sed 's/^/    /'
    chk "G-07 run_l5_diag.sh passes" "[ $R = 0 ]"
fi

rm -rf "$API_DIR"
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
