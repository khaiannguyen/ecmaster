#!/usr/bin/env python3
"""cw_trace.py -- Phase 10.8 (X-05): the CiA402 controlword / statusword
sequence of one axis, read from a capture of the EtherCAT wire, and the
comparison of two such sequences (our master vs TwinCAT NC on the same
IS620N).

    cw_trace.py CAPTURE [--cw LADDR] [--sw LADDR]          print the trace
    cw_trace.py MASTER --ref TWINCAT [--cw ..] [--ref-cw ..]  X-05 check

CAPTURE: classic pcap or pcapng (Wireshark's default), Ethernet, EtherCAT
frames (0x88A4, optionally VLAN tagged). Process data datagrams: LRW, LWR,
LRD. The controlword is taken from the frames the master SENT (outputs),
the statusword from the REPLIES (inputs): a reply has bit 1 of the first
source MAC byte set (the first ESC sets it), or, failing that, WKC > 0.

Where the words are: --cw / --sw give the logical address (0x10000 + byte
offset, as TwinCAT's "process image" / our --pdo-dump shows it). Without
them, the 16-bit word positions are searched: the controlword is the
output word whose values (bits 0-3 and 7) contain 0x06, 0x07, 0x0F in that
order; the statusword is the input word whose decoded drive states contain
Switch on disabled -> Ready -> Switched on -> Operation enabled. The first
match wins; ambiguity is reported. With several axes give the addresses.

Trace: every change of (drive state, controlword & 0x008F) is a step,
printed with Wireshark's frame number (every packet counts, 1-based) and
the raw statusword; --ec LADDR adds the 0x603F error code of the step;
the path is "SOD -06-> RTSO -07-> SO -0F-> OE -07-> SO ...".

X-05 (plan GD10 §10; criteria of 9/10, see x05()): our master's ENABLE
transitions must appear in TwinCAT's trace and its states up to OE in the
same order; no fault and no fault reset on our side. The disable paths,
TwinCAT's reset pulses and TwinCAT's faults (with 0x603F, --ref-ec) are
reported, not graded. Exit 0 when it holds, 1 when not, 2 on errors.
"""
import argparse
import struct
import sys

LRD, LWR, LRW = 10, 11, 12
CW_MASK = 0x008F

STATES = ["?", "NRTSO", "SOD", "RTSO", "SO", "OE", "QSA", "FRA", "FAULT"]


def decode(sw):
    m = sw & 0x4F
    if m == 0x00:
        return "NRTSO"
    if m == 0x40:
        return "SOD"
    if m == 0x0F:
        return "FRA"
    if m == 0x08:
        return "FAULT"
    m = sw & 0x6F
    return {0x21: "RTSO", 0x23: "SO", 0x27: "OE", 0x07: "QSA"}.get(m, "?")


# ---------------------------------------------------------------- capture --
def frames(path):
    """(number, Ethernet frame); number = Wireshark's frame number (1-based, every packet)."""
    data = open(path, "rb").read()
    if len(data) < 24:
        raise ValueError("%s: too short for a capture" % path)
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4", b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
        end = "<" if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
        if struct.unpack(end + "I", data[20:24])[0] != 1:
            raise ValueError("%s: not an Ethernet capture" % path)
        off, num = 24, 0
        while off + 16 <= len(data):
            _, _, incl, _ = struct.unpack(end + "IIII", data[off:off + 16])
            num += 1
            yield num, data[off + 16:off + 16 + incl]
            off += 16 + incl
        return
    if magic == b"\x0a\x0d\x0d\x0a":                     # pcapng
        off, end, linktypes, num = 0, "<", [], 0
        while off + 12 <= len(data):
            btype = struct.unpack(end + "I", data[off:off + 4])[0]
            if btype == 0x0A0D0D0A:
                bom = data[off + 8:off + 12]
                end = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
                linktypes = []
            blen = struct.unpack(end + "I", data[off + 4:off + 8])[0]
            if blen < 12:
                raise ValueError("%s: bad pcapng block" % path)
            body = data[off + 8:off + blen - 4]
            if btype == 1:                               # interface description
                linktypes.append(struct.unpack(end + "H", body[0:2])[0])
            elif btype == 6:                             # enhanced packet
                num += 1
                iface, _, _, cap, _ = struct.unpack(end + "IIIII", body[:20])
                if iface < len(linktypes) and linktypes[iface] == 1:
                    yield num, body[20:20 + cap]
            elif btype == 3:                             # simple packet
                num += 1
                if linktypes and linktypes[0] == 1:
                    plen = struct.unpack(end + "I", body[:4])[0]
                    yield num, body[4:4 + plen]
            off += blen
        return
    raise ValueError("%s: neither pcap nor pcapng" % path)


def datagrams(frame):
    """(is_reply, [(cmd, laddr, data, wkc)])"""
    if len(frame) < 16:
        return None
    et = struct.unpack(">H", frame[12:14])[0]
    p = 14
    if et == 0x8100:
        et = struct.unpack(">H", frame[16:18])[0]
        p = 18
    if et != 0x88A4:
        return None
    reply_mac = bool(frame[6] & 0x02)
    hdr = struct.unpack("<H", frame[p:p + 2])[0]
    if (hdr >> 12) != 1:                                  # EtherCAT commands
        return None
    off, out = p + 2, []
    while off + 10 <= len(frame):
        cmd, _idx, adr, ln, _irq = struct.unpack("<BBIHH", frame[off:off + 10])
        n = ln & 0x07FF
        more = ln & 0x8000
        d = frame[off + 10:off + 10 + n]
        wkc = struct.unpack("<H", frame[off + 10 + n:off + 12 + n])[0] if off + 12 + n <= len(frame) else 0
        out.append((cmd, adr, d, wkc))
        off += 12 + n
        if not more:
            break
    return reply_mac, out


def words(path):
    """Per process-data frame, in order: (is_reply, {laddr: u16}, capture frame number)."""
    seq = []
    for num, fr in frames(path):
        r = datagrams(fr)
        if not r:
            continue
        reply_mac, dgs = r
        pd = [(c, a, d, w) for (c, a, d, w) in dgs if c in (LRD, LWR, LRW)]
        if not pd:
            continue
        is_reply = reply_mac or any(w > 0 for (_, _, _, w) in pd)
        m = {}
        for c, a, d, w in pd:
            if is_reply and c == LWR:
                continue                                 # a LWR reply carries the outputs back
            if not is_reply and c == LRD:
                continue
            for i in range(0, len(d) - 1):
                m[a + i] = d[i] | d[i + 1] << 8
        seq.append((is_reply, m, num))
    return seq


def find_cw(seq):
    cands = []
    addrs = set()
    for rep, m, _ in seq:
        if not rep:
            addrs.update(m.keys())
    for a in sorted(addrs):
        want, k = [0x06, 0x07, 0x0F], 0
        for rep, m, _ in seq:
            if rep or a not in m:
                continue
            if (m[a] & CW_MASK) == want[k]:
                k += 1
                if k == 3:
                    cands.append(a)
                    break
    return cands


def find_sw(seq):
    cands = []
    addrs = set()
    for rep, m, _ in seq:
        if rep:
            addrs.update(m.keys())
    for a in sorted(addrs):
        want, k = ["SOD", "RTSO", "SO", "OE"], 0
        for rep, m, _ in seq:
            if not rep or a not in m:
                continue
            if decode(m[a]) == want[k]:
                k += 1
                if k == 4:
                    cands.append(a)
                    break
    return cands


def trace(path, cw_addr=None, sw_addr=None, quiet=False, ec_addr=None):
    seq = words(path)
    if not seq:
        raise ValueError("%s: no process data frames (LRW/LWR/LRD)" % path)
    if cw_addr is None:
        c = find_cw(seq)
        if not c:
            raise ValueError("%s: no output word walks 0x06 -> 0x07 -> 0x0F; give --cw" % path)
        if len(c) > 1 and not quiet:
            print("  note: controlword candidates %s, taking 0x%X (give --cw)" % (" ".join("0x%X" % x for x in c), c[0]))
        cw_addr = c[0]
    if sw_addr is None:
        c = find_sw(seq)
        if not c:
            raise ValueError("%s: no input word walks SOD -> RTSO -> SO -> OE; give --sw" % path)
        if len(c) > 1 and not quiet:
            print("  note: statusword candidates %s, taking 0x%X (give --sw)" % (" ".join("0x%X" % x for x in c), c[0]))
        sw_addr = c[0]
    cw, st, sw, ec = None, None, None, None
    steps = []            # (capture frame number, state, cw, statusword, error code or None)
    for rep, m, num in seq:
        if not rep and cw_addr in m:
            cw = m[cw_addr] & CW_MASK
        if rep and sw_addr in m:
            sw = m[sw_addr]
            st = decode(sw)
            if ec_addr is not None and ec_addr in m:
                ec = m[ec_addr]
        if cw is None or st is None:
            continue
        if not steps or (steps[-1][1], steps[-1][2]) != (st, cw):
            steps.append((num, st, cw, sw, ec))
    return cw_addr, sw_addr, steps


def transitions(steps):
    """(from state, controlword that was on the wire, to state) per state change."""
    out, prev = [], None
    for _, st, cw, _, _ in steps:
        if prev is not None and st != prev[0]:
            out.append((prev[0], prev[1], st))
        prev = (st, cw)
    return out


def path_str(steps):
    s, last = [], None
    for _, st, cw, _, _ in steps:
        if st != last:
            s.append(st if not s else "-%02X-> %s" % (cw_prev, st))
            last = st
        cw_prev = cw
    return " ".join(s)


def states_order(steps):
    o = []
    for _, st, _, _, _ in steps:
        if not o or o[-1] != st:
            o.append(st)
    return o


def is_subsequence(a, b):
    it = iter(b)
    return all(any(x == y for y in it) for x in a)


def parse_addr(s):
    return None if s is None else int(s, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--cw")
    ap.add_argument("--sw")
    ap.add_argument("--ref", help="TwinCAT capture to compare with (X-05)")
    ap.add_argument("--ref-cw")
    ap.add_argument("--ref-sw")
    ap.add_argument("--ec", help="logical address of 0x603F error code (printed per step)")
    ap.add_argument("--ref-ec")
    a = ap.parse_args()
    try:
        ca, sa, st = trace(a.capture, parse_addr(a.cw), parse_addr(a.sw), ec_addr=parse_addr(a.ec))
        print("%s: controlword at 0x%X, statusword at 0x%X, %d steps" % (a.capture, ca, sa, len(st)))
        print("  path: " + path_str(st))
        if not a.ref:
            for f, s, c, w, e in st:
                print("  frame %7d  %-5s cw 0x%04X sw 0x%04X%s" % (f, s, c, w, "" if e is None else "  0x603F 0x%04X" % e))
            return 0
        rca, rsa, rst = trace(a.ref, parse_addr(a.ref_cw), parse_addr(a.ref_sw), ec_addr=parse_addr(a.ref_ec))
        print("%s: controlword at 0x%X, statusword at 0x%X, %d steps" % (a.ref, rca, rsa, len(rst)))
        print("  path: " + path_str(rst))
    except (ValueError, OSError) as e:
        print("cw_trace: %s" % e, file=sys.stderr)
        return 2
    return x05(st, rst)


ENABLE_TO = ("SO", "OE")          # RTSO -07-> SO, SO -0F-> OE (and SOD/RTSO -> SO shortcuts)
FAULTY = ("FRA", "FAULT")


def x05(st, rst):
    """X-05 (plan GD10 section 10, criteria of 9/10 after the first real capture):
    PASS when
      - every ENABLE transition of our master (into SO or OE) also appears in
        TwinCAT's trace, and our master's states up to its first OE are a
        subsequence of TwinCAT's;
      - our master's trace has no Fault / Fault reaction active and our master
        never sets controlword bit 7 (S7: no automatic fault reset).
    Reported, not graded (both CiA402-legal):
      - the disable path of each (TwinCAT NC: OE -06-> RTSO; ours: OE -07-> SO
        -06-> RTSO -00-> SOD, S6);
      - fault-reset pulses (cw bit 7) TwinCAT sends;
      - faults in TwinCAT's trace, with 0x603F when --ref-ec is given, and
        whether the drive cleared them itself (cw bit 7 never set)."""
    mt, rt = transitions(st), set(transitions(rst))
    en = []
    for t in mt:                    # the master's way up, until its first Operation enabled
        if t[2] in ENABLE_TO and t[0] not in FAULTY:
            en.append(t)
        if t[2] == "OE":
            break
    missing = [t for t in en if t not in rt]
    def upto_oe(o):
        return o[:o.index("OE") + 1] if "OE" in o else o
    mo = upto_oe(states_order(st))
    order_ok = is_subsequence(mo, states_order(rst))
    bad = []
    for t in missing:
        bad.append("enable transition %s -%02X-> %s of the master is not in the reference" % t)
    if not order_ok:
        bad.append("master states up to OE (%s) are not a subsequence of the reference's (%s)"
                   % (" ".join(mo), " ".join(states_order(rst))))
    if "OE" not in states_order(st):
        bad.append("the master never reached Operation enabled")
    if any(s in FAULTY for s in states_order(st)):
        bad.append("the master's trace has a fault")
    if any(c & 0x80 for _, _, c, _, _ in st):
        bad.append("the master sent a fault reset (cw bit 7)")

    # the controlword that caused each change is the one on the wire just before it
    def disable_path2(steps):
        o, oe = [], None
        for i, (_, s_, c, _, _) in enumerate(steps):
            if s_ == "OE":
                oe, o = i, ["OE"]
            elif oe is not None and s_ != steps[i - 1][1]:
                o.append("-%02X-> %s" % (steps[i - 1][2], s_))
        return " ".join(o) if oe is not None else "(no OE)"

    print("  enable  master: %s" % " ".join(mo))
    print("  disable master:    %s" % disable_path2(st))
    print("  disable reference: %s" % disable_path2(rst))
    resets = [f for f, _, c, _, _ in rst if c & 0x80]
    if resets:
        print("  INFO reference sends fault reset (cw bit 7) at frame(s) %s" % " ".join(map(str, resets)))
    for i, (f, s_, c, w, e) in enumerate(rst):
        if s_ in FAULTY and (i == 0 or rst[i - 1][1] not in FAULTY):
            j = i
            while j + 1 < len(rst) and rst[j + 1][1] in FAULTY + ("NRTSO",):
                j += 1
            cleared_by = "a reset (cw bit 7)" if any(rst[k][2] & 0x80 for k in range(i, min(j + 2, len(rst)))) \
                else "the drive itself (no cw bit 7)"
            print("  INFO reference fault at frame %d (sw 0x%04X%s), cleared by %s"
                  % (f, w, "" if e is None else ", 0x603F 0x%04X" % e, cleared_by))
    for b in bad:
        print("  X-05: " + b)
    ok = not bad
    print("X-05 %s: %d master enable transition(s) checked; disable path %s"
          % ("PASS" if ok else "FAIL", len(en),
             "same" if disable_path2(st) == disable_path2(rst) else "DIFF (both CiA402-legal; reported, not graded)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
