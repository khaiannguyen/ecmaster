#!/usr/bin/env bash
# ==========================================================================
# negative_control.sh — Giai doan 7.5: prove the golden check can fail.
# Builds a copy of ecm_run with the two SM watchdog writes (0x0400, 0x0420)
# swapped -- the same frames, only their order changes -- and requires
# check_golden.sh to see the difference.
#
#   sudo -E SOEM_DIR=... ./negative_control.sh
# ==========================================================================
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=$(mktemp -d /tmp/golden_negctl_XXXXXX)
cp -r "$ROOT/apps" "$ROOT/libecmaster" "$WORK/"
python3 - "$WORK/apps/ecm_run/ecm_run.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
a = ("    if (ecx_FPWR(&ctx.port, configadr, REG_SM_WD_DIVIDER, sizeof(divider), &divider, EC_TIMEOUTRET) <= 0) fail++;\n"
     "    if (ecx_FPWR(&ctx.port, configadr, REG_SM_WD_TIME_PROCDATA, sizeof(wd_time), &wd_time, EC_TIMEOUTRET) <= 0) fail++;\n")
if a not in s:
    sys.exit("negative_control.sh: the SM watchdog writes in ecm_run.c changed, update this script")
b = "\n".join(reversed(a.rstrip("\n").split("\n"))) + "\n"
open(p, "w").write(s.replace(a, b))
PY
make -s -C "$WORK/apps/ecm_run" SOEM_DIR="${SOEM_DIR:-$HOME/projects/SOEM}" >/dev/null
ECM_RUN="$WORK/apps/ecm_run/ecm_run" EXPECT_DIFF=1 "$HERE/check_golden.sh"
rc=$?
rm -rf "$WORK"
exit $rc
