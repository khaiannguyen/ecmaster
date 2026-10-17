#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# make_dist.sh -- Phase 12.3: the source tarball of a release
# (make dist): dist/ecmaster-VERSION.tar.gz = git archive of HEAD, prefix
# ecmaster-VERSION/, plus its sha256 file.
#
#   tools/release/make_dist.sh [--allow-dirty] [--out DIR]
#
# Content: every tracked file at HEAD except what .gitattributes marks
# export-ignore (run logs, captures: they go to the evidence pack). The SOEM
# patches, build_soem.sh, install.sh and the packaging are in it: the
# tarball alone rebuilds ecmaster (GPLv3 §6). No binary, no vendor ESI.
# Refused: uncommitted changes to tracked files (the tarball would not be
# what was tested) unless --allow-dirty; not a git tree.
# ==========================================================================
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/dist; DIRTY=0
while [ $# -gt 0 ]; do
    case $1 in --allow-dirty) DIRTY=1 ;; --out) OUT=$2; shift ;; *) echo "unknown option $1"; exit 2 ;; esac
    shift
done
git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 || { echo "make_dist: not a git tree (a tarball is made from the repository)"; exit 2; }
if [ $DIRTY = 0 ] && [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
    echo "make_dist: uncommitted changes (commit them, or --allow-dirty for a test tarball of HEAD):"
    git -C "$ROOT" status --short --untracked-files=no | head; exit 2
fi
V=$(git -C "$ROOT" show HEAD:VERSION 2>/dev/null) || { echo "make_dist: no VERSION at HEAD"; exit 2; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
T=$OUT/ecmaster-$V.tar.gz
git -C "$ROOT" archive --format=tar.gz --prefix="ecmaster-$V/" -o "$T" HEAD || exit 1
(cd "$OUT" && sha256sum "$(basename "$T")" > "$(basename "$T").sha256")
echo "make_dist: $T ($(tar tzf "$T" | grep -vc '/$') files, commit $(git -C "$ROOT" rev-parse --short HEAD))"
