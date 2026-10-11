#!/usr/bin/env python3
"""is_report.py -- Phase 11.5: merge results.csv of run_is_11_5.sh (one per
boot profile, P0 and P1) into Markdown tables for docs/rt_profiles.md.

  python3 tools/gd11/is_report.py log_gd11_is_P0_*/results.csv log_gd11_is_P1_*/results.csv

Rows are sorted by profile, variant (dma before nodma), load (idle before
load). The wake_jitter table carries the Phase 8.5 reference (p99.99 144 us,
max 277 us, measured before isolcpus was restored and before the c7 fix).
Exit 2 when a file does not have the run_is_11_5.sh header.
"""
import csv
import sys

HEADER = ["case", "profile", "variant", "load", "metric", "n", "min", "avg", "p50",
          "p99", "p999", "p9999", "max", "unit", "extra"]
ORDER = {"dma": 0, "nodma": 1, "rest": 0, "bus": 1, "idle": 0, "load": 1}
TITLES = {
    "is02": "IS-02 cyclictest, RT core, 1 ms (µs)",
    "is03": "IS-03 ecm_run N=8, 1 ms, soft_bus on veth (µs)",
    "is06": "IS-06 power (VDD_IN, mW)",
}
REF_8_5 = "| GĐ8 8.5 (29/9, no isolcpus, before c7 fix) | | | | | | | | 144 | 277 | reference |"


def load(paths):
    rows = []
    for p in paths:
        with open(p, newline="") as f:
            r = csv.reader(f)
            head = next(r, None)
            if head != HEADER:
                print(f"{p}: not a run_is_11_5.sh results.csv (header {head})", file=sys.stderr)
                sys.exit(2)
            for x in r:
                if len(x) == len(HEADER):
                    rows.append(dict(zip(HEADER, x)))
    return rows


def label(r):
    prof = r["profile"]
    if r["case"] in ("is02", "is03"):
        prof += "n" if r["variant"] == "nodma" else ""
    return prof


def main(argv):
    if not argv:
        print(__doc__)
        return 3
    rows = load(argv)
    out = []
    for case in ("is02", "is03", "is06"):
        sel = [r for r in rows if r["case"] == case]
        if not sel:
            continue
        sel.sort(key=lambda r: (r["metric"], r["profile"], ORDER.get(r["variant"], 9), ORDER.get(r["load"], 9)))
        out.append(f"### {TITLES[case]}\n")
        pref = ["wake_jitter", "occupancy", "prep_send", "turnaround"]
        metrics = sorted({r["metric"] for r in sel}, key=lambda m: (pref.index(m) if m in pref else 9, m))
        for m in metrics:
            if len(metrics) > 1:
                out.append(f"**{m}**\n")
            if case == "is06":
                out.append("| Profile | Run | n | min | mean | max | |")
                out.append("|---|---|---|---|---|---|---|")
                for r in sel:
                    out.append(f"| {r['profile']} | {r['variant']} | {r['n']} | {r['min']} | {r['avg']} | {r['max']} | {r['extra']} |")
            else:
                out.append("| Profile | Load | n | min | avg | p50 | p99 | p99.9 | p99.99 | max | |")
                out.append("|---|---|---|---|---|---|---|---|---|---|---|")
                for r in sel:
                    if r["metric"] != m:
                        continue
                    out.append(f"| {label(r)} | {r['load']} | {r['n']} | {r['min'] or '–'} | {r['avg'] or '–'} | "
                               f"{r['p50']} | {r['p99']} | {r['p999']} | {r['p9999']} | {r['max']} | {r['extra']} |")
                if m == "wake_jitter":
                    out.append(REF_8_5)
            out.append("")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
