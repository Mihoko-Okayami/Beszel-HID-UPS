#!/usr/bin/env bash
set -uo pipefail

BIN=$1
WORK=$(mktemp -d /tmp/beszel-hid-ups.XXXXXX)
LOG=$WORK/daemon.log
FAILED=0
PID=

ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; FAILED=$((FAILED + 1)); }
expect() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
wait_log() { for _ in $(seq 100); do grep -q "$1" "$LOG" && return 0; sleep 0.1; done; return 1; }
trap '[ -z "$PID" ] || kill -KILL "$PID" 2>/dev/null' EXIT

timeout -k 5 10 "$BIN" --help 2>"$WORK/help.log"
STATUS=$?
expect "--help prints the usage and fails" '[ "$STATUS" = 1 ] && grep -q "usage: " "$WORK/help.log"'
timeout -k 5 10 "$BIN" -x onlinedischarge 2>"$WORK/option.log"
STATUS=$?
expect "an unknown -x option prints the usage and fails" '[ "$STATUS" = 1 ] && grep -q "usage: " "$WORK/option.log"'
mkdir -p "$WORK/out/ups"
echo 87 >"$WORK/out/ups/capacity"
"$BIN" -x onlinedischarge_onbattery "$WORK/out" 2>"$LOG" &
PID=$!
expect "starts without a UPS" 'wait_log "no UPS found"'
expect "removes the capacity left by an earlier run" '[ ! -e "$WORK/out/ups/capacity" ]'
expect "is still running and accepts SIGTERM" 'kill -TERM "$PID"'
timeout 15 tail --pid="$PID" -f /dev/null && wait "$PID"
STATUS=$?
kill -0 "$PID" 2>/dev/null || PID=
expect "stops on SIGTERM with status 0" '[ "$STATUS" = 0 ]'
expect "logs that it stopped" 'grep -q "stopped$" "$LOG"'
expect "no sanitizer report" '! grep -q "Sanitizer\|runtime error" "$WORK"/*.log'
[ "$FAILED" = 0 ] || { tail -n +1 "$WORK"/*.log; exit 1; }
