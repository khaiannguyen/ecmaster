#!/usr/bin/env python3
"""
stats_check.py -- Phase 11.1: checks of stats.json (docs/api.md).

    stats_check.py schema FILE               schema v1 as ecm_run writes it (strict on
                                             what ecm_run produces; extra keys allowed)
    stats_check.py selftest FILE             negative control of `schema`: every
                                             mutation of a valid FILE MUST be rejected,
                                             and an added unknown key MUST be accepted
    stats_check.py compare FILE LOG          D-10: the final stats.json against the
                                             end-of-run report of ecm_run (LOG)
    stats_check.py watch FILE SECONDS        read FILE as fast as possible: parse
                                             failures, publish rate, largest gap

Exit 0 = pass, 1 = fail, 2 = usage / cannot read.
"""
import copy
import json
import re
import sys
import time

BUS = {"STARTING", "RUN", "DEGRADED", "LOST", "RECOVER"}
PATHS = {"single", "ring_closed", "ring_open"}
LINKS = {"up", "down", "unknown"}
AL = {"INIT", "PREOP", "BOOT", "SAFEOP", "OP", "UNKNOWN"}
POLICY = {"none", "ack", "op", "reconfig"}
RECOVER = {"ok", "retrying", "failed", "failed_config"}
SEV = {"info", "warn", "error"}
DC = {"UNANCHORED", "ACQUIRE", "LOCKED"}
DS = {"UNKNOWN", "NOT_READY_TO_SWITCH_ON", "SWITCH_ON_DISABLED", "READY_TO_SWITCH_ON", "SWITCHED_ON",
      "OPERATION_ENABLED", "QUICK_STOP_ACTIVE", "FAULT_REACTION_ACTIVE", "FAULT"}
AXERR = {"none", "timeout", "fault", "bus_lost", "mode", "in_fault", "dropped", "step", "shutdown",
         "handshake", "homing", "config", "slave_failed"}
ESRC = {"603F", "EMCY", "none"}
HEX = re.compile(r"^0x[0-9A-F]+$")


class Bad(Exception):
    pass


def need(c, msg):
    if not c:
        raise Bad(msg)


def isint(v):
    return isinstance(v, int) and not isinstance(v, bool)


def obj(d, k, where):
    need(isinstance(d.get(k), dict), "%s.%s: object expected" % (where, k))
    return d[k]


def lst(d, k, where):
    need(isinstance(d.get(k), list), "%s.%s: list expected" % (where, k))
    return d[k]


def num(d, k, where, opt=False, null=False):
    if opt and k not in d:
        return None
    v = d.get(k)
    if null and v is None:
        return None
    need(isint(v) and v >= 0 if k not in ("min", "max", "err_ns", "pos", "vel", "pid") else isint(v),
         "%s.%s: integer expected, got %r" % (where, k, v))
    return v


def enum(d, k, values, where, opt=False):
    if opt and k not in d:
        return None
    need(d.get(k) in values, "%s.%s: %r not in %s" % (where, k, d.get(k), sorted(values)))
    return d[k]


def hexs(d, k, where, opt=False):
    if opt and k not in d:
        return
    need(isinstance(d.get(k), str) and HEX.match(d[k]), "%s.%s: hex string expected, got %r" % (where, k, d.get(k)))


def check_hist(h, where, buckets):
    if h is None:
        return
    need(isinstance(h, dict), where + ": object or null")
    b = enum(h, "buckets", set(buckets), where)
    n = buckets[b]["n"]
    c = lst(h, "counts", where)
    need(len(c) <= n, "%s.counts: %d > %d bins" % (where, len(c), n))
    need(all(isint(x) and x >= 0 for x in c), where + ".counts: integers")
    need(not c or c[-1] != 0, where + ".counts: trailing zeros not trimmed")
    cnt = num(h, "count", where)
    need(sum(c) == cnt, "%s: sum(counts) %d != count %d" % (where, sum(c), cnt))
    need(h.get("unit") == buckets[b]["unit"], where + ".unit does not match the bucket kind")


def schema(d):
    w = "stats"
    need(d.get("schema") == 1, "schema != 1")
    for k in ("pub_seq", "mono_ns", "pid"):
        num(d, k, w)
    need(isinstance(d.get("final"), bool), "final: bool expected")
    bk = obj(d, "buckets", w)
    lg = obj(bk, "log20_100ns_v1", "buckets")
    up = lst(lg, "upper", "buckets.log20_100ns_v1")
    need(len(up) == 120 and all(up[i] < up[i + 1] for i in range(119)), "log20 upper: 120 increasing bounds")
    need(lg.get("n") == 121 and lg.get("unit") == "ns", "log20: n 121, unit ns")
    li = obj(bk, "lin1us_v1", "buckets")
    need(li.get("n") == 1001 and li.get("width") == 1 and li.get("unit") == "us", "lin1us: n 1001 width 1 unit us")

    bus = obj(d, "bus", w)
    enum(bus, "fsm", BUS, "bus")
    num(bus, "cycle_ns", "bus")
    enum(bus, "link", {"af_packet", "etf"}, "bus")
    ns = num(bus, "n_slaves", "bus")
    ports = lst(bus, "ports", "bus")                      # §1.6: a list, never bus.iface
    need(len(ports) >= 1, "bus.ports: at least one port")
    need("iface" not in bus, "bus.iface must not exist (ports[] instead)")
    for i, p in enumerate(ports):
        pw = "bus.ports[%d]" % i
        need(isinstance(p.get("name"), str) and isinstance(p.get("iface"), str), pw + ": name, iface")
        enum(p, "link", LINKS, pw)
        need("rx" not in p, pw + ".rx removed in v1")
        for k in ("tx", "noframe"):
            num(p, k, pw, opt=True)
        for k in ("crc_err", "rx_missed", "carrier_changes"):
            need(k in p, "%s.%s missing" % (pw, k))
            num(p, k, pw, null=True)
    enum(bus, "path", PATHS, "bus")

    for i, g in enumerate(lst(d, "groups", w)):
        gw = "groups[%d]" % i
        need(isinstance(g.get("name"), str), gw + ".name")
        need(all(isint(s) and 1 <= s <= ns for s in lst(g, "slaves", gw)), gw + ".slaves: 1..n_slaves")
        num(g, "cycle_ns", gw)
        if "cycles" in g:
            ks = ("cycles", "ok", "noframe", "zero", "partial", "over", "max_run_bad", "overrun")
            for k in ks:
                num(g, k, gw)
            tot = g["ok"] + g["noframe"] + g["zero"] + g["partial"] + g["over"]
            need(tot == g["cycles"], "%s: ok+noframe+zero+partial+over %d != cycles %d" % (gw, tot, g["cycles"]))

    h = obj(d, "hist", w)
    for k in ("wake_jitter", "prep_send", "turnaround", "occupancy", "dc_err", "hook"):
        need(k in h, "hist.%s missing (null when not available)" % k)
        check_hist(h[k], "hist." + k, bk)
    if h["dc_err"] is not None:
        need(h["dc_err"]["buckets"] == "lin1us_v1", "hist.dc_err: lin1us_v1")
    for k in ("wake_jitter", "prep_send", "turnaround", "occupancy", "hook"):
        if h[k] is not None:
            need(h[k]["buckets"] == "log20_100ns_v1", "hist.%s: log20_100ns_v1" % k)
            for f in ("sum", "min", "max"):
                num(h[k], f, "hist." + k)

    dc = d.get("dc", "missing")
    need(dc is None or isinstance(dc, dict), "dc: object or null")
    if dc:
        enum(dc, "state", DC, "dc")
        for k in ("ref_pos", "locks", "unlocks", "clamps", "wraps", "stale", "err_ns"):
            num(dc, k, "dc")

    ln = obj(d, "link", w)
    for k in ("etf_missed", "etf_invalid"):
        num(ln, k, "link")
    for k in ("txtime_late", "txtime_bypass", "send_err", "tai_steps"):
        num(ln, k, "link", opt=True)

    sl = lst(d, "slaves", w)
    need(len(sl) == ns, "slaves: %d entries for n_slaves %d" % (len(sl), ns))
    for i, s in enumerate(sl):
        sw = "slaves[%d]" % i
        need(s.get("pos") == i + 1, sw + ".pos")
        for k in ("vendor", "product", "revision"):
            hexs(s, k, sw)
        need(isinstance(s.get("name"), str), sw + ".name")
        if "policy" in s:                                  # monitor has published
            enum(s, "policy", POLICY, sw)
            enum(s, "recover", RECOVER, sw)
            for k in ("attempts", "recoveries", "initcmd_failed", "emcy_count"):
                num(s, k, sw)
            em = lst(s, "emcy", sw)
            need(len(em) <= 4, sw + ".emcy: at most 4")
            for j, e in enumerate(em):
                for k in ("code", "reg"):
                    hexs(e, k, "%s.emcy[%d]" % (sw, j))
                need(isinstance(e.get("data"), str) and re.match(r"^[0-9A-F]{10}$", e["data"]), sw + ".emcy data")
        if "answered" in s:
            for k in ("inv", "rx_err", "fwd_err", "lost_link"):
                a = lst(s, k, sw)
                need(len(a) == 4 and all(isint(x) and x >= 0 for x in a), "%s.%s: 4 ports" % (sw, k))
            num(s, "pu_err", sw)
            num(s, "pdi_err", sw)
        if "al" in s:
            enum(s, "al", AL, sw)
            need(isinstance(s.get("al_err"), bool), sw + ".al_err")
            for k in ("station", "al_code", "dl_status"):
                hexs(s, k, sw)

    for i, a in enumerate(lst(d, "axes", w)):
        aw = "axes[%d]" % i
        need(isinstance(a.get("name"), str), aw + ".name")
        need(isint(a.get("slave")) and 1 <= a["slave"] <= ns, aw + ".slave")
        num(a, "n", aw)
        need("tracking_max" not in a, aw + ".tracking_max removed in v1")
        if "state" in a:
            enum(a, "state", DS, aw)
            enum(a, "err", AXERR, aw)
            enum(a, "error_src", ESRC, aw)
            for k in ("sw", "cw", "error_code"):
                hexs(a, k, aw)
            need(isinstance(a.get("error_text"), str), aw + ".error_text")
            for k in ("mode", "mode_display", "pos", "vel"):
                need(isint(a.get(k)), "%s.%s" % (aw, k))
            for k in ("tick", "used", "late", "underruns", "dropped", "step_refused", "pp_acked"):
                num(a, k, aw)
            need(isinstance(a.get("homed"), bool), aw + ".homed")
            need((a["error_src"] == "none") == (a["error_code"] == "0x0000"), aw + ": error_src vs error_code")

    rt = obj(d, "rt", w)
    need(isinstance(rt.get("dma_latency_held"), bool), "rt.dma_latency_held")
    need(rt.get("hook") is None or isinstance(rt["hook"], str), "rt.hook")
    need(isinstance(rt.get("api_cpus"), str), "rt.api_cpus")

    for i, f in enumerate(lst(d, "findings", w)):
        fw = "findings[%d]" % i
        need(isinstance(f.get("type"), str) and f["type"], fw + ".type")
        enum(f, "sev", SEV, fw)
        need(isinstance(f.get("text"), str), fw + ".text")

    src = obj(d, "sources", w)
    for k in ("rt", "telemetry", "monitor"):
        need(k in src and (src[k] is None or isinstance(src[k], dict)), "sources.%s: object or null" % k)
        if src[k]:
            num(src[k], "seq", "sources." + k)
            num(src[k], "mono_ns", "sources." + k)
    num(src, "handoff_drops", "sources")
    # parts and their sources agree
    need((src["rt"] is None) == (bus["fsm"] == "STARTING"), "fsm STARTING iff no RT data yet")
    need((src["telemetry"] is None) == (h["wake_jitter"] is None), "telemetry source vs histograms")


def load(path):
    with open(path) as f:
        return json.load(f)


def cmd_schema(path):
    try:
        schema(load(path))
    except Bad as e:
        print("schema FAIL %s: %s" % (path, e))
        return 1
    print("schema ok: %s" % path)
    return 0


def mutations(d):
    """(name, mutated copy): each one MUST be rejected"""
    def m(name, fn):
        c = copy.deepcopy(d)
        fn(c)
        return name, c
    return [
        m("schema 2", lambda c: c.__setitem__("schema", 2)),
        m("ports as an object (one-port assumption)", lambda c: c["bus"].__setitem__("ports", c["bus"]["ports"][0])),
        m("bus.iface instead of ports", lambda c: c["bus"].__setitem__("iface", "enP1p1s0")),
        m("path missing", lambda c: c["bus"].pop("path")),
        m("ports[].rx back", lambda c: c["bus"]["ports"][0].__setitem__("rx", 1)),
        m("histogram count off by one", lambda c: c["hist"]["wake_jitter"].__setitem__("count", c["hist"]["wake_jitter"]["count"] + 1)),
        m("dc_err as log buckets", lambda c: c["hist"]["dc_err"].__setitem__("buckets", "log20_100ns_v1")),
        m("trailing zero in counts", lambda c: c["hist"]["occupancy"]["counts"].append(0)),
        m("group counts do not add up", lambda c: c["groups"][0].__setitem__("ok", c["groups"][0]["ok"] + 1)),
        m("recover value unknown to v1", lambda c: c["slaves"][0].__setitem__("recover", "maybe")),
        m("slave port array of 2", lambda c: c["slaves"][0].__setitem__("rx_err", [0, 0])),
        m("axis tracking_max back", lambda c: c["axes"][0].__setitem__("tracking_max", 5)),
        m("axis state as text", lambda c: c["axes"][0].__setitem__("state", "Operation enabled")),
        m("sw as a number", lambda c: c["axes"][0].__setitem__("sw", 5687)),
        m("final as a string", lambda c: c.__setitem__("final", "yes")),
        m("hist.hook missing", lambda c: c["hist"].pop("hook")),
    ]


def cmd_selftest(path):
    d = load(path)
    try:
        schema(d)
    except Bad as e:
        print("selftest: the base document is not valid: %s" % e)
        return 1
    bad = 0
    for name, c in mutations(d):
        try:
            schema(c)
            print("  [FAIL] not rejected: %s" % name)
            bad += 1
        except Bad as e:
            print("  [PASS] rejected: %s (%s)" % (name, e))
        except (KeyError, IndexError, TypeError) as e:
            print("  [FAIL] checker crashed on '%s': %r" % (name, e))
            bad += 1
    c = copy.deepcopy(d)
    c["future_key"] = {"x": 1}
    c["bus"]["ports"][0]["future"] = 1
    try:
        schema(c)
        print("  [PASS] unknown keys accepted (forward compatible)")
    except Bad as e:
        print("  [FAIL] unknown key rejected: %s" % e)
        bad += 1
    print("selftest: %s" % ("ok" if not bad else "%d FAIL" % bad))
    return 1 if bad else 0


def last(rx, text):
    m = None
    for m in re.finditer(rx, text, re.M):
        pass
    return m


def cmd_compare(path, logpath):
    d = load(path)
    log = open(logpath, errors="replace").read()
    res = []

    def eq(name, got, want):
        res.append((got == want, "%s: stats.json %s, report %s" % (name, got, want)))

    eq("final", d["final"], True)
    gnames = {"motion": "GROUP_MOTION", "io": "GROUP_IO"}
    for g in d["groups"]:
        lab = gnames.get(g["name"], g["name"])
        m = last(r"^  \[%s\] cycles=(\d+) wkc_mismatch=(\d+) \S+ overrun=(\d+)" % lab, log)
        if not m:
            res.append((False, "%s: no final stats line in the report" % lab))
            continue
        eq("%s cycles" % lab, g.get("cycles"), int(m.group(1)))
        eq("%s mismatch (cycles - ok)" % lab, g.get("cycles", 0) - g.get("ok", 0), int(m.group(2)))
        eq("%s overrun" % lab, g.get("overrun"), int(m.group(3)))
        m = last(r"^  \[WKC %s\] ok=(\d+) noframe=(\d+) zero=(\d+) partial=(\d+) over=(\d+) max_run_bad=(\d+)" % lab, log)
        if m:
            for k, v in zip(("ok", "noframe", "zero", "partial", "over", "max_run_bad"), m.groups()):
                eq("%s %s" % (lab, k), g.get(k), int(v))
    m = last(r"^  \[DC\] state=(\w+) locks=(\d+) unlocks=(\d+) clamps=(\d+) stale=(\d+) wraps=(\d+)", log)
    if m:
        dc = d["dc"] or {}
        eq("DC state", dc.get("state"), m.group(1))
        for k, v in zip(("locks", "unlocks", "clamps", "stale", "wraps"), m.groups()[1:]):
            eq("DC " + k, dc.get(k), int(v))
    else:
        eq("DC disabled", d["dc"], None)
    m = last(r"^  \[HOOK\] '(\w+)' calls=(\d+)", log)
    if m:
        eq("hook name", d["rt"]["hook"], m.group(1))
        eq("hook calls", d["rt"].get("hook_calls"), int(m.group(2)))
        eq("hook histogram count", (d["hist"]["hook"] or {}).get("count"), int(m.group(2)))
    for k in ("wake_jitter", "prep_send", "occupancy", "turnaround"):
        m = last(r"^%s\s+n=(\d+)" % k, log)
        if m:
            eq("telemetry %s n" % k, (d["hist"][k] or {}).get("count"), int(m.group(1)))
    for m in re.finditer(r"^  \[CIA402\] axis (\d+) \(([^)]*)\): .*? used=(\d+) late=(\d+) underrun=(\d+) dropped=(\d+)", log, re.M):
        k = int(m.group(1))
        a = d["axes"][k] if k < len(d["axes"]) else {}
        eq("axis %d name" % k, a.get("name"), m.group(2))
        for f, v in zip(("used", "late", "underruns", "dropped"), m.groups()[2:]):
            eq("axis %d %s" % (k, f), a.get(f), int(v))
    m = last(r"^  \[DIAG\] reads=(\d+)", log)
    if m:
        eq("diag reads", (d["sources"]["monitor"] or {}).get("diag_reads"), int(m.group(1)))
    for m in re.finditer(r"^  \[POLICY\] slave (\d+): recoveries=(\d+).*?((?: FAILED\(config\)| FAILED)?)$", log, re.M):
        s = d["slaves"][int(m.group(1)) - 1]
        eq("slave %s recoveries" % m.group(1), s.get("recoveries"), int(m.group(2)))
        want = "failed_config" if "config" in m.group(3) else "failed" if "FAILED" in m.group(3) else None
        if want:
            eq("slave %s recover" % m.group(1), s.get("recover"), want)
    bad = [t for ok, t in res if not ok]
    for ok, t in res:
        print("  [%s] %s" % ("ok" if ok else "MISMATCH", t))
    print("compare: %d values, %d mismatch" % (len(res), len(bad)))
    return 1 if bad or not res else 0


def cmd_watch(path, seconds):
    t_end = time.monotonic() + float(seconds)
    reads = fails = 0
    seqs = []                                   # (wall time first seen, pub_seq)
    last_seq = None
    while time.monotonic() < t_end:
        try:
            with open(path) as f:
                txt = f.read()
            d = json.loads(txt)
            reads += 1
            if d["pub_seq"] != last_seq:
                seqs.append((time.monotonic(), d["pub_seq"]))
                last_seq = d["pub_seq"]
        except FileNotFoundError:
            pass
        except (ValueError, KeyError):
            fails += 1
        time.sleep(0.002)
    if len(seqs) < 2:
        print("watch: reads=%d parse_fail=%d publishes_seen=%d rate=0 max_gap_ms=inf" % (reads, fails, len(seqs)))
        return 1
    span = seqs[-1][0] - seqs[0][0]
    rate = (len(seqs) - 1) / span if span > 0 else 0
    gap = max(b[0] - a[0] for a, b in zip(seqs, seqs[1:]))
    back = sum(1 for a, b in zip(seqs, seqs[1:]) if b[1] <= a[1])
    print("watch: reads=%d parse_fail=%d publishes_seen=%d rate=%.2f max_gap_ms=%.0f seq_backwards=%d"
          % (reads, fails, len(seqs), rate, gap * 1e3, back))
    return 1 if fails or back else 0


def main(a):
    try:
        if len(a) == 2 and a[0] == "schema":
            return cmd_schema(a[1])
        if len(a) == 2 and a[0] == "selftest":
            return cmd_selftest(a[1])
        if len(a) == 3 and a[0] == "compare":
            return cmd_compare(a[1], a[2])
        if len(a) == 3 and a[0] == "watch":
            return cmd_watch(a[1], a[2])
    except (OSError, ValueError) as e:
        print("stats_check: %s" % e)
        return 2
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
