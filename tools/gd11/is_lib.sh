# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# shellcheck shell=bash
# ==========================================================================
# is_lib.sh -- Phase 11.5: shared verdict of one ecm_run log (sourced by
# run_is2_11_5.sh and run_is7_11_5.sh). The caller defines ok/bad.
#
#   er_verdict TAG LOG NOFRAME_MAX
#     rc file:   LOG.rc (written by the caller) must be 0
#     overrun:   [GROUP_MOTION] ... overrun=0
#     NOFRAME:   [WKC GROUP_MOTION] noframe <= NOFRAME_MAX
#     policy:    "[POLICY] final ... entered: ... LOST=0" (the ENTERED count;
#                part 2a grepped a bare 'LOST=0' somewhere in the log and
#                passed a run with 18 293 LOST)
#     DC:        "[DC] state=LOCKED ... unlocks=0"
#   er_axes_used TAG LOG    every [CIA402] axis line has used > 0 and
#                           underrun=0 (an axis never enabled is a FAIL)
# ==========================================================================
er_num () {   # LOG regex-with-one-group
    sed -nE "s/.*$2.*/\1/p" "$1" | head -1
}
er_verdict () {
    local tag=$1 E=$2 nmax=$3 rc ovr nof lost dcs unl
    rc=$(cat "$E.rc" 2>/dev/null)
    ovr=$(er_num "$E" '\[GROUP_MOTION\] cycles=[0-9]+ .*overrun=([0-9]+) ')
    nof=$(er_num "$E" '\[WKC GROUP_MOTION\] ok=[0-9]+ noframe=([0-9]+)')
    lost=$(er_num "$E" '\[POLICY\] final bus state [A-Z]+; entered: DEGRADED=[0-9]+ LOST=([0-9]+) ')
    dcs=$(er_num "$E" '\[DC\] state=([A-Z_]+) ')
    unl=$(er_num "$E" '\[DC\] state=[A-Z_]+ locks=[0-9]+ unlocks=([0-9]+)')
    if [ "${rc:-x}" = 0 ]; then ok "$tag rc 0"; else bad "$tag rc ${rc:-?}"; fi
    if [ "${ovr:-x}" = 0 ]; then ok "$tag overrun 0"; else bad "$tag overrun ${ovr:-?}"; fi
    if [ -n "$nof" ] && [ "$nof" -le "$nmax" ]; then ok "$tag NOFRAME $nof <= $nmax"; else bad "$tag NOFRAME ${nof:-?} > $nmax"; fi
    if [ "${lost:-x}" = 0 ]; then ok "$tag policy never entered LOST"; else bad "$tag policy entered LOST ${lost:-?} times"; fi
    if [ "${dcs:-x}" = LOCKED ] && [ "${unl:-x}" = 0 ]; then ok "$tag DC LOCKED, 0 unlocks"
    else bad "$tag DC ${dcs:-?} unlocks ${unl:-?}"; fi
}
er_axes_used () {
    local tag=$1 E=$2 n bad_ax
    n=$(grep -c '\[CIA402\] axis [0-9]* (' "$E")
    bad_ax=$(grep '\[CIA402\] axis [0-9]* (' "$E" | grep -E 'used=0 |underrun=[1-9]' | wc -l)
    if [ "$n" -gt 0 ] && [ "$bad_ax" = 0 ]; then ok "$tag $n axes: setpoints used, 0 underrun"
    else bad "$tag axes: $bad_ax of $n never used a setpoint or underran"; fi
}
