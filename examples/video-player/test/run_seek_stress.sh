#!/bin/bash
# Automated seek stress test against the REAL video-player UI.
#
# Usage: test/run_seek_stress.sh <video-file> [seeks=30] [--mode=pinhole|webgl] [--headless]
#
# Runs build/examples/video-player/video_player with VIDEO_PLAYER_DEBUG=1 and
# an injected driver (seek_stress.js).  Exit code 0 = audio kept playing after
# every seek.  The backend trace (seeks, presentation, STALL lines) is in the
# log printed at the end.  --headless runs under xvfb-run (no window, but
# note: WebKit media under Xvfb is less reliable than on a real desktop).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${VIDEO_PLAYER_BIN:-$ROOT/build/examples/video-player/video_player}"
FILE="$(realpath "$1")"; SEEKS="${2:-30}"; shift $(( $# >= 2 ? 2 : 1 ))
MODE="--mode=pinhole"; WRAP=()
for a in "$@"; do
  case "$a" in --mode=*) MODE="$a" ;; --headless) WRAP=(xvfb-run -a) ;; esac
done

SCRIPT="$(mktemp --suffix=.js)"; LOG="$(mktemp --suffix=.log)"
trap 'rm -f "$SCRIPT"' EXIT
printf 'window.__VP_TEST = %s;\n' "{\"file\": \"$FILE\", \"seeks\": $SEEKS, \"seed\": ${SEED:-12345}, \"noWatchdog\": ${NO_WATCHDOG:-false}, \"measureStart\": ${MEASURE_START:-false}}" > "$SCRIPT"
cat "$HERE/seek_stress.js" >> "$SCRIPT"

unset GTK_PATH GTK_EXE_PREFIX GTK_IM_MODULE_FILE GDK_BACKEND
export GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas GIO_MODULE_DIR=/usr/lib/x86_64-linux-gnu/gio/modules
cd "$(dirname "$BIN")"
# Silence ONLY this test instance's audio on the real output device (the
# device matters for reproducing WebKit media stalls, the speakers don't).
# Finds WebKit processes whose environment carries our unique test script.
mute_loop() {
  declare -A done_ids=()
  while sleep 0.05; do
    pids=$(grep -l "VIDEO_PLAYER_TEST_SCRIPT=$SCRIPT" /proc/[0-9]*/environ 2>/dev/null | cut -d/ -f3 | tr '\n' '|')
    [ -z "$pids" ] && continue
    pactl list sink-inputs 2>/dev/null | awk -v P="|${pids}" '
      /^Sink Input #/ { id = substr($3, 2) }
      /application.process.id/ { gsub(/"/, "", $3); if (index(P, "|" $3 "|")) print id }' |
      while read -r id; do echo "$id"; done > "$LOG.ids"
    while read -r id; do
      [ -n "${done_ids[$id]:-}" ] && continue
      pactl set-sink-input-mute "$id" 1 2>/dev/null && done_ids[$id]=1
    done < "$LOG.ids"
  done
}
MUTER=""
if [ -n "${MUTE_STREAM:-}" ] && command -v pactl >/dev/null; then mute_loop & MUTER=$!; fi

set +e
VIDEO_PLAYER_DEBUG=1 VIDEO_PLAYER_TEST_SCRIPT="$SCRIPT" timeout 600 "${WRAP[@]}" "$BIN" "$MODE" > "$LOG" 2>&1
RC=$?
set -e
[ -n "$MUTER" ] && kill "$MUTER" 2>/dev/null || true
grep -E '^\[test\]' "$LOG" || true
echo "exit=$RC  full log (backend trace): $LOG"
exit $RC
