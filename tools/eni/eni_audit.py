#!/usr/bin/env python3
"""eni_audit.py -- GD9.4: describe EVERYTHING a (TwinCAT) ENI asks the master
to do, as Markdown for docs/eni.md, so that the loader decisions of step 9.6
(which register InitCmds are "known", LRD/LWR, CoE transitions, timeouts)
are taken from real files, not from memory.

Per slave: identity, DC (reference clock, CycleTime0/1, ShiftTime), mailbox
protocols, every register InitCmd (transitions, command, ADP/ADO, data,
comment) and every CoE InitCmd (transitions, ccs, index:sub, CompleteAccess,
timeout, data, comment). Master InitCmds. Cyclic frames: which datagram
commands carry process data (LRW 12 / LRD 10 / LWR 11 / others) -- that is
where "UseLrdLwr" shows up.

With --known FILE (one "Ado" or "Ado/Cmd" per line, hex, '#' comments) every
register InitCmd whose ADO is NOT in the list is flagged "UNKNOWN" -- the
seed of the 9.6 table.

Usage:
  eni_audit.py ENI.xml [ENI2.xml ...] [--known known_regs.txt] > report.md
Exit code 0; 2 if a file cannot be parsed.
"""
import sys
import xml.etree.ElementTree as ET

CMD = {0: "NOP", 1: "APRD", 2: "APWR", 3: "APRW", 4: "FPRD", 5: "FPWR", 6: "FPRW",
       7: "BRD", 8: "BWR", 9: "BRW", 10: "LRD", 11: "LWR", 12: "LRW", 13: "ARMW", 14: "FRMW"}

# Register names for the report (ESC datasheet Section II), not a whitelist.
REG = {0x0010: "station address", 0x0012: "station alias", 0x0100: "DL control",
       0x0103: "DL control (loop)", 0x0120: "AL control", 0x0130: "AL status",
       0x0134: "AL status code", 0x0200: "ECAT event mask", 0x0300: "RX error counters",
       0x0400: "WD divider", 0x0410: "WD time PDI", 0x0420: "WD time PD",
       0x0500: "SII access", 0x0502: "SII control/status", 0x0504: "SII address",
       0x0508: "SII data", 0x0600: "FMMU 0", 0x0610: "FMMU 1", 0x0620: "FMMU 2",
       0x0630: "FMMU 3", 0x0800: "SM 0", 0x0808: "SM 1", 0x0810: "SM 2", 0x0818: "SM 3",
       0x0900: "DC receive times", 0x0910: "DC system time", 0x0920: "DC offset",
       0x0928: "DC delay", 0x0930: "DC speed counter start", 0x0934: "DC filter",
       0x0980: "DC activation (AssignActivate)", 0x0981: "DC activation (SYNC)",
       0x0982: "DC pulse length", 0x0990: "DC start time", 0x09A0: "SYNC0 cycle",
       0x09A4: "SYNC1 cycle", 0x09A8: "Latch0/1 control"}


def num(t, default=None):
    if t is None:
        return default
    t = t.strip()
    if t.startswith(("#x", "#X")):
        return int(t[2:], 16)
    try:
        return int(t, 0)
    except ValueError:
        return default


def txt(e, path, default=""):
    v = e.findtext(path)
    return v.strip() if v is not None else default


def trans(ic):
    return ",".join(t.text.strip() for t in ic.findall("Transition") if t.text) or "-"


def short(data, n=24):
    d = (data or "").strip()
    return d if len(d) <= n else d[:n] + f"...({len(d) // 2} B)"


def reg_name(ado):
    """Name of the register block that contains ado (FMMU 16 byte, SM 8 byte,
    everything else the size of its field)."""
    if ado is None:
        return ""
    size = lambda b: 16 if 0x0600 <= b < 0x0700 else 8 if 0x0800 <= b < 0x0880 else 2
    for base in sorted(REG, reverse=True):
        if base <= ado < base + size(base):
            return REG[base] + ("" if base == ado else f" +{ado - base}")
    return ""


def load_known(path):
    known = set()
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if line:
            parts = line.split("/")
            known.add((int(parts[0], 16), int(parts[1]) if len(parts) > 1 else None))
    return known


def is_known(known, ado, cmd):
    return (ado, None) in known or (ado, cmd) in known


def audit(path, known):
    root = ET.parse(path).getroot()
    cfg = root.find("Config")
    out = [f"## `{path.split('/')[-1]}`", ""]
    slaves = cfg.findall("Slave")
    out.append(f"{len(slaves)} slave(s). Master InitCmds: "
               f"{len(cfg.findall('Master/InitCmds/InitCmd'))}.")
    out.append("")

    # cyclic frames. TwinCAT also polls the mailbox-state FMMU with an LRD
    # at Master/MailboxStates/StartAddr: that one is NOT process data and
    # does not make an ENI an "LRD/LWR" ENI (E-06 must look at the address).
    mbx_start = num(cfg.findtext("Master/MailboxStates/StartAddr"), -1)
    mbx_count = num(cfg.findtext("Master/MailboxStates/Count"), 0)
    rows, lrdlwr = [], False
    for c in cfg.findall("Cyclic/Frame/Cmd"):
        k = num(c.findtext("Cmd"), -1)
        addr = num(c.findtext("Addr"))
        ado = num(c.findtext("Ado"))
        ln = txt(c, "DataLength")
        if k in (10, 11, 12) and addr is not None and mbx_start >= 0 and \
                mbx_start <= addr < mbx_start + max(1, (mbx_count + 7) // 8):
            role = "mailbox state poll"
        elif k in (10, 11, 12):
            role = "process data"
            lrdlwr = lrdlwr or k in (10, 11)
        elif ado is not None:
            role = reg_name(ado) or f"register 0x{ado:04X}"
        else:
            role = ""
        where = f"0x{addr:08X}" if addr is not None else f"ado 0x{ado:04X}" if ado is not None else "-"
        rows.append(f"| {CMD.get(k, k)} | {where} | {ln} | {role} |")
    out.append("**Cyclic datagrams**" + (" -- **process data by LRD/LWR** "
               "(E-06: loader must refuse, SOEM maps one LRW)" if lrdlwr else " (process data by LRW)"))
    out.append("")
    out.append("| cmd | address | length | role |")
    out.append("|---|---|---|---|")
    out.extend(rows)
    out.append("")

    unknown_total = 0
    for i, s in enumerate(slaves):
        info = s.find("Info")
        name = txt(info, "Name")
        out.append(f"### Slave {i + 1}: {name}")
        out.append("")
        out.append(f"- identity: vendor {txt(info, 'VendorId')} product {txt(info, 'ProductCode')} "
                   f"rev {txt(info, 'RevisionNo')} serial {txt(info, 'SerialNo')}; "
                   f"PhysAddr {txt(info, 'PhysAddr')} AutoIncAddr {txt(info, 'AutoIncAddr')}")
        pd = s.find("ProcessData")
        if pd is not None:
            out.append(f"- process data: send {txt(pd, 'Send/BitLength', '0')} bit, "
                       f"recv {txt(pd, 'Recv/BitLength', '0')} bit; "
                       f"RxPdo {[txt(p, 'Index') for p in pd.findall('RxPdo') if p.get('Sm')]} "
                       f"TxPdo {[txt(p, 'Index') for p in pd.findall('TxPdo') if p.get('Sm')]}")
        mbx = s.find("Mailbox")
        if mbx is not None:
            protos = [p.text for p in mbx.findall("Protocol")]
            out.append(f"- mailbox: {', '.join(protos) or '-'}")
        dc = s.find("DC")
        if dc is not None:
            out.append(f"- DC: ReferenceClock {txt(dc, 'ReferenceClock', '-')} CycleTime0 "
                       f"{txt(dc, 'CycleTime0', '-')} CycleTime1 {txt(dc, 'CycleTime1', '-')} "
                       f"ShiftTime {txt(dc, 'ShiftTime', '-')}")
        out.append("")

        regs = s.findall("InitCmds/InitCmd")
        if regs:
            out.append("| # | trans | cmd | adp | ado | register | data | comment |" +
                       (" known |" if known is not None else ""))
            out.append("|---|---|---|---|---|---|---|---|" + ("---|" if known is not None else ""))
            for j, ic in enumerate(regs):
                c = num(ic.findtext("Cmd"), -1)
                ado = num(ic.findtext("Ado"))
                row = (f"| {j + 1} | {trans(ic)} | {CMD.get(c, c)} | {txt(ic, 'Adp')} | "
                       f"{'0x%04X' % ado if ado is not None else '-'} | {reg_name(ado)} | "
                       f"`{short(txt(ic, 'Data'))}` | {txt(ic, 'Comment')} |")
                if known is not None:
                    k = is_known(known, ado, c)
                    unknown_total += 0 if k else 1
                    row += " yes |" if k else " **UNKNOWN** |"
                out.append(row)
            out.append("")

        coes = s.findall("Mailbox/CoE/InitCmds/InitCmd")
        if coes:
            out.append("| # | trans | ccs | object | CA | timeout ms | data | comment | disabled |")
            out.append("|---|---|---|---|---|---|---|---|---|")
            for j, ic in enumerate(coes):
                ca = (ic.get("CompleteAccess") or txt(ic, "CompleteAccess", "0")).lower()
                out.append(f"| {j + 1} | {trans(ic)} | {txt(ic, 'Ccs')} | "
                           f"0x{num(txt(ic, 'Index'), 0):04X}:{num(txt(ic, 'SubIndex'), 0):02X} | "
                           f"{'**yes**' if ca in ('1', 'true') else 'no'} | {txt(ic, 'Timeout', '-')} | "
                           f"`{short(txt(ic, 'Data'), 32)}` | {txt(ic, 'Comment')} | "
                           f"{'yes' if ic.get('Disabled') in ('1', 'true') else ''} |")
            out.append("")
    if known is not None:
        out.append(f"**Register InitCmds not in the known list: {unknown_total}**")
        out.append("")
    return out


def main():
    args = sys.argv[1:]
    known = None
    if "--known" in args:
        k = args.index("--known")
        known = load_known(args[k + 1])
        del args[k:k + 2]
    if not args:
        raise SystemExit(__doc__)
    rc = 0
    for p in args:
        try:
            print("\n".join(audit(p, known)))
        except (ET.ParseError, OSError, AttributeError) as e:
            print(f"eni_audit: {p}: {e}", file=sys.stderr)
            rc = 2
    return rc


if __name__ == "__main__":
    sys.exit(main())
