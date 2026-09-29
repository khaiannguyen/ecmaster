#!/usr/bin/env python3
"""ecat_dump.py -- print every EtherCAT datagram of a pcapng as one text line.

Usage: python3 ecat_dump.py FILE.pcapng [first_frame] [last_frame] [--all]
  default: skip frames that carry cyclic datagrams (LRD/LRW/ARMW)
Line: frame  M|S  CMD adp:ado/len wWKC data-hex(first 16 bytes)
  M = sent by master (Dell), S = returned by soft_bus (MAC with 0x02 bit set)
"""
import struct
import sys

CMD = {0: 'NOP', 1: 'APRD', 2: 'APWR', 3: 'APRW', 4: 'FPRD', 5: 'FPWR', 6: 'FPRW',
       7: 'BRD', 8: 'BWR', 9: 'BRW', 10: 'LRD', 11: 'LWR', 12: 'LRW', 13: 'ARMW', 14: 'FRMW'}


def packets(path):
    d = open(path, 'rb').read()
    i, n = 0, 0
    if d[:4] in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1'):   # classic libpcap (LE)
        i = 24
        while i + 16 <= len(d):
            cap = struct.unpack_from('<I', d, i + 8)[0]
            n += 1
            yield n, d[i + 16:i + 16 + cap]
            i += 16 + cap
        return
    while i + 12 <= len(d):
        bt, bl = struct.unpack_from('<II', d, i)
        if bl < 12:
            break
        if bt == 6:                                   # Enhanced Packet Block
            cap = struct.unpack_from('<I', d, i + 20)[0]
            n += 1
            yield n, d[i + 28:i + 28 + cap]
        elif bt == 3:                                 # Simple Packet Block
            n += 1
            yield n, d[i + 12:i + bl - 4]
        i += bl


def datagrams(pkt):
    if len(pkt) < 16 or struct.unpack_from('>H', pkt, 12)[0] != 0x88A4:
        return
    o = 16
    while o + 12 <= len(pkt):
        cmd, _idx, adp, ado, ln, _irq = struct.unpack_from('<BBHHHH', pkt, o)
        L = ln & 0x7FF
        if o + 12 + L > len(pkt):
            break
        data = pkt[o + 10:o + 10 + L]
        wkc = struct.unpack_from('<H', pkt, o + 10 + L)[0]
        yield cmd, adp, ado, L, data, wkc
        o += 12 + L
        if not ln >> 15:
            break


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    show_all = '--all' in sys.argv
    path = args[0]
    lo = int(args[1]) if len(args) > 1 else 1
    hi = int(args[2]) if len(args) > 2 else 10**9
    for n, p in packets(path):
        if n < lo or n > hi:
            continue
        ds = list(datagrams(p))
        if not ds:
            continue
        if not show_all and any(c in (10, 12, 13) for c, *_ in ds):
            continue
        who = 'S' if p[6] & 0x02 else 'M'
        print(n, who, ' | '.join(
            f"{CMD.get(c, c)} {a:04x}:{o:04x}/{L} w{w} {d[:16].hex()}"
            for c, a, o, L, d, w in ds))


if __name__ == '__main__':
    main()
