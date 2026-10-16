#!/bin/sh
# viz1090 tile: the intro screen (position, receivers), then viz1090 full screen.
# Your position and options: ~/.config/cardputerzero/viz1090.conf (written by the intro screen)
#     LAT=48.8566  LON=2.3522  UISCALE=2  METRIC=1  RADIUS=500
# (AIRPORT= and RECENT1= .. RECENT5= are the intro screen's: the airport of the position, the last places used.)
# Without UISCALE in the file, viz1090's text size follows the screen's density: 1 below 7.5 px/mm, 2 up to
# 14 px/mm (the deck), 3 above (and never more than 1 per 240 screen rows).
# VIZ_TITLE: the display bridge draws the app header above the map (title, the Esc button, the status strip), as the
# intro screen and every native page have it.
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy VIZ_TITLE=viz1090
BRIDGE=/opt/viz1090/libviz_fb.so

# The screen, from the launcher's app contract (else screen.state, else the framebuffer; 640x480 as the last resort):
# width, height, the uiscale for its density, the density (px per mm x100), where the size came from.
set -- $(/opt/viz1090/vizsetup --screen 2>/dev/null)
VIZ_W=${1:-640} VIZ_H=${2:-480} AUTO_UISCALE=${3:-2}
echo "[viz1090] screen ${VIZ_W}x${VIZ_H} (${5:-fallback}), density ${4:-1130} (px/mm x100), uiscale ${AUTO_UISCALE}" >&2

# intro screen (a native app: it draws the panel itself, without the bridge): exits 0 to start, 1 when cancelled
# (or on a screen smaller than 640x480: a notice only)
/opt/viz1090/vizsetup || exit 0

CONF="${XDG_CONFIG_HOME:-$HOME/.config}/cardputerzero/viz1090.conf"
[ -f "$CONF" ] && . "$CONF"
: "${LAT:=48.8566}" "${LON:=2.3522}" "${UISCALE:=$AUTO_UISCALE}" "${RADIUS:=500}"

# viz1090 reads its map files from the current directory: cut a window of the world map around the position
# (under a second) and run from there. The same radius suits every screen shape: viz1090 maps its zoom distance to
# half the longer side, so a square window of RADIUS nautical miles covers the whole screen at any zoom up to it.
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/viz1090"
mkdir -p "$CACHE" || exit 1
KEY="$LAT,$LON,$RADIUS"
if [ "$(cat "$CACHE/center" 2>/dev/null)" != "$KEY" ] || [ ! -f "$CACHE/mapdata.bin" ]; then
    python3 /opt/viz1090/extract_map.py --world /opt/viz1090/world-map.bin --lat "$LAT" --lon "$LON" \
        --radius "$RADIUS" --out "$CACHE" >/dev/null 2>&1 && echo "$KEY" > "$CACHE/center"
fi
ln -sfn /opt/viz1090/font "$CACHE/font"
cd "$CACHE" || exit 1

# The map's window: the screen under the header (its height comes from the launcher's layout service; the bridge
# computes it and prints "width height header" without starting viz1090).
set -- $(VIZ_PRINT_MAP=1 LD_PRELOAD=$BRIDGE /opt/viz1090/viz1090 2>/dev/null)
MAP_W=${1:-$VIZ_W} MAP_H=${2:-$((VIZ_H - 56))}
echo "[viz1090] map ${MAP_W}x${MAP_H} under a ${3:-56} px header" >&2

# the decoder runs only while viz1090 is on screen
/opt/viz1090/readsb --device-type rtlsdr --gain auto --quiet --net --net-bo-port 30005 \
    --net-ri-port 0 --net-ro-port 0 --net-sbs-port 0 --net-bi-port 0 --net-api-port 0 >/dev/null 2>&1 &
DECODER=$!
trap 'kill $DECODER 2>/dev/null' EXIT INT TERM HUP
# viz1090 ends on the header's Esc button, the Esc key, SIGTERM (the launcher's hold-Esc), SIGINT or SIGHUP
LD_PRELOAD=$BRIDGE /opt/viz1090/viz1090 --screensize "$MAP_W" "$MAP_H" --uiscale "$UISCALE" \
    --lat "$LAT" --lon "$LON" ${METRIC:+--metric}
