#!/usr/bin/env python3
"""
check_k05.py -- Phase 10 K-05: libecmaster holds no CiA402 object index.

The CiA402 layer (object meaning) lives in libecm_cia402; libecmaster only
binds numbers (Phase 9.10). This scans the C sources and headers of
libecmaster/, comments, string literals and the offline tests excluded
(tests use 0x6041 etc. as data), for integer literals in the device profile
area 0x6000..0x9FFF (CiA 402 objects, ETG.6010 multi-axis offsets).

    python3 tools/gd10/check_k05.py            exit 1 and list on a hit
    python3 tools/gd10/check_k05.py --self-test  negative control
"""
import os
import re
import sys
import tempfile

LIT = re.compile(r'\b0[xX]0*([6-9][0-9A-Fa-f]{3})[uUlL]*\b')


def strip(src):
    """C source without comments and string/char literals (newlines kept)."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * src.count('\n', i, j))
            i = j
        elif src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def scan(root):
    hits = []
    for d, _, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith(('.c', '.h')) or f.startswith('test_'):
                continue
            p = os.path.join(d, f)
            code = strip(open(p, encoding='utf-8', errors='replace').read())
            for ln, line in enumerate(code.split('\n'), 1):
                for m in LIT.finditer(line):
                    hits.append('%s:%d: 0x%s' % (p, ln, m.group(1).upper()))
    return hits


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(here, '..', '..', 'libecmaster')
    if '--self-test' in sys.argv:
        with tempfile.TemporaryDirectory() as t:
            open(os.path.join(t, 'ok.c'), 'w').write('/* 0x6041 */ const char *s = "0x6041"; int a = 0x5FFF;\n')
            open(os.path.join(t, 'test_x.c'), 'w').write('int sw = 0x6041;\n')
            clean = scan(t)
            open(os.path.join(t, 'bad.c'), 'w').write('int sw = 0x6041;\nint c = 0x00007840u;\n')
            dirty = scan(t)
        ok = clean == [] and len(dirty) == 2
        print('K-05 self-test: comments/strings/tests ignored: %s, two literals caught: %s -> %s'
              % (clean == [], len(dirty) == 2, 'PASS' if ok else 'FAIL'))
        return 0 if ok else 1
    hits = scan(root)
    if hits:
        print('K-05 FAIL: CiA402 object index in libecmaster (belongs in libecm_cia402):')
        print('\n'.join('  ' + h for h in hits))
        return 1
    print('K-05 PASS: no CiA402 object index (0x6000..0x9FFF) in libecmaster code')
    return 0


if __name__ == '__main__':
    sys.exit(main())
