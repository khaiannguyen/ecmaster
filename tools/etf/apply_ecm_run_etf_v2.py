#!/usr/bin/env python3
"""apply_ecm_run_etf_v2.py -- Phase 8.5 R-02 fix for `ecm_run --link etf`.

Run from the ecmaster repo root AFTER apply_ecm_run_etf.py (v1):
    python3 tools/etf/apply_ecm_run_etf_v2.py
Edits apps/ecm_run/ecm_run.c in place (backup *.pre-etf-v2), idempotent.
Needs SOEM with apply_soem_txtime_v2.py (port->txtime_bypass).

R-02 (29/9), etf32: GROUP_IO noframe 16483 / 37499. v1 computed the receive
deadline from the WAKE (next - lead + cycle - guard = launch + 500 us) and the IO
frame waited asap = 150 us in ETF after the motion reply. With soft_bus N=32
(RTT ~250 us per group) RTT_motion + 150 + RTT_io did not fit in 500 us.

v2:
- deadline from the LAUNCH time: next + cycle - asap - guard (launch + 700 us at
  cycle 1000, asap 150, guard 150). The lead only protects the sleep: when the
  RT thread is still busy, the next motion frame is on time as long as it is
  sent before next_launch - asap. af_packet: unchanged (next + cycle - guard).
- IO / mailbox / diag frames leave at once without ETF (SOEM v2 bypass), so
  nothing to do here for them.
- lead must exceed asap (otherwise every motion frame is "late").
- summary prints the bypass count.
"""
import os
import shutil
import sys

C = "apps/ecm_run/ecm_run.c"
if not os.path.isfile(C):
    sys.exit(f"{C} not found -- run from the ecmaster repo root")
src = open(C).read()
if "Phase 8.5" not in src:
    sys.exit(f"{C}: v1 (apply_ecm_run_etf.py) not applied")
if "8.5 v2" in src:
    print(f"{C}: already v2")
    sys.exit(0)
edits = []


def rep(old, new, what):
    global src
    if src.count(old) != 1:
        sys.exit(f"{C}: anchor for '{what}' found {src.count(old)} times (need 1) -- send ecm_run.c to Claude")
    src = src.replace(old, new, 1)
    edits.append(what)


rep("""        g_tick_deadline_ns = ts_to_ns(&next) - (uint64_t)lead_ns
                           + (uint64_t)(motion_cycle_us * 1000L) - RX_GUARD_NS;   /* 8.5: from the wake target */""",
    """        /* 8.5 v2: with --link etf the motion frame leaves at `next`, and the
         * next tick's motion frame only has to be SENT by next + cycle - asap
         * (the lead only covers the sleep), so the receives may run until
         * next + cycle - asap - guard. af_packet: next + cycle - guard. */
        g_tick_deadline_ns = ts_to_ns(&next) + (uint64_t)(motion_cycle_us * 1000L)
                           - (uint64_t)(g_link_etf ? g_etf_asap_us * 1000L : 0) - RX_GUARD_NS;""",
    "deadline from launch")

rep("""        if (g_etf_lead_us <= 0 || g_etf_lead_us >= motion_cycle_us / 2 || g_etf_asap_us <= 0) {""",
    """        if (g_etf_lead_us <= 0 || g_etf_lead_us >= motion_cycle_us / 2 || g_etf_asap_us <= 0
            || g_etf_asap_us >= g_etf_lead_us) {   /* 8.5 v2: asap < lead */""",
    "argument check")
rep("""need 0 < lead < cycle/2 and asap > 0 (lead %ld, asap %ld us)""",
    """need 0 < asap < lead < cycle/2 (lead %ld, asap %ld us)""",
    "argument message")

rep("""SOEM: late(sent asap)=%u send_err=%u; """,
    """SOEM: late(sent now)=%u bypass(no ETF)=%u send_err=%u; """,
    "summary format")
rep("""ctx.port.txtime_late, ctx.port.txtime_send_err,""",
    """ctx.port.txtime_late, ctx.port.txtime_bypass, ctx.port.txtime_send_err,""",
    "summary args")

rep(""" * offload). Every other frame gets now_tai + asap inside SOEM. lead < ETF""",
    """ * offload). Every other frame leaves at once on SO_PRIORITY 0, a queue
 * without ETF (SOEM v2, needs mqprio; a root ETF drops them). lead < ETF""",
    "header comment")

shutil.copy2(C, C + ".pre-etf-v2")
open(C, "w").write(src)
print(f"{C}: {len(edits)} edits: " + ", ".join(edits))
