#!/usr/bin/env bash
# run_etf_veth.sh -- Phase 8.5: `ecm_run --link etf` regression on a multi-queue veth
# pair with mqprio + ETF in SOFTWARE mode (veth has no launch-time offload).
#
# Functional check only: config, cyclic, mailbox, diag and fault policy must still
# work when the motion frame goes through ETF (SO_PRIORITY 3 -> TC0 -> etf) and
# every other frame bypasses it (SO_PRIORITY 0, SOEM txtime v2). Launch-time
# accuracy is measured on the i226 with offload (txtime_probe, R-02), not here.
#
# Uses its own pair (vethe_m / vethe_s) so the golden rig veth_m / veth_s is not
# touched. Needs root, soft_bus and ecm_run built (ecm_run with the SOEM txtime
# patch). Parameters are wider than on the i226 (delta 400 / lead 350 / asap 150)
# because a CI runner is not RT.
#
# Usage: sudo -E tools/etf/run_etf_veth.sh [smoke|l5|all]     (default all)
# Env:   IF_M IF_S DELTA_US LEAD_US ASAP_US DUR
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
IF_M=${IF_M:-vethe_m}; IF_S=${IF_S:-vethe_s}
DELTA_US=${DELTA_US:-500}; LEAD_US=${LEAD_US:-450}; ASAP_US=${ASAP_US:-300}
DUR=${DUR:-20}
ETF_ARGS="--link etf --etf-lead-us $LEAD_US --etf-asap-us $ASAP_US"
SOFT_BUS=$ROOT/tools/soft_bus/soft_bus
ECM_RUN=$ROOT/apps/ecm_run/ecm_run
WHAT=${1:-all}
LOG=$(mktemp -d /tmp/etf_veth.XXXXXX)
PASS=0; FAIL=0

chk() {   # chk "description" "shell condition"
    if eval "$2"; then echo "  [PASS] $1"; PASS=$((PASS + 1));
    else echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); fi
}

etf_sent() {   # packets sent by the etf child qdisc so far
    tc -s qdisc show dev "$IF_M" | awk '/qdisc etf/ { f = 1 } f && /Sent/ { print $4; exit }'
}

for f in "$SOFT_BUS" "$ECM_RUN"; do
    [ -x "$f" ] || { echo "missing $f (build soft_bus and ecm_run first)"; exit 2; }
done
[ "$(id -u)" = 0 ] || { echo "needs root (sudo -E $0)"; exit 2; }

setup() {
    ip link del "$IF_M" 2>/dev/null
    ip link add "$IF_M" numtxqueues 4 numrxqueues 4 type veth \
        peer name "$IF_S" numtxqueues 4 numrxqueues 4 || exit 2
    ip link set "$IF_M" up && ip link set "$IF_S" up || exit 2
    # prio 3 -> TC0 (queue 0, etf); prio 2 -> TC1; everything else -> TC2 (queues 2-3)
    tc qdisc replace dev "$IF_M" parent root handle 100 mqprio num_tc 3 \
        map 2 2 1 0 2 2 2 2 2 2 2 2 2 2 2 2 queues 1@0 1@1 2@2 hw 0 || exit 2
    tc qdisc add dev "$IF_M" parent 100:1 etf clockid CLOCK_TAI delta $((DELTA_US * 1000)) || exit 2
    echo "rig: $IF_M <-> $IF_S, mqprio + etf delta $DELTA_US us (software); ecm_run $ETF_ARGS"
    tc qdisc show dev "$IF_M" | sed 's/^/  /'
}
cleanup() { ip link del "$IF_M" 2>/dev/null; }
trap cleanup EXIT

smoke() {
    echo "=== smoke: N=8, $DUR s, --link etf"
    "$SOFT_BUS" --iface "$IF_S" --n 8 --pdo-size 4 --dc 32 --no-sm-wd > "$LOG/sb_smoke.log" 2>&1 &
    local sbp=$!
    sleep 2
    local before; before=$(etf_sent)
    timeout --foreground $((DUR + 60)) "$ECM_RUN" --iface "$IF_M" --n 8 --motion-slaves 4 \
        --duration-sec "$DUR" --no-tx-ts $ETF_ARGS > "$LOG/er_smoke.log" 2>&1
    local rc=$?
    kill -INT $sbp 2>/dev/null; wait $sbp 2>/dev/null
    local after; after=$(etf_sent)
    grep -E "link etf:|\[LINK\]|\[WKC|\[POLICY\] final" "$LOG/er_smoke.log" | sed 's/^/  /'

    local cycles=$((DUR * 1000))
    local mok; mok=$(grep -o '\[WKC GROUP_MOTION\] ok=[0-9]*' "$LOG/er_smoke.log" | grep -o '[0-9]*$')
    local ook; ook=$(grep -o '\[WKC GROUP_IO\] ok=[0-9]*' "$LOG/er_smoke.log" | grep -o '[0-9]*$')
    local missed; missed=$(grep -o 'missed=[0-9]*' "$LOG/er_smoke.log" | grep -o '[0-9]*$')
    local invalid; invalid=$(grep -o 'invalid=[0-9]*' "$LOG/er_smoke.log" | grep -o '[0-9]*$')
    local bypass; bypass=$(grep -o 'bypass(no ETF)=[0-9]*' "$LOG/er_smoke.log" | grep -o '[0-9]*$')
    local through=$(( ${after:-0} - ${before:-0} ))
    echo "  etf qdisc: $through packets during the run (motion cycles $cycles), bypass ${bypass:-?}"

    chk "ecm_run exit 0"                               "[ $rc = 0 ]"
    chk "reached OP and ran the cyclic loop"           "grep -q 'all slaves in OPERATIONAL' '$LOG/er_smoke.log'"
    chk "qdisc detected as etf (software)"             "grep -q 'qdisc on $IF_M: etf WITHOUT offload' '$LOG/er_smoke.log'"
    chk "motion WKC ok >= 99 % of $cycles cycles"      "[ \"${mok:-0}\" -ge $((cycles * 99 / 100)) ]"
    chk "IO WKC ok >= 99 % of $((cycles / 8)) cycles"  "[ \"${ook:-0}\" -ge $((cycles / 8 * 99 / 100)) ]"
    chk "no ETF drop (missed=0 invalid=0)"             "[ \"${missed:-x}\" = 0 ] && [ \"${invalid:-x}\" = 0 ]"
    chk "motion frames went through ETF (>= 99 %)"     "[ $through -ge $((cycles * 99 / 100)) ]"
    chk "other frames bypassed ETF (bypass >= IO cycles)" "[ \"${bypass:-0}\" -ge $((cycles / 8)) ]"
}

l5() {
    echo "=== L5 with --link etf (same cases as CI)"
    local common="IF_M=$IF_M IF_S=$IF_S SB_ARGS=--no-sm-wd"
    ( cd "$ROOT/apps/ecm_run" && env $common ECM_ARGS="--no-tx-ts $ETF_ARGS" OVR_MAX=50 \
        CASES="l501 l502 l502neg l505 l505neg" ./run_l5_policy.sh ) > "$LOG/l5_policy.log" 2>&1
    local r1=$?
    tail -2 "$LOG/l5_policy.log" | sed 's/^/  /'
    ( cd "$ROOT/apps/ecm_run" && env $common ECM_ARGS="$ETF_ARGS" \
        CASES="l509 l509neg l512" ./run_l5_io.sh ) > "$LOG/l5_io.log" 2>&1
    local r2=$?
    tail -2 "$LOG/l5_io.log" | sed 's/^/  /'
    chk "L5 policy (l501 l502 l502neg l505 l505neg) with --link etf" "[ $r1 = 0 ] && grep -q ' 0 fail' '$LOG/l5_policy.log'"
    chk "L5 io (l509 l509neg l512) with --link etf"                  "[ $r2 = 0 ] && grep -q ' 0 fail' '$LOG/l5_io.log'"
}

setup
case "$WHAT" in
    smoke) smoke ;;
    l5)    l5 ;;
    all)   smoke; l5 ;;
    *)     echo "usage: $0 [smoke|l5|all]"; exit 2 ;;
esac
echo "RESULT: $PASS pass, $FAIL fail   (logs: $LOG)"
[ $FAIL = 0 ]
