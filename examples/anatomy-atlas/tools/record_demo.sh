#!/usr/bin/env bash
# Record the atlas demo tour (test/demo.js) to a GIF for the README.
#
#   examples/anatomy-atlas/tools/record_demo.sh [out.gif] [height]
#
# Runs the real app on a private Xvfb display, grabs it with ffmpeg, trims to
# the DEMO_START/DEMO_END markers the script logs, then encodes a palette GIF.
# Needs: Xvfb, ffmpeg, a built anatomy_atlas (ideally with the full pack).
set -euo pipefail
export LC_ALL=C   # awk must print "8.7", not "8,7"

HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$ROOT/docs/assets/anatomy-atlas.gif}"
HEIGHT="${2:-420}"
FPS="${GIF_FPS:-10}"
COLORS="${GIF_COLORS:-128}"
APP="${ATLAS_BIN:-$ROOT/build/examples/anatomy-atlas/anatomy_atlas}"
W=1440 H=900 DISP=":${XVFB_DISPLAY:-97}"

TMP="$(mktemp -d)"
trap 'kill ${FF:-} ${XV:-} 2>/dev/null || true; rm -rf "$TMP"' EXIT

Xvfb "$DISP" -screen 0 ${W}x${H}x24 -nolisten tcp >/dev/null 2>&1 & XV=$!
sleep 1

T0=$(date +%s%3N)
ffmpeg -loglevel error -f x11grab -draw_mouse 0 -video_size ${W}x${H} -framerate 25 -i "$DISP" \
       -c:v libx264 -preset ultrafast -qp 0 "$TMP/raw.mkv" & FF=$!

# Same environment cleanup as run.sh, but pinned to X11: on a Wayland desktop
# GTK would otherwise open the window on the real screen (wayland-0).
env -u WAYLAND_DISPLAY -u GTK_PATH -u GTK_EXE_PREFIX -u GTK_IM_MODULE_FILE \
    GDK_BACKEND=x11 DISPLAY="$DISP" \
    GSETTINGS_SCHEMA_DIR=/usr/share/glib-2.0/schemas GIO_MODULE_DIR=/usr/lib/x86_64-linux-gnu/gio/modules \
    ANYAR_TEST_SCRIPT="$HERE/test/demo.js" ANYAR_TEST_TIMEOUT=240 ANYAR_CONSOLE=1 \
    "$APP" >"$TMP/app.log" 2>&1 || { cat "$TMP/app.log"; exit 1; }
kill -INT $FF; wait $FF || true; FF=

START=$(grep -o 'DEMO_START [0-9]*' "$TMP/app.log" | cut -d' ' -f2)
END=$(grep -o 'DEMO_END [0-9]*' "$TMP/app.log" | cut -d' ' -f2)
grep -o 'selected: .*' "$TMP/app.log" || true
SS=$(awk "BEGIN{print ($START-$T0)/1000}")
DUR=$(awk "BEGIN{print ($END-$START)/1000}")
echo "clip: start ${SS}s, ${DUR}s long"

F="fps=$FPS,scale=-2:$HEIGHT:flags=lanczos"
ffmpeg -loglevel error -y -ss "$SS" -t "$DUR" -i "$TMP/raw.mkv" \
       -vf "$F,split[a][b];[a]palettegen=max_colors=$COLORS:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" \
       "$OUT"
ls -l "$OUT"
