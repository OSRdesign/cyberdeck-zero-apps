#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the VHS unit tests in WSL from a native-filesystem LF copy (never builds on /mnt/c), -Werror, -j2, nice 15:
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-vhs-p1/apps/vhs/tests/run.sh [SAN=address,undefined]
# The real TurboJPEG: the build box's libturbojpeg.so.0 when installed, else VHS_TURBOJPEG (a path), else a private copy
# unpacked from the distribution's package without installing it (~/cdk-build/vhs/tj, or the spike's
# ~/cdk-build/vhs-spike/tj):
#   mkdir -p ~/cdk-build/vhs/tj && cd ~/cdk-build/vhs/tj && apt-get download libturbojpeg && dpkg-deb -x libturbojpeg_*.deb root
# The MJPEG fixture is made with the build box's ffmpeg (testsrc2, 1280x720, 12 frames: synthetic); without ffmpeg its
# cases skip. The recorder's fixtures (M3) use the build box's libavformat (FFmpeg 6.1 in WSL) and check the files with
# ffprobe / ffmpeg; without them those checks say so loudly.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/vhs-tests-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src tests) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name Makefile \) -exec sed -i 's/\r$//' {} +
nice -n 15 make -s -C "$work/app/tests" APP="$work/app" BUILD="$work/build" -j2 "$@" all
fx="$work/fixtures"
mkdir -p "$fx"
if command -v ffmpeg > /dev/null 2>&1; then
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -frames:v 12 -q:v 4 -f mjpeg \
        "$fx/testsrc-720.mjpg"
fi
if [ -z "${VHS_TURBOJPEG:-}" ] && ! ldconfig -p 2> /dev/null | grep -q 'libturbojpeg.so.0 '; then
    for d in "$HOME/cdk-build/vhs/tj" "$HOME/cdk-build/vhs-spike/tj"; do
        p="$d/root/usr/lib/x86_64-linux-gnu/libturbojpeg.so.0"
        if [ -f "$p" ]; then
            VHS_TURBOJPEG="$p"
            export VHS_TURBOJPEG
            break
        fi
    done
fi
echo "== vhs unit tests (fakes; TurboJPEG: ${VHS_TURBOJPEG:-the system's, if any}; fixtures $fx)"
if command -v ffprobe > /dev/null 2>&1 && command -v ffmpeg > /dev/null 2>&1; then
    echo "== the recorder fixtures are checked with $(ffprobe -version | head -1 | cut -d' ' -f1-3)"
else
    echo "== ffprobe / ffmpeg MISSING: the recorder's ffprobe checks will NOT run (apt install ffmpeg)"
fi
case "$*" in
*SAN=thread*)
    # FFmpeg's uninstrumented decoder threads (tests/tsan.supp: frames inside FFmpeg's libraries only)
    TSAN_OPTIONS="suppressions=$work/app/tests/tsan.supp ${TSAN_OPTIONS:-}"
    export TSAN_OPTIONS
    ;;
esac
(cd "$app" && tar cf - tests/tsan.supp) | (cd "$work/app" && tar xf -)
# VHS_GROUPS="record recorder" runs only those groups
# shellcheck disable=SC2086
VHS_FIXTURES="$fx" VHS_SETTINGS="$work/no-settings.json" "$work/build/test-vhs" ${VHS_GROUPS:-}
