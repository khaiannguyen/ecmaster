#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# run_d15_11_6.sh -- Phase 11.6 D-15: SDO in OP on a slave whose ESC has no
# FMMU left for the SM1 status (found by D-14 on the IS620N, 14/10).
#
# SOEM's cyclic mailbox learns "a reply is waiting" from SM1 status 0x080D,
# mapped into the process image by one more FMMU. The IS620N has 2 FMMUs
# (0x0004, tools/gd11/esc_info.py), both used by the process data: the bit
# stays 0, every SDO in SAFE-OP/OP timed out after 500 ms and its blocking
# mailbox frames were retried by the RT thread. Since 11.6 ecm_mailbox does
# not enable cyclic mode there and the API answers "no_mailbox" at once.
#
#   sudo -E tools/gd11/run_d15_11_6.sh                 veth, ~1 min
#   sudo -E REAL=1 tools/gd11/run_d15_11_6.sh          + the two IS620N on the
#        i226, NO axis enabled (no hook), control power is enough, ~1 min
#
#   f3      virtual drives with 3 FMMUs: cyclic on 2/2, sdo_read ok
#   f2      virtual drives with 2 FMMUs (soft_bus --fmmus N=2): cyclic on 0/2,
#           one start-up line per slave, sdo_read -> no_mailbox in < 50 ms,
#           startup InitCmds still ok (OP reached, 0x6060 set)
#   f2neg   the same with --mbx-cyclic-force (pre-11.6): sdo_read -> sdo_failed
#           after the 500 ms timeout -- the D-14 symptom, reproduced on veth
#   real    (REAL=1) the IS620N: esc_info.py says 2 FMMUs, ecm_run cyclic on
#           0/2, sdo_read -> no_mailbox, 0 overrun over 20 s with 10 Hz reads
#
# Env: REAL IFACE IF_M IF_S LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOFT_BUS=$ROOT/tools/soft_bus/soft_bus; ECM_RUN=$ROOT/apps/ecm_run/ecm_run
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}; IFACE=${IFACE:-enP1p1s0}; REAL=${REAL:-0}
ENI=$ROOT/config/eni/eni_2servo.enicfg
LOG=${LOG:-$ROOT/log_gd11_d15_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$LOG"; LOG=$(cd "$LOG" && pwd)
PASS=0; FAIL=0
ok  () { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E)"; exit 3; }
for p in ecm_run soft_bus; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 3; }; done
ip link show "$IF_M" >/dev/null 2>&1 || { echo "missing $IF_M: create the veth pair first"; exit 3; }
IS=$LOG/is620n_sim.prof
cp "$ROOT/config/profiles/is620n_min.prof" "$IS"
grep -q '^obj 0x6502 ' "$IS" || { echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro ad030000"; } >> "$IS"
CTL=("$HERE/ecm_ctl.py")

veth () {   # tag soft_bus-extra ecm_run-extra...
    local tag=$1 sbx=$2; shift 2
    "$SOFT_BUS" --iface "$IF_S" --n 2 --dc 32 --profile 1="$IS" --profile 2="$IS" $sbx > "$LOG/sb_$tag.log" 2>&1 &
    local sb=$!; sleep 0.5
    "$ECM_RUN" --iface "$IF_M" --n 2 --eni "$ENI" --pdo-scan --no-tx-ts --duration-sec 12 \
        --api --api-dir "$LOG/api_$tag" --api-cpus 1-2 "$@" > "$LOG/er_$tag.log" 2>&1 &
    local ep=$!
    for _ in $(seq 100); do [ -S "$LOG/api_$tag/ctl.sock" ] && grep -q 'all slaves in OPERATIONAL' "$LOG/er_$tag.log" && break; sleep 0.1; done
    sleep 1
    python3 "${CTL[@]}" --sock "$LOG/api_$tag/ctl.sock" read 1 0x1018 1 u32 > "$LOG/read_$tag.json" 2>&1
    wait $ep; kill -INT $sb 2>/dev/null; wait $sb 2>/dev/null
}
ms () { python3 -c "import json,sys; d=json.load(open('$1')); print(d.get('ms', -1))" 2>/dev/null || echo -1; }
err () { python3 -c "import json,sys; d=json.load(open('$1')); print('ok' if d.get('ok') else d.get('error'))" 2>/dev/null || echo '?'; }

echo "=== f3: 3 FMMUs (as before)"
veth f3 ""
chk "f3 cyclic mailbox enabled on 2/2" "grep -q 'cyclic mailbox enabled on 2/2' $LOG/er_f3.log"
chk "f3 sdo_read ok ($(err $LOG/read_f3.json))" "[ \"\$(err $LOG/read_f3.json)\" = ok ]"

echo "=== f2: 2 FMMUs (the IS620N)"
veth f2 "--fmmus 1=2 --fmmus 2=2"
chk "f2 OP reached, start-up InitCmds unaffected" "grep -q 'all slaves in OPERATIONAL' $LOG/er_f2.log"
chk "f2 cyclic mailbox enabled on 0/2, one line per slave" \
    "grep -q 'cyclic mailbox enabled on 0/2' $LOG/er_f2.log && [ \$(grep -c 'cyclic mailbox NOT enabled: SM1 status needs FMMU 2, the ESC has 2' $LOG/er_f2.log) = 2 ]"
chk "f2 sdo_read -> no_mailbox in < 50 ms ($(err $LOG/read_f2.json))" \
    "[ \"\$(err $LOG/read_f2.json)\" = no_mailbox ] && grep -q 'ctl.sock .* max=[0-9.]* ms' $LOG/er_f2.log && \
     awk '/ctl.sock clients/ {for(i=1;i<=NF;i++) if(\$i ~ /^max=/) {sub(/max=/,\"\",\$i); exit !(\$i+0 < 50)}}' $LOG/er_f2.log"

echo "=== f2neg: 2 FMMUs + --mbx-cyclic-force (pre-11.6, negative control)"
veth f2neg "--fmmus 1=2 --fmmus 2=2" --mbx-cyclic-force
chk "f2neg the D-14 symptom: sdo_failed 'no answer' ($(err $LOG/read_f2neg.json))" \
    "[ \"\$(err $LOG/read_f2neg.json)\" = sdo_failed ] && grep -q 'no answer' $LOG/read_f2neg.json"

if [ "$REAL" = 1 ]; then
    echo "=== real: the two IS620N on $IFACE, no axis enabled"
    python3 "$HERE/esc_info.py" "$IFACE" 2 > "$LOG/esc_info.txt" 2>&1; cat "$LOG/esc_info.txt" | sed 's/^/    /'
    chk "real esc_info: 2 FMMUs on both" "[ \$(grep -c 'FMMUs 2 ' $LOG/esc_info.txt) = 2 ]"
    c0=$(cat /sys/class/net/$IFACE/statistics/rx_crc_errors)
    "$ECM_RUN" --iface "$IFACE" --n 2 --eni "$ENI" --pdo-scan --duration-sec 30 \
        --api --api-dir "$LOG/api_real" --api-cpus 1-2 > "$LOG/er_real.log" 2>&1 &
    ep=$!
    for _ in $(seq 150); do grep -q 'all slaves in OPERATIONAL' "$LOG/er_real.log" 2>/dev/null && break; sleep 0.1; done
    sleep 2
    python3 "${CTL[@]}" --sock "$LOG/api_real/ctl.sock" read 1 0x1018 1 u32 > "$LOG/read_real.json" 2>&1
    python3 "${CTL[@]}" --sock "$LOG/api_real/ctl.sock" spam 20 10 2 0x6064 0 i32 > "$LOG/spam_real.txt" 2>&1
    wait $ep
    c1=$(cat /sys/class/net/$IFACE/statistics/rx_crc_errors)
    chk "real cyclic mailbox enabled on 0/2" "grep -q 'cyclic mailbox enabled on 0/2' $LOG/er_real.log"
    chk "real sdo_read -> no_mailbox ($(err $LOG/read_real.json))" "[ \"\$(err $LOG/read_real.json)\" = no_mailbox ]"
    chk "real 0 overrun, 0 NOFRAME with 10 Hz reads ($(grep -o 'overrun=[0-9]*' $LOG/er_real.log | head -1))" \
        "grep -q '\[GROUP_MOTION\] .*overrun=0 ' $LOG/er_real.log && grep -q 'noframe=0 ' $LOG/er_real.log"
    echo "  [INFO] rx_crc_errors +$((c1 - c0)) during the 30 s run; spam: $(tail -1 $LOG/spam_real.txt)"
fi
chown -R "${SUDO_USER:-root}:" "$LOG" 2>/dev/null
echo "RESULT: $PASS pass, $FAIL fail   (log: $LOG)"
[ $FAIL = 0 ]
