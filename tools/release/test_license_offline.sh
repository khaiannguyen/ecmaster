#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# test_license_offline.sh -- Phase 12.1: licence checks, no SOEM, no network.
#
#   tools/release/test_license_offline.sh        (make test-license; CI offline job)
#
#   LIC-01a  `reuse lint` passes on a copy of the tree (tracked + new files)
#   LIC-01b  negative control: one .c without its header -> reuse lint FAILS
#            and add_spdx.py --check FAILS
#   LIC-01c  add_spdx.py: header after the shebang / coding line, CRLF and
#            the file mode kept, a second run changes nothing (idempotent)
#   LIC-01d  every *.sh passes bash -n, every *.py compiles (the headers broke
#            no script)
#   LIC-03a  check_third_party.sh passes on the repository
#   LIC-03b  negative controls: a vendor ESI, a PDF, an IgH copyright line,
#            an ESI-generated profile, each planted alone in a copy -> FAIL
#
# Needs: reuse (pip install --user reuse), python3, git.
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PASS=0; FAIL=0
chk () {   # NAME CONDITION...
    local n=$1; shift
    if eval "$*"; then echo "  PASS $n"; PASS=$((PASS + 1)); else echo "  FAIL $n"; FAIL=$((FAIL + 1)); fi
}
command -v reuse >/dev/null || { echo "reuse missing: pip install --user reuse (or pipx install reuse)"; exit 2; }

copy () {   # DEST: tracked + new (not ignored) files, no .git
    mkdir -p "$1"
    git -C "$ROOT" ls-files -co --exclude-standard -z | (cd "$ROOT" && xargs -0 cp --parents -t "$1" 2>/dev/null)
}

echo "=== LIC-01 reuse / SPDX headers"
copy "$T/tree"
reuse --root "$T/tree" lint > "$T/lint.txt" 2>&1; r=$?
chk "LIC-01a reuse lint on the tree (rc $r)" "[ $r = 0 ]"
[ $r = 0 ] || sed -n '1,20p' "$T/lint.txt"

copy "$T/neg"
C=libecmaster/core/ecm_dc.c
sed -i '1,2d' "$T/neg/$C"
reuse --root "$T/neg" lint > "$T/lint_neg.txt" 2>&1; r=$?
chk "LIC-01b negative: $C without header -> reuse lint fails (rc $r)" "[ $r != 0 ] && grep -q '$C' $T/lint_neg.txt"
python3 "$HERE/add_spdx.py" --check --root "$T/neg" > "$T/chk_neg.txt"; r=$?
chk "LIC-01b negative: add_spdx --check names it (rc $r)" "[ $r = 1 ] && grep -q 'no SPDX header: $C' $T/chk_neg.txt"
python3 "$HERE/add_spdx.py" --root "$T/neg" > /dev/null
chk "LIC-01b add_spdx puts it back byte for byte" "cmp -s '$ROOT/$C' '$T/neg/$C'"

# REUSE-IgnoreStart (the expected headers below are test data)
U=$T/unit; mkdir -p "$U"
printf '#!/usr/bin/env python3\n# -*- coding: utf-8 -*-\n"""doc"""\nprint(1)\n' > "$U/a.py"
printf '#!/bin/sh\r\necho hi\r\n' > "$U/b.sh"; chmod 755 "$U/b.sh"
printf '# shellcheck shell=bash\nx=1\n' > "$U/c.sh"
printf '/* c */\nint x;\n' > "$U/d.c"
printf 'all:\n\ttrue\n' > "$U/Makefile"
cp -r "$U" "$U.0"
python3 "$HERE/add_spdx.py" --root "$U" > /dev/null
H='# SPDX-FileCopyrightText: 2026 khaiannguyen'
L='# SPDX-License-Identifier: GPL-3.0-only'
chk "LIC-01c .py: shebang, coding line, then header; docstring kept" \
    "[ \"\$(sed -n 2p $U/a.py)\" = '# -*- coding: utf-8 -*-' ] && [ \"\$(sed -n 3p $U/a.py)\" = '$H' ] && [ \"\$(python3 -c 'import ast,sys; print(ast.get_docstring(ast.parse(open(sys.argv[1]).read())))' $U/a.py)\" = doc ]"
chk "LIC-01c .sh CRLF: header in CRLF after the shebang, mode 755 kept" \
    "[ \"\$(sed -n 2p $U/b.sh)\" = \"$H\"\$'\\r' ] && [ \"\$(stat -c %a $U/b.sh)\" = 755 ] && [ \"\$(sh $U/b.sh | tr -d '\\r')\" = hi ]"
chk "LIC-01c .sh without shebang / .c / Makefile: header on lines 1-2" \
    "[ \"\$(sed -n 1p $U/c.sh)\" = '$H' ] && [ \"\$(sed -n 2p $U/d.c)\" = '/* SPDX-License-Identifier: GPL-3.0-only */' ] && [ \"\$(sed -n 2p $U/Makefile)\" = '$L' ] && make -s -C $U >/dev/null"
cp -r "$U" "$U.1"
python3 "$HERE/add_spdx.py" --root "$U" > "$T/second.txt"
chk "LIC-01c second run changes nothing" "diff -r '$U' '$U.1' >/dev/null && grep -q '0 file(s) changed' $T/second.txt"
chk "LIC-01c --dry-run writes nothing" "python3 '$HERE/add_spdx.py' --dry-run --root '$U.0' | grep -q '5 file(s) to change' && ! grep -rq SPDX '$U.0'"
# REUSE-IgnoreEnd

bad=0
while IFS= read -r f; do bash -n "$ROOT/$f" 2>>"$T/syntax.txt" || bad=$((bad + 1)); done < <(git -C "$ROOT" ls-files -co --exclude-standard '*.sh')
while IFS= read -r f; do
    python3 -c 'import sys,ast; ast.parse(open(sys.argv[1], "rb").read(), sys.argv[1])' "$ROOT/$f" 2>>"$T/syntax.txt" || bad=$((bad + 1))
done < <(git -C "$ROOT" ls-files -co --exclude-standard '*.py')
chk "LIC-01d bash -n on every .sh, every .py parses ($bad bad)" "[ $bad = 0 ]"
[ $bad = 0 ] || cat "$T/syntax.txt"

echo "=== LIC-03 nothing we may not redistribute"
"$HERE/check_third_party.sh" "$ROOT" > "$T/l3.txt"; r=$?
chk "LIC-03a check_third_party on the repository (rc $r)" "[ $r = 0 ]"
[ $r = 0 ] || cat "$T/l3.txt"
plant () {   # NAME FILE CONTENT -> copy of the tree with that one file, must FAIL
    local d=$T/p_$1
    copy "$d"; mkdir -p "$(dirname "$d/$2")"; printf '%s\n' "$3" > "$d/$2"
    "$HERE/check_third_party.sh" "$d" > "$T/p_$1.txt"; local r=$?
    chk "LIC-03b negative: $1 ($2) -> FAIL (rc $r)" "[ $r = 1 ] && grep -q '$2' $T/p_$1.txt"
    rm -rf "$d"
}
V=Vendor
plant vendor_esi config/esi/vendor.xml "<EtherCAT""Info><$V><Id>#x00100000</Id></$V></EtherCAT""Info>"
plant pdf docs/ETG1000.pdf "%PDF-1.4"
plant igh tools/xcheck/master.c "/* Copyright ""(C) 2006-2024 Florian"" Pose, Ingenieur""gemeinschaft IgH */"
plant prof config/profiles/is620n.prof "vendor 0x00100000"

echo
echo "RESULT: $PASS pass, $FAIL fail"
[ $FAIL = 0 ]
