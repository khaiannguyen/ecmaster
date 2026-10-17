#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
"""add_spdx.py -- Phase 12.1: put the SPDX header (REUSE) on every source
file of the repository, without touching anything else.

  add_spdx.py                 add the header where it is missing (git ls-files)
  add_spdx.py --check         list the files without it; exit 1 if any
  add_spdx.py --dry-run       list what would change, write nothing
  add_spdx.py [--root DIR] [FILE ...]   other tree / only these files
                              (git ls-files at the top of a git tree, else
                              every file under DIR)

Files handled (the code): *.c *.h *.py *.sh, Makefile, .github/workflows/*.yml.
Every other file (docs, ENI, ESI, profiles, golden, logs, captures, patches)
is declared in REUSE.toml instead, so that data files stay byte-identical.

Header (REUSE-IgnoreStart):
  C:      /* SPDX-FileCopyrightText: 2026 khaiannguyen */
          /* SPDX-License-Identifier: GPL-3.0-only */      (lines 1-2)
  others: # SPDX-FileCopyrightText: 2026 khaiannguyen
          # SPDX-License-Identifier: GPL-3.0-only
          after the shebang and after a Python coding line (PEP 263 needs it
          on line 1 or 2), else on line 1.   (REUSE-IgnoreEnd)

Idempotent: a file with "SPDX-License-Identifier" in its first 10 lines is
left alone. Line endings (LF / CRLF) and the file mode are kept.
"""
import os
import re
import subprocess
import sys

COPYRIGHT = "2026 khaiannguyen"
LICENSE = "GPL-3.0-only"
TAG = "SPDX-License-Identifier"
CODING = re.compile(rb"^[ \t\f]*#.*?coding[:=][ \t]*[-\w.]+")


def kind(path):
    base = os.path.basename(path)
    if base == "Makefile" or path.endswith((".py", ".sh")):
        return "hash"
    if path.startswith(".github/workflows/") and path.endswith((".yml", ".yaml")):
        return "hash"
    if path.endswith((".c", ".h")):
        return "c"
    return None


def has_header(data):
    return TAG.encode() in b"\n".join(data.splitlines()[:10])


def header(k, eol):
    if k == "c":
        lines = ["/* SPDX-FileCopyrightText: %s */" % COPYRIGHT, "/* %s: %s */" % (TAG, LICENSE)]
    else:
        lines = ["# SPDX-FileCopyrightText: %s" % COPYRIGHT, "# %s: %s" % (TAG, LICENSE)]
    return b"".join(l.encode() + eol for l in lines)


def add(data, k):
    eol = b"\r\n" if b"\r\n" in data.split(b"\n", 1)[0] + b"\n" else b"\n"
    lines = data.splitlines(keepends=True)
    at = 0
    if k == "hash":
        if lines and lines[0].startswith(b"#!"):
            at = 1
        if len(lines) > at and at < 2 and CODING.match(lines[at]):
            at += 1
        if at and not lines[at - 1].endswith(b"\n"):     # shebang without a newline
            lines[at - 1] += eol
    return b"".join(lines[:at]) + header(k, eol) + b"".join(lines[at:])


def files(root, args):
    if args:
        return args
    r = subprocess.run(["git", "-C", root, "rev-parse", "--show-prefix"], capture_output=True, text=True)
    if r.returncode == 0 and r.stdout.strip() == "":
        out = subprocess.run(["git", "-C", root, "ls-files"], check=True, capture_output=True, text=True).stdout
        paths = out.splitlines()
    else:                                    # not the top of a git tree (a release tarball, a test copy)
        paths = []
        for d, dirs, names in os.walk(root):
            dirs[:] = [x for x in dirs if x not in (".git", "__pycache__")]
            paths += [os.path.relpath(os.path.join(d, n), root) for n in names]
    return sorted(p for p in paths if kind(p))


def main(argv):
    check = "--check" in argv
    dry = "--dry-run" in argv
    root = "."
    rest = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--root" and i + 1 < len(argv):
            root = argv[i + 1]
            i += 2
            continue
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        if not a.startswith("--"):
            rest.append(a)
        i += 1
    missing, changed = [], []
    for p in files(root, rest):
        k = kind(p)
        if not k:
            continue
        full = os.path.join(root, p)
        with open(full, "rb") as fh:
            data = fh.read()
        if has_header(data):
            continue
        missing.append(p)
        if check or dry:
            continue
        mode = os.stat(full).st_mode
        with open(full, "wb") as fh:
            fh.write(add(data, k))
        os.chmod(full, mode)
        changed.append(p)
    if check:
        for p in missing:
            print("no SPDX header: %s" % p)
        print("add_spdx --check: %d file(s) without header" % len(missing))
        return 1 if missing else 0
    for p in missing:
        print("%s %s" % ("would add" if dry else "added", p))
    print("add_spdx: %d file(s) %s" % (len(missing), "to change" if dry else "changed"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
