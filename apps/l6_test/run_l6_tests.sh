#!/usr/bin/env bash
# ==========================================================================
# run_l6_tests.sh — run one L6 scenario: soft_bus (slave side, ground truth)
# + l6_test (master side), then check soft_bus's DC log.
#
#   ./run_l6_tests.sh <dc_width 32|64> <duration_s> [naive] [extra l6_test args...]
#
# Env overrides: SOFT_BUS, L6_TEST, IF_M, IF_S, N, HOP_NS, REF_PPM, WIN_S,
#                SKIP_WIN (windows ignored at start: settle + lock),
#                SB_PRIO (run soft_bus SCHED_FIFO at this prio; empty = SCHED_OTHER),
#                SB_CPU  (pin soft_bus to this core, only with SB_PRIO; default 2),
#                SPREAD_MAX (GT-c limit in ns, default 1000),
#                UP_MAX (GT-d limit in ns, default 10000),
#                EXPECT_GTC_FAIL=1 (L6-05 negative controls: GT-c MUST fail,
#                             GT-b becomes info only),
#                NOFRAME_MAX (tolerated no_frame per checked window, default 0 —
#                             raise ONLY on a non-RT host, and say so in the log)
#
# Slave-side checks (soft_bus log), on every full window after SKIP_WIN:
#   L6-02c  SYNC0 events per window == WIN_S / cycle (+-5), every node
#   GT-a    node 0: |mean arrival phase after SYNC0 - setpoint| < 10 us in
#           every window  (systematic: independent of host jitter)
#   GT-b    SYNC0 intervals without a process-data frame <= NOFRAME_MAX
#           (random: depends on host jitter tail vs. the 700 us late margin)
#   GT-c    reference clock + every node DOWNSTREAM of it: spread of their
#           system times at one instant <= SPREAD_MAX in every window
#   GT-d    nodes UPSTREAM of the reference (only after an L6-05 switch):
#           max |system time - ref's| <= UP_MAX. They never see the ref's
#           time in-frame, so the master feeds them: bounded by master jitter
#   (soft_bus detects the reference itself: the node whose 0x0910 the PD
#    frame reads; ref=nodeK / ref_changes are printed in slave_checks.txt)
#   naive run: EXPECTS GT-a to FAIL (negative control must be able to fail)
#   start_in_past == 0 on every node
# ==========================================================================
set -u
WIDTH=${1:?dc width 32|64}; DUR=${2:?duration seconds}; shift 2
NAIVE=0
if [ "${1:-}" = "naive" ]; then NAIVE=1; shift; fi

HERE=$(cd "$(dirname "$0")" && pwd)
SOFT_BUS=${SOFT_BUS:-$HERE/../../tools/soft_bus/soft_bus}
L6_TEST=${L6_TEST:-$HERE/l6_test}
IF_M=${IF_M:-veth_m}; IF_S=${IF_S:-veth_s}
N=${N:-8}; HOP_NS=${HOP_NS:-800}; REF_PPM=${REF_PPM:-50}
WIN_S=${WIN_S:-5}; SKIP_WIN=${SKIP_WIN:-3}
SB_PRIO=${SB_PRIO:-}; SB_CPU=${SB_CPU:-2}; NOFRAME_MAX=${NOFRAME_MAX:-0}
SPREAD_MAX=${SPREAD_MAX:-1000}; UP_MAX=${UP_MAX:-10000}; EXPECT_GTC_FAIL=${EXPECT_GTC_FAIL:-0}

LOG=log_l6_${WIDTH}bit_$([ $NAIVE = 1 ] && echo naive_)$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOG"

SB_CMD=("$SOFT_BUS")
[ -n "$SB_PRIO" ] && SB_CMD=(chrt -f "$SB_PRIO" taskset -c "$SB_CPU" "$SOFT_BUS")
"${SB_CMD[@]}" --iface "$IF_S" --n "$N" --pdo-size 4 --dc "$WIDTH" \
    --dc-hop-ns "$HOP_NS" --dc-drift-ppm "$REF_PPM" --dc-report-s "$WIN_S" \
    > "$LOG/soft_bus.log" 2>&1 &
SB_PID=$!                      # NOT %1: job control is off in scripts
trap 'kill -INT $SB_PID 2>/dev/null' EXIT
sleep 0.5

L6_ARGS=(--iface "$IF_M" --duration-sec "$DUR" --expect-hop-ns "$HOP_NS" --report-sec "$WIN_S"
         --expect-ref-ppm "$REF_PPM")
[ $NAIVE = 1 ] && L6_ARGS+=(--naive)
timeout --foreground $((DUR + 60)) "$L6_TEST" "${L6_ARGS[@]}" "$@" > "$LOG/l6_test.log" 2>&1
L6_RC=$?

kill -INT $SB_PID 2>/dev/null
for _ in $(seq 50); do kill -0 $SB_PID 2>/dev/null || break; sleep 0.1; done
kill -9 $SB_PID 2>/dev/null
trap - EXIT

# ---- slave-side checks ----
CYCLE_NS=$(grep -o 'cycle=[0-9]*' "$LOG/l6_test.log" | head -1 | cut -d= -f2)
SP_US=$(grep -o 'setpoint=[0-9]*' "$LOG/l6_test.log" | head -1 | cut -d= -f2)
SP_US=$(( SP_US / 1000 ))
EXP=$(( WIN_S * 1000000000 / CYCLE_NS ))
awk -v skip="$SKIP_WIN" -v expn="$EXP" -v naive="$NAIVE" -v sp="$SP_US" -v nfmax="$NOFRAME_MAX" \
    -v spmax="$SPREAD_MAX" -v upmax="$UP_MAX" -v gtcneg="$EXPECT_GTC_FAIL" '
  function abs(x) { return x < 0 ? -x : x }
  /^dcsync win=/ {                       # printed just BEFORE dc[0] of its window
      for (i = 1; i <= NF; i++) {
          split($i, kv, "=")
          if (kv[1] == "spread_ns_max") spr[w + 1] = kv[2] + 0
          if (kv[1] == "up_ns_max")     upr[w + 1] = kv[2] + 0
          if (kv[1] == "ref")           lastref = kv[2]
          if (kv[1] == "ref_changes")   refch = kv[2] + 0
      }
      has_sp = 1
  }
  /^dc\[0\] win=/ { w++ }
  /^dc\[[0-9]+\] win=/ {
      delete v
      for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
      if ($1 == "dc[0]") { for (i = 1; i <= NF; i++) if ($i ~ /^mean=/) { split($i, kv, "="); mph[w] = kv[2] } }
      nof[w] += v["no_frame"]
      if (v["sync0"] < expn - 5 || v["sync0"] > expn + 5) bad[w]++
  }
  /^dc_total/ { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "start_in_past") sip += kv[2] } }
  END {
      full = 0; badw = 0; nofw = 0; tot_nof = 0; phw = 0; worst = 0; spw = 0; spworst = 0; upw = 0; upworst = 0
      for (k = skip + 1; k < w; k++) {
          full++; if (bad[k]) badw++
          tot_nof += nof[k]; if (nof[k] > nfmax) nofw++
          d = abs(mph[k] - sp); if (d > worst) worst = d; if (d >= 10) phw++
          if (spr[k] > spworst) spworst = spr[k]; if (spr[k] > spmax) spw++
          if (upr[k] > upworst) upworst = upr[k]; if (upr[k] > upmax) upw++
      }
      printf("windows: total=%d checked=%d (skip first %d + last partial)\n", w, full, skip)
      printf("  %s L6-02c SYNC0 events per window == %d +-5 on every node (%d bad windows)\n",
             (full > 0 && badw == 0) ? "PASS" : "FAIL", expn, badw)
      printf("  %s start_in_past == 0 on every node (%d)\n", sip == 0 ? "PASS" : "FAIL", sip)
      gta = (full > 0 && phw == 0)
      if (naive)
          printf("  %s GT-a NEGATIVE CONTROL: node0 mean phase leaves setpoint %d us +-10 (worst %.1f us, %d bad windows) -> the check can fail\n",
                 gta ? "FAIL" : "PASS", sp, worst, phw)
      else
          printf("  %s GT-a node0 mean phase within setpoint %d us +-10 in every window (worst %.1f us, %d bad windows)\n",
                 gta ? "PASS" : "FAIL", sp, worst, phw)
      binfo = naive || gtcneg
      printf("  %s GT-b SYNC0 intervals without PD frame: %d total, %d windows above NOFRAME_MAX=%d%s\n",
             (nofw == 0 || binfo) ? "PASS" : "FAIL", tot_nof, nofw, nfmax, binfo ? " [info only]" : "")
      gtc = 1; gtd = 1
      if (has_sp) {
          gtc = (full > 0 && spw == 0); gtd = (upw == 0)
          printf("  info: reference detected by soft_bus at end = %s, reference changes = %d\n", lastref, refch)
          printf("  %s GT-c ref + downstream clocks agree <= %d ns in every window (worst %d ns, %d bad windows)\n",
                 gtc ? "PASS" : "FAIL", spmax, spworst, spw)
          printf("  %s GT-d upstream clocks within %d ns of the ref (worst %d ns, %d bad windows)\n",
                 gtd ? "PASS" : "FAIL", upmax, upworst, upw)
          if (gtcneg)
              printf("  %s NEGATIVE CONTROL: GT-c or GT-d must be violated -> the checks can fail\n",
                     (gtc && gtd) ? "FAIL" : "PASS")
      }
      fails = (full == 0 || badw) + (sip != 0) + (naive ? gta : !gta) + (binfo ? 0 : (nofw != 0)) \
              + (gtcneg ? (gtc && gtd) : (!gtc + !gtd))
      printf("RESULT(slave side): %d fail\n", fails)
      exit fails ? 1 : 0
  }' "$LOG/soft_bus.log" | tee "$LOG/slave_checks.txt"
SB_RC=${PIPESTATUS[0]}

echo "---- master side (tail of $LOG/l6_test.log) ----"
sed -n '/^\[L6-05\] after/,/^$/p;/\[L6-04\]/,$p' "$LOG/l6_test.log"
echo "logs: $LOG/"
# the naive run is EXPECTED to fail master-side lock checks: only the slave side decides
if [ $NAIVE = 1 ]; then exit $SB_RC; fi
exit $(( L6_RC != 0 || SB_RC != 0 ))
