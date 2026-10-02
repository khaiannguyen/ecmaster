#!/usr/bin/env python3
"""esi_check.py -- Phase 8.2: does an ESI file describe the SII image a slave
(soft_bus, later the LAN9252 EEPROM) actually carries?

Compares, field by field:
  identity      Vendor / ProductCode / RevisionNo        vs SII words 8..13
  config area   Eeprom/ConfigData (words 0..n)           vs SII words 0..n
  checksum      CRC-8 of words 0..6 (ETG.2010)           vs SII word 7 low byte
  mailbox       Sm MBoxOut/MBoxIn start/size             vs SII words 24..27
                Mailbox/CoE present                      vs SII word 28 bit 2
  SyncManagers  Sm[i] StartAddress/DefaultSize/ControlByte/Enable
                                                         vs SII category 41
  PDOs          RxPdo/TxPdo: Index, Sm, entries (Index, SubIndex, BitLen)
                                                         vs SII categories 51/50
  order         SyncM category precedes the PDO categories (IgH parses in order)

A mismatch is reported as FAIL, a difference that is legal but worth knowing
as WARN. Exit code 0 only when there is no FAIL.

Usage:
  python3 esi_check.py softbus_esi.xml sii.bin [--device PRODUCTCODE]
  (sii.bin from: tools/soft_bus/sii_dump 4 > sii.bin, or an EEPROM dump)
"""
import struct
import sys
import xml.etree.ElementTree as ET

CAT_STRINGS, CAT_GENERAL, CAT_FMMU, CAT_SYNCM, CAT_TXPDO, CAT_RXPDO = 10, 30, 40, 41, 50, 51
CAT_END = 0xFFFF
CAT_START_WORD = 0x40

results = []          # (level, text)


def report(level, text):
    results.append((level, text))
    print(f"  [{level}] {text}")


def check(name, got, want, fmt="0x{:X}"):
    ok = got == want
    g = fmt.format(got) if isinstance(got, int) else str(got)
    w = fmt.format(want) if isinstance(want, int) else str(want)
    report("PASS" if ok else "FAIL", f"{name}: SII {g}, ESI {w}")
    return ok


def esi_int(text):
    """ESI numbers: '#x1A00', '0x1A00' or decimal."""
    t = (text or "").strip()
    if t.startswith("#x") or t.startswith("#X"):
        return int(t[2:], 16)
    return int(t, 0)


def crc8_sii(data):
    """ETG.2010 SII config-area checksum: CRC-8, poly x^8+x^2+x+1 (0x07),
    initial value 0xFF, no reflection, no final XOR, over bytes 0..13."""
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


# ---------------------------------------------------------------- SII side
def load_sii(path):
    raw = open(path, "rb").read()
    if len(raw) < CAT_START_WORD * 2:
        raise SystemExit(f"{path}: only {len(raw)} bytes, shorter than the fixed area")
    words = list(struct.unpack(f"<{len(raw) // 2}H", raw[: len(raw) // 2 * 2]))
    return raw, words


def sii_categories(words):
    cats, w = [], CAT_START_WORD
    while w + 1 < len(words) and words[w] != CAT_END:
        ctype, size = words[w], words[w + 1]
        body = struct.pack(f"<{size}H", *words[w + 2:w + 2 + size])
        cats.append((ctype, w, body))
        w += 2 + size
    return cats


def parse_syncm(body):
    sms = []
    for i in range(len(body) // 8):
        start, length, ctrl, status, act, pdi = struct.unpack("<HHBBBB", body[i * 8:i * 8 + 8])
        sms.append({"start": start, "len": length, "ctrl": ctrl, "enable": act & 1})
    return sms


def parse_pdos(body):
    pdos, off = [], 0
    while off + 8 <= len(body):
        index, n, sm, sync, name, flags = struct.unpack("<HBBBBH", body[off:off + 8])
        off += 8
        entries = []
        for _ in range(n):
            e_idx, e_sub, e_name, e_type, e_bits, e_flags = struct.unpack("<HBBBBH", body[off:off + 8])
            entries.append((e_idx, e_sub, e_bits))
            off += 8
        pdos.append({"index": index, "sm": sm, "entries": entries})
    return pdos


# ---------------------------------------------------------------- ESI side
def find_device(root, product):
    devs = root.findall("./Descriptions/Devices/Device")
    if not devs:
        raise SystemExit("ESI: no Descriptions/Devices/Device")
    if product is None:
        if len(devs) > 1:
            print(f"ESI has {len(devs)} devices, checking the first (use --device)")
        return devs[0]
    for d in devs:
        if esi_int(d.find("Type").get("ProductCode")) == product:
            return d
    raise SystemExit(f"ESI: no device with ProductCode 0x{product:X}")


def default_pd_bytes(dev, tag, sm):
    """Bytes of the PDOs of one direction that the ESI assigns to SM sm."""
    bits = sum(e[2] for p in esi_pdos(dev, tag) if p["sm"] == sm for e in p["entries"])
    return (bits + 7) // 8


def esi_pdos(dev, tag):
    out = []
    for p in dev.findall(tag):
        entries = []
        for e in p.findall("Entry"):
            entries.append((esi_int(e.findtext("Index")),
                            esi_int(e.findtext("SubIndex", "0")),
                            esi_int(e.findtext("BitLen"))))
        out.append({"index": esi_int(p.findtext("Index")),
                    "sm": esi_int(p.get("Sm", "-1")), "entries": entries})
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    product = None
    if "--device" in sys.argv:
        product = esi_int(sys.argv[sys.argv.index("--device") + 1])
        args = [a for a in args if a != sys.argv[sys.argv.index("--device") + 1]]
    if len(args) != 2:
        print(__doc__)
        return 2
    esi_path, sii_path = args

    root = ET.parse(esi_path).getroot()
    dev = find_device(root, product)
    raw, words = load_sii(sii_path)
    print(f"ESI: {esi_path}  device '{dev.findtext('Type')}'")
    print(f"SII: {sii_path}  ({len(raw)} bytes, {len(words)} words)\n")

    print("[identity]")
    check("Vendor ID", words[8] | words[9] << 16, esi_int(root.findtext("./Vendor/Id")))
    t = dev.find("Type")
    check("Product code", words[10] | words[11] << 16, esi_int(t.get("ProductCode")))
    check("Revision", words[12] | words[13] << 16, esi_int(t.get("RevisionNo")))

    print("[config area, words 0..7]")
    cfg_hex = (dev.findtext("./Eeprom/ConfigData") or "").strip()
    cfg = bytes.fromhex(cfg_hex) if cfg_hex else b""
    if not cfg:
        report("WARN", "ESI has no Eeprom/ConfigData")
    for i in range(0, min(len(cfg), 14) // 2 * 2, 2):
        check(f"config word {i // 2}", words[i // 2], cfg[i] | cfg[i + 1] << 8, "0x{:04X}")
    crc = crc8_sii(raw[:14])
    if (words[7] & 0xFF) == crc:
        report("PASS", f"word 7 checksum 0x{crc:02X} = CRC-8 of words 0..6")
    else:
        report("FAIL", f"word 7 checksum: SII 0x{words[7] & 0xFF:02X}, expected CRC-8 0x{crc:02X} "
                       f"(a real ESC would flag 0x0502 bit 11 and not load its config)")

    print("[mailbox]")
    sms_esi = dev.findall("Sm")
    mb = {s.text: s for s in sms_esi if s.text in ("MBoxOut", "MBoxIn")}
    if "MBoxOut" in mb:
        check("std mailbox out offset (word 24)", words[24], esi_int(mb["MBoxOut"].get("StartAddress")))
        check("std mailbox out size (word 25)", words[25], esi_int(mb["MBoxOut"].get("DefaultSize")))
    if "MBoxIn" in mb:
        check("std mailbox in offset (word 26)", words[26], esi_int(mb["MBoxIn"].get("StartAddress")))
        check("std mailbox in size (word 27)", words[27], esi_int(mb["MBoxIn"].get("DefaultSize")))
    check("mailbox protocol CoE bit (word 28)", (words[28] >> 2) & 1,
          1 if dev.find("./Mailbox/CoE") is not None else 0, "{}")
    for proto, bit in (("EoE", 1), ("FoE", 3), ("SoE", 4), ("VoE", 5)):
        if (words[28] >> bit) & 1 != (1 if dev.find(f"./Mailbox/{proto}") is not None else 0):
            report("FAIL", f"mailbox protocol {proto} bit disagrees")

    cats = sii_categories(words)
    types = [c[0] for c in cats]
    print(f"[categories] in SII order: {types}")
    # Phase 9.3: General category CoE details (ETG.2010: bit0 SDO, bit1 SDO Info,
    # bit2 PDO assign, bit3 PDO config, bit4 upload at startup, bit5 SDO
    # Complete Access) against the ESI Mailbox/CoE attributes. Without a
    # General category a master sees CoE details 0 (SOEM: no CA).
    coe = dev.find("./Mailbox/CoE")
    def esi_flag(attr):
        return 1 if coe is not None and coe.get(attr, "false").lower() in ("true", "1") else 0
    want_det = 0
    if coe is not None:
        want_det = (0x01 | esi_flag("SdoInfo") << 1 | esi_flag("PdoAssign") << 2 |
                    esi_flag("PdoConfig") << 3 | esi_flag("PdoUpload") << 4 | esi_flag("CompleteAccess") << 5)
    gen = next((b for t, _, b in cats if t == CAT_GENERAL), None)
    if gen is not None:
        print("[general]")
        check("General CoE details (body byte 5)", gen[5] if len(gen) > 5 else -1, want_det, "0x{:02X}")
    elif want_det & 0x3E:
        report("FAIL", f"ESI CoE flags need CoE details 0x{want_det:02X} but the SII has no General "
                       f"category (a master would not see them)")
    for missing, name in ((CAT_GENERAL, "General (30)"), (CAT_STRINGS, "Strings (10)"), (CAT_FMMU, "FMMU (40)")):
        if missing not in types:
            report("WARN", f"SII has no {name} category; an ESI-generated image (TwinCAT) will carry one")
    if CAT_SYNCM in types:
        first_pdo = min([types.index(t) for t in (CAT_TXPDO, CAT_RXPDO) if t in types], default=len(types))
        if types.index(CAT_SYNCM) < first_pdo:
            report("PASS", "SyncM category precedes the PDO categories")
        else:
            report("FAIL", "SyncM category after a PDO category (IgH: 'Invalid SM index')")

    print("[sync managers]")
    syncm = next((parse_syncm(b) for t, _, b in cats if t == CAT_SYNCM), [])
    check("SM count", len(syncm), len(sms_esi), "{}")
    for i, (s_sii, s_esi) in enumerate(zip(syncm, sms_esi)):
        want = (esi_int(s_esi.get("StartAddress")), esi_int(s_esi.get("DefaultSize", "0")),
                esi_int(s_esi.get("ControlByte")), esi_int(s_esi.get("Enable", "0")))
        got = (s_sii["start"], s_sii["len"], s_sii["ctrl"], s_sii["enable"])
        ok = got == want
        if not ok and i >= 2 and want[1] == 0 and got[:1] + got[2:] == want[:1] + want[2:]:
            # Phase 9.9: a process data SM with DefaultSize 0 (IS620N) -- the SII
            # carries the size of the PDOs assigned by default, as an
            # ESI-generated image does (tools/esi/esi2profile.py)
            dflt = default_pd_bytes(dev, "RxPdo" if i == 2 else "TxPdo", i)
            if got[1] == dflt:
                report("PASS", f"SM{i} ({s_esi.text}): SII start 0x{got[0]:04X} len {got[1]} "
                               f"= default PDOs (ESI DefaultSize 0) ctrl 0x{got[2]:02X} en {got[3]}")
                continue
        report("PASS" if ok else "FAIL",
               f"SM{i} ({s_esi.text}): SII start 0x{got[0]:04X} len {got[1]} ctrl 0x{got[2]:02X} en {got[3]}"
               + ("" if ok else f" | ESI start 0x{want[0]:04X} len {want[1]} ctrl 0x{want[2]:02X} en {want[3]}"))

    for tag, ctype, label in (("RxPdo", CAT_RXPDO, "RxPDO"), ("TxPdo", CAT_TXPDO, "TxPDO")):
        print(f"[{label}]")
        sii_p = [p for t, _, b in cats if t == ctype for p in parse_pdos(b)]
        esi_p = esi_pdos(dev, tag)
        check(f"{label} count", len(sii_p), len(esi_p), "{}")
        for a, b in zip(sii_p, esi_p):
            check(f"{label} 0x{b['index']:04X} index", a["index"], b["index"])
            if b["sm"] == -1 and a["sm"] == 0xFF:
                # Phase 9.9: no Sm attribute = not assigned by default; SII SM 0xFF
                report("PASS", f"{label} 0x{b['index']:04X} SM: SII 255 (not assigned), ESI no Sm")
            else:
                check(f"{label} 0x{b['index']:04X} SM", a["sm"], b["sm"], "{}")
            same = a["entries"] == b["entries"]
            report("PASS" if same else "FAIL",
                   f"{label} 0x{b['index']:04X} entries: SII "
                   + ", ".join(f"0x{i:04X}:{s:02X}/{n}b" for i, s, n in a["entries"])
                   + ("" if same else " | ESI " + ", ".join(f"0x{i:04X}:{s:02X}/{n}b" for i, s, n in b["entries"])))

    print("[eeprom size]")
    esz = dev.findtext("./Eeprom/ByteSize")
    if esz and len(raw) > esi_int(esz):
        report("FAIL", f"SII image {len(raw)} bytes > ESI ByteSize {esz}")
    elif esz:
        report("PASS", f"SII image {len(raw)} bytes fits ESI ByteSize {esz}")

    n_fail = sum(1 for lvl, _ in results if lvl == "FAIL")
    n_warn = sum(1 for lvl, _ in results if lvl == "WARN")
    n_pass = sum(1 for lvl, _ in results if lvl == "PASS")
    print(f"\nRESULT: {n_pass} pass, {n_fail} fail, {n_warn} warn")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
