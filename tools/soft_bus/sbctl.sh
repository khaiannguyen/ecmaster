#!/bin/bash
# ==========================================================================
# sbctl.sh — send one fault-injection command to a running soft_bus.
#
#   ./sbctl.sh mute 100
#   ./sbctl.sh bad_cable 3 10
#   ./sbctl.sh status            (output appears in soft_bus's own log)
#   SB_CTL=/path/to/fifo ./sbctl.sh ...   (if soft_bus was started with --ctl)
#
# Writes with dd conv=nocreat instead of "echo > fifo": a shell redirection
# opens with O_CREAT, which fs.protected_fifos=1 (Ubuntu default) refuses for
# a FIFO in /tmp owned by another user, e.g. when soft_bus runs under sudo
# and this script does not (or the other way round). Also refuses to create
# a regular file when soft_bus is not running.
# ==========================================================================
CTL=${SB_CTL:-/tmp/soft_bus.ctl}
[ $# -ge 1 ] || { echo "usage: $0 <command> [args...]   (see esc_fault.h)"; exit 2; }
[ -p "$CTL" ] || { echo "no control FIFO at $CTL — is soft_bus running?"; exit 1; }
printf '%s\n' "$*" | dd of="$CTL" conv=nocreat,notrunc oflag=nonblock status=none \
    || { echo "write to $CTL failed"; exit 1; }
