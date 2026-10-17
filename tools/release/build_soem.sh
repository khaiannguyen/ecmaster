#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# build_soem.sh -- Phase 12.3: the SOEM ecmaster is built against, from
# scratch and checked: tag v2.0.0, the two patches of patches/ (sha256 in
# patches/SHA256SUMS), -DEC_MAXGROUP=4. Replaces the manual ~/projects/SOEM.
#
#   tools/release/build_soem.sh [DIR]            default build/SOEM; prints SOEM_DIR=...
#   tools/release/build_soem.sh --check-patches  only the sha256 of the patches
#   make all SOEM_DIR=$PWD/build/SOEM
#
# DIR absent: git clone --depth 1 --branch v2.0.0 (network). DIR present: it
# must be that tag, and either untouched (patched + built now) or built by
# this script with the same patches (stamp DIR/.ecmaster-soem; nothing done).
# Refused (exit 1): a patch missing or not matching SHA256SUMS, DIR at
# another revision or changed by hand, a patch that does not apply.
#
# Env: SOEM_URL (default https://github.com/OpenEtherCATsociety/SOEM),
#      SOEM_TAG (default v2.0.0), PATCH_DIR (default patches/), JOBS
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SOEM_URL=${SOEM_URL:-https://github.com/OpenEtherCATsociety/SOEM}
SOEM_TAG=${SOEM_TAG:-v2.0.0}
PATCH_DIR=${PATCH_DIR:-$ROOT/patches}
PATCHES="soem-mbx-cnt.patch soem-txtime.patch"      # order of application
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
die () { echo "build_soem: $*" >&2; exit 1; }

check_patches () {
    [ -s "$PATCH_DIR/SHA256SUMS" ] || die "no $PATCH_DIR/SHA256SUMS"
    for p in $PATCHES; do
        [ -s "$PATCH_DIR/$p" ] || die "patch missing: $PATCH_DIR/$p"
        want=$(awk -v f="$p" '$2 == f {print $1}' "$PATCH_DIR/SHA256SUMS")
        [ -n "$want" ] || die "$p not listed in SHA256SUMS"
        got=$(sha256sum < "$PATCH_DIR/$p" | cut -d' ' -f1)
        [ "$got" = "$want" ] || die "$p: sha256 $got, SHA256SUMS says $want (patch changed?)"
    done
    echo "build_soem: patches ok ($PATCHES)"
}

if [ "${1:-}" = --check-patches ]; then check_patches; exit 0; fi
DIR=${1:-$ROOT/build/SOEM}
check_patches
STAMP_WANT="$SOEM_TAG $(cat "$PATCH_DIR/SHA256SUMS" | cut -d' ' -f1 | tr '\n' ' ')EC_MAXGROUP=4"

if [ ! -e "$DIR" ]; then
    mkdir -p "$(dirname "$DIR")"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$SOEM_TAG" "$SOEM_URL" "$DIR" || die "git clone $SOEM_URL ($SOEM_TAG) failed (network?)"
fi
[ -d "$DIR/.git" ] || die "$DIR is not a git clone of SOEM"
DIR=$(cd "$DIR" && pwd)
tag=$(git -C "$DIR" describe --tags --exact-match 2>/dev/null)
[ "$tag" = "$SOEM_TAG" ] || die "$DIR is at '${tag:-$(git -C "$DIR" rev-parse --short HEAD)}', not $SOEM_TAG"

if [ -s "$DIR/.ecmaster-soem" ]; then
    [ "$(cat "$DIR/.ecmaster-soem")" = "$STAMP_WANT" ] || die "$DIR was built with other patches/options ($(cat "$DIR/.ecmaster-soem")): remove it and run again"
    ls "$DIR"/build/libsoem*.a >/dev/null 2>&1 || find "$DIR/build" -name 'libsoem*.a' | grep -q . || die "$DIR: stamp but no libsoem: remove it and run again"
    echo "build_soem: $DIR already built ($STAMP_WANT)"
else
    [ -z "$(git -C "$DIR" status --porcelain --untracked-files=no)" ] || die "$DIR has local changes: use a fresh directory"
    for p in $PATCHES; do
        patch -d "$DIR" -p1 --dry-run -s < "$PATCH_DIR/$p" > /dev/null || die "$p does not apply to $SOEM_TAG"
        patch -d "$DIR" -p1 -s < "$PATCH_DIR/$p" || die "$p failed"
    done
    cmake -S "$DIR" -B "$DIR/build" -DEC_MAXGROUP=4 -DCMAKE_BUILD_TYPE=Release > "$DIR/.ecmaster-cmake.log" 2>&1 ||
        die "cmake configure failed ($DIR/.ecmaster-cmake.log)"
    cmake --build "$DIR/build" -j "$JOBS" >> "$DIR/.ecmaster-cmake.log" 2>&1 || die "build failed ($DIR/.ecmaster-cmake.log)"
    grep -q 'ECMASTER_SOEM_MBXCNT_PATCH' "$DIR/include/soem/ec_main.h" || die "mbx-cnt patch not in the headers"
    echo "$STAMP_WANT" > "$DIR/.ecmaster-soem"
    echo "build_soem: built $DIR ($STAMP_WANT)"
fi
echo "SOEM_DIR=$DIR"
