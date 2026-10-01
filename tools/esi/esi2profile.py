#!/usr/bin/env python3
"""esi2profile.py -- GD9.9: turn an ESI device description into a soft_bus
node profile, so soft_bus can stand in for a vendor slave (Inovance IS620N)
or for the draft of our own slave (P1) on a virtual bus.

What a profile carries (all of it from the ESI, nothing invented):
  identity      Type ProductCode / RevisionNo, Vendor Id
  config        Eeprom/ConfigData words 0..6 (SII config area; soft_bus adds
                the CRC of word 7)
  mailbox       Sm MBoxOut / MBoxIn start + DefaultSize, CoE flags
                (SdoInfo, PdoAssign, PdoConfig, CompleteAccess)
  sm            the four SyncManagers: start, size, control byte, enable.
                A process data SM with DefaultSize 0 gets the size of the
                PDOs the ESI assigns to it by default (Sm attribute)
  dc            Dc/OpMode present -> DC capable, AssignActivate of the first
  pdo           every RxPdo/TxPdo: index, default SM (255 = not assigned),
                Fixed, entries (index:sub:bits), Exclude list
  obj / sub     the object dictionary (Profile/Dictionary): every object,
                every subindex with its size, access (ro / rw / rw_preop)
                and default value (little endian hex). RECORD and ARRAY
                types are expanded from DataTypes (ArrayInfo elements).
                Assign objects 0x1C12/0x1C13 are rw_preop (ETG.1020) and
                start with the ESI's default assignment (the PDOs with an Sm
                attribute), not with the dictionary DefaultData, which some
                vendors (IS620N) leave at 0. A mapping object whose
                DefaultData disagrees with its PDO description (IS620N
                0x1702) gets the PDO description, with a warning.

Usage:
  esi2profile.py ESI.xml [-o out.prof] [--device PRODUCTCODE]
Exit code 0 ok, 1 if the ESI cannot be turned into a profile.
"""
import hashlib
import os
import sys
import xml.etree.ElementTree as ET


def num(t, default=None):
    if t is None:
        return default
    t = t.strip()
    if not t:
        return default
    if t[:2] in ("#x", "#X"):
        return int(t[2:], 16)
    try:
        return int(t, 0)
    except ValueError:
        return int(t, 10)          # decimal with leading zeros ("0010")


def boolattr(e, name):
    v = (e.get(name) or "0").strip().lower()
    return 1 if v in ("1", "true") else 0


def fail(msg):
    raise ValueError(msg)


class Dictionary:
    def __init__(self, dic):
        self.types = {}
        if dic is not None:
            for dt in dic.findall("DataTypes/DataType"):
                self.types[(dt.findtext("Name") or "").strip()] = dt
        self.objects = dic.findall("Objects/Object") if dic is not None else []

    def bits_of(self, tname, given):
        if given is not None:
            return given
        dt = self.types.get(tname)
        return num(dt.findtext("BitSize"), 0) if dt is not None else 0

    def subs_of(self, tname):
        """Expanded (sub, name, bits, access) of a RECORD/ARRAY type, or None for a
        simple type."""
        dt = self.types.get(tname)
        if dt is None or not dt.findall("SubItem"):
            return None
        out = []
        for si in dt.findall("SubItem"):
            st = (si.findtext("Type") or "").strip()
            acc = access_of(si)
            sdt = self.types.get(st)
            ai = sdt.find("ArrayInfo") if sdt is not None else None
            if si.findtext("SubIdx") is None and ai is not None:
                lb = num(ai.findtext("LBound"), 1)
                n = num(ai.findtext("Elements"), 0)
                ebits = self.bits_of((sdt.findtext("BaseType") or "").strip(), None)
                for k in range(n):
                    out.append((lb + k, f"SubIndex {lb + k:03d}", ebits, acc))
            else:
                out.append((num(si.findtext("SubIdx"), 0), (si.findtext("Name") or "").strip(),
                            num(si.findtext("BitSize"), 0), acc))
        return out


def access_of(e):
    a = e.find("Flags/Access")
    if a is None:
        return None
    t = (a.text or "").strip().lower()
    if t in ("rw", "wo"):
        w = (a.get("WriteRestrictions") or "").lower()
        return "rw_preop" if w == "preop" else "rw"
    return "ro"


def value_bytes(info, nbytes):
    """Default of one entry: DefaultData (hex, already little endian),
    DefaultValue (number), DefaultString; 0 when the ESI has none."""
    if info is None:
        return bytes(nbytes)
    dd = info.findtext("DefaultData")
    if dd and dd.strip():
        b = bytes.fromhex(dd.strip())
        return (b + bytes(nbytes))[:nbytes]
    dv = info.findtext("DefaultValue")
    if dv and dv.strip():
        v = num(dv)
        if v < 0:
            v &= (1 << (8 * nbytes)) - 1
        return (v & ((1 << (8 * nbytes)) - 1)).to_bytes(nbytes, "little")
    ds = info.findtext("DefaultString")
    if ds is not None:
        return (ds.encode("latin-1", "replace") + bytes(nbytes))[:nbytes]
    return bytes(nbytes)


def build(path, product=None):
    raw = open(path, "rb").read()
    root = ET.fromstring(raw)
    vendor = num(root.findtext("Vendor/Id"))
    devs = root.findall("Descriptions/Devices/Device")
    if product is not None:
        devs = [d for d in devs if num(d.find("Type").get("ProductCode")) == product]
    if not devs:
        fail("no matching Device")
    d = devs[0]
    t = d.find("Type")
    name = (t.text or "").strip()
    pc, rev = num(t.get("ProductCode")), num(t.get("RevisionNo"))

    out = [f"# generated by tools/esi/esi2profile.py -- do not edit, regenerate from the ESI",
           f"# source {os.path.basename(path)} sha256 {hashlib.sha256(raw).hexdigest()}",
           "profile 1",
           f'name "{name}"',
           f"identity vendor 0x{vendor:08X} product 0x{pc:08X} rev 0x{rev:08X} serial 0x00000000"]

    cfg = (d.findtext("Eeprom/ConfigData") or "").strip()
    if cfg:
        b = (bytes.fromhex(cfg) + bytes(14))[:14]
        words = [b[2 * i] | b[2 * i + 1] << 8 for i in range(7)]
        out.append("config " + " ".join(f"0x{w:04X}" for w in words))

    # PDOs
    pdos = []
    for tag, dirn in (("RxPdo", "rx"), ("TxPdo", "tx")):
        for p in d.findall(tag):
            ents = [(num(e.findtext("Index"), 0), num(e.findtext("SubIndex"), 0), num(e.findtext("BitLen"), 0))
                    for e in p.findall("Entry")]
            sm = num(p.get("Sm"), 255) if p.get("Sm") is not None else 255
            pdos.append({"dir": dirn, "index": num(p.findtext("Index")), "sm": sm,
                         "fixed": boolattr(p, "Fixed"), "entries": ents,
                         "ex": [num(x.text) for x in p.findall("Exclude")]})
    def_bytes = {2: 0, 3: 0}
    for p in pdos:
        if p["sm"] in def_bytes:
            def_bytes[p["sm"]] += sum(b for _, _, b in p["entries"])
    for k in def_bytes:
        if def_bytes[k] % 8:
            fail(f"SM{k}: default PDOs are {def_bytes[k]} bits, not whole bytes")
        def_bytes[k] //= 8

    # SMs
    sms = d.findall("Sm")
    if len(sms) < 4:
        fail(f"{len(sms)} Sm elements, soft_bus profiles need MBoxOut, MBoxIn, Outputs, Inputs")
    kinds = [(s.text or "").strip() for s in sms[:4]]
    if kinds != ["MBoxOut", "MBoxIn", "Outputs", "Inputs"]:
        fail(f"Sm order {kinds}, expected MBoxOut MBoxIn Outputs Inputs")
    sm_lines = []
    for i, s in enumerate(sms[:4]):
        size = num(s.get("DefaultSize"), 0)
        if i >= 2 and size == 0:
            size = def_bytes[i]
        sm_lines.append((i, num(s.get("StartAddress")), size, num(s.get("ControlByte")), boolattr(s, "Enable")))
    mo, mi = sm_lines[0], sm_lines[1]

    coe = d.find("Mailbox/CoE")
    out.append(f"mailbox out 0x{mo[1]:04X} {mo[2]} in 0x{mi[1]:04X} {mi[2]} coe {1 if coe is not None else 0}")
    if coe is not None:
        out.append(f"coe sdoinfo {boolattr(coe, 'SdoInfo')} pdoassign {boolattr(coe, 'PdoAssign')} "
                   f"pdoconfig {boolattr(coe, 'PdoConfig')} ca {boolattr(coe, 'CompleteAccess')}")
    for (i, st, sz, ctrl, en) in sm_lines:
        out.append(f"sm {i} start 0x{st:04X} len {sz} ctrl 0x{ctrl:02X} en {en}")

    dc = d.find("Dc")
    if dc is not None and dc.findall("OpMode"):
        aa = num(dc.find("OpMode").findtext("AssignActivate"), 0)
        out.append(f"dc 1 assign 0x{aa:04X}")
    else:
        out.append("dc 0 assign 0x0000")

    for p in pdos:
        ents = ",".join(f"0x{i:04X}:{s:02X}:{b}" for i, s, b in p["entries"]) or "-"
        ex = ",".join(f"0x{x:04X}" for x in p["ex"]) or "-"
        out.append(f"pdo {p['dir']} 0x{p['index']:04X} sm {p['sm']} fixed {p['fixed']} entries {ents} excludes {ex}")

    # Object dictionary
    dic = Dictionary(d.find("Profile/Dictionary"))
    if not dic.objects:
        fail("no Profile/Dictionary: soft_bus needs the objects of the slave")
    assign_default = {0x1C12: [p["index"] for p in pdos if p["dir"] == "rx" and p["sm"] == 2],
                      0x1C13: [p["index"] for p in pdos if p["dir"] == "tx" and p["sm"] == 3]}
    nobj = nsub = 0
    notes = []
    for o in dic.objects:
        ix = num(o.findtext("Index"))
        tname = (o.findtext("Type") or "").strip()
        oacc = access_of(o) or "ro"
        subs = dic.subs_of(tname)
        info = o.find("Info")
        lines = []
        if subs is None:
            bits = num(o.findtext("BitSize"), 0) or dic.bits_of(tname, None)
            nb = max(1, (bits + 7) // 8)
            lines.append((0, bits, oacc, value_bytes(info, nb)))
            code = "var"
        else:
            isubs = info.findall("SubItem") if info is not None else []
            for k, (sub, sname, bits, sacc) in enumerate(subs):
                nb = max(1, (bits + 7) // 8)
                si = next((x for x in isubs if (x.findtext("Name") or "").strip() == sname), None)
                if si is None and k < len(isubs):
                    si = isubs[k]
                lines.append((sub, bits, sacc or oacc, value_bytes(si.find("Info") if si is not None else None, nb)))
            code = "array" if any(s[1].startswith("SubIndex ") for s in subs[1:]) else "record"
        if ix in assign_default:
            lst = assign_default[ix]
            n = len(lines) - 1
            if len(lst) > n:
                fail(f"0x{ix:04X}: {len(lst)} default PDOs, object has {n} entries")
            new = [(0, 8, "rw_preop", bytes([len(lst)]))]
            for k in range(1, n + 1):
                v = lst[k - 1] if k <= len(lst) else 0
                new.append((k, 16, "rw_preop", v.to_bytes(2, "little")))
            lines = new
        pd = next((q for q in pdos if q["index"] == ix), None)
        if pd is not None and len(lines) > 1:
            # A mapping object: its default must be the PDO description
            # (what TwinCAT and the SII use). The IS620N ESI has 0x1702 with
            # DefaultData that disagrees (0x60B8 twice, no 0x60FF): the PDO
            # description wins, the difference is reported.
            want = [len(pd["entries"])] + [i << 16 | sb << 8 | b for i, sb, b in pd["entries"]]
            n = len(lines) - 1
            if len(pd["entries"]) > n:
                fail(f"0x{ix:04X}: PDO has {len(pd['entries'])} entries, object only {n}")
            got = [int.from_bytes(lines[0][3][:1], "little")] + \
                  [int.from_bytes(lines[k][3][:4], "little") for k in range(1, 1 + len(pd["entries"]))]
            if got != want:
                notes.append(f"0x{ix:04X}: dictionary DefaultData {' '.join(f'{v:08X}' for v in got[1:])} "
                             f"differs from the {pd['dir'].capitalize()}Pdo description; mapping from the PDO")
                new = [(lines[0][0], lines[0][1], lines[0][2], bytes([want[0]]))]
                for k in range(1, n + 1):
                    v = want[k] if k < len(want) else 0
                    new.append((lines[k][0], lines[k][1], lines[k][2], v.to_bytes(4, "little")))
                lines = new
        out.append(f"obj 0x{ix:04X} {code} {len(lines)}")
        for (sub, bits, acc, val) in lines:
            out.append(f"sub 0x{ix:04X} {sub} bits {bits} {acc} {val.hex() or '00'}")
        nobj += 1
        nsub += len(lines)

    # Objects the CoE PDO services need but the dictionary does not list
    # (the P1 draft ESI has none of them): made from the PDO descriptions,
    # read-only mapping objects, assign objects writable in PREOP.
    have = {num(o.findtext("Index")) for o in dic.objects}
    added = []
    if 0x1C00 not in have:
        added.append(0x1C00)
        out.append("obj 0x1C00 array 5")
        out.append("sub 0x1C00 0 bits 8 ro 04")
        for k, v in enumerate((1, 2, 3, 4), start=1):
            out.append(f"sub 0x1C00 {k} bits 8 ro {v:02x}")
    for ix, dirn in ((0x1C12, "rx"), (0x1C13, "tx")):
        if ix in have:
            continue
        added.append(ix)
        cands = [p for p in pdos if p["dir"] == dirn]
        lst = assign_default[ix]
        out.append(f"obj 0x{ix:04X} array {len(cands) + 1}")
        out.append(f"sub 0x{ix:04X} 0 bits 8 rw_preop {len(lst):02x}")
        for k in range(1, len(cands) + 1):
            v = lst[k - 1] if k <= len(lst) else 0
            out.append(f"sub 0x{ix:04X} {k} bits 16 rw_preop {v.to_bytes(2, 'little').hex()}")
    for p in pdos:
        if p["index"] in have:
            continue
        added.append(p["index"])
        out.append(f"obj 0x{p['index']:04X} record {len(p['entries']) + 1}")
        out.append(f"sub 0x{p['index']:04X} 0 bits 8 ro {len(p['entries']):02x}")
        for k, (i, sb, b) in enumerate(p["entries"], start=1):
            out.append(f"sub 0x{p['index']:04X} {k} bits 32 ro {(i << 16 | sb << 8 | b).to_bytes(4, 'little').hex()}")
    for t in notes:
        out.append("# " + t)
        print(f"esi2profile: warning: {t}", file=sys.stderr)
    if added:
        out.append("# not in the ESI dictionary, made from the PDO descriptions: " +
                   " ".join(f"0x{i:04X}" for i in added))
    out.append(f"# {len(pdos)} PDOs, {nobj} dictionary objects, {nsub} entries")
    return "\n".join(out) + "\n"


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    dst = args[args.index("-o") + 1] if "-o" in args else None
    prod = num(args[args.index("--device") + 1]) if "--device" in args else None
    try:
        text = build(args[0], prod)
    except (ValueError, ET.ParseError) as e:
        print(f"esi2profile: {args[0]}: {e}", file=sys.stderr)
        return 1
    if dst:
        with open(dst, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
