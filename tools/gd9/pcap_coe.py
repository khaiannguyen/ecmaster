#!/usr/bin/env python3
"""pcap_coe.py -- Phase 9.3: list the CoE SDO traffic of a master session.

Reads a classic pcap (tshark -F pcap) captured on the master's interface
and prints one line per CoE SDO request the master wrote into a mailbox
(FPWR to the slave's SM0) and per SDO response it read back (FPRD of SM1
with WKC 1 in the reply), for example

    req  1001 CA-UP    1C12:00
    resp 1001 CA-UP-EXP 1C12:00 4 byte
    resp 1001 ABORT    1C00:00 06020000

Only requests/replies that carry a CoE header are listed; empty mailbox
reads are skipped. Used by run_coe.sh for the wire checks of C-02/C-05.

Usage: pcap_coe.py capture.pcap [--sm0 0x1000] [--sm1 0x1080]
"""
import struct
import sys

sys.dont_write_bytecode = True   # no __pycache__ next to pcap2struct.py
sys.path.insert(0, __import__("os").path.join(__import__("os").path.dirname(__file__), "..", "golden"))
from pcap2struct import read_pcap  # noqa: E402


def dgrams(frame):
    if len(frame) < 16 or frame[12:14] != b"\x88\xa4":
        return []
    out, off = [], 16
    while off + 10 <= len(frame):
        cmd, _idx, adp, ado, ln, _irq = struct.unpack("<BBHHHH", frame[off:off + 10])
        dlen = ln & 0x07FF
        data = frame[off + 10:off + 10 + dlen]
        wkc = struct.unpack("<H", frame[off + 10 + dlen:off + 12 + dlen])[0]
        out.append((cmd, adp, ado, data, wkc))
        off += 12 + dlen
        if not ln & 0x8000:
            break
    return out


def req_name(c):
    if c == 0x40: return "UP"
    if c == 0x50: return "CA-UP"
    if (c & 0xEF) == 0x60: return "UP-SEG"
    if c == 0x80: return "ABORT"
    if (c & 0xE0) == 0x20:
        ca = "CA-" if c & 0x10 else ""
        return ca + ("DOWN-EXP" if c & 0x02 else "DOWN")
    if (c & 0xE0) == 0x00: return "DOWN-SEG"
    return "?%02X" % c


def resp_name(service, c):
    if service == 2 and c == 0x80: return "ABORT"
    ca = "CA-" if c & 0x10 else ""
    if (c & 0xE0) == 0x40: return ca + ("UP-EXP" if c & 0x02 else "UP")
    if (c & 0xE0) == 0x60: return ca + "DOWN"
    if (c & 0xE0) == 0x20: return "DOWN-SEG"
    if (c & 0xE0) == 0x00: return "UP-SEG"
    return "?%02X" % c


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    sm0, sm1 = 0x1000, 0x1080
    if "--sm0" in args: sm0 = int(args[args.index("--sm0") + 1], 0)
    if "--sm1" in args: sm1 = int(args[args.index("--sm1") + 1], 0)
    for frame in read_pcap(args[0]):
        reply = len(frame) > 6 and (frame[6] & 0x02)
        for cmd, adp, ado, d, wkc in dgrams(frame):
            if len(d) < 12 or (d[5] & 0x0F) != 0x03:
                continue
            service = struct.unpack("<H", d[6:8])[0] >> 12
            c, index, sub = d[8], struct.unpack("<H", d[9:11])[0], d[11]
            if cmd == 5 and ado == sm0 and not reply:          # FPWR mailbox out
                seg = (c & 0xE0) == 0x00 or (c & 0xEF) == 0x60
                where = "" if seg else " %04X:%02X" % (index, sub)
                print("req  %04X %-9s%s" % (adp, req_name(c), where))
            elif cmd == 4 and ado == sm1 and reply and wkc == 1:  # FPRD mailbox in
                name = resp_name(service, c)
                if name == "ABORT":
                    print("resp %04X ABORT     %04X:%02X %08X" % (adp, index, sub,
                                                                struct.unpack("<I", d[12:16])[0]))
                elif name.endswith("UP") and not name.endswith("SEG"):
                    print("resp %04X %-9s %04X:%02X %d byte" % (adp, name, index, sub,
                                                               struct.unpack("<I", d[12:16])[0]))
                elif name.endswith("UP-EXP"):
                    print("resp %04X %-9s %04X:%02X %d byte" % (adp, name, index, sub,
                                                               4 - ((c >> 2) & 3)))
                elif name in ("UP-SEG", "DOWN-SEG"):
                    print("resp %04X %s" % (adp, name))
                else:
                    print("resp %04X %-9s %04X:%02X" % (adp, name, index, sub))


if __name__ == "__main__":
    main()
