#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# check_third_party.sh -- Phase 12.1 LIC-03: nothing in the repository that
# this project may not redistribute under GPL-3.0-only.
#
#   tools/release/check_third_party.sh [DIR]      default: the repository
#
# Looks at the files of the tree (git ls-files -co --exclude-standard: tracked
# and new, not ignored; every file when DIR is not a git tree) for:
#   T1  a vendor ESI: <EtherCATInfo with a vendor Id other than the project's
#       test vendor #x00000499 (config/esi/*.xml are the project's own)
#   T2  documents that are not ours to ship: *.pdf *.chm *.doc(x)
#   T3  copied third-party code: IgH EtherLab copyright lines (Florian Pose,
#       Ingenieurgemeinschaft IgH), Intel driver copyright lines (igc), any
#       other "Copyright (C)" line outside patches/ (the SOEM patches carry
#       SOEM context, declared in REUSE.toml; THIRD_PARTY.md quotes the
#       copyright lines of what it lists)
#   T4  ESI-generated profiles that .gitignore keeps local (config/profiles/is620n.prof)
# Exit 0 and "LIC-03 PASS" when nothing is found, else 1 and the hits.
# Negative control: tools/release/test_license_offline.sh plants one file of
# each kind in a copy and expects this script to fail.
# ==========================================================================
set -u
DIR=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
cd "$DIR" || exit 2
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    mapfile -t FILES < <(git ls-files -co --exclude-standard)
else
    mapfile -t FILES < <(find . -type f -not -path './.git/*' | sed 's|^\./||')
fi
hits=0
hit () { echo "  $1 $2"; hits=$((hits + 1)); }

for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    case "$f" in
        *.pdf|*.PDF|*.chm|*.doc|*.docx) hit T2 "$f: vendor/standard document" ;;
        config/profiles/is620n.prof) hit T4 "$f: profile generated from the vendor ESI (local only)" ;;
    esac
    case "$f" in LICENSE|LICENSES/*|THIRD_PARTY.md|tools/release/check_third_party.sh) continue ;; esac
    if grep -Iq '<EtherCATInfo' "$f" 2>/dev/null; then
        v=$(grep -o -m1 '<Id>[^<]*</Id>' "$f" | sed 's/<[^>]*>//g')
        [ "$v" = "#x00000499" ] || hit T1 "$f: EtherCATInfo of vendor '${v:-?}' (vendor ESI)"
    fi
    case "$f" in patches/*) continue ;; esac
    grep -InE 'Florian Pose|Ingenieurgemeinschaft IgH|Copyright.*Intel Corporation|Copyright \([cC]\)' "$f" 2>/dev/null |
        head -3 | while IFS= read -r l; do echo "  T3 $f:$l"; done
    grep -IqE 'Florian Pose|Ingenieurgemeinschaft IgH|Copyright.*Intel Corporation|Copyright \([cC]\)' "$f" 2>/dev/null && hits=$((hits + 1))
done

if [ $hits = 0 ]; then
    echo "LIC-03 PASS: ${#FILES[@]} files, no vendor ESI, no third-party document, no copied copyright"
    exit 0
fi
echo "LIC-03 FAIL: $hits file(s) above"
exit 1
