#!/usr/bin/env python3
"""events_report.py -- Phase 10.0 R-07: per-event view of what ecm_run saw.

  events_report.py --log er_r07.log --events events.txt --dir LOGDIR --settle 20 --alive 1

er_r07.log: every ecm_run output line prefixed with CLOCK_MONOTONIC on arrival
(run_events_10_0.sh). events.txt: "<event> do|undo <mono>". Prints markdown;
exit 0 when ecm_run lived through every event and the bus ended in RUN.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bringup_report import nic_delta, sw_bits, sw_state  # noqa: E402

EVENT_RE = re.compile(r"\[(BUS|EMCY|RECOVER|RECOVERY|AXIS|SDO|SOEM|FRESH)\]|AL 0x|lost link|LOST|ecm_eni: ")
WHAT = {
    "estop": "E-stop pressed / released",
    "mainpower": "main power (L1/L2) off at OP / on",
    "cable": "cable drive 1 OUT -> drive 2 IN unplugged / plugged",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--events", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--settle", type=float, default=20)
    ap.add_argument("--alive", type=int, default=1)
    ap.add_argument("--sim", default="0")
    a = ap.parse_args()

    lines = []
    for ln in open(a.log, errors="replace"):
        m = re.match(r"(\d+\.\d+) (.*)", ln.rstrip("\n"))
        if m:
            lines.append((float(m.group(1)), m.group(2)))
    text = "\n".join(t for _, t in lines)
    ev = {}
    order = []
    for ln in open(a.events):
        name, kind, t = ln.split()
        if name not in ev:
            ev[name] = {}
            order.append(name)
        ev[name][kind] = float(t)

    print("# Phase 10.0 R-07 — events with the bus in OP, no axis enabled\n")
    if a.sim == "1":
        print("**SIMULATION** — events injected into soft_bus (stand-ins), not the real drives\n")
    m = re.search(r"state transitions: (.*)", text)
    if m:
        print(f"- start: {m.group(1)}")

    bad = []
    for name in order:
        d = ev[name]
        t0 = d.get("do")
        t1 = d.get("undo", t0)
        print(f"\n## {name}: {WHAT.get(name, name)}\n")
        if t0 is None:
            print("- (no DO time recorded)")
            continue
        print(f"- DO at t=0, RESTORE at t=+{t1 - t0:.1f} s; window -5.0 s .. +{t1 - t0 + a.settle:.1f} s\n")
        # operators press Enter 1..2 s after the action (seen on 9/10): start 5 s early
        win = [(t, s) for t, s in lines if t0 - 5.0 <= t <= t1 + a.settle and EVENT_RE.search(s)]
        # one lost frame = RUN -> DEGRADED (motion NOFRAME) and DEGRADED -> RUN within a few
        # ticks: counted, not listed. A DEGRADED -> RUN that ends a real episode is listed.
        hits, nblip, prev = [], 0, None
        for t, s in win:
            m = re.search(r"\[BUS\] tick=(\d+) .*?(RUN -> DEGRADED \(motion NOFRAME\)|DEGRADED -> RUN)\s*$", s)
            if m and m.group(2).startswith("RUN"):
                prev = (int(m.group(1)), t, s)
                continue
            if m and prev and int(m.group(1)) - prev[0] <= 5:
                nblip += 1
                prev = None
                continue
            if prev:
                hits.append(prev[1:])
                prev = None
            hits.append((t, s))
        if prev:
            hits.append(prev[1:])
        if nblip:
            print(f"- single-frame NOFRAME blips (RUN -> DEGRADED -> RUN within 5 ticks) in the window: {nblip}")
        if hits:
            print("| t (s) | ecm_run |")
            print("|---|---|")
            for t, s in hits[:60]:
                s = s.replace("|", "\\|")
                print(f"| {t - t0:+.3f} | `{s[:200]}` |")
            if len(hits) > 60:
                print(f"| | ... {len(hits) - 60} more lines |")
            emcy = sorted({mm.group(1) for _, s in hits for mm in [re.search(r"code (0x[0-9A-F]{4})", s)] if mm and "[EMCY]" in s})
            print(f"\n- EMCY codes: {', '.join(emcy) or 'none'}")
            states = [mm.group(1) for _, s in hits for mm in [re.search(r"\[BUS\] .* (\S+ -> \S+)", s)] if mm]
            if states:
                print(f"- bus states: {'; '.join(states[:12])}{' ...' if len(states) > 12 else ''}")
            print(f"\nVERDICT R-07/{name} OBSERVED {len(hits)} line(s)")
        else:
            print("- nothing on the master side (no bus state change, no EMCY)")
            print(f"\nVERDICT R-07/{name} NONE no master-side event")
        for tag in ("do", "after"):
            f = os.path.join(a.dir, f"diag_{name}_{tag}.txt")
            if os.path.exists(f):
                fl = [ln.rstrip() for ln in open(f, errors="replace") if re.search(r"\[(ERROR|WARN)\]", ln)]
                label = "3 s after DO" if tag == "do" else "after settling"
                print(f"- diag {label}: " + ("; ".join(x.strip() for x in fl[:6]) if fl else "no ERROR/WARN finding"))

    print("\n## End of run\n")
    m = re.search(r"\[POLICY\] final bus state (\S+); entered: (.*)", text)
    final = m.group(1) if m else None
    print(f"- final bus state: {final or '(not printed)'}" + (f"; entered {m.group(2)}" if m else ""))
    m = re.search(r"\[POLICY\] recoveries: (.*)", text)
    if m:
        print(f"- recoveries: {m.group(1)}")
    m = re.search(r"\[WKC GROUP_MOTION\] (.*)", text)
    if m:
        print(f"- WKC {m.group(1)}")
    for mm in re.finditer(r"\[PDO\] get (\S+) = 0x([0-9A-F]+)", text):
        name, v = mm.group(1), int(mm.group(2), 16)
        extra = f" {sw_state(v)} [{sw_bits(v)}]" if ":0x6041:" in name else ""
        print(f"- {name} = 0x{v:X}{extra}")
    b, af = os.path.join(a.dir, "ethtool_S_before.txt"), os.path.join(a.dir, "ethtool_S_after.txt")
    if os.path.exists(b) and os.path.exists(af):
        dd = nic_delta(b, af)
        print(f"- NIC error counters: {', '.join(f'{k} +{v}' for k, v in dd.items()) or 'none'}")
    if not a.alive:
        bad.append("ecm_run ended before the last event")
    if final != "RUN":
        bad.append(f"bus did not end in RUN ({final})")
    v = "FAIL" if bad else "PASS"
    print(f"\nVERDICT R-07 {v} " + ("; ".join(bad) if bad else "ecm_run ran through every event, bus back in RUN"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
