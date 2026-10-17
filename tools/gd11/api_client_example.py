#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
"""api_client_example.py -- Phase 11.6 (D-13): a sample client of stats.json
written the way docs/api.md §10 asks, so that it keeps working when the file
grows (v1.1: a second port for cable redundancy, bus.path "ring_open", new
keys, new finding types).

  api_client_example.py [FILE]            default /run/ecmaster/stats.json
  api_client_example.py --naive [FILE]    negative control: the shortcuts a
                                          v1.0-only client takes (one port,
                                          known finding types only)

Rules followed (forward compatible):
  - unknown keys are ignored, never an error;
  - lists (ports, slaves, findings) are iterated, never indexed by a fixed size;
  - an unknown string value (path, finding type) is shown as it is;
  - a missing optional key / null counter is shown as "-".
Exit 0 on a file it could read, 1 otherwise.
"""
import json
import sys


def g(d, *keys, default="-"):
    for k in keys:
        if not isinstance(d, dict) or k not in d or d[k] is None:
            return default
        d = d[k]
    return d


def main(argv):
    naive = "--naive" in argv
    args = [a for a in argv if a != "--naive"]
    path = args[0] if args else "/run/ecmaster/stats.json"
    try:
        d = json.load(open(path))
    except (OSError, ValueError) as e:
        print("cannot read %s: %s" % (path, e))
        return 1
    bus = d.get("bus", {})
    ports = bus.get("ports", [])
    if naive:
        print("naive client (assumes one port, v1.0 finding types)")
        p = ports[0] if ports else {}
        print("ports: 1")
        print("  %s %s link %s noframe %s" % (g(p, "name"), g(p, "iface"), g(p, "link"), g(p, "noframe")))
        known = {"lost_link", "state", "pdi", "pu_only", "saturated", "diag_lost", "emcy", "emcy_lost", "wkc"}
        for f in d.get("findings", []):
            if f.get("type") in known:
                print("finding %s: %s" % (f["type"], f.get("text", "")))
        return 0
    print("bus fsm %s link %s path %s" % (g(bus, "fsm"), g(bus, "link"), g(bus, "path")))
    print("ports: %d" % len(ports))
    for p in ports:
        print("  %s %s link %s tx %s noframe %s crc %s" % (g(p, "name"), g(p, "iface"), g(p, "link"),
              g(p, "tx"), g(p, "noframe"), g(p, "crc_err")))
    for s in d.get("slaves", []):
        print("slave %s %s emcy %s" % (g(s, "pos"), g(s, "name"), g(s, "emcy_count")))
    for f in d.get("findings", []):
        print("finding %s [%s]: %s" % (g(f, "type"), g(f, "sev"), g(f, "text")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
