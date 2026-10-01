#!/usr/bin/env python3
"""eni2cfg.py -- GD8 8.4: convert an ENI (EtherCATConfig XML, e.g. exported by
TwinCAT) into the line-based .enicfg that libecmaster loads at runtime.

Why not SOEM's eniconv.py / add_eni(): see docs/eni.md. In short, SOEM's
path is build-time (one binary per ENI), keeps only slaves that have CoE
InitCmds, silently skips a slave whose identity does not match, and swallows
SDO failures. ecm_run needs the FULL slave list (for the identity/topology
check, E-03), the expected I/O sizes (layout check), DC settings, and every
CoE InitCmd with its transitions -- and it runs them itself.

What is read, per Config/Slave:
  Info/{VendorId, ProductCode, RevisionNo, PhysAddr, AutoIncAddr, Name}
  InitCmds: presence of "check revision number" (-> check_rev 1);
            the PS write of 0x0980 (-> DC AssignActivate)
  ProcessData/{Send,Recv}/BitLength (-> osize/isize), cross-checked against
            the sum of RxPdo/TxPdo entry BitLen assigned to an SM
  DC/{ReferenceClock, CycleTime0, CycleTime1, ShiftTime}
  Mailbox/CoE/InitCmds/InitCmd (Disabled=1 skipped)
And Config/Cyclic/CycleTime.

GD9.6 (enicfg 2) also records, for the loader to CHECK (not to execute):
  every register InitCmd of every slave and of the master (reg records):
            the loader holds the table of the ones SOEM + libecmaster do
            themselves and refuses an ENI with any other (E-08)
  the AL Control (0x0120) write Timeout of the IP, PS and SO transitions
            (-> preop_ms / safeop_ms / op_ms of the slave record): the
            ESI's state machine timeouts, which TwinCAT copies there
  the datagram command that carries the process data in Config/Cyclic
            (pd_cmd lrw | lrd_lwr | none): mailbox-state polling (an LRD
            inside Master/MailboxStates) is not process data (E-06)

Deliberately NOT executed: register InitCmds (SOEM + libecmaster configure SM,
FMMU, AL state and DC themselves), logical addresses / process image offsets
(SOEM builds its own IOmap), cyclic frame layout. docs/eni.md explains why.

Output format (one record per line, '#' comments, fields space separated,
numbers in hex with 0x prefix except counts/times in decimal):

  enicfg 2
  source <file name> sha256 <hex>
  cycle_us <n>
  pd_cmd lrw|lrd_lwr|none
  slaves <n>
  slave <pos> name <quoted> vendor 0x.. product 0x.. rev 0x.. check_rev 0|1
        addr 0x.. osize_bits <n> isize_bits <n>
        dc 0|1 refclock 0|1 sync0_ns <n> sync1_ns <n> shift_ns <n> assign 0x....
        preop_ms <n> safeop_ms <n> op_ms <n>          (0 = not in the ENI)
  reg <pos> trans <IP,..> cmd <n> ado 0x.... len <n> data <hex|->
  pdo <pos> dir out|in pdo 0x.... index 0x.... sub 0x.. bits <n>
        (GD9.10: every mapped PDO entry in process image order; index 0 =
        padding; the order of PDOs follows the 0x1C12/0x1C13 InitCmds)
        (pos 0 = master InitCmd; data truncated to 32 byte, len is the real one)
  coe <pos> trans <IP,PS,..> ccs <1|2> index 0x.... sub 0x.. ca 0|1 timeout_ms <n> data <hex|->

(the slave record is one physical line; wrapped here for reading)

Usage:
  eni2cfg.py config/eni/eni_8node_dc_sdo.xml [-o out.enicfg]
Exit code 0 on success, 1 on any inconsistency found in the ENI.
"""
import hashlib
import os
import sys
import xml.etree.ElementTree as ET

REG_DC_ACTIVATION = 0x0980
REG_AL_CONTROL = 0x0120
REG_DATA_MAX = 32
# AL Control state -> transition whose Timeout is that state's timeout
AL_STATE_TIMEOUT = {"IP": ("preop", 0x02), "PS": ("safeop", 0x04), "SO": ("op", 0x08)}
TRANSITIONS = {"IP", "PI", "PS", "SP", "SO", "OS", "OP", "SI", "OI", "IB", "BI", "II", "PP", "SS", "OO"}


def num(text, default=None):
    """ENI numbers: decimal, '#x..' or '0x..'."""
    if text is None:
        return default
    t = text.strip()
    if t.startswith(("#x", "#X")):
        return int(t[2:], 16)
    return int(t, 0)


def fail(msg):
    raise ValueError(msg)


def slave_position(info, index_in_file):
    """SOEM position (1-based) from AutoIncAddr, same rule as eniconv.py:
    position = 1 - AutoIncAddr (16-bit). Falls back to file order."""
    aia = info.findtext("AutoIncAddr")
    if aia is None:
        return index_in_file + 1
    return (1 - num(aia)) & 0xFFFF


def pdo_bits(pd, tag):
    """Sum of entry BitLen over PDOs of kind tag that are assigned to an SM."""
    total = 0
    for pdo in pd.findall(tag):
        if pdo.get("Sm") is None:
            continue  # not assigned -> not in the process image
        for e in pdo.findall("Entry"):
            total += num(e.findtext("BitLen"), 0)
    return total


def pdo_assignment(coe, sm_index):
    """GD9.10: the PDO assignment the ENI's CoE InitCmds leave in 0x1C12
    (outputs) / 0x1C13 (inputs), in order; None if the ENI does not write
    it. Complete Access: data = SI0 (16 bit, padded) + 16-bit PDO numbers.
    Without CA: SI0 = n and SIk = PDO, as separate downloads (last wins)."""
    vals, count, seen = {}, None, False
    for c in coe:
        if c["index"] != sm_index or c["ccs"] != 1 or c["data"] == "-":
            continue
        seen = True
        d = bytes.fromhex(c["data"])
        if c["ca"]:
            n = d[0]
            vals = {k + 1: d[2 + 2 * k] | d[3 + 2 * k] << 8 for k in range(n) if 3 + 2 * k < len(d)}
            count = n
        elif c["sub"] == 0:
            count = d[0]
        elif len(d) >= 2:
            vals[c["sub"]] = d[0] | d[1] << 8
    if not seen:
        return None
    return [vals[k] for k in range(1, (count or 0) + 1) if k in vals]


def pdo_entries(pd, tag, assign, who):
    """GD9.10: (pdo, index, sub, bits) of every entry the slave maps, in
    process image order: PDOs in the order of the assignment the ENI writes
    (or file order when it writes none), entries in PDO order. Index 0 =
    padding gap, kept: it shifts every entry after it."""
    pdos = [p for p in pd.findall(tag) if p.get("Sm") is not None]
    by_index = {num(p.findtext("Index")): p for p in pdos}
    if assign is not None:
        if set(assign) != set(by_index):
            fail(f"{who}: {tag} assigned in the ENI's SM ({sorted(hex(i) for i in by_index)}) "
                 f"differs from its 0x1C1x InitCmds ({[hex(i) for i in assign]})")
        pdos = [by_index[i] for i in assign]
    out = []
    for p in pdos:
        pi = num(p.findtext("Index"))
        for e in p.findall("Entry"):
            out.append((pi, num(e.findtext("Index"), 0), num(e.findtext("SubIndex"), 0),
                        num(e.findtext("BitLen"), 0)))
    return out


def reg_initcmds(inits, who):
    """Every enabled register InitCmd under an InitCmds element."""
    out = []
    if inits is None:
        return out
    for ic in inits.findall("InitCmd"):
        if (ic.findtext("Disabled") or "").strip() in ("1", "true"):
            continue
        trans = [t.text.strip() for t in ic.findall("Transition")]
        bad = [t for t in trans if t not in TRANSITIONS]
        if not trans or bad:
            fail(f"{who}: register InitCmd with transitions {trans}")
        data = (ic.findtext("Data") or "").strip()
        raw = bytes.fromhex(data) if data else b""
        length = num(ic.findtext("Len"), None)
        if length is None:
            length = len(raw)
        out.append({
            "trans": trans,
            "cmd": num(ic.findtext("Cmd"), 0),
            "ado": num(ic.findtext("Ado"), 0),
            "len": length,
            "raw": raw,
            "timeout": num(ic.findtext("Timeout"), 0),
        })
    return out


def pd_command(cfg):
    """Datagram command of the cyclic process data: 'lrw', 'lrd_lwr' or 'none'.
    An LRD inside Master/MailboxStates polls the mailbox state, it is not
    process data (every TwinCAT ENI has one, GD9.4)."""
    start = num(cfg.findtext("Master/MailboxStates/StartAddr"), None)
    count = num(cfg.findtext("Master/MailboxStates/Count"), 0)
    mbx = range(start, start + (count + 7) // 8) if start is not None else range(0)
    kinds = set()
    for c in cfg.findall("Cyclic/Frame/Cmd"):
        cmd = num(c.findtext("Cmd"), -1)
        if cmd not in (10, 11, 12):
            continue
        addr = num(c.findtext("Addr"), None)
        if cmd == 10 and addr is not None and addr in mbx:
            continue
        kinds.add(cmd)
    if not kinds:
        return "none"
    if kinds == {12}:
        return "lrw"
    return "lrd_lwr"


def reg_lines(pos, regs):
    out = []
    for r in regs:
        d = r["raw"][:REG_DATA_MAX].hex() or "-"
        out.append(f'reg {pos} trans {",".join(r["trans"])} cmd {r["cmd"]} ado 0x{r["ado"]:04X} '
                   f'len {r["len"]} data {d}')
    return out


def parse_slave(s, idx):
    info = s.find("Info")
    if info is None:
        fail(f"slave #{idx}: no Info")
    rec = {
        "pos": slave_position(info, idx),
        "name": (info.findtext("Name") or "").strip(),
        "vendor": num(info.findtext("VendorId"), 0),
        "product": num(info.findtext("ProductCode"), 0),
        "rev": num(info.findtext("RevisionNo"), 0),
        "addr": num(info.findtext("PhysAddr"), 0),
    }

    inits = s.find("InitCmds")
    rec["check_rev"] = 0
    rec["assign"] = 0
    rec["preop"] = rec["safeop"] = rec["op"] = 0
    rec["reg"] = reg_initcmds(inits, f"slave {rec['pos']}")
    for r in rec["reg"]:
        if r["ado"] != REG_AL_CONTROL or r["cmd"] not in (2, 5, 8) or len(r["raw"]) < 1:
            continue
        for t in r["trans"]:
            if t in AL_STATE_TIMEOUT:
                key, state = AL_STATE_TIMEOUT[t]
                if (r["raw"][0] & 0x0F) == state:
                    rec[key] = max(rec[key], r["timeout"])
    if inits is not None:
        for ic in inits.findall("InitCmd"):
            comment = (ic.findtext("Comment") or "").lower()
            if "check revision" in comment:
                rec["check_rev"] = 1
            trans = {t.text for t in ic.findall("Transition")}
            if num(ic.findtext("Ado"), -1) == REG_DC_ACTIVATION and "PS" in trans:
                data = bytes.fromhex(ic.findtext("Data") or "")
                if len(data) >= 2:
                    rec["assign"] = data[0] | data[1] << 8

    pd = s.find("ProcessData")
    osize = isize = 0
    if pd is not None:
        send, recv = pd.find("Send"), pd.find("Recv")
        osize = num(send.findtext("BitLength"), 0) if send is not None else 0
        isize = num(recv.findtext("BitLength"), 0) if recv is not None else 0
        rx, tx = pdo_bits(pd, "RxPdo"), pdo_bits(pd, "TxPdo")
        if rx != osize:
            fail(f"slave {rec['pos']}: Send BitLength {osize} != RxPdo entries {rx}")
        if tx != isize:
            fail(f"slave {rec['pos']}: Recv BitLength {isize} != TxPdo entries {tx}")
    rec["osize"], rec["isize"] = osize, isize

    dc = s.find("DC")
    if dc is not None:
        rec["dc"] = 1
        rec["refclock"] = 1 if (dc.findtext("ReferenceClock") or "").strip().lower() == "true" else 0
        rec["sync0"] = num(dc.findtext("CycleTime0"), 0)
        rec["sync1"] = num(dc.findtext("CycleTime1"), 0)
        rec["shift"] = num(dc.findtext("ShiftTime"), 0)
        if rec["assign"] == 0:
            fail(f"slave {rec['pos']}: DC block but no PS write of 0x0980 (AssignActivate)")
    else:
        rec.update(dc=0, refclock=0, sync0=0, sync1=0, shift=0)
        if rec["assign"] != 0:
            fail(f"slave {rec['pos']}: 0x0980 written but no DC block")

    coe = []
    mbx_coe = s.find("Mailbox/CoE/InitCmds")
    if mbx_coe is not None:
        for ic in mbx_coe.findall("InitCmd"):
            if (ic.findtext("Disabled") or "").strip() in ("1", "true"):
                continue
            trans = [t.text.strip() for t in ic.findall("Transition")]
            bad = [t for t in trans if t not in TRANSITIONS]
            if not trans or bad:
                fail(f"slave {rec['pos']}: CoE InitCmd with transitions {trans}")
            ccs = num(ic.findtext("Ccs"), 0)
            if ccs not in (1, 2):
                fail(f"slave {rec['pos']}: CoE InitCmd ccs {ccs} (only 1=download, 2=upload)")
            data = (ic.findtext("Data") or "").strip()
            if data:
                bytes.fromhex(data)  # validates
            ca = (ic.get("CompleteAccess") or ic.findtext("CompleteAccess") or "0").strip()
            coe.append({
                "trans": ",".join(trans),
                "ccs": ccs,
                "index": num(ic.findtext("Index")),
                "sub": num(ic.findtext("SubIndex"), 0),
                "ca": 1 if ca in ("1", "true") else 0,
                "timeout": num(ic.findtext("Timeout"), 0),
                "data": data.lower() or "-",
            })
    rec["coe"] = coe

    rec["pdo"] = []
    if pd is not None:
        who = f"slave {rec['pos']}"
        for tag, sm_obj, d in (("RxPdo", 0x1C12, "out"), ("TxPdo", 0x1C13, "in")):
            for (pi, ix, sb, bl) in pdo_entries(pd, tag, pdo_assignment(coe, sm_obj), who):
                rec["pdo"].append((d, pi, ix, sb, bl))
    return rec


def convert(path):
    raw = open(path, "rb").read()
    root = ET.fromstring(raw)
    cfg = root.find("Config")
    if cfg is None:
        fail("no Config element (not an EtherCATConfig file?)")
    slaves = [parse_slave(s, i) for i, s in enumerate(cfg.findall("Slave"))]
    if not slaves:
        fail("no slaves")

    positions = [r["pos"] for r in slaves]
    if sorted(positions) != list(range(1, len(slaves) + 1)):
        fail(f"slave positions not 1..{len(slaves)}: {positions}")
    slaves.sort(key=lambda r: r["pos"])

    addrs = [r["addr"] for r in slaves]
    if len(set(addrs)) != len(addrs):
        fail(f"duplicate station addresses {addrs}")
    refs = [r["pos"] for r in slaves if r["refclock"]]
    if len(refs) > 1:
        fail(f"more than one reference clock: {refs}")
    if any(r["dc"] for r in slaves) and not refs:
        fail("DC slaves but no reference clock")

    cycle_us = num(cfg.findtext("Cyclic/CycleTime"), 0)
    master_regs = reg_initcmds(cfg.find("Master/InitCmds"), "master")

    out = [
        "# generated by tools/eni/eni2cfg.py -- do not edit, regenerate from the ENI",
        "enicfg 2",
        f"source {os.path.basename(path)} sha256 {hashlib.sha256(raw).hexdigest()}",
        f"cycle_us {cycle_us}",
        f"pd_cmd {pd_command(cfg)}",
        f"slaves {len(slaves)}",
    ]
    for r in slaves:
        name = r["name"].replace('"', "'")
        out.append(
            f'slave {r["pos"]} name "{name}" vendor 0x{r["vendor"]:X} product 0x{r["product"]:X} '
            f'rev 0x{r["rev"]:X} check_rev {r["check_rev"]} addr 0x{r["addr"]:04X} '
            f'osize_bits {r["osize"]} isize_bits {r["isize"]} '
            f'dc {r["dc"]} refclock {r["refclock"]} sync0_ns {r["sync0"]} sync1_ns {r["sync1"]} '
            f'shift_ns {r["shift"]} assign 0x{r["assign"]:04X} '
            f'preop_ms {r["preop"]} safeop_ms {r["safeop"]} op_ms {r["op"]}'
        )
    out += reg_lines(0, master_regs)
    for r in slaves:
        out += reg_lines(r["pos"], r["reg"])
    for r in slaves:
        for (d, pi, ix, sb, bl) in r["pdo"]:
            out.append(f'pdo {r["pos"]} dir {d} pdo 0x{pi:04X} index 0x{ix:04X} sub 0x{sb:02X} bits {bl}')
    for r in slaves:
        for c in r["coe"]:
            out.append(
                f'coe {r["pos"]} trans {c["trans"]} ccs {c["ccs"]} index 0x{c["index"]:04X} '
                f'sub 0x{c["sub"]:02X} ca {c["ca"]} timeout_ms {c["timeout"]} data {c["data"]}'
            )
    return "\n".join(out) + "\n"


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    src = args[0]
    dst = None
    if "-o" in args:
        dst = args[args.index("-o") + 1]
    try:
        text = convert(src)
    except (ValueError, ET.ParseError) as e:
        print(f"eni2cfg: {src}: {e}", file=sys.stderr)
        return 1
    if dst:
        with open(dst, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
