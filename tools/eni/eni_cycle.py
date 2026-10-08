#!/usr/bin/env python3
"""eni_cycle.py -- Phase 10.0 R-09: a copy of an enicfg for another cycle time.

  eni_cycle.py eni_2servo.enicfg 500 -o /tmp/eni_2servo_500us.enicfg

Changes exactly what depends on the cycle:
  cycle_us                      -> CYCLE
  sync0_ns of every DC slave    -> CYCLE * 1000
  the PS register InitCmd 0x09A0 (SYNC0 cycle, + SYNC1 when 8 byte) of
  every DC slave                -> CYCLE * 1000 (SYNC1 bytes unchanged)
and marks the file as derived (header comment, `source` line keeps the
original name + sha256, so it is clear what it came from). ecm_run's own
cross-check (0x09A0 data == sync0_ns, Phase 9.5) then still holds.

This is for measuring the drive's cycle limits. A product ENI for another
cycle comes from TwinCAT (the ENI may carry more than the SYNC0 time, e.g.
0x1C32:02 InitCmds); the tool refuses a file whose CoE InitCmds write
0x1C32/0x1C33 so such a difference cannot be missed.
"""
import argparse
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("enicfg")
    ap.add_argument("cycle_us", type=int)
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    if not 50 <= a.cycle_us <= 100000:
        sys.exit("cycle_us out of range (50 .. 100000)")
    ns = a.cycle_us * 1000
    le = ns.to_bytes(4, "little").hex()
    lines = open(a.enicfg).read().splitlines()
    dc_slaves = set()
    out, n_sync0, n_reg = [], 0, 0
    for ln in lines:
        if re.match(r"coe \d+ .*0x1C3[23]", ln, re.I):
            sys.exit(f"refusing: the ENI writes 0x1C32/0x1C33 by CoE InitCmd ({ln.strip()}); regenerate it in TwinCAT")
        if ln.startswith("cycle_us "):
            ln = f"cycle_us {a.cycle_us}"
        m = re.match(r"slave (\d+) ", ln)
        if m and re.search(r" dc 1 ", ln):
            dc_slaves.add(int(m.group(1)))
            ln, k = re.subn(r" sync0_ns \d+ ", f" sync0_ns {ns} ", ln)
            n_sync0 += k
        m = re.match(r"(reg (\d+) trans \S+ cmd \d+ ado 0x09A0 len (\d+) data )([0-9a-fA-F]+)(.*)", ln)
        if m and int(m.group(2)) in dc_slaves:
            data = m.group(4)
            ln = m.group(1) + le + data[8:] + m.group(5)
            n_reg += 1
        out.append(ln)
    if n_sync0 == 0:
        sys.exit("no DC slave with sync0_ns in the file")
    hdr = f"# DERIVED by tools/eni/eni_cycle.py from {a.enicfg}: cycle {a.cycle_us} us ({n_sync0} sync0_ns, {n_reg} 0x09A0 InitCmd(s)) -- measurement only, not a TwinCAT ENI"
    with open(a.out, "w") as f:
        f.write(hdr + "\n" + "\n".join(out) + "\n")
    print(f"{a.out}: cycle {a.cycle_us} us, {n_sync0} slave(s) sync0_ns, {n_reg} 0x09A0 InitCmd(s)")


if __name__ == "__main__":
    main()
