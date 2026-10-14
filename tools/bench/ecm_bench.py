#!/usr/bin/env python3
"""ecm_bench.py -- Phase 11.7 BM-01: benchmark tables from the final
stats.json of ecm_run runs (claude/giai_doan_11_ke_hoach.md §8.2), laid out
like the acontis "EC-Master on Jetson Orin" figures (min / avg / max) plus
the project's percentiles (p50 / p99 / p99.9 / p99.99). Outside the RT path.

  ecm_bench.py [--label NAME=FILE ...] FILE [FILE ...]     Markdown on stdout
  ecm_bench.py --csv ...                                   one row per (run, metric)

A FILE is a stats.json written at the end of a run ("final": true). The
label of a run is NAME when given with --label NAME=FILE, else the file's
parent directory name.

Refused (exit 2, nothing printed for that file):
  - "final" is not true (a file taken during the run: its counts are partial)
  - rt.isolated_cpus is absent (the RT configuration of the run is unknown,
    the numbers would not be reproducible); an empty value is accepted with
    a warning (no isolcpus on that machine)

Mapping (§8.2): cycle time = cycle +/- the wake jitter spread; task duration
= occupancy (send + receive + DC + hook); EC-Master CPU load ~ prep_send;
CtlErr ~ DC |e| (master-shift PI error); turnaround and hook have no acontis
counterpart. Percentiles are bucket upper bounds (log20: ~12 % wide), clamped
to the max seen.
"""
import json
import os
import sys

METRICS = [   # key, label, acontis counterpart
    ("wake_jitter", "wake jitter (cycle start lateness)", "Cycle Time = cycle +/- spread"),
    ("occupancy", "occupancy (send+receive+DC+hook)", "Task duration"),
    ("prep_send", "prep_send (frame build + send)", "~ EC-Master CPU load"),
    ("turnaround", "turnaround (SW TX ts -> SW RX ts)", "-"),
    ("hook", "hook (CiA402 layer)", "~ App part of task duration"),
    ("dc_err", "DC |e| (master-shift error)", "~ CtlErr (-dcmlog)"),
]
QS = (0.50, 0.99, 0.999, 0.9999)


class Refused(Exception):
    pass


def stats(doc, key):
    h = (doc.get("hist") or {}).get(key)
    if not h or not h.get("count"):
        return None
    counts = h.get("counts") or []
    b = doc["buckets"][h["buckets"]]
    if h["buckets"].startswith("lin1us"):
        upper = [float(i + 1) for i in range(len(counts))]          # us
        nz = [i for i, c in enumerate(counts) if c]
        mn = float(nz[0]) if nz else 0.0
        mx = float(nz[-1] + 1) if nz else 0.0
        avg = sum(i * c for i, c in enumerate(counts)) / max(1, sum(counts))
        scale = 1.0
    else:
        upper = [float(u) for u in b["upper"]] + [float("inf")]
        scale = 1000.0                                                 # ns -> us
        mn, mx = h["min"] / scale, h["max"] / scale
        avg = h["sum"] / h["count"] / scale
        upper = [u / scale for u in upper]
    total = sum(counts)
    out = {"n": h["count"], "min": mn, "avg": avg, "max": mx}
    for q in QS:
        want, acc, v = q * total, 0, mx
        for i, c in enumerate(counts):
            acc += c
            if acc >= want:
                v = min(upper[i], mx)
                break
        out["p%g" % (q * 100)] = v
    return out


def load(path):
    doc = json.load(open(path))
    if doc.get("final") is not True:
        raise Refused("%s: not a final stats.json (\"final\": %s) -- take the file after ecm_run has stopped"
                      % (path, doc.get("final")))
    rt = doc.get("rt") or {}
    if "isolated_cpus" not in rt:
        raise Refused("%s: rt.isolated_cpus missing -- RT configuration unknown (ecm_run before Phase 11.3?)" % path)
    return doc


def config_lines(label, doc):
    rt, bus = doc.get("rt", {}), doc.get("bus", {})
    g = (doc.get("groups") or [{}])[0]
    cpuidle = rt.get("cpuidle") or {}
    port = (bus.get("ports") or [{}])[0]
    yield "- **%s**: cycle %g us, link %s, %s slaves, %s cycles, overrun %s, NOFRAME %s, DC %s (unlocks %s)" % (
        label, bus.get("cycle_ns", 0) / 1000.0, bus.get("link"), bus.get("n_slaves"), g.get("cycles"),
        g.get("overrun"), g.get("noframe"), (doc.get("dc") or {}).get("state"), (doc.get("dc") or {}).get("unlocks"))
    yield "  kernel %s, isolated CPUs '%s', RT CPU %s, cpuidle %s, dma_latency held %s, cmdline %s, hook %s, API CPUs %s, NIC CRC %s" % (
        rt.get("kernel"), rt.get("isolated_cpus"), rt.get("rt_cpu"),
        "off" if cpuidle.get("off") else (cpuidle.get("driver") or "?"), rt.get("dma_latency_held"),
        " ".join(rt.get("cmdline_flags") or []), rt.get("hook"), rt.get("api_cpus"), port.get("crc_err"))
    link = doc.get("link") or {}
    if bus.get("link") == "etf":
        yield "  ETF missed %s invalid %s late %s bypass %s" % (link.get("etf_missed"), link.get("etf_invalid"),
                                                          link.get("txtime_late"), link.get("txtime_bypass"))


def f(v):
    if v is None:
        return "-"
    return "%.1f" % v if v < 1000 else "%.0f" % v


def main(argv):
    csv = "--csv" in argv
    args = [a for a in argv if a != "--csv"]
    runs, i = [], 0
    while i < len(args):
        if args[i] == "--label" and i + 1 < len(args) and "=" in args[i + 1]:
            name, path = args[i + 1].split("=", 1)
            runs.append((name, path))
            i += 2
        else:
            runs.append((os.path.basename(os.path.dirname(os.path.abspath(args[i]))) or args[i], args[i]))
            i += 1
    if not runs:
        print(__doc__)
        return 2
    docs, rc = [], 0
    for label, path in runs:
        try:
            d = load(path)
        except (Refused, OSError, ValueError, KeyError) as e:
            print("ecm_bench: refused: %s" % e, file=sys.stderr)
            rc = 2
            continue
        if not (d.get("rt") or {}).get("isolated_cpus"):
            print("ecm_bench: warning: %s: no isolated CPU on that machine" % path, file=sys.stderr)
        docs.append((label, d))
    if csv:
        print("run,metric,n,min,avg,max,p50,p99,p99.9,p99.99,unit")
        for label, d in docs:
            for k, _, _ in METRICS:
                s = stats(d, k)
                if s:
                    print(",".join([label, k, str(s["n"])] + ["%.3f" % s[x] for x in
                          ("min", "avg", "max", "p50", "p99", "p99.9", "p99.99")] + ["us"]))
        return rc
    if not docs:
        return rc
    print("## Configuration\n")
    for label, d in docs:
        for ln in config_lines(label, d):
            print(ln)
    print()
    for k, name, ac in METRICS:
        rows = [(label, stats(d, k)) for label, d in docs]
        rows = [(l, s) for l, s in rows if s]
        if not rows:
            continue
        print("### %s — acontis: %s (us)\n" % (name, ac))
        print("| run | n | min | avg | max | p50 | p99 | p99.9 | p99.99 |")
        print("|---|---|---|---|---|---|---|---|---|")
        for l, s in rows:
            print("| %s | %d | %s | %s | %s | %s | %s | %s | %s |" % (l, s["n"], f(s["min"]), f(s["avg"]), f(s["max"]),
                  f(s["p50"]), f(s["p99"]), f(s["p99.9"]), f(s["p99.99"])))
        if k == "wake_jitter":
            for l, d in docs:
                s = stats(d, k)
                if s:
                    c = d["bus"]["cycle_ns"] / 1000.0
                    spread = s["max"] - s["min"]
                    print("\n%s: cycle time %g us, bounds %s ... %s us (cycle -/+ (max - min) of the wake jitter)"
                          % (l, c, f(max(0.0, c - spread)), f(c + spread)))
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
