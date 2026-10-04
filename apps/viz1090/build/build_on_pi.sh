#!/bin/sh
# Rebuilds the binaries of the viz1090 package on a Raspberry Pi (64-bit Raspberry Pi OS, Debian 13) and
# stages them in ../root/, ready for  python3 tools/make_registry.py  at the root of this repository.
#
#     sh build_on_pi.sh
#
# Builds from their upstream repositories, at the commits the package was tested with:
#   viz1090 (BSD)   https://github.com/nmatsuda/viz1090    fb7bf016d5d95ddca5b27f260e871484864b5c96
#   readsb  (GPL-3) https://github.com/wiedehopf/readsb     094720939c01943de82b14df6f42f67fff1cd514
# and the display bridge (libviz_fb.so) and intro screen (vizsetup) from this folder.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/../root/opt/viz1090"
WORK=${WORK:-/var/tmp/viz1090-build}

sudo apt-get update
sudo apt-get install -y --no-install-recommends build-essential git pkg-config \
    libsdl2-dev libsdl2-ttf-dev libsdl2-gfx-dev librtlsdr-dev \
    libncurses-dev zlib1g-dev libzstd-dev libusb-1.0-0-dev libfreetype-dev

mkdir -p "$WORK"
clone() {   # name url commit
    [ -d "$WORK/$1/.git" ] || git clone -q "$2" "$WORK/$1"
    git -C "$WORK/$1" fetch -q origin
    git -C "$WORK/$1" checkout -q -f "$3"
}
clone viz1090 https://github.com/nmatsuda/viz1090.git fb7bf016d5d95ddca5b27f260e871484864b5c96
clone readsb https://github.com/wiedehopf/readsb.git 094720939c01943de82b14df6f42f67fff1cd514

# Tweak for this small screen / slow frame rate (sed, because upstream uses Windows line endings):
# labels averaged their position over 15 frames and moved 1 px per step, so after a pan they took seconds
# to come back to their plane.
sed -i 's/int buffer_length = 15;/int buffer_length = 3;/; s/float velocity_limit = 1.0f;/float velocity_limit = 6.0f;/; s/float damping_force = 0.65f;/float damping_force = 0.5f;/' \
    "$WORK/viz1090/AircraftLabel.h"
( cd "$WORK/readsb" && make -j2 RTLSDR=yes )
( cd "$WORK/viz1090" && make -j2 )

# display bridge and intro screen
gcc -shared -fPIC -O2 -Wall "$HERE/viz_fb_shim.c" "$HERE/cp0_statusbar.c" -I"$HERE" \
    $(sdl2-config --cflags) $(pkg-config --cflags --libs freetype2) -lSDL2_ttf -ldl -lm -o "$WORK/libviz_fb.so"
g++ -O2 -std=c++11 -Wall "$HERE/vizsetup.cpp" -o "$WORK/vizsetup" \
    $(sdl2-config --cflags) -lSDL2 -lSDL2_ttf -lSDL2_gfx -lpthread

mkdir -p "$ROOT/font"
cp "$WORK/viz1090/viz1090" "$WORK/readsb/readsb" "$WORK/libviz_fb.so" "$WORK/vizsetup" "$ROOT/"
cp "$WORK/viz1090/font/"*.ttf "$ROOT/font/"
cp "$HERE/Montserrat-Medium.ttf" "$HERE/FontAwesome5-Solid+Brands+Regular.woff" "$ROOT/font/"
cp "$HERE/run_viz1090.sh" "$HERE/extract_map.py" "$ROOT/"
[ -f "$HERE/world-map.bin" ] && cp "$HERE/world-map.bin" "$ROOT/"      # produced by make_world.py on a PC
echo "staged in $ROOT - now run: python3 tools/make_registry.py"
