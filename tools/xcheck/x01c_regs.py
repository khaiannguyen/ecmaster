#!/usr/bin/env python3
"""x01c_regs.py -- Phase 8 X-01c: which ESC registers does each master touch?

Reads two pcap files (SOEM run, IgH run) captured on the master side of a
soft_bus rig, walks every EtherCAT datagram in frames SENT BY THE MASTER, and
prints a per-register table. Registers touched by only one master are flagged:
those are soft_bus code paths the other master never exercised.

Usage: python3 x01c_regs.py soem.pcap igh.pcap [--data]
Master frames: soft_bus sets bit 1 of the source MAC in its replies (Phase 7.1).
Only pcap (not pcapng) -- capture with `tshark -F pcap`.
"""
import struct
import sys
from collections import defaultdict

CMD = {0x00: "NOP", 0x01: "APRD", 0x02: "APWR", 0x03: "APRW", 0x04: "FPRD",
       0x05: "FPWR", 0x06: "FPRW", 0x07: "BRD", 0x08: "BWR", 0x09: "BRW",
       0x0A: "LRD", 0x0B: "LWR", 0x0C: "LRW", 0x0D: "ARMW", 0x0E: "FRMW"}
LOGICAL = {0x0A, 0x0B, 0x0C}
READS = {0x01, 0x04, 0x07}
WRITES = {0x02, 0x05, 0x08}

REGS = [  # (start, end_inclusive, name) -- Beckhoff ESC datasheet Section II
    (0x0000, 0x0000, "Type"), (0x0001, 0x0001, "Revision"), (0x0002, 0x0003, "Build"),
    (0x0004, 0x0004, "FMMUs supported"), (0x0005, 0x0005, "SMs supported"),
    (0x0006, 0x0006, "RAM size"), (0x0007, 0x0007, "Port descriptor"),
    (0x0008, 0x0009, "ESC features"),
    (0x0010, 0x0011, "Station address"), (0x0012, 0x0013, "Station alias"),
    (0x0020, 0x0031, "Write/ESC reset regs"), (0x0040, 0x0041, "ESC reset"),
    (0x0100, 0x0103, "DL control"), (0x0108, 0x0109, "Physical RW offset"),
    (0x0110, 0x0111, "DL status"),
    (0x0120, 0x0121, "AL control"), (0x0130, 0x0131, "AL status"),
    (0x0134, 0x0135, "AL status code"), (0x0138, 0x0139, "RUN/ERR LED override"),
    (0x0140, 0x0140, "PDI control"), (0x0141, 0x0141, "ESC config"),
    (0x0150, 0x0153, "PDI config"),
    (0x0200, 0x0201, "ECAT event mask"), (0x0204, 0x0207, "AL event mask"),
    (0x0210, 0x0211, "ECAT event request"), (0x0220, 0x0223, "AL event request"),
    (0x0300, 0x0307, "RX error counters"), (0x0308, 0x030B, "Fwd RX error counters"),
    (0x030C, 0x030C, "ECAT PU error counter"), (0x030D, 0x030F, "PDI error counter/code"),
    (0x0310, 0x0313, "Lost link counters"),
    (0x0400, 0x0401, "WD divider"), (0x0410, 0x0411, "WD time PDI"),
    (0x0420, 0x0421, "WD time process data"), (0x0440, 0x0441, "WD status PD"),
    (0x0442, 0x0442, "WD counter PD"), (0x0443, 0x0443, "WD counter PDI"),
    (0x0500, 0x0500, "SII config/ECAT access"), (0x0501, 0x0501, "SII PDI access"),
    (0x0502, 0x0503, "SII control/status"), (0x0504, 0x0507, "SII address"),
    (0x0508, 0x050F, "SII data"),
    (0x0510, 0x0515, "MII management"), (0x0516, 0x0517, "MII access"),
    (0x0600, 0x06FF, "FMMU"), (0x0800, 0x087F, "SyncManager"),
    (0x0900, 0x090F, "DC receive time ports"), (0x0910, 0x0917, "DC system time"),
    (0x0918, 0x091F, "DC receive time PU"), (0x0920, 0x0927, "DC system time offset"),
    (0x0928, 0x092B, "DC system time delay"), (0x092C, 0x092F, "DC system time diff"),
    (0x0930, 0x0931, "DC speed counter start"), (0x0932, 0x0933, "DC speed counter diff"),
    (0x0934, 0x0934, "DC time diff filter depth"), (0x0935, 0x0935, "DC speed filter depth"),
    (0x0980, 0x0980, "DC cyclic unit control"), (0x0981, 0x0981, "DC activation"),
    (0x0982, 0x0983, "DC SYNC pulse length"), (0x0984, 0x0984, "DC activation status"),
    (0x098E, 0x098F, "DC SYNC0/1 status"), (0x0990, 0x0997, "DC start time cyclic op"),
    (0x0998, 0x099F, "DC next SYNC1 pulse"), (0x09A0, 0x09A3, "DC SYNC0 cycle time"),
    (0x09A4, 0x09A7, "DC SYNC1 cycle time"), (0x09A8, 0x09AF, "DC latch control/status"),
    (0x0E00, 0x0EFF, "ESC specific"), (0x1000, 0xFFFF, "DPRAM (mailbox/PD)"),
]


def reg_name(ado):
    for lo, hi, name in REGS:
        if lo <= ado <= hi:
            return name
    return "?"


def read_pcap(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        if len(hdr) < 24:
            raise SystemExit(f"{path}: not a pcap file")
        magic = struct.unpack("<I", hdr[:4])[0]
        if magic in (0xA1B2C3D4, 0xA1B23C4D):
            endian = "<"
        elif magic in (0xD4C3B2A1, 0x4D3CB2A1):
            endian = ">"
        elif magic == 0x0A0D0D0A:
            raise SystemExit(f"{path}: pcapng -- capture again with `tshark -F pcap`")
        else:
            raise SystemExit(f"{path}: unknown magic 0x{magic:08x}")
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            _, _, incl, _ = struct.unpack(endian + "IIII", rh)
            yield f.read(incl)


def datagrams(frame):
    """Datagrams of a frame SENT BY THE MASTER.

    Master vs returned frame: a master sends every datagram with WKC = 0; the
    returned copy almost always has WKC > 0 somewhere. (The source-MAC bit-1
    rule failed for IgH: its MAC 02:00:00:00:00:10 already has that bit set.)
    A returned frame nobody answered also has all-zero WKCs and gets counted
    twice -- that only inflates the counts, never the register set."""
    if len(frame) < 16 or frame[12:14] != b"\x88\xa4":
        return
    ec_len = struct.unpack("<H", frame[14:16])[0] & 0x07FF
    off, end = 16, min(16 + ec_len, len(frame))
    dgs = []
    while off + 12 <= end:
        cmd = frame[off]
        adp, ado, lf = struct.unpack("<HHH", frame[off + 2:off + 8])
        dlen = lf & 0x07FF
        if off + 10 + dlen + 2 > len(frame):
            break
        wkc = struct.unpack("<H", frame[off + 10 + dlen:off + 12 + dlen])[0]
        dgs.append((cmd, adp, ado, dlen, frame[off + 10:off + 10 + dlen], wkc))
        off += 10 + dlen + 2
        if not (lf & 0x8000):
            break
    if any(d[5] != 0 for d in dgs):
        return                                   # returned frame
    for cmd, adp, ado, dlen, data, _ in dgs:
        yield cmd, adp, ado, dlen, data


def collect(path):
    stats = defaultdict(lambda: {"cmds": set(), "n": 0, "addrs": set(), "wvals": set()})
    frames = 0
    for fr in read_pcap(path):
        got = False
        for cmd, adp, ado, dlen, data in datagrams(fr):
            got = True
            if cmd in LOGICAL:
                s = stats[("LOG", f"logical {CMD[cmd]}")]
                s["cmds"].add(CMD[cmd]); s["n"] += 1
                continue
            if cmd == 0x00:
                continue
            kind = "R" if cmd in READS else "W" if cmd in WRITES else "RW"
            end_ado = ado + max(dlen, 1) - 1
            # attribute the datagram to EVERY register its byte range covers:
            # IgH reads/writes several adjacent registers in one datagram
            for lo, hi, name in REGS:
                if lo > end_ado or hi < ado:
                    continue
                s = stats[(kind, name)]
                s["cmds"].add(CMD.get(cmd, f"0x{cmd:02X}"))
                s["n"] += 1
                s["addrs"].add((ado, dlen))
                if kind != "R" and name != "DPRAM (mailbox/PD)" and hi - lo < 8:
                    a, b = max(lo, ado), min(hi, end_ado)
                    s["wvals"].add((a, data[a - ado:b - ado + 1].hex()))
        frames += got
    return stats, frames


def fmt_addrs(addrs):
    items = sorted(addrs)
    txt = ", ".join(f"0x{a:04X}/{l}" for a, l in items[:4])
    return txt + (f" +{len(items) - 4}" if len(items) > 4 else "")


def first_addr(k, soem, igh):
    a = (soem.get(k) or igh.get(k))["addrs"]
    return min(x for x, _ in a) if a else 0x10000


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_data = "--data" in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    soem, nf_s = collect(args[0])
    igh, nf_i = collect(args[1])
    print(f"SOEM: {args[0]}  ({nf_s} master frames)")
    print(f"IgH : {args[1]}  ({nf_i} master frames)\n")
    keys = sorted(set(soem) | set(igh), key=lambda k: (first_addr(k, soem, igh), k[0]))
    print(f"{'kind':4} {'register':28} {'SOEM n':>8} {'IgH n':>8}  flag   SOEM cmds addr/len  |  IgH cmds addr/len")
    only_igh, only_soem = [], []
    for k in keys:
        s, i = soem.get(k), igh.get(k)
        flag = ""
        if s and not i:
            flag = "SOEM"; only_soem.append(k)
        elif i and not s:
            flag = "IgH!"; only_igh.append(k)
        sc = ("/".join(sorted(s["cmds"])) + " " + fmt_addrs(s["addrs"])) if s else "-"
        ic = ("/".join(sorted(i["cmds"])) + " " + fmt_addrs(i["addrs"])) if i else "-"
        print(f"{k[0]:4} {k[1]:28} {s['n'] if s else 0:8} {i['n'] if i else 0:8}  {flag:5}  {sc}  |  {ic}")
    print("\n=== touched by IgH only (soft_bus paths SOEM never exercises) ===")
    for k in only_igh:
        print(f"  {k[0]:3} {k[1]}")
    print("\n=== touched by SOEM only ===")
    for k in only_soem:
        print(f"  {k[0]:3} {k[1]}")
    if show_data:
        print("\n=== distinct values WRITTEN (<= 8 bytes, non-DPRAM) ===")
        for k in keys:
            if k[0] in ("R", "LOG"):
                continue
            sv = sorted(soem[k]["wvals"]) if k in soem else []
            iv = sorted(igh[k]["wvals"]) if k in igh else []
            if not sv and not iv:
                continue
            print(f"-- {k[0]} {k[1]}")
            print(f"   SOEM: {', '.join(f'0x{a:04X}={v}' for a, v in sv[:6])}{' ...' if len(sv) > 6 else ''}")
            print(f"   IgH : {', '.join(f'0x{a:04X}={v}' for a, v in iv[:6])}{' ...' if len(iv) > 6 else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
