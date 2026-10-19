#!/usr/bin/env bash
# cc-status.sh <state>
#
# Push one Claude Code state to the AMOLED board over its USB-C serial port.
# Wire it into Claude Code hooks (see README.md). Valid states:
#   idle  working  waiting  error
#
# Port resolution: $CC_STATUS_PORT, else the first /dev/cu.usbmodem* (macOS)
# or /dev/ttyACM* (Linux). We write to the call-up (cu.*) device so opening it
# does NOT assert DTR and reset the board.
#
# This runs as a Claude Code hook, so it must never block or fail the session:
# every path exits 0, and problems are appended to a log instead of thrown.
set -u

state="${1:-}"
log="${CC_STATUS_LOG:-${TMPDIR:-/tmp}/cc-status.log}"

note() { printf '%s %s\n' "$(date '+%H:%M:%S')" "$*" >>"$log" 2>/dev/null || true; }

case "$state" in
  idle|working|waiting|error) ;;
  *) note "ignored invalid state: '$state'"; exit 0 ;;
esac

port="${CC_STATUS_PORT:-}"
if [[ -z "$port" ]]; then
  for p in /dev/cu.usbmodem* /dev/ttyACM*; do
    [[ -e "$p" ]] && { port="$p"; break; }
  done
fi

if [[ -z "$port" || ! -e "$port" ]]; then
  note "no board port found (set CC_STATUS_PORT); wanted to send '$state'"
  exit 0
fi

# One line, newline-terminated. Guard the write so a busy/held port can't hang
# the hook (the monitor and this script can't own the port at the same time).
if ! printf 'STATE:%s\n' "$state" >"$port" 2>>"$log"; then
  note "write failed on $port for '$state'"
fi
exit 0
