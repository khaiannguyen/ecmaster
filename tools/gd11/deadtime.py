#!/usr/bin/env python3
"""deadtime.py -- Phase 11.5 IS-08 / IS-10: end-to-end dead time and the
cycle budget of one ecm_run run, from its log (outside the RT path).

  deadtime.py LOG [LOG ...]            Markdown, one block per log
  deadtime.py --csv LOG [LOG ...]      one CSV row per log

Model (ecm_run with DC, one motion frame per cycle; claude/giai_doan_11_ke_hoach.md §6.3):
  - the master sends frame k at T_k (af_packet: tick + wake + prep_send;
    etf: launch time = tick, launch error < 0.2 us, IS-04); DC keeps T_k at
    SYNC0 + p*c (--dc-setpoint-pct p, cycle c), within the DC error |e|
  - the slave latches its inputs at the SYNC0 edge p*c before T_k; frame k
    carries them back; the hook runs after the receive; its outputs go out
    in frame k+1 at T_k + c and are applied at the next SYNC0 edge,
    T_k + c + (1 - p)*c
  => dead time input latch -> output applied = p*c + c + (1 - p)*c = 2 c,
     whatever p is. p only moves the margins:
       output margin = (1 - p)*c - |e| - send offset - arrival (frame must
                       reach the last slave before its SYNC0 edge; arrival
                       bounded by the turnaround median: its tail is on the
                       master's receive side)
       input margin  = p*c - |e| (inputs latched before the frame passes)
       receive margin = c - asap - guard - turnaround (reply before the
                       receive deadline, else NOFRAME: that cycle's inputs
                       are lost and the dead time of that sample is 3 c)
       cycle margin  = c - occupancy (work done before the next tick)
  The input / output margins are computed from p99.99 and from max.

Negative control (test_is_offline O-07): the same numbers with p = 90 % at
250 us give a negative output margin.
"""
import re
import sys


def num(rx, txt, cast=float):
    m = re.search(rx, txt)
    return cast(m.group(1)) if m else None


def hist(txt, name):
    """last summary line of an ecm_hist in us: (p99.99, max)"""
    lines = [ln for ln in txt.splitlines() if ln.startswith(name + " ")]
    if not lines:
        return None, None, None
    ln = lines[-1]
    n = re.search(r"n=\s*(\d+)", ln)
    if n and int(n.group(1)) == 0:          # e.g. turnaround without HW timestamps (--no-tx-ts)
        return None, None, None

    def g(k):
        m = re.search(re.escape(k) + r"=\s*(\d+)", ln)
        return int(m.group(1)) / 1000.0 if m else None
    mx = g("max")
    p = g("p99.99")
    p50 = g("p50")
    if p is not None and mx is not None:
        p = min(p, mx)
    return p, mx, p50


def parse(path):
    txt = open(path, errors="replace").read()
    r = {"log": path}
    sync0 = num(r"SYNC0 (\d+) ns on \d+ motion slave", txt)
    cyc = num(r"--motion-cycle-us (\d+)", txt) or num(r"cycle (\d+) us", txt)
    r["cycle_us"] = sync0 / 1000.0 if sync0 else cyc
    r["setpoint_pct"] = num(r"setpoint (\d+)% after SYNC0", txt)
    r["dc"] = r["setpoint_pct"] is not None
    r["etf"] = "link etf: lead" in txt
    r["lead_us"] = num(r"link etf: lead (\d+) us", txt) or 0.0
    r["asap_us"] = num(r"link etf: lead \d+ us, asap (\d+) us", txt) or 0.0
    r["guard_us"] = num(r"rx timeout = tick deadline - (\d+) us", txt)
    if r["guard_us"] is None:
        r["guard_us"] = 150.0
    r["dc_e"] = num(r"\[DC\] \|e\| \(after 5 s\).*?p99\.99=(\d+) us", txt)
    for m in ("wake_jitter", "prep_send", "turnaround", "occupancy"):
        r[m] = hist(txt, m)
    hk = re.search(r"\[HOOK\].*?p99\.99=(\d+) max=(\d+) ns", txt)
    r["hook"] = (int(hk.group(1)) / 1000.0, int(hk.group(2)) / 1000.0) if hk else (0.0, 0.0)
    r["cycles"] = num(r"\[GROUP_MOTION\] cycles=(\d+)", txt, int)
    r["noframe"] = num(r"\[WKC GROUP_MOTION\] ok=\d+ noframe=(\d+)", txt, int)
    return r


def model(r):
    c = r["cycle_us"]
    if not c or not r["dc"]:
        return None
    p = r["setpoint_pct"] / 100.0
    e = r["dc_e"] or 0.0
    out = {"dead_us": 2 * c, "dead_cycles": 2}
    for i, tag in ((0, "p9999"), (1, "max")):
        w = r["wake_jitter"][i] or 0.0
        ps = r["prep_send"][i] or 0.0
        ta = r["turnaround"][i]
        oc = r["occupancy"][i]
        send = 0.2 if r["etf"] else w + ps          # etf: launch at the tick (IS-04 launch_err)
        out["send_" + tag] = send
        # arrival at the last slave: the frame's way out only. The turnaround
        # tail is on the master's receive side (i226 IRQ on CPU0, 11.5 §13),
        # so its median is the bound used here, not its p99.99 / max
        arr = r["turnaround"][2] or 0.0
        out["arr"] = arr
        out["out_margin_" + tag] = (1 - p) * c - e - send - arr
        out["in_margin_" + tag] = p * c - e
        out["rx_margin_" + tag] = None if ta is None else c - r["asap_us"] - r["guard_us"] - ta
        out["cyc_margin_" + tag] = None if oc is None else c - oc
    if r["cycles"]:
        out["nofr_rate"] = (r["noframe"] or 0) / r["cycles"]
    return out


def f(v):
    return "?" if v is None else ("%.1f" % v)


def report(r, m):
    lk = "etf (lead %d / asap %d)" % (r["lead_us"], r["asap_us"]) if r["etf"] else "af_packet"
    print("### %s" % r["log"])
    if not m:
        print("no DC or no cycle in this log: no dead-time model\n")
        return
    print("cycle %s us, %s, DC setpoint %d %%, rx guard %d us, DC |e| p99.99 %s us; NOFRAME %s / %s"
          % (f(r["cycle_us"]), lk, r["setpoint_pct"], r["guard_us"], f(r["dc_e"]), r["noframe"], r["cycles"]))
    print()
    print("| | p99.99 | max |")
    print("|---|---|---|")
    print("| dead time input latch -> output applied | %s us (2 cycles) | %s us |" % (f(m["dead_us"]), f(m["dead_us"])))
    print("| send offset after the tick (wake + prep_send, or launch error) | %s | %s |" % (f(m["send_p9999"]), f(m["send_max"])))
    print("| turnaround | %s | %s |" % (f(r["turnaround"][0]), f(r["turnaround"][1])))
    print("| hook | %s | %s |" % (f(r["hook"][0]), f(r["hook"][1])))
    print("| occupancy | %s | %s |" % (f(r["occupancy"][0]), f(r["occupancy"][1])))
    print("| output margin to SYNC0 ((1-p)c - e - send - turnaround p50) | %s | %s |" % (f(m["out_margin_p9999"]), f(m["out_margin_max"])))
    print("| input margin (p c - e) | %s | %s |" % (f(m["in_margin_p9999"]), f(m["in_margin_max"])))
    print("| receive margin (c - asap - guard - turnaround) | %s | %s |" % (f(m["rx_margin_p9999"]), f(m["rx_margin_max"])))
    print("| cycle margin (c - occupancy) | %s | %s |" % (f(m["cyc_margin_p9999"]), f(m["cyc_margin_max"])))
    bad = [k for k in ("out_margin_max", "in_margin_max", "rx_margin_max", "cyc_margin_max")
           if m.get(k) is not None and m[k] < 0]
    if "nofr_rate" in m:
        print()
        print("A NOFRAME cycle loses that cycle's inputs: dead time 3 c for that sample; rate %.1e" % m["nofr_rate"])
    print()
    print("verdict: %s\n" % ("negative margin at max: " + ", ".join(bad) if bad else "every margin >= 0 at max"))


def main(argv):
    csv = "--csv" in argv
    logs = [a for a in argv if a != "--csv"]
    if not logs:
        print(__doc__)
        return 2
    rc = 0
    if csv:
        print("log,cycle_us,link,setpoint_pct,guard_us,dead_us,out_margin_p9999,out_margin_max,"
              "in_margin_max,rx_margin_p9999,rx_margin_max,cyc_margin_max,noframe,cycles")
    for lg in logs:
        r = parse(lg)
        m = model(r)
        if csv:
            if not m:
                print("%s,,,,,,,,,,,,," % lg)
                rc = 1
                continue
            print(",".join(str(x) for x in (lg, f(r["cycle_us"]), "etf" if r["etf"] else "af", r["setpoint_pct"],
                  r["guard_us"], f(m["dead_us"]), f(m["out_margin_p9999"]), f(m["out_margin_max"]),
                  f(m["in_margin_max"]), f(m["rx_margin_p9999"]), f(m["rx_margin_max"]),
                  f(m["cyc_margin_max"]), r["noframe"], r["cycles"])))
        else:
            report(r, m)
        if not m:
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
