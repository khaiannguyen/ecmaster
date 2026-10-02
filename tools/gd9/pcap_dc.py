#!/usr/bin/env python3
"""pcap_dc.py -- Phase 9.5: what DC configuration the master wrote, per slave.

Reads a classic pcap (tshark -F pcap) captured on the master's interface and
prints, for every station address that received an FPWR of a DC register,
the LAST value written (the configuration that stays), plus which station the
cyclic FRMW of 0x0910 reads (the reference clock):

    1001 0980=0000 0981=03 09A0=1000000 09A4=- shift=12345
    ref 1002

shift = SYNC0 start time (0x0990) modulo the SYNC0 cycle, i.e. the CyclShift
the master added to a whole cycle (ecx_dcsync0: start = k * cycle + shift).
Only meaningful against the same soft_bus run; slaves never armed show
0981=- (no write). Used by run_dc_9_5.sh (DC-01..DC-04).

Usage: pcap_dc.py capture.pcap
"""
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "..", "golden"))
from pcap2struct import read_pcap  # noqa: E402

FPWR, FRMW = 5, 14
REGS = (0x0980, 0x0981, 0x0990, 0x09A0, 0x09A4)


def dgrams(frame):
    if len(frame) < 16 or frame[12:14] != b"\x88\xa4":
        return []
    out, off = [], 16
    while off + 10 <= len(frame):
        cmd, _idx, adp, ado, ln, _irq = struct.unpack("<BBHHHH", frame[off:off + 10])
        dlen = ln & 0x07FF
        out.append((cmd, adp, ado, frame[off + 10:off + 10 + dlen]))
        off += 12 + dlen
        if not ln & 0x8000:
            break
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    last = {}      # station -> {reg: bytes}
    refs = {}      # station read by FRMW 0x0910 -> count
    for frame in read_pcap(sys.argv[1]):
        for cmd, adp, ado, data in dgrams(frame):
            if cmd == FRMW and ado == 0x0910:
                refs[adp] = refs.get(adp, 0) + 1
            if cmd != FPWR:
                continue
            for r in REGS:
                if ado <= r < ado + len(data):
                    o = r - ado
                    width = {0x0980: 1, 0x0981: 1, 0x0990: 8, 0x09A0: 4, 0x09A4: 4}[r]
                    if o + width <= len(data):
                        last.setdefault(adp, {})[r] = data[o:o + width]
    for st in sorted(last):
        d = last[st]
        f = []
        f.append("0980=%02X" % d[0x0980][0] if 0x0980 in d else "0980=-")
        f.append("0981=%02X" % d[0x0981][0] if 0x0981 in d else "0981=-")
        c0 = struct.unpack("<I", d[0x09A0])[0] if 0x09A0 in d else None
        f.append("09A0=%s" % (c0 if c0 is not None else "-"))
        f.append("09A4=%s" % (struct.unpack("<I", d[0x09A4])[0] if 0x09A4 in d else "-"))
        if 0x0990 in d and c0:
            start = struct.unpack("<Q", d[0x0990])[0]
            sh = start % c0
            if sh > c0 // 2:
                sh -= c0          # negative shift
            f.append("shift=%d" % sh)
        else:
            f.append("shift=-")
        print("%04X %s" % (st, " ".join(f)))
    if refs:
        print("ref %04X" % max(refs, key=refs.get))
    else:
        print("ref -")
    return 0


if __name__ == "__main__":
    sys.exit(main())
