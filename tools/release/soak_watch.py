#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
"""soak_watch.py -- Phase 12.5: watch a soak with moving drives from
outside the RT path, log the machine every PERIOD s, and stop the run at the
first fault (SIGTERM to ecm_run = its normal end: S6 walk-down of the axes).

  soak_watch.py --stats /run/ecmaster/soak/stats.json --log DIR [--period 10]
                [--warmup 60] [--temp-max 95] [--rss-growth-kib 1024]
                [--duration S]   planned run length: from S - 15 s on, axes
                                 leaving OPERATION_ENABLED are the S6 walk-down
  soak_watch.py --self-test        the fault rules on built-in samples,
                                   with negative controls

Reads stats.json (docs/api.md) and the machine (sysfs); writes DIR/watch.csv
(one line per period) and, at the end, DIR/watch_verdict.txt.

Fault rules (the first one ends the run):
  F1  an axis error (err != none), or an axis that was OPERATION_ENABLED
      leaves it (the master's latches already disabled it; stop the soak)
  F2  DC not LOCKED after the warm-up, or a DC unlock
  F3  overrun, or a zero / partial / over WKC in the motion group
  F4  more than 2 NOFRAME within one period (a burst, not single frames)
  F5  stats.json not updated for 5 periods while ecm_run lives
Recorded, judged at the end (not a stop):
  M1  CPU 3 frequency below 95 % of its maximum (throttling) in any sample
  M2  hottest thermal zone above --temp-max degC in any sample
  M3  ecm_run VmRSS grew more than --rss-growth-kib from the first sample
      after the warm-up to the last one
Exit: 0 the run ended without a fault and M1..M3 hold, 1 a fault or M1..M3,
2 usage. Ends when ecm_run's final stats.json ("final": true) appears.
"""
import glob
import json
import os
import signal
import sys
import time

MOTION = 0     # groups[0] is GROUP_MOTION


def read_json(path):
    try:
        with open(path) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def machine(pid):
    m = {}
    try:
        cur = int(open("/sys/devices/system/cpu/cpu3/cpufreq/scaling_cur_freq").read())
        mx = int(open("/sys/devices/system/cpu/cpu3/cpufreq/cpuinfo_max_freq").read())
        m["cpu3_khz"], m["cpu3_max_khz"] = cur, mx
    except (OSError, ValueError):
        pass
    temps = []
    for z in glob.glob("/sys/class/thermal/thermal_zone*/temp"):
        try:
            temps.append(int(open(z).read()) / 1000.0)
        except (OSError, ValueError):
            pass
    if temps:
        m["temp_max_c"] = max(temps)
    try:
        for ln in open("/proc/%d/status" % pid):
            if ln.startswith("VmRSS:"):
                m["rss_kib"] = int(ln.split()[1])
    except (OSError, ValueError, TypeError):
        pass
    return m


class Judge:
    """the fault rules; fed one stats.json document at a time"""

    def __init__(self, warmup_s, end_s=None):
        self.warmup_s = warmup_s
        self.end_s = end_s          # planned end: the S6 walk-down disables the axes then
        self.t0 = None
        self.enabled = set()
        self.prev_nofr = None
        self.fault = None

    def feed(self, doc, now):
        if self.fault or not doc:
            return self.fault
        if self.t0 is None:
            self.t0 = now
        warm = now - self.t0 >= self.warmup_s
        for i, a in enumerate(doc.get("axes") or []):
            if a.get("err", "none") != "none":
                self.fault = "F1 axis %d error %s (state %s)" % (i, a.get("err"), a.get("state"))
                return self.fault
            if a.get("state") == "OPERATION_ENABLED":
                self.enabled.add(i)
            elif i in self.enabled and not doc.get("final") and (self.end_s is None or now < self.end_s - 15):
                self.fault = "F1 axis %d left OPERATION_ENABLED (now %s)" % (i, a.get("state"))
                return self.fault
        dc = doc.get("dc") or {}
        if dc.get("unlocks"):
            self.fault = "F2 DC unlocks %s" % dc.get("unlocks")
            return self.fault
        if warm and dc and dc.get("state") != "LOCKED":
            self.fault = "F2 DC %s after the warm-up" % dc.get("state")
            return self.fault
        g = (doc.get("groups") or [{}])[MOTION]
        for k in ("overrun", "zero", "partial", "over"):
            if g.get(k):
                self.fault = "F3 motion %s %s" % (k, g.get(k))
                return self.fault
        nf = g.get("noframe")
        if nf is not None:
            if self.prev_nofr is not None and nf - self.prev_nofr > 2:
                self.fault = "F4 %d NOFRAME in one period (burst)" % (nf - self.prev_nofr)
                return self.fault
            self.prev_nofr = nf
        return None


def self_test():
    ok = 0
    bad = 0

    def doc(axes=("OPERATION_ENABLED", "OPERATION_ENABLED"), err="none", dc="LOCKED", unl=0, nf=0, ovr=0, final=False):
        return {"final": final, "axes": [{"state": s, "err": err} for s in axes],
                "dc": {"state": dc, "unlocks": unl}, "groups": [{"noframe": nf, "overrun": ovr, "zero": 0, "partial": 0, "over": 0}]}

    def case(name, docs, want):
        nonlocal ok, bad
        j = Judge(60)
        got = None
        for t, d in enumerate(docs):
            got = j.feed(d, t * 10.0)
        hit = (got or "").split(" ")[0] if got else None
        if hit == want:
            ok += 1
            print("  PASS %s -> %s" % (name, got or "no fault"))
        else:
            bad += 1
            print("  FAIL %s -> %s (want %s)" % (name, got, want))

    good = [doc(axes=("SWITCH_ON_DISABLED",) * 2, dc="ACQUIRE")] + [doc(nf=n // 5) for n in range(30)]
    case("clean run, 6 single NOFRAME, ends final", good + [doc(axes=("SWITCH_ON_DISABLED",) * 2, nf=6, final=True)], None)
    case("negative: axis error", good + [doc(err="following")], "F1")
    case("negative: axis drops out of OPERATION_ENABLED", good + [doc(axes=("OPERATION_ENABLED", "SWITCH_ON_DISABLED"))], "F1")
    case("negative: DC unlock", good + [doc(unl=1)], "F2")
    case("negative: DC not LOCKED after warm-up", good[:8] + [doc(dc="ACQUIRE")], "F2")
    case("DC ACQUIRE during the warm-up is fine", [doc(dc="ACQUIRE"), doc(dc="ACQUIRE"), doc()], None)
    case("negative: overrun", good + [doc(ovr=1)], "F3")
    j = Judge(60, end_s=300)
    for t in range(30):
        j.feed(doc(), t * 10.0)
    walk = j.feed(doc(axes=("SWITCH_ON_DISABLED",) * 2), 295.0)
    print("  %s S6 walk-down near the planned end is not a fault -> %s" % ("PASS" if walk is None else "FAIL", walk or "no fault"))
    ok, bad = (ok + 1, bad) if walk is None else (ok, bad + 1)
    case("negative: NOFRAME burst", good + [doc(nf=6), doc(nf=10)], "F4")
    print("RESULT: %d pass, %d fail" % (ok, bad))
    return 0 if bad == 0 else 1


def main(av):
    if "--self-test" in av:
        return self_test()
    opt = {"--period": "10", "--warmup": "60", "--temp-max": "95", "--rss-growth-kib": "1024", "--duration": ""}
    i = 0
    while i < len(av):
        if av[i] in ("--stats", "--log", "--period", "--warmup", "--temp-max", "--rss-growth-kib", "--duration") and i + 1 < len(av):
            opt[av[i]] = av[i + 1]
            i += 2
        else:
            print(__doc__)
            return 2
    if "--stats" not in opt or "--log" not in opt:
        print(__doc__)
        return 2
    period = float(opt["--period"])
    os.makedirs(opt["--log"], exist_ok=True)
    csv = open(os.path.join(opt["--log"], "watch.csv"), "w", buffering=1)
    csv.write("t_s,cycles,noframe,dc,axes,cpu3_khz,cpu3_max_khz,temp_max_c,rss_kib\n")
    judge = Judge(float(opt["--warmup"]), float(opt["--duration"]) if opt["--duration"] else None)
    t_start = time.monotonic()
    last_mtime, stale, pid = None, 0, None
    m1 = m2 = 0
    rss_first = rss_last = None
    fault = None
    while True:
        time.sleep(period)
        now = time.monotonic() - t_start
        doc = read_json(opt["--stats"])
        try:
            mt = os.stat(opt["--stats"]).st_mtime
        except OSError:
            mt = None
        if doc:
            pid = doc.get("pid", pid)                 # stats.json top level (docs/api.md)
        alive = pid is not None and os.path.exists("/proc/%d" % pid)
        if doc is None and pid is None:
            continue                                  # ecm_run not started yet
        stale = stale + 1 if (mt == last_mtime and alive) else 0
        last_mtime = mt
        m = machine(pid) if alive else {}
        g = ((doc or {}).get("groups") or [{}])[MOTION]
        csv.write("%.0f,%s,%s,%s,%s,%s,%s,%s,%s\n" % (
            now, g.get("cycles", ""), g.get("noframe", ""), ((doc or {}).get("dc") or {}).get("state", ""),
            "/".join(a.get("state", "?") for a in (doc or {}).get("axes") or []),
            m.get("cpu3_khz", ""), m.get("cpu3_max_khz", ""), m.get("temp_max_c", ""), m.get("rss_kib", "")))
        if "cpu3_khz" in m and m["cpu3_khz"] < 0.95 * m["cpu3_max_khz"]:
            m1 += 1
        if m.get("temp_max_c", 0) > float(opt["--temp-max"]):
            m2 += 1
        if "rss_kib" in m and now >= float(opt["--warmup"]):
            rss_first = rss_first if rss_first is not None else m["rss_kib"]
            rss_last = m["rss_kib"]
        if doc and doc.get("final"):
            break
        fault = judge.feed(doc, now) or (("F5 stats.json not updated for %d periods" % stale) if stale >= 5 else None)
        if fault:
            if alive:
                os.kill(pid, signal.SIGTERM)          # normal end: S6, final stats.json
            for _ in range(60):
                time.sleep(1)
                d = read_json(opt["--stats"])
                if d and d.get("final"):
                    break
            break
        if not alive and pid is not None:
            fault = "F5 ecm_run is gone without a final stats.json"
            break
    growth = (rss_last - rss_first) if (rss_first is not None and rss_last is not None) else None
    lines = ["fault: %s" % (fault or "none"),
             "M1 samples with CPU 3 below 95 %% of max: %d" % m1,
             "M2 samples above %s degC: %d" % (opt["--temp-max"], m2),
             "M3 ecm_run VmRSS growth after the warm-up: %s KiB (limit %s)" % (growth, opt["--rss-growth-kib"])]
    verdict_ok = fault is None and m1 == 0 and m2 == 0 and (growth is None or growth <= int(opt["--rss-growth-kib"]))
    lines.append("VERDICT: %s" % ("PASS" if verdict_ok else "FAIL"))
    with open(os.path.join(opt["--log"], "watch_verdict.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join("soak_watch: " + ln for ln in lines))
    return 0 if verdict_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
