#!/usr/bin/env python3
"""is_parse.py -- Phase 11.5: turn one raw log into results.csv rows
(header: see is_report.py HEADER). Used by run_is_11_5.sh; Python 3.8+.

  is_parse.py cyclictest FILE CASE PROFILE VARIANT LOAD HIST_US
  is_parse.py ecm_run    FILE CASE PROFILE VARIANT LOAD
  is_parse.py tegrastats FILE CASE PROFILE VARIANT LOAD
  is_parse.py probe      FILE CASE PROFILE VARIANT LOAD   (tools/etf/txtime_probe output)

cyclictest: the -h histogram (1 us bins); a percentile beyond the histogram
is written ">HIST". ecm_run: the LAST block of wake_jitter / prep_send /
occupancy / turnaround lines (the end-of-run summary; bucket upper bounds of
ecm_hist, ~12 % wide), in us, with overrun / NOFRAME / DC unlocks / ETF
counters in `extra`. tegrastats: VDD_IN instant values (mW), max cpu@ temp.
A log without the expected lines gives a row with n=0 and the reason in
`extra` (the report shows it; nothing is silently dropped).
"""
import re
import sys


def row(case, prof, var, load, metric, n="", mn="", av="", p50="", p99="", p999="",
        p9999="", mx="", unit="us", extra=""):
    extra = str(extra).replace(",", ";")
    return ",".join(str(x) for x in (case, prof, var, load, metric, n, mn, av, p50, p99,
                                     p999, p9999, mx, unit, extra))


def cyclictest(path, case, prof, var, load, hist):
    cnt, mn, av, mx, ovf = {}, "", "", "", 0
    for ln in open(path, errors="replace"):
        p = ln.split()
        if len(p) == 2 and p[0].isdigit() and p[1].isdigit():
            cnt[int(p[0])] = int(p[1])
        elif ln.startswith("# Min Latencies:"):
            mn = int(p[3])
        elif ln.startswith("# Avg Latencies:"):
            av = int(p[3])
        elif ln.startswith("# Max Latencies:"):
            mx = int(p[3])
        elif ln.startswith("# Histogram Overflows:"):
            ovf = int(p[3])
    n = sum(cnt.values()) + ovf
    if not n:
        return [row(case, prof, var, load, "cyclictest", 0, extra="no histogram in " + path)]

    def pct(q):
        acc = 0
        for k in sorted(cnt):
            acc += cnt[k]
            if acc >= q * n:
                return k
        return ">" + str(hist)
    return [row(case, prof, var, load, "cyclictest", n, mn, av, pct(.5), pct(.99), pct(.999),
                pct(.9999), mx, "us", "overflow=%d" % ovf)]


def ecm_run(path, case, prof, var, load):
    txt = open(path, errors="replace").read()
    lines = txt.splitlines()

    def first(rx):
        m = re.search(rx, txt)
        return m.group(1) if m else "?"
    ovr = first(r"\[GROUP_MOTION\] cycles=\d+ .*?overrun=(\d+)")
    nof = first(r"\[WKC GROUP_MOTION\] ok=\d+ noframe=(\d+)")
    wkm = first(r"\[GROUP_MOTION\] cycles=\d+ wkc_mismatch=(\d+)")
    unl = first(r"\[DC\] state=\S+ locks=\d+ unlocks=(\d+)")
    late = first(r"missed=(\d+) invalid=\d+")
    inv = first(r"missed=\d+ invalid=(\d+)")
    extra = "overrun=%s wkc_mismatch=%s noframe=%s dc_unlocks=%s" % (ovr, wkm, nof, unl)
    if late != "?":
        extra += " etf_missed=%s etf_invalid=%s etf_sent_now=%s" % (
            late, inv, first(r"late\(sent now\)=(\d+)"))
    dce = first(r"\[DC\] \|e\| \(after 5 s\).*?p99\.99=(\d+) us")
    if dce != "?":
        extra += " dc_e_p9999=%sus" % dce
    und = re.findall(r"\[CIA402\] axis \d+ .*?underrun=(\d+)", txt)
    if und:
        extra += " cia402_underrun=%d" % sum(int(u) for u in und)
    hk = first(r"\[HOOK\].*?p99\.99=(\d+)")
    if hk != "?":
        extra += " hook_p9999=%sns" % hk
    out = []
    for m in ("wake_jitter", "prep_send", "occupancy", "turnaround"):
        last = [ln for ln in lines if ln.startswith(m + " ")]
        if not last:
            out.append(row(case, prof, var, load, m, 0, extra="no %s line (rc?) %s" % (m, extra)))
            continue
        ln = last[-1]

        def raw(k):
            mm = re.search(re.escape(k) + r"=\s*(\d+)", ln)
            return int(mm.group(1)) if mm else None
        mx_ns = raw("max")

        def g(k):
            v = raw(k)
            if v is None:
                return ""
            if mx_ns is not None and k != "max":
                v = min(v, mx_ns)      # a bucket upper bound can lie above the max seen
            return "%.1f" % (v / 1000.0)
        n = re.search(r"n=\s*(\d+)", ln)
        out.append(row(case, prof, var, load, m, n.group(1) if n else "", "", "", g("p50"), g("p99"),
                       g("p99.9"), g("p99.99"), g("max"), "us", extra))
    return out


def probe(path, case, prof, var, load):
    """txtime_probe summary: launch_err (ns), wake jitter, send duration (us)"""
    txt = open(path, errors="replace").read()

    def first(rx):
        m = re.search(rx, txt)
        return m.group(1) if m else "?"
    extra = "sent=%s missed=%s invalid=%s late_wake=%s" % (
        first(r"attempts \d+\s+sent (\d+)"), first(r"ETF drops: missed (\d+)"),
        first(r"invalid_param (\d+)"), first(r"late wake \(woke after t_target\) (\d+)"))
    out = []
    for name, metric, div, unit in (("launch_err", "launch_err", 1, "ns"),
                                    ("wake jitter", "probe_wake", 1000.0, "us"),
                                    ("send call duration", "probe_send", 1000.0, "us")):
        m = re.search(re.escape(name) + r".*?min\s+(-?\d+)\s+p50\s+(-?\d+)\s+p99\s+(-?\d+)\s+p99\.9\s+(-?\d+)"
                      r"\s+p99\.99\s+(-?\d+)\s+max\s+(-?\d+) ns \(n=(\d+)\)", txt)
        if not m:
            out.append(row(case, prof, var, load, metric, 0, unit=unit, extra="no '%s' line %s" % (name, extra)))
            continue
        v = [int(x) for x in m.groups()[:6]]
        f = (lambda x: str(x)) if div == 1 else (lambda x: "%.1f" % (x / div))
        out.append(row(case, prof, var, load, metric, m.group(7), f(v[0]), "", f(v[1]), f(v[2]), f(v[3]),
                       f(v[4]), f(v[5]), unit, extra))
    return out


def tegrastats(path, case, prof, var, load):
    p, t = [], []
    for ln in open(path, errors="replace"):
        m = re.search(r"VDD_IN (\d+)mW", ln)
        if m:
            p.append(int(m.group(1)))
        t += [float(x) for x in re.findall(r"cpu@(-?[\d.]+)C", ln)]
    if not p:
        return [row(case, prof, var, load, "vdd_in", 0, unit="mW", extra="no VDD_IN in " + path)]
    return [row(case, prof, var, load, "vdd_in", len(p), min(p), "%.0f" % (sum(p) / len(p)),
                "", "", "", "", max(p), "mW", "cpu_temp_max=%sC" % (max(t) if t else "?"))]


def main(a):
    if len(a) < 6:
        print(__doc__)
        return 3
    kind, path = a[0], a[1]
    try:
        if kind == "cyclictest":
            rows = cyclictest(path, *a[2:6], a[6] if len(a) > 6 else "?")
        elif kind == "ecm_run":
            rows = ecm_run(path, *a[2:6])
        elif kind == "probe":
            rows = probe(path, *a[2:6])
        elif kind == "tegrastats":
            rows = tegrastats(path, *a[2:6])
        else:
            print("unknown kind " + kind, file=sys.stderr)
            return 3
    except OSError as e:
        rows = [row(*a[2:6], kind, 0, extra="unreadable: %s" % e)]
    print("\n".join(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
