#!/bin/sh
# Builds Vinyl in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh <app>/build.sh cross [--arch arm64|amd64]   the device binary: root/usr/share/APPLaunch/bin/vinyl (arm64) or
#                                   root.amd64/... (amd64), stripped, with usr/share/doc/vinyl/build-info (both untracked).
#                                   No codec, sound or FFmpeg library is linked: libmpg123, libvorbisfile, libFLAC, libopus,
#                                   libasound, libpulse and FFmpeg are loaded at run time (dlopen), FFmpeg only when a file
#                                   needs it
#   wsl -e sh <app>/build.sh host   the unit tests (test-vinyl: core/ with the fakes, and the real libraries of the build box
#                                   where installed), then the headless scenes on the demonstration world (VINYL_DEMO=1: no
#                                   sound hardware, nothing played): PNGs, the harness's layout checks, contact sheets in
#                                   $CDK_BUILD/host/out (CDK_BUILD default ~/cdk-build/vinyl)
#   wsl -e sh <app>/build.sh [all]  both
# Both build from a staged LF copy of the app in $CDK_BUILD/stage/vinyl. host compiles LVGL into $CDK_BUILD/_lvgl-host
# (LVBUILD; never the shared ~/cdk-build/_lvgl-host). The launcher checkout and tree come from LAUNCHER_SRC /
# LAUNCHER_TREE (LAUNCHER_TREE_AMD64 for --arch amd64) as for every app.
# The core tests alone, without LVGL: wsl -e sh <app>/tests/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
mode=all
arch=arm64
prev=
for a in "$@"; do
    case $a in cross | host | all) mode=$a ;; esac
    [ "$prev" = --arch ] && arch=$a
    prev=$a
done
CDK_BUILD="${CDK_BUILD:-$HOME/cdk-build/vinyl}"
LVBUILD="${LVBUILD:-$CDK_BUILD/_lvgl-host}"
export CDK_BUILD LVBUILD

# stage <dir> <with tests: 0|1>
stage() {
    rm -rf "$1"
    mkdir -p "$1"
    (cd "$here" && tar cf - app.json src third_party) | (cd "$1" && tar xf -)
    if [ "$2" = 1 ]; then
        (cd "$here" && tar cf - tests root) | (cd "$1" && tar xf -)
    fi
    find "$1" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' -o -name Makefile -o -name '*.scene' \
        -o -name '*.json' -o -name '*.txt' -o -name '*.sh' -o -name '*.mk' -o -name '*.desktop' \) -exec sed -i 's/\r$//' {} +
}

cross() {
    st="$CDK_BUILD/stage-cross-$arch/vinyl"
    stage "$st" 0
    bash "$here/../_common/build_app.sh" --name Vinyl "$st" cross "$@"
    root=root
    [ "$arch" = amd64 ] && root=root.amd64
    mkdir -p "$here/$root/usr/share/APPLaunch/bin" "$here/$root/usr/share/doc/vinyl"
    cp "$st/$root/usr/share/APPLaunch/bin/vinyl" "$here/$root/usr/share/APPLaunch/bin/vinyl"
    cp "$st/$root/usr/share/doc/vinyl/build-info" "$here/$root/usr/share/doc/vinyl/build-info"
    echo "== vinyl: $here/$root/usr/share/APPLaunch/bin/vinyl"
}

host() {
    st="$CDK_BUILD/stage/vinyl"
    stage "$st" 1
    bash "$here/../_common/build_app.sh" --name Vinyl "$st" host "$@"
}

args=
for a in "$@"; do
    case $a in cross | host | all) ;; *) args="$args $a" ;; esac
done
# shellcheck disable=SC2086
case $mode in
    cross) cross $args ;;
    host) host $args ;;
    all) cross $args && host $args ;;
esac
