#!/bin/sh
# Builds VHS in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh <app>/build.sh cross [--arch arm64|amd64]   the device binary: root/usr/share/APPLaunch/bin/vhs (arm64) or
#                                   root.amd64/... (amd64), stripped, with usr/share/doc/vhs/build-info (both untracked).
#                                   libturbojpeg is not linked: it is loaded at run time (dlopen)
#   wsl -e sh <app>/build.sh host   the unit tests (test-vhs: core/ with the fake camera), then the headless scenes on the
#                                   fake camera (VHS_FAKE): PNGs, the harness's layout checks, contact sheets in
#                                   $CDK_BUILD/host/out (CDK_BUILD default ~/cdk-build/vhs). The MJPEG scenes need a
#                                   libturbojpeg.so.0: the build box's, VHS_TURBOJPEG, or the private copy of
#                                   tests/run.sh (~/cdk-build/vhs/tj, else the spike's ~/cdk-build/vhs-spike/tj)
#   wsl -e sh <app>/build.sh [all]  both
# Both build from a staged LF copy of the app in $CDK_BUILD/stage/vhs. host compiles LVGL into $CDK_BUILD/_lvgl-host
# (LVBUILD; never the shared ~/cdk-build/_lvgl-host). The launcher checkout and tree come from LAUNCHER_SRC /
# LAUNCHER_TREE (LAUNCHER_TREE_AMD64 for --arch amd64) as for every app.
# The core tests alone, without LVGL (-Werror, sanitizers on request): wsl -e sh <app>/tests/run.sh
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
CDK_BUILD="${CDK_BUILD:-$HOME/cdk-build/vhs}"
LVBUILD="${LVBUILD:-$CDK_BUILD/_lvgl-host}"
export CDK_BUILD LVBUILD

# stage <dir> <with tests: 0|1>
stage() {
    rm -rf "$1"
    mkdir -p "$1"
    (cd "$here" && tar cf - app.json src) | (cd "$1" && tar xf -)
    if [ "$2" = 1 ]; then
        (cd "$here" && tar cf - tests root) | (cd "$1" && tar xf -)
    fi
    find "$1" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' -o -name Makefile -o -name '*.scene' \
        -o -name '*.json' -o -name '*.txt' -o -name '*.sh' -o -name '*.mk' -o -name '*.desktop' \) -exec sed -i 's/\r$//' {} +
}

cross() {
    st="$CDK_BUILD/stage-cross-$arch/vhs"
    stage "$st" 0
    bash "$here/../_common/build_app.sh" --name Vhs "$st" cross "$@"
    root=root
    [ "$arch" = amd64 ] && root=root.amd64
    mkdir -p "$here/$root/usr/share/APPLaunch/bin" "$here/$root/usr/share/doc/vhs"
    cp "$st/$root/usr/share/APPLaunch/bin/vhs" "$here/$root/usr/share/APPLaunch/bin/vhs"
    cp "$st/$root/usr/share/doc/vhs/build-info" "$here/$root/usr/share/doc/vhs/build-info"
    echo "== vhs: $here/$root/usr/share/APPLaunch/bin/vhs"
}

host() {
    st="$CDK_BUILD/stage/vhs"
    stage "$st" 1
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
    echo "== vhs host: TurboJPEG ${VHS_TURBOJPEG:-the system's, if any}"
    bash "$here/../_common/build_app.sh" --name Vhs "$st" host "$@"
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
