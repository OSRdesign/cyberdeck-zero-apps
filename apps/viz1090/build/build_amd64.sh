#!/bin/sh
# Builds the amd64 binaries of the viz1090 package (viz1090, readsb, the display bridge libviz_fb.so) in WSL (Ubuntu
# 24.04, x86_64) and stages them in ../root.amd64/opt/viz1090/, the overlay tools/make_registry.py lays over root/.
# No sudo, no board: a private root of -dev packages is filled with "apt-get download" + "dpkg -x".
#
#     sh build_amd64.sh                 build in $WORK (default ~/viz1090-amd64-build) from a checkout on the WSL
#                                       file system (not /mnt/c: compile there is slow and loses file modes)
#
# Same upstream commits and the same AircraftLabel.h change as build_on_pi.sh (see there):
#   viz1090 (BSD)   https://github.com/nmatsuda/viz1090    fb7bf016d5d95ddca5b27f260e871484864b5c96
#   readsb  (GPL-3) https://github.com/wiedehopf/readsb     094720939c01943de82b14df6f42f67fff1cd514
# readsb is unmodified; the bridge is built from this folder (with ../src/core/viz_screen.h).
#
# The one trap: the build box's librtlsdr2 (Ubuntu 24.04, SONAME librtlsdr.so.2) is not what the tablet (Ubuntu 26.04)
# and the Pis (Debian 13) have (librtlsdr0, librtlsdr.so.0). readsb is therefore linked against the 26.04 librtlsdr0
# and librtlsdr-dev (two small files of the Ubuntu archive, pinned by sha256 below); the script checks the NEEDED
# entry. viz1090 is linked with --allow-shlib-undefined (24.04's libSDL2 hard-links pulse, samplerate, Xss ... whose own
# dependencies are not in the private root); the headless smoke run at the end is the check. The three binaries are
# stripped (arm64 ships them with debug info).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/../root.amd64/opt/viz1090"
WORK=${WORK:-$HOME/viz1090-amd64-build}
JOBS=${JOBS:-3}
DEVROOT=$WORK/devroot
R=$DEVROOT/root
LIBDIR=$R/usr/lib/x86_64-linux-gnu
VIZ_COMMIT=fb7bf016d5d95ddca5b27f260e871484864b5c96
READSB_COMMIT=094720939c01943de82b14df6f42f67fff1cd514
POOL=http://archive.ubuntu.com/ubuntu/pool/universe/r/rtl-sdr
RTL_VER=2.0.2-2build1
RTL0_SHA=8c9e101f0d70fab3c08a4f1ea76120895c9089a2b0998ff672e6e940f87fb4de
RTLDEV_SHA=63135870280652c8ba33bde868d538722887f3c24041265391d0e643a25b15e2
GLIBC_FLOOR=2.38

[ "$(uname -m)" = x86_64 ] || { echo "build_amd64.sh: an x86_64 build box is needed" >&2; exit 2; }
case "$HERE" in /mnt/*) echo "build_amd64.sh: run from a checkout on the WSL file system, not $HERE" >&2; exit 2 ;; esac
mkdir -p "$WORK/debs" "$WORK/debs-rtl" "$R"

# ---------------------------------------------------------------- the private root of -dev packages
if [ ! -f "$DEVROOT/done" ]; then
    ( cd "$WORK/debs" && apt-get download libsdl2-dev libsdl2-2.0-0 libsdl2-ttf-dev libsdl2-ttf-2.0-0 \
        libsdl2-gfx-dev libsdl2-gfx-1.0-0 libzstd-dev )
    for f in librtlsdr0 librtlsdr-dev; do
        [ -f "$WORK/debs-rtl/${f}_${RTL_VER}_amd64.deb" ] || \
            curl -fsSL -o "$WORK/debs-rtl/${f}_${RTL_VER}_amd64.deb" "$POOL/${f}_${RTL_VER}_amd64.deb"
    done
    ( cd "$WORK/debs-rtl" && printf '%s  librtlsdr0_%s_amd64.deb\n%s  librtlsdr-dev_%s_amd64.deb\n' \
        "$RTL0_SHA" "$RTL_VER" "$RTLDEV_SHA" "$RTL_VER" | sha256sum -c - )
    for d in "$WORK"/debs/*.deb "$WORK"/debs-rtl/*.deb; do dpkg -x "$d" "$R"; done
    touch "$DEVROOT/done"
fi
ls "$LIBDIR"/librtlsdr.so.0 "$LIBDIR"/libSDL2.so "$LIBDIR"/libSDL2_ttf.so "$LIBDIR"/libSDL2_gfx.so > /dev/null

# ---------------------------------------------------------------- upstream sources at the pinned commits
clone() {   # name url commit
    [ -d "$WORK/$1/.git" ] || git clone -q "$2" "$WORK/$1"
    git -C "$WORK/$1" fetch -q origin
    git -C "$WORK/$1" checkout -q -f "$3"
    git -C "$WORK/$1" clean -qfdx
}
clone viz1090 https://github.com/nmatsuda/viz1090.git "$VIZ_COMMIT"
clone readsb https://github.com/wiedehopf/readsb.git "$READSB_COMMIT"
# the same change as build_on_pi.sh (labels settle quickly after a pan)
sed -i 's/int buffer_length = 15;/int buffer_length = 3;/; s/float velocity_limit = 1.0f;/float velocity_limit = 6.0f;/; s/float damping_force = 0.65f;/float damping_force = 0.5f;/' \
    "$WORK/viz1090/AircraftLabel.h"

# ---------------------------------------------------------------- build
RPATH="-L$LIBDIR -Wl,-rpath-link,$LIBDIR"
( cd "$WORK/readsb" && make -j"$JOBS" RTLSDR=yes RTLSDR_PREFIX="$R/usr" LDFLAGS="$RPATH" )
# (the upstream Makefile's rules ignore EXTRACFLAGS: the compile flags are given whole, the private root added)
VIZINC="-O2 -g -I$R/usr/include -I$R/usr/include/SDL2 -I$R/usr/include/x86_64-linux-gnu -D_REENTRANT"
( cd "$WORK/viz1090" && make -j"$JOBS" CFLAGS="$VIZINC" CXXFLAGS="-std=c++11 $VIZINC" \
    LDFLAGS="$RPATH -Wl,--allow-shlib-undefined" )
gcc -shared -fPIC -O2 -Wall "$HERE/viz_fb_shim.c" "$HERE/cp0_fb_output.c" "$HERE/cp0_statusbar.c" \
    "$HERE/cp0_ui_metrics.c" "$HERE/cp0_ui_roller.c" "$HERE/cp0_ui_status_strip.c" "$HERE/cp0_power_supply.c" \
    "$HERE/cp0_app_contract.c" -I"$HERE" -I"$HERE/../src/core" -I"$R/usr/include" -I"$R/usr/include/SDL2" -I"$R/usr/include/x86_64-linux-gnu" -D_REENTRANT \
    $(pkg-config --cflags --libs freetype2) $RPATH -lSDL2_ttf -ldl -lm -Wl,--allow-shlib-undefined \
    -o "$WORK/libviz_fb.so"

# ---------------------------------------------------------------- strip, check, stage
mkdir -p "$WORK/stage"
strip -o "$WORK/stage/viz1090" "$WORK/viz1090/viz1090"
strip -o "$WORK/stage/readsb" "$WORK/readsb/readsb"
strip --strip-unneeded -o "$WORK/stage/libviz_fb.so" "$WORK/libviz_fb.so"

fail=0
for f in viz1090 readsb libviz_fb.so; do
    p=$WORK/stage/$f
    readelf -h "$p" | grep -q 'Machine:.*X86-64' || { echo "FAIL $f: not x86-64" >&2; fail=1; }
    top=$(readelf -V "$p" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
    [ "$(printf '%s\n%s\n' "$top" "$GLIBC_FLOOR" | sort -V | tail -1)" = "$GLIBC_FLOOR" ] || \
        { echo "FAIL $f: needs GLIBC_$top, floor is $GLIBC_FLOOR" >&2; fail=1; }
    echo "$f: needs $(readelf -d "$p" | grep -o 'NEEDED.*\[.*\]' | sed 's/.*\[//; s/\]//' | tr '\n' ' ') (GLIBC_$top)"
done
readelf -d "$WORK/stage/readsb" | grep -q 'librtlsdr\.so\.0\]' || { echo "FAIL readsb: no librtlsdr.so.0 in NEEDED" >&2; fail=1; }
readelf -d "$WORK/stage/readsb" | grep -q 'librtlsdr\.so\.2' && { echo "FAIL readsb: links librtlsdr.so.2" >&2; fail=1; }
readelf -d "$WORK/stage/libviz_fb.so" | grep -q 'libSDL2' && { echo "FAIL libviz_fb.so: links SDL" >&2; fail=1; }
[ "$fail" = 0 ] || exit 1

mkdir -p "$OUT"
cp "$WORK/stage/viz1090" "$WORK/stage/readsb" "$WORK/stage/libviz_fb.so" "$OUT/"
echo "staged in $OUT (vizsetup: ../build.sh cross --arch amd64)"
