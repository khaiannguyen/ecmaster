#!/usr/bin/env python3
"""bringup_report.py -- Phase 10.0: grade R-02, R-04, R-05, R-06 of the IS620N
bring-up from the raw files run_bringup_10_0.sh collects, and print the
"real behaviour" tables the plan asks for (claude/giai_doan_10_ke_hoach.md
section 2.2, input of the virtual servo 10.2).

  bringup_report.py --n 2 --eni eni_2servo.enicfg --peek peek.txt
                    [--esi IS620N.xml] [--er5 er_r05.log] [--er6 er_r06.log]

Prints markdown on stdout. Verdict lines have the form
  VERDICT R-0x PASS|FAIL|DIFF|SKIP text
(DIFF: a difference to record, not an error -- e.g. the ESI's DefaultData).
Exit 0 when no FAIL.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "esi"))

VERDICTS = []


def verdict(rid, v, text):
    VERDICTS.append((rid, v, text))
    print(f"\nVERDICT {rid} {v} {text}")


# ------------------------------------------------------------------ inputs
def load_peek(path):
    slaves, sdo, nslaves = {}, {}, None
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        m = re.match(r"SLAVES (\d+)", line)
        if m:
            nslaves = int(m.group(1))
            continue
        m = re.match(r'SLAVE (\d+) name "(.*)" (.*)', line)
        if m:
            d = {"name": m.group(2)}
            toks = m.group(3).split()
            for k, v in zip(toks[0::2], toks[1::2]):
                d[k] = v
            slaves[int(m.group(1))] = d
            continue
        m = re.match(r"SDO (\d+) 0x([0-9A-F]{4}):([0-9A-F]{2}) (.*)", line)
        if m:
            s, idx, sub, rest = int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
            if rest.startswith("size"):
                mm = re.match(r"size (\d+) = (.*)", rest)
                val = mm.group(2)
                if val.startswith("0x"):
                    sdo[(s, idx, sub)] = ("val", int(val, 16), int(mm.group(1)))
                else:
                    sdo[(s, idx, sub)] = ("raw", val, int(mm.group(1)))
            else:
                sdo[(s, idx, sub)] = ("err", rest, 0)
    return nslaves, slaves, sdo


def load_eni(path):
    out = {}
    for line in open(path):
        m = re.match(r"slave (\d+) .*vendor (0x[0-9A-Fa-f]+) product (0x[0-9A-Fa-f]+) rev (0x[0-9A-Fa-f]+)", line)
        if m:
            out[int(m.group(1))] = tuple(int(x, 16) for x in m.group(2, 3, 4))
    return out


def load_esi(path):
    import xml.etree.ElementTree as ET
    import esi_check as ec
    root = ET.parse(path).getroot()
    vendor = ec.esi_int(root.findtext("./Vendor/Id"))
    devs = []
    for d in root.findall("./Descriptions/Devices/Device"):
        t = d.find("Type")
        pdos = {}
        for tag in ("RxPdo", "TxPdo"):
            for p in ec.esi_pdos(d, tag):
                pdos[p["index"]] = p
        devs.append({"product": ec.esi_int(t.get("ProductCode")), "rev": ec.esi_int(t.get("RevisionNo")),
                     "name": (t.text or "").strip(), "pdos": pdos})
    return vendor, devs


# --------------------------------------------------------------- decoding
MODES = [(0, "PP"), (1, "VL"), (2, "PV"), (3, "TQ"), (5, "HM"), (6, "IP"), (7, "CSP"), (8, "CSV"), (9, "CST")]
MODE_NUM = {1: "PP", 2: "VL", 3: "PV", 4: "TQ", 6: "HM", 7: "IP", 8: "CSP", 9: "CSV", 10: "CST", 0: "none"}


def modes_str(v):
    return " ".join(n for b, n in MODES if v >> b & 1) or "none"


def sw_state(sw):
    if sw & 0x4F == 0x00:
        return "Not ready to switch on"
    if sw & 0x4F == 0x40:
        return "Switch on disabled"
    if sw & 0x6F == 0x21:
        return "Ready to switch on"
    if sw & 0x6F == 0x23:
        return "Switched on"
    if sw & 0x6F == 0x27:
        return "Operation enabled"
    if sw & 0x6F == 0x07:
        return "Quick stop active"
    if sw & 0x4F == 0x0F:
        return "Fault reaction active"
    if sw & 0x4F == 0x08:
        return "Fault"
    return "?"


def sw_bits(sw):
    names = {4: "voltage", 5: "quickstop(n)", 7: "warning", 9: "remote", 10: "target", 11: "int.limit"}
    return ",".join(n for b, n in names.items() if sw >> b & 1) or "-"


SYNC = {0: "free run", 1: "SM-synchronous", 2: "DC SYNC0", 3: "DC SYNC1", 0x22: "synchronous with SM2 event"}


def sval(sdo, s, idx, sub=0):
    v = sdo.get((s, idx, sub))
    if not v:
        return None
    return v[1] if v[0] == "val" else None


def sshow(sdo, s, idx, sub=0, fmt=None):
    v = sdo.get((s, idx, sub))
    if not v:
        z = sdo.get((s, idx, 0))
        return z[1] if z and z[0] == "err" else "(not read)"
    if v[0] == "err":
        return v[1]
    if v[0] == "raw":
        return v[1]
    return fmt(v[1]) if fmt else f"0x{v[1]:0{2 * v[2]}X} ({v[1]})"


def ascii4(v):
    """A short visible string that ecm_peek printed as a number (<= 8 byte, little-endian)."""
    b = v.to_bytes(8, "little").rstrip(b"\0")
    if b and all(32 <= c < 127 for c in b):
        return f'"{b.decode()}" (0x{v:X})'
    return f"0x{v:X}"


def signed(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def pdo_read(sdo, s, idx):
    n = sval(sdo, s, idx, 0)
    if n is None:
        return None
    ents = []
    for k in range(1, n + 1):
        e = sval(sdo, s, idx, k)
        if e is None:
            return None
        ents.append((e >> 16, e >> 8 & 0xFF, e & 0xFF))
    return ents


def ents_str(ents):
    return ", ".join(f"{i:04X}:{s:02X}/{b}" for i, s, b in ents) if ents else "(empty)"


# ------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--eni", required=True)
    ap.add_argument("--peek", help="ecm_peek output (R-02/R-04); without it only R-05/R-06")
    ap.add_argument("--esi")
    ap.add_argument("--er5")
    ap.add_argument("--er6")
    ap.add_argument("--er9", action="append", help="CYCLE_US:er_r09_CYCLE.log (repeatable)")
    a = ap.parse_args()

    eni = load_eni(a.eni)
    esi_vendor, esi_devs = load_esi(a.esi) if a.esi else (None, [])
    if a.peek:
        peek_part(a, eni, esi_vendor, esi_devs)
    run_part(a)
    print("\n## Summary\n")
    for rid, v, text in VERDICTS:
        print(f"- {rid}: **{v}** {text}")
    return 1 if any(v == "FAIL" for _, v, _ in VERDICTS) else 0


def peek_part(a, eni, esi_vendor, esi_devs):
    nslaves, slaves, sdo = load_peek(a.peek)
    # ---------------------------------------------------------- R-02
    print("## R-02 identity (SII and 0x1018) vs ENI" + (" and ESI" if a.esi else ""))
    print()
    print("| slave | name | vendor | product | revision | serial | 0x1018:01..03 | state | AL | DC | ports |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    bad = []
    for s in sorted(slaves):
        d = slaves[s]
        man, pid, rev = (int(d[k], 16) for k in ("man", "id", "rev"))
        o = tuple(sval(sdo, s, 0x1018, k) for k in (1, 2, 3))
        ostr = "/".join("-" if x is None else f"0x{x:08X}" for x in o)
        print(f"| {s} | {d['name']} | 0x{man:08X} | 0x{pid:08X} | **0x{rev:08X}** | {d['serial']} | {ostr} | "
              f"{d['state']} | {d['al']} | {d['hasdc']} (delay {d['pdelay']} ns) | {d['ports']} |")
        if s in eni and (man, pid, rev) != eni[s]:
            bad.append(f"slave {s} SII 0x{man:08X}/0x{pid:08X}/0x{rev:08X} != ENI "
                       f"0x{eni[s][0]:08X}/0x{eni[s][1]:08X}/0x{eni[s][2]:08X}")
        if None not in o and o != (man, pid, rev):
            bad.append(f"slave {s} 0x1018 {ostr} != SII")
        if a.esi:
            if man != esi_vendor:
                bad.append(f"slave {s} vendor 0x{man:08X} not the ESI vendor 0x{esi_vendor:08X}")
            elif not any(dv["product"] == pid and dv["rev"] == rev for dv in esi_devs):
                revs = ", ".join(f"0x{dv['rev']:08X}" for dv in esi_devs if dv["product"] == pid)
                bad.append(f"slave {s} product/revision 0x{pid:08X}/0x{rev:08X} not in the ESI (ESI revisions: {revs or 'none'})")
    if nslaves != a.n:
        bad.insert(0, f"{nslaves} slave(s) on the bus, expected {a.n}")
    if bad:
        for b in bad:
            print(f"- **{b}**")
        print("\nRevision/identity differs: the ENI must be regenerated (TwinCAT, ESI of the real revision; "
              "plan Phase 9 section 6.1) before R-05 can pass.")
    verdict("R-02", "FAIL" if bad else "PASS",
            "; ".join(bad) if bad else f"{nslaves} slaves, identity = ENI" + (" = ESI" if a.esi else ""))

    # ---------------------------------------------------------- R-04
    print("\n## R-04 objects read by SDO in PRE-OP (read only)")
    r04_bad, r04_diff = [], []
    for s in sorted(slaves):
        print(f"\n### slave {s}\n")
        print("| object | value |")
        print("|---|---|")
        rows = [
            ("0x1000 device type", sshow(sdo, s, 0x1000)),
            ("0x1008 name", sshow(sdo, s, 0x1008)),
            ("0x1009 hw version", sshow(sdo, s, 0x1009, 0, ascii4)),
            ("0x100A sw version", sshow(sdo, s, 0x100A, 0, ascii4)),
            ("0x1C12 Rx assign", " ".join(sshow(sdo, s, 0x1C12, k, lambda v: f"0x{v:04X}") for k in range(1, (sval(sdo, s, 0x1C12) or 0) + 1)) or sshow(sdo, s, 0x1C12)),
            ("0x1C13 Tx assign", " ".join(sshow(sdo, s, 0x1C13, k, lambda v: f"0x{v:04X}") for k in range(1, (sval(sdo, s, 0x1C13) or 0) + 1)) or sshow(sdo, s, 0x1C13)),
            ("0x6502 supported modes", sshow(sdo, s, 0x6502, 0, lambda v: f"0x{v:08X} = {modes_str(v)}")),
            ("0x6060 mode (PRE-OP, before InitCmd)", sshow(sdo, s, 0x6060, 0, lambda v: f"{signed(v, 8)} {MODE_NUM.get(signed(v, 8), '?')}")),
            ("0x6061 mode display", sshow(sdo, s, 0x6061, 0, lambda v: f"{signed(v, 8)} {MODE_NUM.get(signed(v, 8), '?')}")),
            ("0x6041 statusword", sshow(sdo, s, 0x6041, 0, lambda v: f"0x{v:04X} {sw_state(v)} [{sw_bits(v)}]")),
            ("0x603F error code", sshow(sdo, s, 0x603F, 0, lambda v: f"0x{v:04X}")),
            ("0x6064 position actual", sshow(sdo, s, 0x6064, 0, lambda v: str(signed(v, 32)))),
            ("0x6098 homing method", sshow(sdo, s, 0x6098, 0, lambda v: str(signed(v, 8)))),
            ("0x6099:01/02 homing speeds", sshow(sdo, s, 0x6099, 1) + " / " + sshow(sdo, s, 0x6099, 2)),
            ("0x6072 max torque (0.1 %)", sshow(sdo, s, 0x6072)),
            ("0x607F max profile velocity", sshow(sdo, s, 0x607F)),
            ("0x6080 max motor speed", sshow(sdo, s, 0x6080)),
            ("0x6065 following error window", sshow(sdo, s, 0x6065)),
            ("0x6066 following error timeout", sshow(sdo, s, 0x6066)),
            ("0x607D:01/02 software limits", sshow(sdo, s, 0x607D, 1, lambda v: str(signed(v, 32))) + " / " + sshow(sdo, s, 0x607D, 2, lambda v: str(signed(v, 32)))),
            ("0x6091:01/02 gear ratio", sshow(sdo, s, 0x6091, 1) + " / " + sshow(sdo, s, 0x6091, 2)),
            ("0x608F:01/02 encoder increments / rev", sshow(sdo, s, 0x608F, 1) + " / " + sshow(sdo, s, 0x608F, 2)),
            ("0x605A quick stop option", sshow(sdo, s, 0x605A)),
            ("0x605B/0x605C shutdown / disable op option", sshow(sdo, s, 0x605B) + " / " + sshow(sdo, s, 0x605C)),
            ("0x605E fault reaction option", sshow(sdo, s, 0x605E)),
            ("0x6007 abort connection option", sshow(sdo, s, 0x6007)),
            ("0x10F1:02 sync error count limit", sshow(sdo, s, 0x10F1, 2)),
        ]
        for sm, name in ((0x1C32, "0x1C32 SM2 out"), (0x1C33, "0x1C33 SM3 in")):
            rows.append((f"{name} :01 sync type", sshow(sdo, s, sm, 1, lambda v: f"{v} {SYNC.get(v, '?')}")))
            rows.append((f"{name} :02 cycle time ns", sshow(sdo, s, sm, 2)))
            rows.append((f"{name} :04 sync types supported", sshow(sdo, s, sm, 4, lambda v: f"0x{v:04X}")))
            rows.append((f"{name} :05 min cycle time ns", sshow(sdo, s, sm, 5)))
            rows.append((f"{name} :06 calc+copy time ns", sshow(sdo, s, sm, 6)))
            rows.append((f"{name} :09 delay time ns", sshow(sdo, s, sm, 9)))
        for k, v in rows:
            print(f"| {k} | {v} |")

        m6502 = sval(sdo, s, 0x6502)
        if m6502 is None:
            r04_bad.append(f"slave {s}: 0x6502 not readable ({sshow(sdo, s, 0x6502)})")
        elif not m6502 >> 7 & 1:
            r04_bad.append(f"slave {s}: 0x6502 0x{m6502:08X} without CSP")
        rx = sval(sdo, s, 0x1C12, 1)
        tx = sval(sdo, s, 0x1C13, 1)
        if (rx, tx) != (0x1701, 0x1B01):
            r04_diff.append(f"slave {s}: PDO assignment in PRE-OP {sshow(sdo, s, 0x1C12, 1)}/{sshow(sdo, s, 0x1C13, 1)} "
                            "(ESI default 0x1701/0x1B01; the ENI InitCmds set it again)")

        print(f"\nPDO mapping of slave {s}" + (" vs ESI" if a.esi else "") + ":\n")
        print("| PDO | read from the drive | ESI | |")
        print("|---|---|---|---|")
        dev = None
        if esi_devs:
            pid, rev = int(slaves[s]["id"], 16), int(slaves[s]["rev"], 16)
            dev = next((d for d in esi_devs if d["product"] == pid and d["rev"] == rev), None) or \
                next((d for d in esi_devs if d["product"] == pid), None)
        for idx in (0x1600, 0x1701, 0x1702, 0x1703, 0x1704, 0x1705, 0x1A00, 0x1B01, 0x1B02, 0x1B03, 0x1B04):
            got = pdo_read(sdo, s, idx)
            g = ents_str(got) if got is not None else sshow(sdo, s, idx, 0)
            if dev is None:
                print(f"| 0x{idx:04X} | {g} | - | |")
                continue
            e = dev["pdos"].get(idx)
            es = ents_str(e["entries"]) if e else "(not in the ESI)"
            same = got is not None and e is not None and [tuple(x) for x in e["entries"]] == got
            mark = "=" if same else ("**differs**" if got is not None and e is not None else "")
            if got is not None and e is not None and not same:
                r04_diff.append(f"slave {s}: 0x{idx:04X} drive {ents_str(got)} vs ESI {es}")
            print(f"| 0x{idx:04X} | {g} | {es} | {mark} |")

    if r04_diff:
        print("\nDifferences to record in claude/is620n_esi_review.md:\n")
        for d in r04_diff:
            print(f"- {d}")
    if r04_bad:
        verdict("R-04", "FAIL", "; ".join(r04_bad))
    elif r04_diff:
        verdict("R-04", "DIFF", f"{len(r04_diff)} difference(s) to record (see the list)")
    else:
        verdict("R-04", "PASS", "0x6502 has CSP on every slave, mapping = ESI" if a.esi else "0x6502 has CSP on every slave")



def nic_delta(before, after):
    """Error counters of `ethtool -S` that grew between two snapshots."""
    def load(p):
        d = {}
        for ln in open(p, errors="replace"):
            if ":" in ln:
                k, v = ln.split(":", 1)
                try:
                    d[k.strip()] = int(v.strip())
                except ValueError:
                    pass
        return d
    b, a_ = load(before), load(after)
    return {k: a_[k] - b[k] for k in sorted(a_) if k in b and a_[k] != b[k]
            and re.search(r"err|crc|missed|align|symbol|fifo|over|drop", k)}


def grade_run(rid, path, title, cycle_note=None, nic=None):
    """R-05 / R-09: one ecm_run in OP without hook. Returns the log text."""
    t = open(path, errors="replace").read()
    print(f"\n## {rid} {title}\n")
    if cycle_note:
        print(f"- {cycle_note}")
    bad = []
    checks = [
        ("OP reached", "all slaves in OPERATIONAL" in t),
        ("ENI identity check", "identity check OK" in t),
        ("PDO table ENI == bus", "PDO table: ENI == bus" in t),
        ("DC LOCKED, 0 unlock", re.search(r"\[DC\] state=LOCKED locks=\d+ unlocks=0", t) is not None),
    ]
    m = re.search(r"(\d+) CoE InitCmd\(s\)", t)
    n_init = int(m.group(1)) if m else -1
    n_ok = len(re.findall(r"CoE download 0x[0-9A-F]{4}:[0-9A-F]{2} ok", t))
    checks.append((f"CoE InitCmds ok {n_ok}/{n_init}", n_init > 0 and n_ok == n_init))
    m = re.search(r"\[GROUP_MOTION\] cycles=(\d+) wkc_mismatch=(\d+) .*overrun=(\d+)", t)
    if m:
        checks.append((f"cycles {m.group(1)}, WKC mismatch {m.group(2)}, overrun {m.group(3)}",
                       m.group(2) == "0" and m.group(3) == "0"))
    else:
        checks.append(("final stats printed", False))
    for name, ok in checks:
        print(f"- [{'x' if ok else ' '}] {name}")
        if not ok:
            bad.append(name)
    m = re.search(r"state transitions: (.*)", t)
    print(f"- state transitions (ESI allows 3000/9000/9000 ms): {m.group(1) if m else '(not printed)'}")
    for pat, label in ((r"SM watchdog configured explicitly .*?motion=(\d+)ms", "SM watchdog motion {} ms"),
                       (r"\[DC\] \|e\| \(after 5 s\) (.*)", "DC |e| {}")):
        m = re.search(pat, t)
        if m:
            print("- " + label.format(m.group(1)))
    for mm in re.finditer(r"dc slave (\d+) hasdc=(\d) pdelay=(\d+) ns", t):
        print(f"- DC slave {mm.group(1)}: port delay {mm.group(3)} ns")
    m = re.search(r"\[WKC GROUP_MOTION\] (.*)", t)
    if m:
        print(f"- WKC {m.group(1)}")
    tl = re.findall(r"^turnaround +(n=.*)$", t, re.M)
    if tl:
        print(f"- turnaround (software timestamps, send -> reply) {re.sub(' +', ' ', tl[-1])}")
    m = re.search(r"\[LATE\] index quarantine \S+: parked=(\d+)", t)
    if m:
        print(f"- replies that came back after the receive deadline (parked): {m.group(1)}")
    m = re.search(r"ecm_run: (WARNING /dev/cpu_dma_latency.*?\)|/dev/cpu_dma_latency = 0 held|--no-dma-latency)", t)
    print(f"- CPU idle: {m.group(1) if m else '(no cpu_dma_latency line: ecm_run before patch 0017)'}")
    enabled = False
    for mm in re.finditer(r"\[PDO\] get (\S+) = 0x([0-9A-F]+)", t):
        name, v = mm.group(1), int(mm.group(2), 16)
        extra = ""
        if ":0x6041:" in name:
            extra = f" {sw_state(v)} [{sw_bits(v)}]"
            enabled |= sw_state(v) == "Operation enabled"
        elif ":0x6064:" in name:
            extra = f" ({signed(v, 32)})"
        print(f"- {name} = 0x{v:X}{extra}")
    for mm in re.finditer(r".*(\[EMCY\]|\[AXIS\]|\[RECOVERY\]|AL status|ALstatus).*", t):
        print(f"- `{mm.group(0).strip()[:200]}`")
    if nic and all(os.path.exists(x) for x in nic):
        d = nic_delta(*nic)
        print(f"- NIC error counters during the run: {', '.join(f'{k} +{v}' for k, v in d.items()) or 'none'}")
        if d:
            print("  (counted on the i226 itself; a frame lost to CRC would also show as a WKC error above)")
    if enabled:
        bad.append("A DRIVE REPORTS OPERATION ENABLED although the master never enabled it")
    verdict(rid, "FAIL" if bad else "PASS",
            ("not met: " + "; ".join(bad)) if bad else "OP, ENI == bus, InitCmds ok, DC LOCKED, 0 WKC error, 0 overrun")
    return t


def run_part(a):
    # ---------------------------------------------------------- R-05 / R-06
    if a.er5 and os.path.exists(a.er5):
        d = os.path.dirname(a.er5)
        t = grade_run("R-05", a.er5, "ecm_run --eni --pdo-scan, no hook (axes not driven)",
                      nic=(os.path.join(d, "ethtool_S_r05_before.txt"), os.path.join(d, "ethtool_S_r05_after.txt")))
        if "PDO table: ENI == bus" in t:
            verdict("R-06", "PASS", "the drive's PDO assignment matches the ENI (--pdo-scan: ENI == bus)")
        elif "PDO table of the ENI differs from the bus" in t:
            m = re.search(r"PDO table of the ENI differs from the bus.*\n((?:.*\n){0,6})", t)
            verdict("R-06", "DIFF", "--pdo-scan refused, ENI != bus (N-03 on real hardware): "
                    + (m.group(1).replace("\n", " | ")[:300] if m else ""))
        else:
            verdict("R-06", "SKIP", "no PDO comparison: ecm_run stopped before it (see R-05)")
    for spec in a.er9 or []:
        cyc, path = spec.split(":", 1)
        if os.path.exists(path):
            d = os.path.dirname(path)
            grade_run(f"R-09/{cyc}us", path, f"cycle {cyc} us (derived ENI, tools/eni/eni_cycle.py)",
                      nic=(os.path.join(d, f"ethtool_S_r09_{cyc}_before.txt"),
                           os.path.join(d, f"ethtool_S_r09_{cyc}_after.txt")))
    if a.er6 and os.path.exists(a.er6):
        t = open(a.er6, errors="replace").read()
        print("\n## R-06b axes configured, not driven (--axis without --hook)\n")
        for mm in re.finditer(r"ecm_run: axis .*", t):
            print(f"- `{mm.group(0)[:220]}`")
        ok = re.search(rf"{a.n} CiA402 axis/axes configured \(no --hook cia402: not driven\)", t) and \
            "all slaves in OPERATIONAL" in t
        verdict("R-06b", "PASS" if ok else "FAIL",
                "axes bound, 0x6502 read by the master, not driven" if ok else "see er_r06.log")


if __name__ == "__main__":
    sys.exit(main())
