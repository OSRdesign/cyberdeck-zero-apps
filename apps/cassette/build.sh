#!/bin/sh
# Builds Cassette in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh <app>/build.sh cross [--arch arm64|amd64]   the device binary: root/usr/share/APPLaunch/bin/cassette (arm64)
#                                   or root.amd64/... (amd64), stripped, with usr/share/doc/cassette/build-info (both
#                                   untracked); libasound is loaded at run time (dlopen), never linked
#   wsl -e sh <app>/build.sh host   the UI helper tests (tests/ui), the binding tests (tests/bind: the mapping, the
#                                   actions, end to end on the demo's scripted sound system), then the headless scenes:
#                                   the UI's own on its synthetic model (tests/scenes) and the bound app on the scripted
#                                   sound system (tests/bind/scenes, CASSETTE_DEMO=1: no sound hardware, nothing played;
#                                   the files they record are checked afterwards). PNGs, the harness's and the app's
#                                   layout checks, contact sheets in $CDK_BUILD/host/out (CDK_BUILD default
#                                   ~/cdk-build/cassette)
#   wsl -e sh <app>/build.sh [all]  both
# Both build from a staged LF copy of the app in $CDK_BUILD/stage/cassette without src/core/alsa/tools (the
# cassette-probe CLI has a main() of its own; tests/alsa builds it). host compiles LVGL into $CDK_BUILD/_lvgl-host
# (LVBUILD; never the shared ~/cdk-build/_lvgl-host). The launcher checkout and tree come from LAUNCHER_SRC /
# LAUNCHER_TREE (LAUNCHER_TREE_AMD64 for --arch amd64) as for every app.
# The core tests alone, without LVGL: wsl -e sh <app>/tests/run.sh (and tests/<unit>/run.sh, tests/ui/run.sh,
# tests/bind/run.sh).
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
CDK_BUILD="${CDK_BUILD:-$HOME/cdk-build/cassette}"
LVBUILD="${LVBUILD:-$CDK_BUILD/_lvgl-host}"
export CDK_BUILD LVBUILD

# stage <dir> <with tests: 0|1>
stage() {
    rm -rf "$1"
    mkdir -p "$1"
    extra=
    [ -f "$here/cp0_config.mk" ] && extra=cp0_config.mk
    # shellcheck disable=SC2086
    (cd "$here" && tar cf - --exclude=src/core/alsa/tools app.json src $extra) | (cd "$1" && tar xf -)
    if [ "$2" = 1 ]; then
        (cd "$here" && tar cf - tests/scenes tests/ui tests/bind) | (cd "$1" && tar xf -)
        cp "$1"/tests/bind/scenes/*.scene "$1/tests/scenes/"
    fi
    find "$1" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name Makefile -o -name '*.scene' \
        -o -name '*.json' -o -name '*.txt' -o -name '*.sh' -o -name '*.mk' \) -exec sed -i 's/\r$//' {} +
}

# the device binary, built from the stage, copied back into the app's root (untracked)
cross() {
    st="$CDK_BUILD/stage-cross-$arch/cassette"
    stage "$st" 0
    bash "$here/../_common/build_app.sh" --name Cassette "$st" cross "$@"
    root=root
    [ "$arch" = amd64 ] && root=root.amd64
    mkdir -p "$here/$root/usr/share/APPLaunch/bin" "$here/$root/usr/share/doc/cassette"
    cp "$st/$root/usr/share/APPLaunch/bin/cassette" "$here/$root/usr/share/APPLaunch/bin/cassette"
    cp "$st/$root/usr/share/doc/cassette/build-info" "$here/$root/usr/share/doc/cassette/build-info"
    echo "== cassette: $here/$root/usr/share/APPLaunch/bin/cassette"
}

host() {
    st="$CDK_BUILD/stage/cassette"
    stage "$st" 1
    echo "== cassette: UI helper tests"
    make -s -C "$st/tests/ui" APP="$st" BUILD="$CDK_BUILD/ui-tests" -j3 test
    echo "== cassette: binding tests"
    make -s -C "$st/tests/bind" APP="$st" BUILD="$CDK_BUILD/bind-tests" -j3 test
    rc=0
    bash "$here/../_common/build_app.sh" --name Cassette "$st" host "$@" || rc=1
    echo "== cassette: files the bound scenes recorded"
    sh "$st/tests/bind/check_scene_files.sh" "$st/tests/bind/scenes/expect-files.txt" "$CDK_BUILD/host/out" || rc=1
    return $rc
}

# the arguments build_app.sh gets (its own mode word is passed by cross() / host())
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
