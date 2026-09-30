#!/usr/bin/env python3
"""make_ca_variant.py -- GD9.3: derive the ESI of `soft_bus --coe-ca` from
config/esi/softbus_esi.xml, for the TwinCAT session of step 9.4 (ENI with
Complete Access PDO assign InitCmds, eni_8node_ca.xml).

Differences from the default ESI (everything else is copied verbatim, so
the two files cannot drift apart):
  - Mailbox/CoE: PdoAssign="true" CompleteAccess="true" (SII General
    category CoE details 0x25 = SDO | PDOASSIGN | SDOCA)
  - Dictionary: 0x1C00 (SM types), 0x1C12/0x1C13 (PDO assign, writable in
    PREOP), 0x1600/0x1A00 (PDO mapping, read-only), 0x8002 (RW octet string,
    16 byte by default -- soft_bus accepts 1..400)
  - Type name SOFTBUS-PD4-CA; identity (vendor/product/revision) unchanged,
    so only ONE of the two files may sit in TwinCAT's ESI folder at a time.

The generated file is committed (config/esi/softbus_esi_ca.xml); CI checks
that it is up to date (--check) and esi_check.py compares it with the SII of
`sii_dump --coe-ca`. Default pdo_size 4 only (one 32 bit entry per PDO).

Usage: make_ca_variant.py [--check]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "config", "esi", "softbus_esi.xml")
DST = os.path.join(ROOT, "config", "esi", "softbus_esi_ca.xml")


def sub(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f"make_ca_variant: expected exactly one {old!r} in {SRC}")
    return text.replace(old, new)


def si(idx, name, typ, bits, offs, access):
    sub_idx = f"<SubIdx>{idx}</SubIdx>" if idx is not None else ""
    return (f"                <SubItem>{sub_idx}<Name>{name}</Name><Type>{typ}</Type>"
            f"<BitSize>{bits}</BitSize><BitOffs>{offs}</BitOffs>"
            f"<Flags><Access{access[1]}>{access[0]}</Access></Flags></SubItem>\n")


RO = ("ro", "")
RW = ("rw", "")
RW_PREOP = ("rw", ' WriteRestrictions="PreOP"')

DATATYPES = (
    "              <DataType>\n"
    "                <Name>UINT</Name>\n"
    "                <BitSize>16</BitSize>\n"
    "              </DataType>\n"
    "              <DataType>\n"
    "                <Name>OCTET_STRING(16)</Name>\n"
    "                <BitSize>128</BitSize>\n"
    "              </DataType>\n"
    "              <DataType>\n"
    "                <Name>DT1C00ARR</Name>\n"
    "                <BaseType>USINT</BaseType>\n"
    "                <BitSize>32</BitSize>\n"
    "                <ArrayInfo><LBound>1</LBound><Elements>4</Elements></ArrayInfo>\n"
    "              </DataType>\n"
    "              <DataType>\n"
    "                <Name>DT1C00</Name>\n"
    "                <BitSize>48</BitSize>\n"
    + si(0, "SubIndex 000", "USINT", 8, 0, RO)
    + si(None, "Elements", "DT1C00ARR", 32, 16, RO) +
    "              </DataType>\n"
    "              <DataType>\n"
    "                <Name>DT1C12ARR</Name>\n"
    "                <BaseType>UINT</BaseType>\n"
    "                <BitSize>16</BitSize>\n"
    "                <ArrayInfo><LBound>1</LBound><Elements>1</Elements></ArrayInfo>\n"
    "              </DataType>\n"
    "              <DataType>\n"
    "                <Name>DT1C12</Name>\n"
    "                <BitSize>32</BitSize>\n"
    + si(0, "SubIndex 000", "USINT", 8, 0, RW_PREOP)
    + si(None, "Elements", "DT1C12ARR", 16, 16, RW_PREOP) +
    "              </DataType>\n"
    "              <DataType>\n"
    "                <Name>DT1600</Name>\n"
    "                <BitSize>48</BitSize>\n"
    + si(0, "SubIndex 000", "USINT", 8, 0, RO)
    + si(1, "SubIndex 001", "UDINT", 32, 16, RO) +
    "              </DataType>\n"
)


def obj(index, name, typ, bits, defaults, access="ro"):
    info = ""
    if defaults:
        info = "                <Info>\n" + "".join(
            f"                  <SubItem><Name>{n}</Name><Info><DefaultData>{d}</DefaultData></Info></SubItem>\n"
            for n, d in defaults) + "                </Info>\n"
    return (f"              <Object>\n"
            f"                <Index>#x{index:04X}</Index>\n"
            f"                <Name>{name}</Name>\n"
            f"                <Type>{typ}</Type>\n"
            f"                <BitSize>{bits}</BitSize>\n"
            f"{info}"
            f"                <Flags><Access>{access}</Access></Flags>\n"
            f"              </Object>\n")


OBJECTS = (
    obj(0x1600, "Outputs (RxPDO mapping)", "DT1600", 48,
        [("SubIndex 000", "01"), ("SubIndex 001", "20010070")])
    + obj(0x1A00, "Inputs (TxPDO mapping)", "DT1600", 48,
          [("SubIndex 000", "01"), ("SubIndex 001", "20010060")])
    + obj(0x1C00, "Sync manager type", "DT1C00", 48,
          [("SubIndex 000", "04"), ("SubIndex 001", "01"), ("SubIndex 002", "02"),
           ("SubIndex 003", "03"), ("SubIndex 004", "04")])
    + obj(0x1C12, "RxPDO assign", "DT1C12", 32,
          [("SubIndex 000", "01"), ("SubIndex 001", "0016")], "rw")
    + obj(0x1C13, "TxPDO assign", "DT1C12", 32,
          [("SubIndex 000", "01"), ("SubIndex 001", "001A")], "rw")
)
OBJECT_8002 = obj(0x8002, "Download test string", "OCTET_STRING(16)", 128,
                  [], "rw")


def build():
    with open(SRC, encoding="utf-8") as f:
        t = f.read()
    t = sub(t, "softbus_esi.xml - EtherCAT Slave Information for the soft_bus simulator",
            "softbus_esi_ca.xml - EtherCAT Slave Information for soft_bus with its coe-ca option\n"
            "  GENERATED by tools/esi/make_ca_variant.py from softbus_esi.xml - do not edit.\n"
            "  Only one of softbus_esi.xml / softbus_esi_ca.xml may be in TwinCAT's ESI\n"
            "  folder at a time (same identity). Original header:\n\n"
            "  softbus_esi.xml - EtherCAT Slave Information for the soft_bus simulator")
    t = sub(t, ">SOFTBUS-PD4</Type>", ">SOFTBUS-PD4-CA</Type>")
    t = sub(t, "soft_bus ESC, 4 byte out / 4 byte in</Name>",
            "soft_bus ESC, 4 byte out / 4 byte in, Complete Access (coe-ca)</Name>")
    t = sub(t, 'PdoAssign="false" PdoConfig="false" CompleteAccess="false"',
            'PdoAssign="true" PdoConfig="false" CompleteAccess="true"')
    t = sub(t, "            </DataTypes>\n", DATATYPES + "            </DataTypes>\n")
    t = sub(t, "              <Object>\n                <Index>#x8000</Index>",
            OBJECTS + "              <Object>\n                <Index>#x8000</Index>")
    t = sub(t, "            </Objects>\n", OBJECT_8002 + "            </Objects>\n")
    return t


def main():
    text = build()
    if "--check" in sys.argv[1:]:
        with open(DST, encoding="utf-8") as f:
            if f.read() != text:
                print(f"{DST} is out of date: run tools/esi/make_ca_variant.py")
                return 1
        print(f"{DST} up to date")
        return 0
    with open(DST, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"wrote {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
