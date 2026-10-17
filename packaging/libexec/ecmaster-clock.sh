#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 khaiannguyen
# SPDX-License-Identifier: GPL-3.0-only
# ==========================================================================
# ecmaster-clock.sh -- Phase 12.2: the clock set-up of `ecm_run --link etf`
# (ecmaster-clock@IFACE.service), as in docs/i226.md:
#
#   ecmaster-clock.sh pre IFACE    remember whether systemd-timesyncd runs,
#                                  stop it (an NTP step would move TAI under
#                                  the launch times)
#   ecmaster-clock.sh run IFACE    exec phc2sys: the PHC of IFACE follows
#                                  CLOCK_REALTIME
#   ecmaster-clock.sh post IFACE   start systemd-timesyncd again if, and only
#                                  if, it was running before `pre`
#
# Phase 11: timesyncd stopped by hand and never restarted left the Jetson
# clock 2 days off; `post` is that fix. Without ETF this unit is not needed.
#
# Env: PHC2SYS (default phc2sys), PHC2SYS_ARGS (replaces the default
#      options), SYSTEMCTL, ECM_STATE_DIR (default /run/ecmaster)
# ==========================================================================
set -u
SYSTEMCTL=${SYSTEMCTL:-systemctl}
PHC2SYS=${PHC2SYS:-phc2sys}
STATE_DIR=${ECM_STATE_DIR:-/run/ecmaster}
[ $# = 2 ] || { echo "usage: ecmaster-clock.sh pre|run|post IFACE" >&2; exit 2; }
CMD=$1; IF=$2
[[ "$IF" =~ ^[A-Za-z0-9_.:-]{1,15}$ ]] || { echo "ecmaster-clock: bad interface name '$IF'" >&2; exit 2; }
ST=$STATE_DIR/clock-$IF.timesyncd

case $CMD in
pre)
    mkdir -p "$STATE_DIR"
    if [ -s "$ST" ]; then
        echo "ecmaster-clock: timesyncd state already saved ($(cat "$ST")), kept"
    elif $SYSTEMCTL is-active --quiet systemd-timesyncd; then
        echo active > "$ST"
    else
        echo inactive > "$ST"
    fi
    $SYSTEMCTL stop systemd-timesyncd 2>/dev/null
    echo "ecmaster-clock: systemd-timesyncd stopped (was $(cat "$ST"))"
    ;;
run)
    # shellcheck disable=SC2086
    exec $PHC2SYS ${PHC2SYS_ARGS:--s CLOCK_REALTIME -c $IF -O 0 -m --step_threshold=1}
    ;;
post)
    s=$(cat "$ST" 2>/dev/null)
    if [ "$s" = active ]; then
        $SYSTEMCTL start systemd-timesyncd && echo "ecmaster-clock: systemd-timesyncd started again"
    else
        echo "ecmaster-clock: systemd-timesyncd was ${s:-not recorded} before, left stopped"
    fi
    rm -f "$ST"
    ;;
*) echo "usage: ecmaster-clock.sh pre|run|post IFACE" >&2; exit 2 ;;
esac
