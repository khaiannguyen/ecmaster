#!/usr/bin/env python3
"""pcap2struct.py -- Giai doan 7.5: structure of an EtherCAT master session.

Reads a classic pcap captured on the master's interface (tshark -F pcap)
and prints a normalised, timing-independent description of what the master
sent and what came back, for comparison against a golden file.

Kept on purpose:   datagram commands, their order inside each frame, ADP/ADO
                   (or the logical address), data length, the WKC of the reply
Thrown away:       frame index, data bytes, timestamps, how many times a
                   polling loop ran

Sections:
  CONFIG   everything before the first frame carrying process data
           (LRD/LWR/LRW), in order; runs of an identical block of 1..8
           frames are collapsed to "x+" (EEPROM busy polls, statechecks,
           the keepalive loop: their count depends on timing)
  CYCLIC   from the first to the last process data frame: the SET of frame
           shapes seen (sorted), no counts -- how often each one appears
           depends on the run length and on the phase of the IO/diag ticks
  SHUTDOWN after the last process data frame, like CONFIG

Why not `tshark -T fields`: Wireshark spreads the datagrams of one frame
over repeated fields (ecat.sub1.cmd, ecat.sub2.cmd ...) whose names and
layout depend on the Wireshark version; the Jetson and a CI runner need not
have the same one. The frame format itself does not change.

Direction: a reply has bit 1 of the first source MAC byte set (the first
ESC marks returning frames; soft_bus does the same since 7.5). A request
without a reply (lost frame) is shown with "noreply".

Usage: pcap2struct.py capture.pcap > structure.txt
"""
import struct
import sys

CMD = {0: "NOP", 1: "APRD", 2: "APWR", 3: "APRW", 4: "FPRD", 5: "FPWR", 6: "FPRW",
       7: "BRD", 8: "BWR", 9: "BRW", 10: "LRD", 11: "LWR", 12: "LRW", 13: "ARMW", 14: "FRMW"}
LOGICAL = {10, 11, 12}
AUTOINC = {1, 2, 3, 13}          # ADP is a position, changed by every ESC on the way
BROADCAST = {7, 8, 9}            # ADP is incremented by every ESC on the way


def read_pcap(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        raise SystemExit("not a pcap file")
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        end = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        end = ">"
    else:
        raise SystemExit("not a classic pcap file (capture with: tshark -F pcap)")
    linktype = struct.unpack(end + "I", data[20:24])[0]
    if linktype != 1:
        raise SystemExit("link type %d, expected Ethernet (1)" % linktype)
    off = 24
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack(end + "IIII", data[off:off + 16])
        off += 16
        yield data[off:off + incl]
        off += incl


def datagrams(frame):
    """[(cmd, idx, adp, ado, length, wkc)] of one EtherCAT frame, or None."""
    if len(frame) < 16 or frame[12:14] != b"\x88\xa4":
        return None
    hdr = struct.unpack("<H", frame[14:16])[0]
    if (hdr >> 12) != 1:                       # EtherCAT type 1 = datagrams
        return None
    out, off = [], 16
    while off + 10 <= len(frame):
        cmd, idx, adp, ado, ln, _irq = struct.unpack("<BBHHHH", frame[off:off + 10])
        dlen = ln & 0x07FF
        end = off + 10 + dlen
        if end + 2 > len(frame):
            return None
        wkc = struct.unpack("<H", frame[end:end + 2])[0]
        out.append((cmd, idx, adp, ado, dlen, wkc))
        off = end + 2
        if not (ln & 0x8000):
            break
    return out


def dg_text(d, wkc):
    cmd, _idx, adp, ado, dlen, _ = d
    name = CMD.get(cmd, "CMD%d" % cmd)
    if cmd in LOGICAL:
        addr = "%08x" % ((ado << 16) | adp)
    elif cmd in AUTOINC:
        addr = "pos%d:%04x" % ((-adp) & 0xFFFF, ado)   # request ADP = -position
    elif cmd in BROADCAST:
        addr = "*:%04x" % ado
    else:
        addr = "%04x:%04x" % (adp, ado)
    return "%s %s/%d%s" % (name, addr, dlen, "" if wkc is None else " w%d" % wkc)


def frames_with_replies(path):
    """Requests in send order, each with the WKCs of its reply (or None)."""
    reqs, pending = [], {}
    for fr in read_pcap(path):
        dgs = datagrams(fr)
        if not dgs:
            continue
        is_reply = len(fr) >= 7 and (fr[6] & 0x02)
        idx = dgs[0][1]
        if not is_reply:
            entry = {"dgs": dgs, "wkc": None}
            reqs.append(entry)
            pending[idx] = entry                 # a newer request with this index wins
        else:
            entry = pending.pop(idx, None)
            if entry is not None and entry["wkc"] is None and len(dgs) == len(entry["dgs"]):
                entry["wkc"] = [d[5] for d in dgs]
    return reqs


def shape(entry):
    w = entry["wkc"]
    parts = [dg_text(d, None if w is None else w[i]) for i, d in enumerate(entry["dgs"])]
    return " | ".join(parts) + ("" if w is not None else "  noreply")


def collapse(lines, maxblock=8):
    """1) identical consecutive lines -> one line, no marker: a busy poll
          that ran once or twice is the same behaviour;
       2) consecutive repeats of a block of 2..maxblock lines -> one copy
          followed by 'x+' (loops over slaves / SII words / keepalive).
       Repeated until nothing changes."""
    dd = []
    for ln in lines:
        if not dd or dd[-1] != ln:
            dd.append(ln)
    lines = dd
    changed = True
    while changed:
        changed = False
        out, i = [], 0
        while i < len(lines):
            done = False
            for b in range(2, maxblock + 1):
                blk = lines[i:i + b]
                if len(blk) < b:
                    break
                j = i + b
                reps = 1
                while lines[j:j + b] == blk:
                    reps += 1
                    j += b
                if reps > 1:
                    out.extend(blk if b == 1 or blk[-1] == "  x+" else blk)
                    out.append("  x+")
                    i = j
                    changed = True
                    done = True
                    break
            if not done:
                out.append(lines[i])
                i += 1
        # "  x+" followed by another "  x+" is the same thing
        dedup = []
        for ln in out:
            if ln == "  x+" and dedup and dedup[-1] == "  x+":
                continue
            dedup.append(ln)
        lines = dedup
    return lines


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    reqs = frames_with_replies(sys.argv[1])
    shapes = [shape(e) for e in reqs]
    pd = [i for i, e in enumerate(reqs) if any(d[0] in LOGICAL for d in e["dgs"])]
    if not pd:
        print("# no process data frame in the capture")
        first = last = len(reqs)
    else:
        first, last = pd[0], pd[-1]
    print("# ecmaster golden structure v1 (tools/golden/pcap2struct.py)")
    print("# requests=%d with_reply=%d process_data_frames=%d" %
          (len(reqs), sum(1 for e in reqs if e["wkc"] is not None), len(pd)))
    print("[CONFIG]")
    for ln in collapse(shapes[:first]):
        print(ln)
    # Lost replies are timing (a non-RT host, a capture drop), not
    # behaviour: in the cyclic phase only answered frames count.
    cyclic = set(sh for sh, e in zip(shapes[first:last + 1], reqs[first:last + 1]) if e["wkc"] is not None)
    # The 1 s diagnostic frame may or may not land on the very last tick,
    # i.e. after the last process data frame, depending on whether the run
    # ended on tick 2000 or 2001. A MULTI-datagram frame of a kind already
    # seen in the cyclic phase belongs to it. Single-datagram frames are not
    # moved: the shutdown sequence (AL control INIT, statecheck) is made of
    # those, and it must stay visible even though the same kinds were used
    # to go to OP.
    tail = last + 1
    while tail < len(shapes) and len(reqs[tail]["dgs"]) > 1 and \
            (shapes[tail] in cyclic or reqs[tail]["wkc"] is None):
        tail += 1
    print("[CYCLIC]")
    for ln in sorted(cyclic):
        print(ln)
    print("[SHUTDOWN]")
    for ln in collapse(shapes[tail:]):
        print(ln)


if __name__ == "__main__":
    main()
