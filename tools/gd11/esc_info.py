#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
"""esc_info.py -- Phase 11.6: read the ESC information registers 0x0000-0x0009
of every slave with APRD (no SOEM, no configuration, nothing written).

  sudo python3 tools/gd11/esc_info.py IFACE [N]       N slaves (default 2)

Prints per slave: ESC type/revision/build, number of FMMUs (0x0004), of
SyncManagers (0x0005), RAM size (0x0006), port descriptor (0x0007), features
(0x0008). Why (D-14, 14/10): SOEM's cyclic mailbox maps SM1 status through
one more FMMU; a slave with only as many FMMUs as process data needs never
shows "mailbox in full" and every SDO in OP times out.

Do not run it while ecm_run owns the interface.
"""
import socket
import struct
import sys
import time


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    iface = argv[0]
    n = int(argv[1]) if len(argv) > 1 else 2
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x88A4))
    s.bind((iface, 0))
    s.settimeout(0.2)
    src = s.getsockname()[4]
    rlen = 10
    dgs = b""
    for k in range(n):
        more = 0x8000 if k < n - 1 else 0
        # APRD (cmd 1), idx k, ADP = -k (auto increment), ADO 0x0000
        dgs += struct.pack("<BBhHHH", 1, 0x40 + k, -k, 0x0000, rlen | more, 0) + bytes(rlen) + b"\0\0"
    ecat = struct.pack("<H", len(dgs) | 0x1000) + dgs
    frame = b"\xff" * 6 + src + b"\x88\xa4" + ecat
    frame += bytes(max(0, 60 - len(frame)))
    s.send(frame)
    t_end = time.time() + 1.0
    while time.time() < t_end:
        try:
            r, addr = s.recvfrom(1514)
        except socket.timeout:
            continue
        if addr[2] == socket.PACKET_OUTGOING or len(r) < 16:   # our own frame on the way out
            continue
        p = 16
        for k in range(n):
            cmd, idx, adp, ado, ln, irq = struct.unpack_from("<BBhHHH", r, p)
            data = r[p + 10:p + 10 + rlen]
            wkc = struct.unpack_from("<H", r, p + 10 + rlen)[0]
            p += 10 + rlen + 2
            if wkc == 0:
                print("slave %d: no answer (wkc 0)" % (k + 1))
                continue
            typ, rev, build, nfmmu, nsm, ram, ports, feat = struct.unpack("<BBHBBBBH", data)
            print("slave %d: ESC type 0x%02X rev %d build %d  FMMUs %d  SyncManagers %d  RAM %d KiB  ports 0x%02X  features 0x%04X"
                  % (k + 1, typ, rev, build, nfmmu, nsm, ram, ports, feat))
        return 0
    print("no reply on %s (link? ecm_run running?)" % iface)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
