#!/usr/bin/env bash
# ==========================================================================
# run_bringup_10_0.sh — Phase 10.0: first contact with the two IS620N drives,
# R-01 .. R-06 of claude/giai_doan_10_ke_hoach.md section 2.2. NOTHING here
# enables an axis: no --hook cia402, no --cia402-script, no --pdo-set; the
# outputs stay 0 (controlword 0 = Disable voltage), so a drive cannot leave
# "Switch on disabled" because of this script.
#
#   Real drives, control power only (L1C/L2C), the evening of 10.0:
#     sudo -E IS620N_ESI=$HOME/esi/IS620N-Ecat_v2.6.9.xml tools/gd10/run_bringup_10_0.sh
#     sudo -E ... LINK_SEC=60 R05_SEC=30 tools/gd10/run_bringup_10_0.sh      short pass
#   Rehearsal on veth, two virtual IS620N on soft_bus (same steps, same report):
#     sudo -E SIM=1 IS620N_ESI=... tools/gd10/run_bringup_10_0.sh
#     sudo -E SIM=1 SIM_REV=0x00010002 ...   negative control: R-02 must FAIL
#
#   r01  link: speed/duplex 100/Full, no carrier change and no error counter
#        increase during LINK_SEC (plan: 10 min), EEE off (check_env)
#   r02  apps/ecm_peek (read only, never above PRE-OP): SII of every slave,
#        identity, 0x1018, ~150 SDO uploads -> R-02 (identity vs ENI/ESI),
#        R-03 (tools/esi/esi_check.py on each SII dump), R-04 (0x6502, PDO
#        assignment/mapping vs the ESI, 0x1C32/0x1C33, limits)
#   r05  ecm_run --eni eni_2servo --pdo-scan --pdo-dump R05_SEC s, no hook:
#        OP, InitCmds, ENI == bus (= R-06), DC LOCKED, WKC, real state
#        transition times, statusword/0x603F/0x6064 at the end; tshark
#        capture (CAP=1) for the record
#   r09  (not in the default STEPS) cycle limits: the same R-05 run at each
#        cycle of R09_CYCLES (default "2000 500" us) for R09_SEC s, with an
#        ENI copy for that cycle (tools/eni/eni_cycle.py: sync0_ns + 0x09A0)
#   r06  ecm_run --axis 1:0,2:0 --axis-modes csp WITHOUT --hook: the master's
#        axis checks against the real drive (0x6502 read, bits bound), still
#        not driven
#
# Output: LOG/report.md (+ peek.txt, sii/, esi_check_N.txt, er_r05.log, ...).
# Exit 0 when no FAIL. DIFF = a difference to record, not an error.
#
# NIC error counters (ethtool -S) are taken before/after every ecm_run.
# Env: IFACE (enP1p1s0) N (2) ENI IS620N_ESI STEPS LINK_SEC R05_SEC CAP YES
#      R09_CYCLES R09_SEC
#      SIM SIM_REV IF_S SB_PRIO SB_CPU ECM_RUN ECM_PEEK SOFT_BUS LOG
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SIM=${SIM:-0}
if [ "$SIM" = 1 ]; then IFACE=${IFACE:-veth_m}; else IFACE=${IFACE:-enP1p1s0}; fi
IF_S=${IF_S:-veth_s}
N=${N:-2}
ENI=${ENI:-$ROOT/config/eni/eni_2servo.enicfg}
IS620N_ESI=${IS620N_ESI:-}
STEPS=${STEPS:-"r01 r02 r05 r06"}
if [ "$SIM" = 1 ]; then LINK_SEC=${LINK_SEC:-5}; else LINK_SEC=${LINK_SEC:-600}; fi
R05_SEC=${R05_SEC:-60}
R09_CYCLES=${R09_CYCLES:-"2000 500"}; R09_SEC=${R09_SEC:-60}
CAP=${CAP:-1}
YES=${YES:-0}
SIM_REV=${SIM_REV:-}
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}   # SIM: like the soaks (FIFO 79, core 2)
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
ECM_PEEK=${ECM_PEEK:-$ROOT/apps/ecm_peek/ecm_peek}
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
EMCY_MAP=$ROOT/config/emcy/is620n.emcy
LOG=${LOG:-log_gd10_bringup_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
REP=$LOG/report.md
CTL=/tmp/soft_bus_10_0.ctl
NFAIL=0

say () { echo "$*" | tee -a "$REP"; }
v ()   { say ""; say "VERDICT $1 $2 $3"; [ "$2" = FAIL ] && NFAIL=$((NFAIL+1)); return 0; }

# ---------------------------------------------------------------- checks
for f in "$ECM_RUN" "$ECM_PEEK" "$ENI"; do [ -e "$f" ] || { echo "missing $f (make all caps)"; exit 2; }; done
if [ -n "$IS620N_ESI" ] && [ ! -f "$IS620N_ESI" ]; then
    echo "IS620N_ESI=$IS620N_ESI is not a file (give the real path of the vendor ESI)"; exit 2
fi
ip link show "$IFACE" >/dev/null 2>&1 || {
    echo "interface $IFACE missing"
    [ "$SIM" = 1 ] && echo "  sudo ip link add veth_m type veth peer name veth_s && sudo ip link set veth_m up && sudo ip link set veth_s up"
    exit 2; }
for p in ecm_run soft_bus ecm_peek ecm_diag l4_test; do
    pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first (two masters on one bus)"; exit 2; }
done
lsmod 2>/dev/null | grep -q '^ec_master' && { echo "IgH ec_master is loaded: sudo rmmod ec_generic ec_master"; exit 2; }

if [ "$SIM" != 1 ]; then
    cat <<EOF

=== Phase 10.0 bring-up on $IFACE: before you continue ===
  [ ] Only CONTROL power (L1C/L2C) on; main power L1/L2 OFF (MCB down)
  [ ] Motors loose on the bench / clamped, nothing on the shafts
  [ ] Cable: $IFACE -> drive 1 CN3 (IN), drive 1 CN4 (OUT) -> drive 2 CN3
  [ ] Drive parameters checked in InoDriverShop (EtherCAT mode, torque and
      speed limits, E-stop DI) -- written down in the 10.0 log
  [ ] No other master on $IFACE (TwinCAT laptop unplugged)
This script never enables an axis (outputs stay 0).
EOF
    if [ "$YES" != 1 ]; then
        read -r -p "Type yes to start: " ans
        [ "$ans" = yes ] || { echo "stopped"; exit 3; }
    fi
fi

{
    echo "# Phase 10.0 bring-up — $(date '+%Y-%m-%d %H:%M:%S')"
    echo
    echo "- host: $(uname -n) $(uname -r); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
    echo "- interface: $IFACE; expected slaves: $N; ENI: $(basename "$ENI")"
    echo "- ESI: ${IS620N_ESI:-none (R-03 skipped, R-04 without the ESI column)}"
    [ "$SIM" = 1 ] && echo "- **SIMULATION** (soft_bus on $IF_S, two virtual IS620N${SIM_REV:+, revision $SIM_REV}) -- not the real drives"
    echo "- steps: $STEPS"
} > "$REP"
cat "$REP"

# --------------------------------------------------------- SIM: soft_bus
SBP=
sim_start () {
    [ "$SIM" = 1 ] || return 0
    rm -f "$CTL"
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    "${sb[@]}" --iface "$IF_S" --n "$N" --dc 32 --no-sm-wd --ctl "$CTL" $SB_PROFS > "$LOG/soft_bus_$1.log" 2>&1 &
    SBP=$!
    sleep 0.5
    kill -0 $SBP 2>/dev/null || { echo "soft_bus did not start:"; cat "$LOG/soft_bus_$1.log"; exit 2; }
}
sim_stop () { [ -n "$SBP" ] && { kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null; SBP=; }; return 0; }
trap 'sim_stop' EXIT

if [ "$SIM" = 1 ]; then
    # Same profile as K-02 (run_axes_10_3.sh): from the vendor ESI when given,
    # else the reduced committed one; 0x6502 = 0x1A5 is a TEST ASSUMPTION.
    if [ -n "$IS620N_ESI" ]; then
        python3 "$ROOT/tools/esi/esi2profile.py" "$IS620N_ESI" -o "$LOG/is620n.prof" 2> "$LOG/esi2profile.err" || { cat "$LOG/esi2profile.err"; exit 2; }
        IS=$LOG/is620n.prof
        if grep -q '^sub 0x6502 0 ' "$IS"; then
            sed -i 's/^\(sub 0x6502 0 bits 32 [a-z_]*\) 00000000$/\1 a5010000/' "$IS"
        else
            { echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro a5010000"; } >> "$IS"
        fi
    else
        IS=$LOG/is620n_min_6502.prof
        { cat "$ROOT/config/profiles/is620n_min.prof"; echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro a5010000"; } > "$IS"
    fi
    if [ -n "$SIM_REV" ]; then
        sed -i "s/^\(identity vendor [0-9a-fA-Fx]* product [0-9a-fA-Fx]* rev\) [0-9a-fA-Fx]*/\1 $SIM_REV/" "$IS"
        # the SII and 0x1018:03 follow the profile; check that they changed
        grep -q "rev $SIM_REV" "$IS" || { echo "SIM_REV: could not patch the profile identity"; exit 2; }
        sed -i "s/^\(sub 0x1018 3 bits 32 ro\) .*/\1 $(printf '%08x' "$SIM_REV" | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')/" "$IS"
    fi
    SB_PROFS=""
    for s in $(seq 1 "$N"); do SB_PROFS="$SB_PROFS --profile $s=$IS --cia402 $s"; done
fi

# ------------------------------------------------------------------ r01
r01 () {
    say ""; say "## R-01 link on $IFACE (${LINK_SEC} s)"
    local et sp du ln cc0 cc1 bad=""
    et=$(ethtool "$IFACE" 2>/dev/null)
    sp=$(echo "$et" | awk -F': ' '/Speed:/{print $2}')
    du=$(echo "$et" | awk -F': ' '/Duplex:/{print $2}')
    ln=$(echo "$et" | awk -F': ' '/Link detected:/{print $2}')
    say "- speed ${sp:-?}, duplex ${du:-?}, link ${ln:-?}"
    ethtool --show-eee "$IFACE" 2>/dev/null | grep -i "EEE status" | sed 's/^\s*/- /' | tee -a "$REP"
    [ "$SIM" != 1 ] && { "$ROOT/tools/jetson/check_env.sh" --iface "$IFACE" > "$LOG/check_env.txt" 2>&1
                         say "- check_env: $(grep -c '\[FAIL\]' "$LOG/check_env.txt") FAIL, $(grep -c '\[WARN\]' "$LOG/check_env.txt") WARN (check_env.txt)"; }
    cc0=$(cat "/sys/class/net/$IFACE/carrier_changes" 2>/dev/null || echo 0)
    ethtool -S "$IFACE" > "$LOG/ethtool_S_before.txt" 2>/dev/null
    say "- watching for ${LINK_SEC} s ..."
    sleep "$LINK_SEC"
    cc1=$(cat "/sys/class/net/$IFACE/carrier_changes" 2>/dev/null || echo 0)
    ethtool -S "$IFACE" > "$LOG/ethtool_S_after.txt" 2>/dev/null
    local errs
    errs=$(join <(awk -F': ' '{gsub(/ /,"",$1); print $1, $2}' "$LOG/ethtool_S_before.txt" | sort) \
                <(awk -F': ' '{gsub(/ /,"",$1); print $1, $2}' "$LOG/ethtool_S_after.txt" | sort) 2>/dev/null |
           awk '$1 ~ /err|crc|missed|align|symbol|fifo|over|drop/ && $3 != $2 {printf "%s +%d; ", $1, $3-$2}')
    say "- carrier_changes during the watch: $((cc1 - cc0)); error counters increased: ${errs:-none}"
    if [ "$SIM" = 1 ]; then
        [ $((cc1 - cc0)) = 0 ] && v R-01 PASS "SIM (veth): no carrier change (speed not graded)" || v R-01 FAIL "carrier changed"
        return
    fi
    [ "$ln" = yes ] || bad="no link; "
    [ "$sp" = "100Mb/s" ] || bad="${bad}speed ${sp:-?} (expected 100Mb/s); "
    [ "$du" = Full ] || bad="${bad}duplex ${du:-?}; "
    [ $((cc1 - cc0)) = 0 ] || bad="${bad}$((cc1 - cc0)) carrier change(s); "
    [ -z "$errs" ] || bad="${bad}error counters: $errs"
    [ -z "$bad" ] && v R-01 PASS "100/Full, stable ${LINK_SEC} s, no error counter increase" || v R-01 FAIL "$bad"
}

# ------------------------------------------------------- r02 (R-02/03/04)
r02 () {
    say ""; say "## ecm_peek (read only: SII dump, PRE-OP, SDO uploads)"
    sim_start peek
    local t0 rc
    t0=$(date +%s.%N)
    "$ECM_PEEK" --iface "$IFACE" --sii-dir "$LOG/sii" > "$LOG/peek.txt" 2> "$LOG/peek.err"
    rc=$?
    sim_stop
    say "- rc $rc, $(awk "BEGIN{printf \"%.1f\", $(date +%s.%N) - $t0}") s, $(grep -c '^SDO .* size' "$LOG/peek.txt") SDO values, $(grep -c ' ABORT ' "$LOG/peek.txt") aborts (peek.txt)"
    [ -s "$LOG/peek.err" ] && sed 's/^/- stderr: /' "$LOG/peek.err" | tee -a "$REP"
    if [ $rc != 0 ]; then v R-02 FAIL "ecm_peek rc $rc (see peek.err)"; return; fi

    # R-03: every SII dump against the ESI
    say ""; say "## R-03 SII of the drive vs ESI (tools/esi/esi_check.py)"
    if [ -z "$IS620N_ESI" ]; then
        v R-03 SKIP "no IS620N_ESI"
    else
        local s worst=PASS idf
        for f in "$LOG"/sii/sii_*.bin; do
            s=$(basename "$f" .bin); s=${s#sii_}
            python3 "$ROOT/tools/esi/esi_check.py" "$IS620N_ESI" "$f" > "$LOG/esi_check_$s.txt" 2>&1
            idf=$(awk '/^\[identity\]/{x=1;next} /^\[/{x=0} x && /\[FAIL\]/' "$LOG/esi_check_$s.txt" | wc -l)
            say "- slave $s: $(grep -o 'RESULT:.*' "$LOG/esi_check_$s.txt") (esi_check_$s.txt)"
            grep '\[FAIL\]\|\[WARN\]' "$LOG/esi_check_$s.txt" | sed 's/^ */  - /' | tee -a "$REP"
            if [ "$idf" != 0 ]; then worst=FAIL
            elif grep -q '\[FAIL\]' "$LOG/esi_check_$s.txt" && [ $worst = PASS ]; then worst=DIFF; fi
        done
        case $worst in
            PASS) v R-03 PASS "SII = ESI on every slave" ;;
            DIFF) v R-03 DIFF "identity = ESI; other differences listed above (record them, V-01 on real EEPROM)" ;;
            *)    v R-03 FAIL "identity in the SII differs from the ESI" ;;
        esac
    fi

    say ""
    python3 "$HERE/bringup_report.py" --n "$N" --eni "$ENI" --peek "$LOG/peek.txt" ${IS620N_ESI:+--esi "$IS620N_ESI"} \
        > "$LOG/report_peek.md" 2>&1
    sed '/^## Summary/,$d' "$LOG/report_peek.md" >> "$REP"
    grep '^VERDICT' "$LOG/report_peek.md"
    grep -q '^VERDICT [^ ]* FAIL' "$LOG/report_peek.md" && NFAIL=$((NFAIL + $(grep -c '^VERDICT [^ ]* FAIL' "$LOG/report_peek.md")))
    return 0
}

# ------------------------------------------------------------- ecm_run
GETS=""
for s in $(seq 1 "$N"); do GETS="$GETS${GETS:+,}$s:0x6041:0,$s:0x603F:0,$s:0x6064:0"; done
ER_COMMON=(--iface "$IFACE" --n "$N" --eni "$ENI" --pdo-scan --emcy-map "$EMCY_MAP")
[ "$SIM" = 1 ] && ER_COMMON+=(--no-tx-ts)
for a in "${ER_COMMON[@]}"; do      # belt and braces: nothing that could drive an axis
    case $a in --hook|--cia402-script|--pdo-set|--xchg-out|--xchg-sine) echo "refusing: $a"; exit 2 ;; esac
done

r05 () {
    say ""; say "## R-05 ecm_run ${R05_SEC} s (no hook)"
    sim_start r05
    local tp="" cap=""
    if [ "$CAP" = 1 ] && command -v tshark >/dev/null; then
        cap=/tmp/bringup_r05_$$.pcapng
        tshark -i "$IFACE" -w "$cap" -f "ether proto 0x88a4" > "$LOG/tshark.log" 2>&1 &
        tp=$!
        sleep 1.5
    fi
    say "- \`ecm_run ${ER_COMMON[*]} --pdo-dump --pdo-get $GETS --duration-sec $R05_SEC\`"
    ethtool -S "$IFACE" > "$LOG/ethtool_S_r05_before.txt" 2>/dev/null
    "$ECM_RUN" "${ER_COMMON[@]}" --pdo-dump --pdo-get "$GETS" --duration-sec "$R05_SEC" \
        --diag-file "$LOG/diag_r05.txt" > "$LOG/er_r05.log" 2>&1
    say "- rc $?"
    ethtool -S "$IFACE" > "$LOG/ethtool_S_r05_after.txt" 2>/dev/null
    if [ -n "$tp" ]; then
        sleep 0.5; kill -INT $tp 2>/dev/null; wait $tp 2>/dev/null
        mv "$cap" "$LOG/r05.pcapng" 2>/dev/null && say "- capture: r05.pcapng ($(du -h "$LOG/r05.pcapng" | cut -f1))"
    fi
    sim_stop
}

r09 () {
    local cyc eni
    for cyc in $R09_CYCLES; do
        say ""; say "## R-09 cycle ${cyc} us, ${R09_SEC} s (no hook)"
        eni=$LOG/$(basename "$ENI" .enicfg)_${cyc}us.enicfg
        python3 "$ROOT/tools/eni/eni_cycle.py" "$ENI" "$cyc" -o "$eni" | sed 's/^/- /' | tee -a "$REP"
        [ -s "$eni" ] || { v "R-09/${cyc}us" FAIL "eni_cycle.py could not derive the ENI"; continue; }
        sim_start "r09_$cyc"
        local er=("${ER_COMMON[@]}")
        er[5]=$eni                                   # --eni value (index: --iface IF --n N --eni ENI)
        ethtool -S "$IFACE" > "$LOG/ethtool_S_r09_${cyc}_before.txt" 2>/dev/null
        "$ECM_RUN" "${er[@]}" --motion-cycle-us "$cyc" --pdo-get "$GETS" --duration-sec "$R09_SEC" \
            --diag-file "$LOG/diag_r09_$cyc.txt" > "$LOG/er_r09_$cyc.log" 2>&1
        say "- rc $?"
        ethtool -S "$IFACE" > "$LOG/ethtool_S_r09_${cyc}_after.txt" 2>/dev/null
        sim_stop
    done
}

r06 () {
    say ""; say "## R-06b ecm_run --axis 1:0..$N:0 --axis-modes csp, no hook (5 s)"
    sim_start r06
    local ax=""
    for s in $(seq 1 "$N"); do ax="$ax${ax:+,}$s:0"; done
    "$ECM_RUN" "${ER_COMMON[@]}" --axis "$ax" --axis-modes csp --pdo-get "$GETS" --duration-sec 5 \
        --diag-file "$LOG/diag_r06.txt" > "$LOG/er_r06.log" 2>&1
    say "- rc $?"
    sim_stop
}

for st in $STEPS; do
    case $st in
        r01) r01 ;;
        r02) r02 ;;
        r05) r05 ;;
        r06) r06 ;;
        r09) r09 ;;
        *) echo "unknown step $st"; exit 2 ;;
    esac
done

# R-05 / R-06 grading (one place: bringup_report.py)
R9=()
for f in "$LOG"/er_r09_*.log; do [ -f "$f" ] && { c=${f##*_}; R9+=(--er9 "${c%.log}:$f"); }; done
if [ -f "$LOG/er_r05.log" ] || [ -f "$LOG/er_r06.log" ] || [ ${#R9[@]} -gt 0 ]; then
    python3 "$HERE/bringup_report.py" --n "$N" --eni "$ENI" --er5 "$LOG/er_r05.log" --er6 "$LOG/er_r06.log" \
        "${R9[@]}" > "$LOG/report_run.md" 2>&1
    sed '/^## Summary/,$d' "$LOG/report_run.md" | tee -a "$REP"
    NFAIL=$((NFAIL + $(grep -c '^VERDICT [^ ]* FAIL' "$LOG/report_run.md")))
fi

say ""; say "## Summary"
grep '^VERDICT' "$REP" | sed 's/^VERDICT /- /' > "$LOG/summary.txt"
cat "$LOG/summary.txt" | tee -a "$REP"
say ""; say "RESULT: $NFAIL FAIL   (report: $REP)"
[ $NFAIL = 0 ]
