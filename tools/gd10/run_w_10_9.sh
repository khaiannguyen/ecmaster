#!/usr/bin/env bash
# ==========================================================================
# run_w_10_9.sh — Phase 10.9: the master drives the two real IS620N axes
# (plan claude/giai_doan_10_ke_hoach.md section 11, W-01..W-08).
#
# THIS ENABLES THE DRIVES. Safety, with the hardware as it is (no E-stop
# button, one breaker for control + main power, decision of 9/10):
#   - the breaker is the emergency stop: hand next to it on every enable
#   - motors clamped, nothing on the shafts
#   - drive limits written by the ENI at every start and after every power
#     loss (tools/eni/eni_limits.py): 0x6072 torque TQ (default 150 = 15 %),
#     0x6065 following error window (default 1/8 rev), 0x607F max velocity;
#     w00 checks them by SDO before anything is enabled, and the runner stops
#     if they are not what was asked
#   - master: S4 step limit from MAX_RPM (default 60 rpm), S1/S2/S5/S6/S7
#   - slow, small motion: (1 - cos) profiles start and end at velocity 0
#
#   sudo -E tools/gd10/run_w_10_9.sh                    w00 w01 w02 (evening 1)
#   sudo -E CASES="w03pp w03hm w04" tools/gd10/run_w_10_9.sh
#   sudo -E CASES="w05 w06 w07" tools/gd10/run_w_10_9.sh     (w05/w06 ask you to act)
#   sudo -E CASES="w08" tools/gd10/run_w_10_9.sh        1 h, in tmux
#   sudo -E SIM=1 tools/gd10/run_w_10_9.sh              rehearsal on veth (virtual drives)
#
#   w00   limits: ecm_run with the limits ENI (no hook), then 0x6072/0x6065/0x607F
#         read back by SDO on both drives == asked. Always run first.
#   w01   axis 1 enable, hold 5 s, disable: one Operation enabled, |pos at
#         enable - pos at disable| <= JUMP (S2: target = actual, no jerk)
#   w02   axis 1 CSP, (1 - cos) of 1 rev at 0.2 Hz, 2 periods: no error, no
#         step refused, back at the start position, tracking max reported
#   w03pp axis 1 PP: +1/4, +1/4, -1/2 rev (relative): 3 set-points
#         acknowledged, back at the start
#   w03hm axis 1 homing method HM_METHOD (default 35: current position)
#   w04   both axes CSP (1 - cos) 1/2 rev 0.2 Hz, 2 periods, at the same time
#   w05   both axes moving slowly; YOU switch the breaker off, then on again
#         (replaces the E-stop test W-05): bus LOST, after RECOVER both axes
#         stay disabled (S5), never enabled a second time
#   w06   both axes moving slowly; YOU unplug the cable drive 1 OUT -> drive
#         2 IN, then plug it back: axis 2 latched disabled (S5), not re-enabled
#   w07   both axes moving; SIGINT after 6 s: S6 walks both down before the
#         slaves leave OP, both end in Switch on disabled
#   w08   both axes CSP (1 - cos) 1/2 rev 0.2 Hz for DUR8 s (default 3600):
#         0 WKC error, 0 overrun, DC LOCKED, no axis error, S6 at the end
#   w09csv axis 1 CSV, velocity (1 - cos) up to VMAX/2 (30 rpm) at 0.5 Hz, 2
#         periods: mode 9 at enable, moved 1 rev (+- RES/10), no error
#   w10pv axis 1 PV: VMAX/2 for 2 s, ramps RES inc/s^2 (0x6083/0x6084), then 0:
#         mode 3 at enable, moved ~1 rev (+- RES/5), no error
#   w09csv / w10pv need 0x60FF in the PDOs: ENI=config/eni/eni_2servo_mm.enicfg
#   (0x1702/0x1B02, TwinCAT 9/10, X-05b). That RxPDO also carries 0x607F: the
#   master would send 0 every cycle over the InitCmd limit, so the runner adds
#   --pdo-set S:0x607F:0=VMAX for every slave whose ENI maps it (patch 0022).
#
# Env: IFACE N ENI CASES RES (inc/rev, 8388608 measured 9/10) MAX_RPM TQ NOFRAME_MAX
#      FERR (inc) JUMP (inc) HM_METHOD DUR8 YES SIM SB_PRIO SB_CPU IS620N_ESI LOG
#      PDOSET_OFF=1 (negative control: 0x607F in the RxPDO left to 0 -> w00 FAILs)
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SIM=${SIM:-0}
if [ "$SIM" = 1 ]; then IFACE=${IFACE:-veth_m}; else IFACE=${IFACE:-enP1p1s0}; fi
IF_S=${IF_S:-veth_s}
N=2
ENI=${ENI:-$ROOT/config/eni/eni_2servo.enicfg}
CASES=${CASES:-"w01 w02"}
RES=${RES:-8388608}
MAX_RPM=${MAX_RPM:-60}
TQ=${TQ:-150}
FERR=${FERR:-$((RES / 8))}
JUMP=${JUMP:-$((RES / 100))}
HM_METHOD=${HM_METHOD:-35}
DUR8=${DUR8:-3600}
# one lost reply now and then (single frame, recovered the next cycle) is what an
# Ethernet link does; W-08 on the real drives: 1 in 3 600 151 cycles
NOFRAME_MAX=${NOFRAME_MAX:-2}
YES=${YES:-0}
SB_PRIO=${SB_PRIO-79}; SB_CPU=${SB_CPU:-2}
IS620N_ESI=${IS620N_ESI:-}
ECM_RUN=${ECM_RUN:-$ROOT/apps/ecm_run/ecm_run}
ECM_PEEK=${ECM_PEEK:-$ROOT/apps/ecm_peek/ecm_peek}
SOFT_BUS=${SOFT_BUS:-$ROOT/tools/soft_bus/soft_bus}
LOG=${LOG:-log_gd10_w_$(date +%Y%m%d_%H%M%S)}
LOG=$(mkdir -p "$LOG" && cd "$LOG" && pwd)
REP=$LOG/report.md
CTL=/tmp/soft_bus_10_9.ctl
PASS=0; FAIL=0

# derived numbers: S4 step per 1 ms cycle = MAX_RPM in inc/ms (+10 %), max velocity in inc/s
STEP=$(( RES * MAX_RPM / 60 / 1000 * 11 / 10 ))
VMAX=$(( RES * MAX_RPM / 60 ))
# CSV (0022): S4 refuses a velocity step above 1 % of VMAX per cycle (the (1 - cos)
# velocity profile changes by <= VC pi hz / 1000 = 0.08 % per cycle)
VSTEP=$(( VMAX / 100 )); VC=$(( VMAX / 2 ))
A1=$RES; A2=$((RES / 2)); Q=$((RES / 4))

say () { echo "$*" | tee -a "$REP"; }
ok  () { say "  [PASS] $1"; PASS=$((PASS+1)); }
bad () { say "  [FAIL] $1"; FAIL=$((FAIL+1)); }
chk () { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
info () { say "  [INFO] $1"; }

for f in "$ECM_RUN" "$ECM_PEEK" "$ENI"; do [ -e "$f" ] || { echo "missing $f (make all caps)"; exit 2; }; done
ip link show "$IFACE" >/dev/null 2>&1 || { echo "interface $IFACE missing"; exit 2; }
for p in ecm_run soft_bus ecm_peek; do pgrep -x "$p" >/dev/null && { echo "a $p process is running -- stop it first"; exit 2; }; done
[ "$STEP" -gt 0 ] || { echo "bad RES/MAX_RPM"; exit 2; }

{
    echo "# Phase 10.9 — master drives the IS620N axes — $(date '+%Y-%m-%d %H:%M:%S')"
    echo
    echo "- host $(uname -n); commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?'); interface $IFACE"
    [ "$SIM" = 1 ] && echo "- **SIMULATION** (virtual drives on soft_bus)"
    echo "- encoder $RES inc/rev; master step limit $STEP inc/ms (~$MAX_RPM rpm); drive limits 0x6072=$TQ ($((TQ / 10)) %), 0x6065=$FERR inc, 0x607F=$VMAX inc/s"
    echo "- cases: w00 $CASES"
} > "$REP"
cat "$REP"

if [ "$SIM" != 1 ]; then
    cat <<EOF

=== Phase 10.9 on $IFACE: THE DRIVES WILL BE ENABLED ===
  [ ] Motors clamped, nothing on the shafts, nobody near them
  [ ] Your hand next to the breaker (it is the emergency stop)
  [ ] Cable: $IFACE -> drive 1 IN, drive 1 OUT -> drive 2 IN
  [ ] Drive limits: written by this script (w00 checks them before enabling)
Limits: torque $((TQ / 10)) %, ~$MAX_RPM rpm, following error window $FERR inc.
EOF
    if [ "$YES" != 1 ]; then read -r -p "Type yes to start: " ans; [ "$ans" = yes ] || { echo stopped; exit 3; }; fi
fi

# ---- limits ENI (always derived fresh, kept in the log) ----
LIM=$LOG/$(basename "$ENI" .enicfg)_limits.enicfg
python3 "$ROOT/tools/eni/eni_limits.py" "$ENI" -o "$LIM" --torque-permille "$TQ" --ferr "$FERR" --max-vel "$VMAX" \
    | sed 's/^/- /' | tee -a "$REP"
[ -s "$LIM" ] || { echo "eni_limits.py failed"; exit 2; }
# 0022: 0x607F in an RxPDO (IS620N 0x1702) -> the master writes VMAX there every cycle,
# else the 0 of the output image would replace the InitCmd limit once in OP
PDOSET=
for s7f in $(awk '$1 == "pdo" && $3 == "dir" && $4 == "out" && $8 == "0x607F" {print $2}' "$ENI" | sort -u); do
    PDOSET="${PDOSET:+$PDOSET,}$s7f:0x607F:0=$VMAX"
done
if [ -n "$PDOSET" ] && [ "${PDOSET_OFF:-0}" = 1 ]; then   # negative control: w00 must FAIL (0x607F = 0)
    say "- NEGATIVE CONTROL PDOSET_OFF=1: 0x607F is in the RxPDO but NOT written by the master"; PDOSET=
fi
[ -n "$PDOSET" ] && say "- 0x607F is in the RxPDO: --pdo-set $PDOSET (every cycle)"
HAS_60FF=0; awk '$1 == "pdo" && $4 == "out" && $8 == "0x60FF"' "$ENI" | grep -q . && HAS_60FF=1

# ---- SIM: two virtual IS620N with the limit objects ----
SBP=
if [ "$SIM" = 1 ]; then
    IS=$LOG/is620n_sim.prof
    if [ -n "$IS620N_ESI" ]; then
        python3 "$ROOT/tools/esi/esi2profile.py" "$IS620N_ESI" -o "$IS" || exit 2
        sed -i 's/^\(sub 0x6502 0 bits 32 [a-z_]*\) 00000000$/\1 ad030000/' "$IS"
    else
        cp "$ROOT/config/profiles/is620n_min.prof" "$IS"
    fi
    grep -q '^obj 0x6502 ' "$IS" || { echo "obj 0x6502 var 1"; echo "sub 0x6502 0 bits 32 ro ad030000"; } >> "$IS"
    grep -q '^obj 0x6072 ' "$IS" || { echo "obj 0x6072 var 1"; echo "sub 0x6072 0 bits 16 rw b80b"; } >> "$IS"
    grep -q '^obj 0x6065 ' "$IS" || { echo "obj 0x6065 var 1"; echo "sub 0x6065 0 bits 32 rw e76aa301"; } >> "$IS"
    grep -q '^obj 0x607F ' "$IS" || { echo "obj 0x607F var 1"; echo "sub 0x607F 0 bits 32 rw 00000080"; } >> "$IS"
    # PP / homing objects the reduced profile lacks (values of config/profiles/cia402_4ax.prof)
    for o in "0x6081 32 a0860100" "0x6083 32 40420f00" "0x6084 32 40420f00" "0x6098 8 21" "0x609A 32 40420f00" "0x607C 32 00000000"; do
        set -- $o
        grep -q "^obj $1 " "$IS" || { echo "obj $1 var 1"; echo "sub $1 0 bits $2 rw $3"; } >> "$IS"
    done
fi
sim_start () {
    [ "$SIM" = 1 ] || return 0
    rm -f "$CTL"
    local sb=("$SOFT_BUS")
    [ -n "$SB_PRIO" ] && sb=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
    "${sb[@]}" --iface "$IF_S" --n 2 --dc 32 --no-sm-wd --ctl "$CTL" \
        --profile 1="$IS" --cia402 1 --profile 2="$IS" --cia402 2 > "$LOG/sb_$1.log" 2>&1 &
    SBP=$!; sleep 0.5
}
sim_stop () { [ -n "$SBP" ] && { kill -INT $SBP 2>/dev/null; wait $SBP 2>/dev/null; SBP=; }; return 0; }
ctl () { printf '%s\n' "$*" | dd of="$CTL" conv=nocreat,notrunc oflag=nonblock status=none; }
trap 'sim_stop' EXIT

BASE=(--iface "$IFACE" --n "$N" --eni "$LIM" --pdo-scan --emcy-map "$ROOT/config/emcy/is620n.emcy")
[ "$SIM" = 1 ] && BASE+=(--no-tx-ts)
[ -n "$PDOSET" ] && BASE+=(--pdo-set "$PDOSET")
MOT=(--hook cia402 --cia402-lead 20 --axis-max-step "$STEP:$VSTEP")

# run TAG DURATION "extra args" SCRIPT   (foreground)
run () {
    local tag=$1 dur=$2 extra=$3 script=$4
    sim_start "$tag"
    # shellcheck disable=SC2086
    ethtool -S "$IFACE" > "$LOG/ethtool_S_${tag}_before.txt" 2>/dev/null
    "$ECM_RUN" "${BASE[@]}" "${MOT[@]}" $extra --cia402-script "$script" --duration-sec "$dur" \
        --diag-file "$LOG/diag_$tag.txt" > "$LOG/er_$tag.log" 2>&1
    RC=$?
    ethtool -S "$IFACE" > "$LOG/ethtool_S_${tag}_after.txt" 2>/dev/null
    sim_stop
    E=$LOG/er_$tag.log
}
nic_delta () {   # error counters of ethtool -S that grew during case $1
    local r
    r=$(join <(awk -F': ' '{gsub(/ /,"",$1); print $1, $2}' "$LOG/ethtool_S_$1_before.txt" | sort) \
             <(awk -F': ' '{gsub(/ /,"",$1); print $1, $2}' "$LOG/ethtool_S_$1_after.txt" | sort) 2>/dev/null |
        awk '$1 ~ /err|crc|missed|align|symbol|fifo|over|drop/ && $3 != $2 {printf "%s +%d; ", $1, $3-$2}')
    echo "${r:-none}"
}
# helpers on $E
axline () { grep "  \[CIA402\] axis $1 (" "$E" | head -1; }
axf ()    { axline "$1" | grep -o " $2=[-0-9a-zA-Z]*" | head -1 | cut -d= -f2; }
axerr ()  { axline "$1" | sed -n 's/.* err=\(.*\) 0x603F=.*/\1/p'; }
axstate () { axline "$1" | sed 's/.*): //; s/ sw=.*//'; }
n_oe ()   { grep -c "\[CIA402\] t=.* axis $1 Switched on -> Operation enabled (" "$E"; }
pos_oe () { grep "\[CIA402\] t=.* axis $1 Switched on -> Operation enabled (" "$E" | head -1 | grep -o 'pos=[-0-9]*' | cut -d= -f2; }
pos_leave () { grep "\[CIA402\] t=.* axis $1 Operation enabled -> " "$E" | head -1 | grep -o 'pos=[-0-9]*' | cut -d= -f2; }
reset_sent () {   # did any controlword of axis $1 in the transition lines carry bit 7 (fault reset)?
    local h
    for h in $(grep "\[CIA402\] t=.* axis $1 " "$E" | grep -o 'cw 0x[0-9A-F]*' | cut -d' ' -f2); do
        (( h & 0x80 )) && return 0
    done
    return 1
}
absdiff () { local d=$(( ${1:-0} - ${2:-0} )); echo ${d#-}; }
bus_ok () {   # 0 overrun; WKC: no zero/partial/over, NOFRAME <= NOFRAME_MAX and never two in a row;
              # DC LOCKED 0 unlock; no [AXIS] diagnosis
    local nf
    nf=$(grep -o '\[WKC GROUP_MOTION\] ok=[0-9]* noframe=[0-9]*' "$E" | grep -o '[0-9]*$')
    grep -q '\[GROUP_MOTION\] cycles=[0-9]* wkc_mismatch=[0-9]* .*overrun=0 ' "$E" &&
    grep -q '\[WKC GROUP_MOTION\] .* zero=0 partial=0 over=0 max_run_bad=[01]$' "$E" &&
    [ -n "$nf" ] && [ "$nf" -le "$NOFRAME_MAX" ] &&
    grep -q '\[DC\] state=LOCKED locks=[0-9]* unlocks=0' "$E" && ! grep -q 'ecm_run: \[AXIS\]' "$E"
}
common () {   # common checks of a motion case: TAG AXES...
    local tag=$1; shift
    chk "$tag ecm_run rc 0, OP, ENI limits applied (6 extra InitCmds ok)" \
        "[ $RC = 0 ] && grep -q 'all slaves in OPERATIONAL' $E && [ \$(grep -c 'CoE download 0x60\(72\|65\|7F\):00 ok' $E) -ge 6 ]"
    chk "$tag bus: 0 overrun, NOFRAME $(grep -o 'noframe=[0-9]*' $E | head -1 | cut -d= -f2) <= $NOFRAME_MAX (single frames only), no zero/partial WKC, DC LOCKED, no [AXIS] diagnosis" "bus_ok"
    [ -f "$LOG/ethtool_S_${tag}_before.txt" ] && info "$tag NIC error counters during the run: $(nic_delta $tag)"
    chk "$tag script parsed" "! grep -q 'cannot parse --cia402-script' $E"
    chk "$tag S6 walk-down before leaving OP" "grep -q 'shutdown (S6): every axis walked down' $E"
    local a
    for a in "$@"; do
        chk "$tag axis $a: err none, 0 step refused, ends Switch on disabled, 0x603F 0 ($(axstate $a), err $(axerr $a), 0x603F $(axf $a 0x603F))" \
            "[ \"\$(axerr $a)\" = none ] && [ \"\$(axstate $a)\" = 'Switch on disabled' ] && [ \"\$(axf $a 0x603F)\" = 0x0000 ] && ! grep -q \"S4 axis $a .*step_refused=[1-9]\" $E"
        info "$tag axis $a: tracking max $(axf $a track_max) inc (n $(axline $a | grep -o '(n=[0-9]*' | cut -d= -f2)), used $(axf $a used), late $(axf $a late), underrun $(axf $a underrun)"
    done
}

# ---------------------------------------------------------------- w00
say ""; say "## w00 drive limits written and read back"
sim_start w00
"$ECM_RUN" "${BASE[@]}" --duration-sec 3 --diag-file "$LOG/diag_w00.txt" > "$LOG/er_w00.log" 2>&1; RC=$?
E=$LOG/er_w00.log
"$ECM_PEEK" --iface "$IFACE" --no-default-sdo --sdo 0x6072:0,0x6065:0,0x607F:0 > "$LOG/peek_w00.txt" 2>&1
sim_stop
rb () { grep "^SDO $1 0x$2:00 size" "$LOG/peek_w00.txt" | awk '{print $NF}'; }
chk "w00 ecm_run OP with the limits ENI, 6 limit InitCmds ok" \
    "[ $RC = 0 ] && [ \$(grep -c 'CoE download 0x60\(72\|65\|7F\):00 ok' $E) = 6 ]"
for s in 1 2; do
    chk "w00 slave $s read back: 0x6072=$(rb $s 6072) 0x6065=$(rb $s 6065) 0x607F=$(rb $s 607F)" \
        "[ \$((\$(rb $s 6072))) = $TQ ] && [ \$((\$(rb $s 6065))) = $FERR ] && [ \$((\$(rb $s 607F))) = $VMAX ]"
done
if [ $FAIL != 0 ]; then
    say ""; say "w00 FAILED: the limits are not in the drives -- nothing is enabled. See $LOG/er_w00.log, peek_w00.txt"
    say "RESULT: $PASS pass, $FAIL fail"; exit 1
fi

# ---------------------------------------------------------------- cases
interactive () {   # TAG "what" "do text" "undo text" "sim do;..." "sim undo;..."   (ecm_run already running)
    local tag=$1 do_t=$3 undo_t=$4 sdo=$5 sundo=$6
    sleep 8
    if [ "$SIM" = 1 ]; then
        local IFS=';' c; for c in $sdo; do ctl "$c"; done; unset IFS
        sleep 10
        local IFS=';' c; for c in $sundo; do ctl "$c"; done; unset IFS
    else
        echo "  NEXT: $do_t"; read -r -p "  press Enter at the moment you do it: " _
        sleep 10
        echo "  NEXT: $undo_t"; read -r -p "  press Enter at the moment you do it: " _
    fi
    echo "  settling 40 s (the axes must stay disabled) ..."
    sleep 40
}

for c in $CASES; do
say ""
case $c in
w01)
    say "## w01 axis 1 enable / hold 5 s / disable"
    run w01 9 "--axis 1:0 --axis-modes csp" "1 enable 0; 6 disable 0"
    common w01 0
    chk "w01 Operation enabled exactly once" "[ \$(n_oe 0) = 1 ]"
    chk "w01 S2 no jump: |pos at enable ($(pos_oe 0)) - pos when disabled ($(pos_leave 0))| = $(absdiff "$(pos_oe 0)" "$(pos_leave 0)") <= $JUMP" \
        "[ -n \"\$(pos_oe 0)\" ] && [ \$(absdiff \"\$(pos_oe 0)\" \"\$(pos_leave 0)\") -le $JUMP ]"
    ;;
w02)
    say "## w02 axis 1 CSP (1 - cos) 1 rev 0.2 Hz, 2 periods"
    run w02 16 "--axis 1:0 --axis-modes csp" "1 enable 0; 2 cos 0 $A1 0.2; 12 stop 0; 13 disable 0"
    common w02 0
    chk "w02 back at the start: |pos at enable - pos when disabled| <= $JUMP ($(absdiff "$(pos_oe 0)" "$(pos_leave 0)"))" \
        "[ -n \"\$(pos_oe 0)\" ] && [ \$(absdiff \"\$(pos_oe 0)\" \"\$(pos_leave 0)\") -le $JUMP ]"
    chk "w02 tracking measured, max below the following error window ($(axf 0 track_max) < $FERR)" \
        "[ -n \"\$(axf 0 track_max)\" ] && [ \"\$(axf 0 track_max)\" -lt $FERR ]"
    ;;
w03pp)
    say "## w03pp axis 1 PP +1/4 +1/4 -1/2 rev"
    run w03pp 14 "--axis 1:0 --axis-modes pp --axis-pp $((RES / 2)):$RES:$RES" \
        "1 enable 0; 2 pp 0 $Q 1; 2 pp 0 $Q 1; 2 pp 0 -$((2 * Q)) 1; 11 disable 0"
    common w03pp 0
    chk "w03pp 3 set-points acknowledged" "[ \$(grep -c 'axis 0 PP set-point acknowledged' $E) -ge 3 ]"
    chk "w03pp back at the start (|delta| <= $JUMP: $(absdiff "$(pos_oe 0)" "$(pos_leave 0)"))" \
        "[ -n \"\$(pos_oe 0)\" ] && [ \$(absdiff \"\$(pos_oe 0)\" \"\$(pos_leave 0)\") -le $JUMP ]"
    ;;
w03hm)
    say "## w03hm axis 1 homing method $HM_METHOD"
    run w03hm 10 "--axis 1:0 --axis-modes hm --axis-homing $HM_METHOD:0" "1 enable 0; 2 home 0; 7 disable 0"
    common w03hm 0
    chk "w03hm homing attained" "grep -q 'axis 0 homing attained' $E"
    ;;
w04)
    say "## w04 both axes CSP (1 - cos) 1/2 rev 0.2 Hz, 2 periods"
    run w04 16 "--axis 1:0,2:0 --axis-modes csp" "1 enable all; 2 cos all $A2 0.2; 12 stop all; 13 disable all"
    common w04 0 1
    for a in 0 1; do
        chk "w04 axis $a back at the start ($(absdiff "$(pos_oe $a)" "$(pos_leave $a)") <= $JUMP)" \
            "[ -n \"\$(pos_oe $a)\" ] && [ \$(absdiff \"\$(pos_oe $a)\" \"\$(pos_leave $a)\") -le $JUMP ]"
    done
    ;;
w05|w06)
    if [ $c = w05 ]; then
        say "## w05 breaker off / on while both axes move (S5)"
        dot="Switch the BREAKER OFF (both drives lose all power)"; undot="Switch the BREAKER back ON"
        sdo="drop_node 1;drop_node 0"; sundo="restore_node 0;restore_node 1"
    else
        say "## w06 cable drive 1 OUT -> drive 2 IN unplugged while both axes move (S5)"
        dot="UNPLUG the cable drive 1 OUT -> drive 2 IN"; undot="PLUG it back in"
        sdo="drop_node 1"; sundo="restore_node 1"
    fi
    sim_start $c
    "$ECM_RUN" "${BASE[@]}" "${MOT[@]}" --axis 1:0,2:0 --axis-modes csp \
        --cia402-script "1 enable all; 2 cos all $A2 0.1" --duration-sec 300 \
        --diag-file "$LOG/diag_$c.txt" > "$LOG/er_$c.log" 2>&1 &
    ERP=$!
    interactive $c "" "$dot" "$undot" "$sdo" "$sundo"
    kill -INT $ERP 2>/dev/null; wait $ERP; RC=$?
    sim_stop
    E=$LOG/er_$c.log
    chk "$c ecm_run survived and ended cleanly (rc $RC)" "[ $RC = 0 ]"
    if [ $c = w05 ]; then
        chk "w05 bus LOST and recovered" "grep -q 'LOST -> RECOVER' $E && grep -q 'RECOVER -> RUN' $E"
        chk "w05 recovery re-ran the ENI incl. limits (no InitCmd FAILED after OP)" \
            "! awk '/all slaves in OPERATIONAL/{r=1} r' $E | grep -q 'CoE download .* FAILED'"
        AX="0 1"
    else
        chk "w06 slave 2 recovered" "grep -q 'slave 2 back in OP' $E"
        AX="1"
    fi
    for a in $AX; do
        # Seen on the real drives (9/10): breaker off -> the drive leaves Operation enabled while
        # it still talks (S1 'left Operation enabled ...'); cable out -> drive 2 goes to Fault
        # 0x0E08 ('drive fault', S7), and clears it itself when it re-enters OP. Whatever the
        # latch: enabled once only, an error kept, not enabled at the end, and the master never
        # sent a fault reset (controlword bit 7) -- S7.
        chk "$c axis $a: enabled once only, latched error kept (err: $(axerr $a)), ends $(axstate $a)" \
            "[ \$(n_oe $a) = 1 ] && [ -n \"\$(axerr $a)\" ] && [ \"\$(axerr $a)\" != none ] && [ \"\$(axstate $a)\" != 'Operation enabled' ]"
        chk "$c axis $a: no fault reset sent by the master (cw bit 7 never set)" \
            "! reset_sent $a"
    done
    ;;
w07)
    say "## w07 SIGINT while both axes move (S6)"
    sim_start w07
    "$ECM_RUN" "${BASE[@]}" "${MOT[@]}" --axis 1:0,2:0 --axis-modes csp \
        --cia402-script "1 enable all; 2 cos all $A2 0.2" --duration-sec 60 \
        --diag-file "$LOG/diag_w07.txt" > "$LOG/er_w07.log" 2>&1 &
    ERP=$!
    for _ in $(seq 1 100); do grep -q 'all slaves in OPERATIONAL' "$LOG/er_w07.log" 2>/dev/null && break; sleep 0.1; done
    sleep 6
    kill -INT $ERP; wait $ERP; RC=$?
    sim_stop
    E=$LOG/er_w07.log
    common w07 0 1
    ;;
w09csv|w10pv)
    if [ "$HAS_60FF" != 1 ]; then
        bad "$c needs 0x60FF in the PDOs: ENI=$ROOT/config/eni/eni_2servo_mm.enicfg (now: $ENI)"
        continue
    fi
    if [ $c = w09csv ]; then
        say "## w09csv axis 1 CSV, velocity (1 - cos) up to $VC inc/s (~$((MAX_RPM / 2)) rpm), 0.5 Hz, 2 periods"
        run w09csv 9 "--axis 1:0 --axis-modes csv" "1 enable 0; 2 vcos 0 $VC 0.5; 6 stop 0; 7 disable 0"
        md=9; want=$RES; tol=$((RES / 10))
    else
        say "## w10pv axis 1 PV $VC inc/s for 2 s, ramps $RES inc/s^2"
        run w10pv 9 "--axis 1:0 --axis-modes pv --axis-pp $VC:$RES:$RES" "1 enable 0; 2 pv 0 $VC; 4 pv 0 0; 6 disable 0"
        md=3; want=$RES; tol=$((RES / 5))
    fi
    common $c 0
    chk "$c Operation enabled once, in mode $md (0x6061 via the PDO)" \
        "[ \$(n_oe 0) = 1 ] && grep -q 'axis 0 Switched on -> Operation enabled (.* mode $md)' $E"
    p0=$(pos_oe 0); p1=$(pos_leave 0); mv=$(absdiff "$p1" "$p0")
    chk "$c moved $mv inc (enable $p0 -> disable $p1; want $want +- $tol)" \
        "[ -n \"$p0\" ] && [ -n \"$p1\" ] && [ \$(absdiff $mv $want) -le $tol ]"
    ;;
w08)
    say "## w08 both axes CSP (1 - cos) 1/2 rev 0.2 Hz, $DUR8 s"
    run w08 "$DUR8" "--axis 1:0,2:0 --axis-modes csp" "1 enable all; 2 cos all $A2 0.2"
    common w08 0 1
    grep -E '^\s*\[(GROUP_MOTION|DC|WKC GROUP_MOTION|POLICY|HOOK)\]' "$E" | sed 's/^/    /' | tee -a "$REP"
    ;;
*) bad "unknown case $c" ;;
esac
done

say ""
say "RESULT: $PASS pass, $FAIL fail   (report: $REP)"
[ $FAIL = 0 ]
