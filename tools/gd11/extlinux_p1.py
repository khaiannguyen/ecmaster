#!/usr/bin/env python3
"""extlinux_p1.py -- Phase 11.5: add the P1 boot entry (cpuidle.off=1) to
/boot/extlinux/extlinux.conf without touching the existing entries.

  python3 tools/gd11/extlinux_p1.py                       dry run: print the diff
  sudo python3 tools/gd11/extlinux_p1.py --apply          add LABEL real-time-noidle
  sudo python3 tools/gd11/extlinux_p1.py --set-default real-time-noidle
  sudo python3 tools/gd11/extlinux_p1.py --set-default real-time     (back)

The new entry is a copy of the DEFAULT entry (or --from LABEL) with the
LABEL / MENU LABEL renamed and cpuidle.off=1 appended to APPEND. DEFAULT is
left alone by --apply. Before any write the file is copied to
extlinux.conf.bak-YYYYMMDD-HHMMSS next to it. Without a serial console the
boot menu cannot be used, so --set-default switches the default entry (and
prints how to switch back). docs/rt_profiles.md §2.

Refuses (exit 2): the new label exists already, the source entry has no
APPEND line, the source entry already has cpuidle.off, an unknown label for
--set-default. --file is for the offline test.
"""
import argparse
import datetime
import difflib
import re
import shutil
import sys

NEW_LABEL = "real-time-noidle"
FLAG = "cpuidle.off=1"


def parse(lines):
    """-> (default label, {label: (start, end)}) ; end exclusive"""
    default = None
    blocks = {}
    cur, start = None, None
    for i, ln in enumerate(lines):
        s = ln.strip()
        m = re.match(r"(?i)^DEFAULT\s+(\S+)", s)
        if m and cur is None:
            default = m.group(1)
        m = re.match(r"(?i)^LABEL\s+(\S+)", s)
        if m:
            if cur is not None:
                blocks[cur] = (start, i)
            cur, start = m.group(1), i
    if cur is not None:
        blocks[cur] = (start, len(lines))
    return default, blocks


def add_entry(lines, src):
    default, blocks = parse(lines)
    src = src or default
    if NEW_LABEL in blocks:
        raise SystemExit(f"refused: LABEL {NEW_LABEL} exists already")
    if src not in blocks:
        raise SystemExit(f"refused: no LABEL {src} (labels: {', '.join(blocks)})")
    a, b = blocks[src]
    body = lines[a:b]
    while body and not body[-1].strip():
        body = body[:-1]
    out, seen_append = [], False
    for ln in body:
        s = ln.strip()
        if re.match(r"(?i)^LABEL\s", s):
            ln = re.sub(r"(?i)(LABEL\s+)\S+", r"\g<1>" + NEW_LABEL, ln, count=1)
        elif re.match(r"(?i)^MENU\s+LABEL\s", s):
            ln = ln.rstrip("\n") + " (P1: cpuidle.off=1, Phase 11.5)\n"
        elif re.match(r"(?i)^APPEND\s", s):
            if "cpuidle.off" in ln:
                raise SystemExit(f"refused: LABEL {src} already has cpuidle.off")
            ln = ln.rstrip("\n") + " " + FLAG + "\n"
            seen_append = True
        out.append(ln)
    if not seen_append:
        raise SystemExit(f"refused: LABEL {src} has no APPEND line")
    new = list(lines)
    if new and not new[-1].endswith("\n"):
        new[-1] += "\n"
    return new + ["\n"] + out, src


def set_default(lines, label):
    _, blocks = parse(lines)
    if label not in blocks:
        raise SystemExit(f"refused: no LABEL {label} (labels: {', '.join(blocks)})")
    new, done = [], False
    for ln in lines:
        if not done and re.match(r"(?i)^\s*DEFAULT\s+\S+", ln):
            ln = re.sub(r"(?i)(DEFAULT\s+)\S+", r"\g<1>" + label, ln, count=1)
            done = True
        new.append(ln)
    if not done:
        raise SystemExit("refused: no DEFAULT line")
    return new


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="/boot/extlinux/extlinux.conf")
    ap.add_argument("--from", dest="src")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--set-default")
    a = ap.parse_args()
    with open(a.file) as f:
        lines = f.readlines()
    try:
        if a.set_default:
            new = set_default(lines, a.set_default)
            what = f"DEFAULT -> {a.set_default}"
        else:
            new, src = add_entry(lines, a.src)
            what = f"LABEL {NEW_LABEL} (copy of {src} + {FLAG})"
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2
    sys.stdout.writelines(difflib.unified_diff(lines, new, a.file, a.file + " (new)"))
    if not (a.apply or a.set_default):
        print(f"dry run: {what}; add --apply to write it")
        return 0
    bak = a.file + ".bak-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    shutil.copy2(a.file, bak)
    with open(a.file, "w") as f:
        f.writelines(new)
    print(f"written: {what}; backup {bak}")
    if a.set_default:
        print("reboot to use it; to undo: sudo python3 tools/gd11/extlinux_p1.py --set-default <previous label>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
