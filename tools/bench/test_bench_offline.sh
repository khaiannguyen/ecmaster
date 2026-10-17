#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# test_bench_offline.sh -- Phase 11.7 BM-01: ecm_bench.py on fixture stats.json
# files (testdata/), with its negative controls. No network, no root.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
B=$HERE/ecm_bench.py; D=$HERE/testdata
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
PASS=0; FAIL=0
chk () { if eval "$2"; then echo "  [PASS] $1"; PASS=$((PASS+1)); else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi; }

python3 "$B" --label af=$D/stats_af.json --label etf=$D/stats_etf.json > "$T/o.md" 2> "$T/e.txt"; r=$?
chk "B-01 two final files: rc 0, one table per metric, both runs in it" \
    "[ $r = 0 ] && grep -q '^### occupancy' $T/o.md && [ \$(grep -c '^| af |' $T/o.md) -ge 5 ] && [ \$(grep -c '^| etf |' $T/o.md) -ge 5 ]"
chk "B-02 configuration lines: isolated CPUs, kernel, ETF counters for the etf run" \
    "grep -q \"isolated CPUs '3'\" $T/o.md && grep -q '6.8.12-1021-rt-tegra' $T/o.md && grep -q 'ETF missed 0' $T/o.md"
chk "B-03 percentiles never above max (row of wake jitter, af)" \
    "awk -F'|' '/^\| af \|/ && !done {mx=\$6+0; for (i=7;i<=10;i++) if (\$i+0 > mx) bad=1; done=1} END {exit bad}' $T/o.md"
python3 "$B" --csv $D/stats_af.json > "$T/o.csv"; r=$?
chk "B-04 --csv: header + 5 metrics (hook absent in the fixture)" "[ $r = 0 ] && [ \$(wc -l < $T/o.csv) = 6 ]"
python3 "$B" $D/stats_notfinal.json > "$T/n.md" 2> "$T/n.txt"; r=$?
chk "B-05 negative control: not final -> refused (rc 2), no table" "[ $r = 2 ] && grep -q 'not a final' $T/n.txt && ! grep -q '^###' $T/n.md"
python3 "$B" $D/stats_old.json > "$T/m.md" 2> "$T/m.txt"; r=$?
chk "B-06 negative control: rt.isolated_cpus missing -> refused (rc 2)" "[ $r = 2 ] && grep -q 'isolated_cpus missing' $T/m.txt"
python3 "$B" $D/stats_af.json $D/stats_notfinal.json > "$T/x.md" 2> "$T/x.txt"; r=$?
chk "B-07 one good + one refused: the good one is printed, rc 2" "[ $r = 2 ] && grep -q '^### occupancy' $T/x.md"
echo "RESULT: $PASS pass, $FAIL fail"
[ $FAIL = 0 ]
