#!/usr/bin/env python3
"""eni_dc_variant.py -- GD9.5: make a DC variant of a TwinCAT ENI, edited the
way TwinCAT itself would write it (DC element AND the matching PS register
InitCmds), so tools/eni/eni2cfg.py and the loader see a coherent file.

Edits (slave numbers are bus positions, 1-based, as in the ENI):
  --no-dc N[,N..]        remove DC from these slaves: the DC element and the
                         PS writes of 0x0980/0x0990/0x09A0/0x09A8 (TwinCAT:
                         Box > DC > "FreeRun")
  --shift N=NS           DC/ShiftTime of slave N (ns, may be negative)
  --sync1 N=NS           SYNC1 on slave N: CycleTime1 = NS, 0x0980 = 0x0700,
                         0x09A0 write gets the SYNC1 cycle in bytes 4..7
  --ref N                reference clock = slave N (the others false)
  --assign N=0xXXXX      write this AssignActivate into slave N's 0x0980 InitCmd
  --stale-initcmds       with --sync1: change only the DC element, leave the
                         register InitCmds as they were (an ENI that says two
                         different things -> the loader must refuse it)

Usage: eni_dc_variant.py IN.xml OUT.xml [edits...]
"""
import argparse
import xml.etree.ElementTree as ET


def num(t):
    t = (t or "0").strip()
    return int(t[2:], 16) if t.startswith(("#x", "#X")) else int(t, 0)


def ps_writes(slave, ado):
    out = []
    for ic in slave.findall("InitCmds/InitCmd"):
        if num(ic.findtext("Ado")) == ado and num(ic.findtext("Cmd")) in (2, 5, 8) and \
                "PS" in [t.text for t in ic.findall("Transition")]:
            out.append(ic)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--no-dc", default="")
    ap.add_argument("--shift", action="append", default=[])
    ap.add_argument("--sync1", action="append", default=[])
    ap.add_argument("--assign", action="append", default=[])
    ap.add_argument("--ref", type=int, default=0)
    ap.add_argument("--stale-initcmds", action="store_true")
    a = ap.parse_args()

    tree = ET.parse(a.src)
    slaves = tree.getroot().findall("Config/Slave")

    def sl(n):
        return slaves[n - 1]

    for n in [int(x) for x in a.no_dc.split(",") if x]:
        s = sl(n)
        dc = s.find("DC")
        if dc is not None:
            s.remove(dc)
        inits = s.find("InitCmds")
        for ado in (0x0980, 0x0990, 0x09A0, 0x09A8):
            for ic in ps_writes(s, ado):
                inits.remove(ic)
    for kv in a.shift:
        n, v = kv.split("=")
        sl(int(n)).find("DC/ShiftTime").text = str(int(v))
    for kv in a.sync1:
        n, v = kv.split("=")
        s = sl(int(n))
        s.find("DC/CycleTime1").text = str(int(v))
        if not a.stale_initcmds:
            for ic in ps_writes(s, 0x0980):
                ic.find("Data").text = "0007"
            for ic in ps_writes(s, 0x09A0):
                c0 = num(s.findtext("DC/CycleTime0"))
                ic.find("Data").text = (c0.to_bytes(4, "little") + int(v).to_bytes(4, "little")).hex()
    for kv in a.assign:
        n, v = kv.split("=")
        x = int(v, 0)
        for ic in ps_writes(sl(int(n)), 0x0980):
            ic.find("Data").text = bytes([x & 0xFF, x >> 8]).hex()
    if a.ref:
        for i, s in enumerate(slaves, 1):
            dc = s.find("DC")
            if dc is not None:
                dc.find("ReferenceClock").text = "true" if i == a.ref else "false"
    tree.write(a.dst, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    main()
