#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# make_evidence.sh -- Phase 12.6: the files of the GitHub Release of a tag,
# built from the TAG itself (not from the working tree), and the
# validation-evidence pack made of the logs of the runs on that commit.
#
#   sudo -E tools/release/make_evidence.sh [--tag master-v1.0]
#        [--final DIR] [--soak DIR] [--bm DIR] [--d14 DIR] [--is7 DIR]
#        [--no-build] [--no-capture] [--out DIR]
#
# Out (default dist/release-<VERSION>/):
#   ecmaster-<V>.tar.gz              make dist in a clean worktree of the tag
#   ecmaster_<V>-1_<arch>.deb        make all + make deb in that worktree
#   ecmaster-<V>-evidence.tar.gz     the evidence pack, MANIFEST.md inside
#   SHA256SUMS                       of the three
#
# Evidence pack:
#   final/   run_final_12_5.sh of the tagged commit (summary, regression
#            summaries, check_env, cyclictest, valgrind)
#   soak/    run_soak_12_5.sh 8 h (summary, W-08 report, watch.csv, verdict,
#            final stats.json, check_env, NIC counters, ecm_run log gz)
#   golden/  the golden structures of the tag
#   bench/   docs/benchmark.md + the BM-02 stats.json files; D-14, IS-07 logs
#   pcap/    >= 3 captures made now on veth with the tag's binaries:
#            esm_pd (INIT -> OP -> cyclic -> INIT), sdo_startup (ENI with a
#            Startup SDO: CoE download in the capture), cable_pull (veth_s
#            down 2 s / up: LOST -> RECOVER), plus the committed L2 ESM capture
#   machine/ kernel, cmdline, boot profile, check_env, SOEM stamp, versions
# Refused (exit 2): the tag does not exist; the soak or final summary is
# missing or not PASS for that commit; fewer than 3 captures.
# Directories default to the newest log_gd12_final_* / log_gd12_soak_* whose
# summary names the tag's commit; --bm / --d14 / --is7 are optional.
# ==========================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TAG=master-v1.0; FINAL=; SOAK=; BM=; D14=; IS7=; BUILD=1; CAPTURE=1; OUT=
while [ $# -gt 0 ]; do
    case $1 in
        --tag) TAG=$2; shift ;;
        --final) FINAL=$2; shift ;;
        --soak) SOAK=$2; shift ;;
        --bm) BM=$2; shift ;;
        --d14) D14=$2; shift ;;
        --is7) IS7=$2; shift ;;
        --no-build) BUILD=0 ;;
        --no-capture) CAPTURE=0 ;;
        --out) OUT=$2; shift ;;
        -h|--help) sed -n '4,36p' "$0"; exit 0 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
die () { echo "make_evidence: $*" >&2; exit 2; }
git -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG" >/dev/null || die "no tag $TAG (git tag -a $TAG <commit>)"
C=$(git -C "$ROOT" rev-list -n 1 "$TAG"); CS=$(git -C "$ROOT" rev-parse --short "$C")
V=$(git -C "$ROOT" show "$TAG:VERSION")
OUT=${OUT:-$ROOT/dist/release-$V}
U=${SUDO_USER:-$(id -un)}
[ $CAPTURE = 0 ] || [ "$(id -u)" = 0 ] || die "captures need root (sudo -E), or --no-capture"

pick () {   # GLOB: newest directory whose summary.md names the tag's commit
    local d
    for d in $(ls -td $1 2>/dev/null); do grep -q "g$CS\|$CS" "$d/summary.md" 2>/dev/null && { echo "${d%/}"; return; }; done
}
FINAL=${FINAL:-$(pick "$ROOT/log_gd12_final_*/")}
SOAK=${SOAK:-$(pick "$ROOT/log_gd12_soak_*/")}
[ -n "$FINAL" ] && [ -s "$FINAL/summary.md" ] || die "no run_final_12_5.sh summary for $CS (--final DIR)"
[ -n "$SOAK" ] && [ -s "$SOAK/summary.md" ] || die "no run_soak_12_5.sh summary for $CS (--soak DIR)"
grep -q '\*\*FAIL' "$FINAL/summary.md" && die "$FINAL/summary.md has a FAIL row"
grep -q "| W-08 checks .* | PASS |" "$SOAK/summary.md" && grep -q "| soak_watch .* | PASS |" "$SOAK/summary.md" \
    || die "$SOAK/summary.md is not a PASS"
grep -q '^- 28800 s' "$SOAK/summary.md" || echo "make_evidence: note: $SOAK is not an 8 h run"

mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d); trap 'git -C "$ROOT" worktree remove --force "$W/tree" >/dev/null 2>&1; rm -rf "$W"' EXIT
E=$W/ecmaster-$V-evidence; mkdir -p "$E"/{final,soak,golden,bench,pcap,machine}

echo "=== worktree of $TAG ($CS)"
git -C "$ROOT" worktree add -q --detach "$W/tree" "$TAG" || die "git worktree add failed"
T=$W/tree
chown -R "$U:" "$W" 2>/dev/null
if [ $BUILD = 1 ] || [ $CAPTURE = 1 ]; then
    S=${SOEM_DIR:-$ROOT/build/SOEM}
    sudo -u "$U" "$T/tools/release/build_soem.sh" "$S" > "$W/build_soem.txt" 2>&1 || { cat "$W/build_soem.txt"; die "build_soem failed"; }
    sudo -u "$U" make -s -C "$T" all SOEM_DIR="$S" > "$W/make_all.txt" 2>&1 || { tail -20 "$W/make_all.txt"; die "make all failed"; }
    cp "$S/.ecmaster-soem" "$E/machine/soem_stamp.txt" 2>/dev/null
fi
if [ $BUILD = 1 ]; then
    echo "=== release files"
    sudo -u "$U" "$T/tools/release/make_dist.sh" --out "$W/dist" > /dev/null || die "make dist failed"
    sudo -u "$U" "$T/tools/release/build_deb.sh" --out "$W/dist" > "$W/deb.txt" 2>&1 || { cat "$W/deb.txt"; die "build_deb failed"; }
    cp "$W/dist/ecmaster-$V.tar.gz" "$W"/dist/ecmaster_"$V"-1_*.deb "$OUT/"
fi

echo "=== evidence"
cp "$FINAL/summary.md" "$E/final/"
(cd "$FINAL" && find . -name summary.md -path './regression/*' -exec cp --parents {} "$E/final/" \;)
for f in check_env.txt rt_tail/cyclictest.txt valgrind/leak.txt; do [ -f "$FINAL/$f" ] && cp "$FINAL/$f" "$E/final/"; done
find "$FINAL/valgrind" -name 'valgrind_l503.txt' -exec cp {} "$E/final/" \; 2>/dev/null
cp "$SOAK/summary.md" "$E/soak/"
for f in w/report.md watch/watch.csv watch/watch_verdict.txt stats_final.json check_env.txt ethtool_S_before.txt ethtool_S_after.txt; do
    [ -f "$SOAK/$f" ] && cp "$SOAK/$f" "$E/soak/"
done
[ -f "$SOAK/w/er_w08.log" ] && gzip -c "$SOAK/w/er_w08.log" > "$E/soak/er_w08.log.gz"
cp "$T"/tools/golden/golden_*.txt "$E/golden/"
cp "$T/docs/benchmark.md" "$T/docs/rt_profiles.md" "$E/bench/"
BM=${BM:-$(ls -td "$ROOT"/log_gd11_bm_*/ 2>/dev/null | head -1)}
[ -n "$BM" ] && for d in "$BM"/*/; do [ -f "$d/stats.json" ] && cp "$d/stats.json" "$E/bench/bm02_$(basename "$d").json"; done
[ -n "$BM" ] && [ -f "$BM/benchmark.md" ] && cp "$BM/benchmark.md" "$E/bench/bm02_benchmark_run.md"
D14=${D14:-$(ls -td "$ROOT"/log_gd11_d14_*/ 2>/dev/null | head -1)}
[ -n "$D14" ] && find "$D14" -maxdepth 1 \( -name '*.md' -o -name '*.txt' \) -exec cp {} "$E/bench/" \; 2>/dev/null
IS7=${IS7:-$(ls -td "$ROOT"/log_gd11_is7_*/ 2>/dev/null | head -1)}
[ -n "$IS7" ] && find "$IS7" -maxdepth 2 -name '*.md' -exec sh -c 'cp "$1" "$2/is7_$(basename "$1")"' _ {} "$E/bench" \; 2>/dev/null

if [ $CAPTURE = 1 ]; then
    echo "=== captures on veth with the tag's binaries"
    command -v tshark >/dev/null || die "tshark missing"
    for i in veth_m veth_s; do ip link show "$i" >/dev/null 2>&1 || { ip link add veth_m type veth peer name veth_s; ip link set veth_m up; ip link set veth_s up; break; }; done
    cap () {   # NAME N SECONDS EVENTS -- ecm_run args...
        local name=$1 n=$2 dur=$3 ev=$4; shift 5
        "$T/tools/soft_bus/soft_bus" --iface veth_s --n "$n" --pdo-size 4 --dc 32 --no-sm-wd > "$W/sb_$name.log" 2>&1 & local sb=$!
        tshark -q -i veth_m -w "$E/pcap/$name.pcapng" -f "ether proto 0x88a4" > /dev/null 2>&1 & local ts=$!
        for _ in $(seq 50); do [ -s "$E/pcap/$name.pcapng" ] && break; sleep 0.1; done; sleep 1
        "$T/apps/ecm_run/ecm_run" --iface veth_m --n "$n" --no-tx-ts --duration-sec "$dur" "$@" > "$W/er_$name.log" 2>&1 & local er=$!
        [ -n "$ev" ] && bash -c "$ev"
        wait $er; sleep 0.5
        kill -INT $ts; wait $ts 2>/dev/null; kill -INT $sb; wait $sb 2>/dev/null
        cp "$W/er_$name.log" "$E/pcap/$name.ecm_run.log"
    }
    cap esm_pd 8 2 "" -- --motion-slaves 4
    cap sdo_startup 8 2 "" -- --motion-slaves 4 --eni "$T/config/eni/eni_8node_dc_sdo.enicfg"
    cap cable_pull 8 12 "sleep 4; ip link set veth_s down; sleep 2; ip link set veth_s up" -- --motion-slaves 4
    grep -q 'CoE download' "$E/pcap/sdo_startup.ecm_run.log" || echo "make_evidence: note: sdo_startup log shows no CoE download"
    grep -q 'LOST -> RECOVER' "$E/pcap/cable_pull.ecm_run.log" || echo "make_evidence: note: cable_pull has no LOST -> RECOVER"
fi
cp "$T/tools/soft_bus/log_l2_20260917_143908/esm_transition.pcapng" "$E/pcap/l2_esm_transition_gd2.pcapng" 2>/dev/null
n=$(ls "$E"/pcap/*.pcapng 2>/dev/null | wc -l)
[ "$n" -ge 3 ] || die "only $n captures (need >= 3)"

{ uname -a; echo; cat /proc/cmdline; echo; cat /etc/nv_tegra_release 2>/dev/null; } > "$E/machine/kernel.txt"
"$T/packaging/sbin/ecmaster-bootprofile" status > "$E/machine/bootprofile.txt" 2>&1
"$T/tools/jetson/check_env.sh" > "$E/machine/check_env_now.txt" 2>&1
{ echo "tag $TAG -> $C"; git -C "$ROOT" log -1 --format='%H %an %ad %s' "$TAG"; dpkg-query -W ecmaster 2>/dev/null; } > "$E/machine/versions.txt"

{
    echo "# ecmaster $V — validation evidence ($TAG, \`$CS\`)"
    echo
    echo "Made $(date '+%Y-%m-%d %H:%M') on $(uname -n) by tools/release/make_evidence.sh. Index of the files; the results are"
    echo "in each summary. Test IDs: TEST_PLAN.md of the tag."
    echo
    echo "| Path | Content | Source |"
    echo "|---|---|---|"
    echo "| final/ | final regression of $CS: commit, check_env, Phase 9–11 regression, licence, L0, valgrind | \`$(basename "$FINAL")\` |"
    echo "| soak/ | 8 h soak, 2 IS620N, both axes CSP | \`$(basename "$SOAK")\` |"
    echo "| golden/ | golden datagram structures | tag |"
    echo "| bench/ | benchmark (BM-02), RT profiles, D-14 / IS-07 when present | ${BM:+\`$(basename "$BM")\`} ${D14:+\`$(basename "$D14")\`} ${IS7:+\`$(basename "$IS7")\`} |"
    echo "| pcap/ | $n captures (EtherCAT 0x88A4) | $([ $CAPTURE = 1 ] && echo "veth now, tag's binaries;") GD2 L2 capture |"
    echo "| machine/ | kernel, cmdline, boot profile, check_env, SOEM stamp, versions | this Jetson |"
    echo
    echo "## Files"
    echo
    (cd "$E" && find . -type f ! -name MANIFEST.md ! -name SHA256SUMS | sort | sed 's|^\./|- |')
} > "$E/MANIFEST.md"
(cd "$E" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS)
tar czf "$OUT/ecmaster-$V-evidence.tar.gz" -C "$W" "ecmaster-$V-evidence"
(cd "$OUT" && sha256sum ecmaster-* ecmaster_*.deb 2>/dev/null > SHA256SUMS)
chown -R "$U:" "$OUT" 2>/dev/null
echo; ls -la "$OUT"; echo; cat "$OUT/SHA256SUMS"
echo "make_evidence: $OUT (tag $TAG = $CS; evidence: $(tar tzf "$OUT/ecmaster-$V-evidence.tar.gz" | grep -vc '/$') files)"
