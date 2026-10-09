#!/usr/bin/env python3
"""eni_limits.py -- Phase 10.9: a copy of an enicfg whose PS CoE InitCmds also
write the drive limits of the first enable on the real IS620N drives.

  eni_limits.py eni_2servo.enicfg -o eni_2servo_lim.enicfg \
      --torque-permille 150 --ferr 1048576 --max-vel 8388608

Why in the ENI: the PS InitCmds run at every start AND (since patch 0018)
again when a power-cycled drive is reconfigured, and a failed one stops the
slave before SAFE-OP / OP. A limit written once by hand would be lost at the
next power loss (SDO writes go to RAM; the drive powers up with its own
defaults: 0x6072 = 3000 = 300 %).

Objects (axis 0 of every CoE slave with DC; offset 0x800 x n is not used):
  0x6072:00 u16  max torque, 0.1 % of rated       (--torque-permille, 1..1000)
  0x6065:00 u32  following error window, inc       (--ferr)
  0x607F:00 u32  max profile velocity, inc/s       (--max-vel, PP/PV/HM)
The file is marked DERIVED (header comment); `source` keeps the original.
"""
import argparse
import re
import sys


def le(v, n):
    return v.to_bytes(n, "little").hex()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("enicfg")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--torque-permille", type=int, required=True)
    ap.add_argument("--ferr", type=int, required=True)
    ap.add_argument("--max-vel", type=int, required=True)
    a = ap.parse_args()
    if not 1 <= a.torque_permille <= 1000:
        sys.exit("--torque-permille 1..1000 (0.1 % units: 200 = 20 %)")
    if not 1 <= a.ferr < 2 ** 32 or not 1 <= a.max_vel < 2 ** 32:
        sys.exit("--ferr / --max-vel out of range")
    lines = open(a.enicfg).read().splitlines()
    slaves = [int(m.group(1)) for ln in lines for m in [re.match(r"slave (\d+) .* dc 1 ", ln)] if m]
    if not slaves:
        sys.exit("no DC slave in the ENI")
    for ln in lines:
        m = re.match(r"coe \d+ .*index (0x60(72|65|7F)) ", ln, re.I)
        if m:
            sys.exit(f"refusing: the ENI already writes {m.group(1)} ({ln.strip()})")
    add = []
    for s in slaves:
        for idx, n, v in ((0x6072, 2, a.torque_permille), (0x6065, 4, a.ferr), (0x607F, 4, a.max_vel)):
            add.append(f"coe {s} trans PS ccs 1 index 0x{idx:04X} sub 0x00 ca 0 timeout_ms 0 data {le(v, n)}")
    hdr = (f"# DERIVED by tools/eni/eni_limits.py from {a.enicfg}: 0x6072 = {a.torque_permille} "
           f"({a.torque_permille / 10:g} %), 0x6065 = {a.ferr}, 0x607F = {a.max_vel} on slave(s) "
           f"{','.join(map(str, slaves))} -- limits for the first enable, not a TwinCAT ENI")
    with open(a.out, "w") as f:
        f.write(hdr + "\n" + "\n".join(lines + add) + "\n")
    print(f"{a.out}: +{len(add)} PS CoE InitCmd(s): 0x6072={a.torque_permille} 0x6065={a.ferr} "
          f"0x607F={a.max_vel} on slave(s) {','.join(map(str, slaves))}")


if __name__ == "__main__":
    main()
