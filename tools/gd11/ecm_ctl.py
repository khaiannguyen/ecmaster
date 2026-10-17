#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
"""
ecm_ctl.py -- Phase 11.2: command-line client of ecm_run's ctl.sock (docs/api.md §10).

    ecm_ctl.py [--sock PATH] info [--pdo]
    ecm_ctl.py [--sock PATH] read  SLAVE INDEX SUB [TYPE]        TYPE: u8 i8 u16 i16 u32 i32 u64 i64 f32 raw
    ecm_ctl.py [--sock PATH] write SLAVE INDEX SUB TYPE VALUE    (allowlist only, see --api-allow)
    ecm_ctl.py [--sock PATH] dump
    ecm_ctl.py [--sock PATH] raw '{"cmd":"info"}' ['{...}' ...] several requests on one connection
    ecm_ctl.py [--sock PATH] spam SECONDS HZ SLAVE INDEX SUB TYPE   sdo_read at HZ; prints a summary line
    ecm_ctl.py [--sock PATH] hold SECONDS                       connect, send nothing, stay connected

Default socket: /run/ecmaster/ctl.sock. Prints the reply line(s); exit 0 when
every reply has "ok": true, 1 otherwise, 2 when the socket cannot be used.
"""
import json
import socket
import sys
import time

SOCK = "/run/ecmaster/ctl.sock"


class Ctl:
    def __init__(self, path, timeout=5.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(path)
        self.buf = b""
        self.n = 0

    def line(self):
        while b"\n" not in self.buf:
            try:
                d = self.s.recv(65536)
            except ConnectionResetError:
                d = b""
            if not d:
                raise ConnectionError("ctl.sock closed the connection")
            self.buf += d
        l, _, self.buf = self.buf.partition(b"\n")
        return l.decode()

    def send(self, obj):
        self.n += 1
        if isinstance(obj, dict) and "id" not in obj:
            obj = dict(obj, id=self.n)
        txt = obj if isinstance(obj, str) else json.dumps(obj, separators=(",", ":"))
        self.s.sendall(txt.encode() + b"\n")

    def call(self, obj):
        try:
            self.send(obj)
        except (BrokenPipeError, ConnectionResetError):
            pass            # refused at once (too_many_clients): its reply may still be readable
        return json.loads(self.line())


def num(s):
    s = s.strip()
    return s if s.lower().startswith(("0x", "-0x")) else (float(s) if any(c in s for c in ".eE") else int(s))


def out(r):
    print(json.dumps(r, separators=(",", ":")))
    return r.get("ok") is True


def main(a):
    path = SOCK
    if len(a) >= 2 and a[0] == "--sock":
        path, a = a[1], a[2:]
    if not a:
        print(__doc__)
        return 2
    try:
        c = Ctl(path)
    except OSError as e:
        print("ecm_ctl: %s: %s" % (path, e), file=sys.stderr)
        return 2
    cmd, args = a[0], a[1:]
    try:
        if cmd == "info":
            r = c.call({"cmd": "info"})
            if r.get("ok") and "--pdo" not in args:
                r["info"]["pdo"]["entries"] = "%d entries (--pdo to list)" % len(r["info"]["pdo"]["entries"])
            return 0 if out(r) else 1
        if cmd == "read" and len(args) in (3, 4):
            q = {"cmd": "sdo_read", "slave": int(args[0], 0), "index": args[1], "sub": int(args[2], 0)}
            if len(args) == 4:
                q["type"] = args[3]
            return 0 if out(c.call(q)) else 1
        if cmd == "write" and len(args) == 5:
            q = {"cmd": "sdo_write", "slave": int(args[0], 0), "index": args[1], "sub": int(args[2], 0),
                 "type": args[3], "value": num(args[4])}
            return 0 if out(c.call(q)) else 1
        if cmd == "dump":
            return 0 if out(c.call({"cmd": "diag_dump"})) else 1
        if cmd == "raw" and args:
            for t in args:
                c.send(t)
            ok = True
            for _ in args:
                ok = out(json.loads(c.line())) and ok
            return 0 if ok else 1
        if cmd == "spam" and len(args) == 6:
            dur, hz = float(args[0]), float(args[1])
            q = {"cmd": "sdo_read", "slave": int(args[2], 0), "index": args[3], "sub": int(args[4], 0), "type": args[5]}
            t_end = time.monotonic() + dur
            nxt = time.monotonic()
            res, lat = {}, []
            while time.monotonic() < t_end:
                t0 = time.monotonic()
                r = c.call(q)
                lat.append(time.monotonic() - t0)
                k = "ok" if r.get("ok") else r.get("error", "?")
                res[k] = res.get(k, 0) + 1
                nxt += 1.0 / hz
                time.sleep(max(0.0, nxt - time.monotonic()))
            lat.sort()
            print("spam: sent=%d %s latency_ms p50=%.1f max=%.1f" % (
                len(lat), " ".join("%s=%d" % kv for kv in sorted(res.items())),
                1e3 * lat[len(lat) // 2] if lat else 0, 1e3 * lat[-1] if lat else 0))
            return 0 if set(res) <= {"ok"} else 1
        if cmd == "hold" and len(args) == 1:
            time.sleep(float(args[0]))
            return 0
    except (OSError, ValueError, ConnectionError) as e:
        print("ecm_ctl: %s" % e, file=sys.stderr)
        return 2
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
