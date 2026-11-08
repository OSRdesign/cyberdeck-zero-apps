#!/bin/sh
# Builds Cassette in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh <app>/build.sh host   the UI helper tests (tests/ui) and the headless scenes (tests/scenes, a synthetic
#                                   model, no sound hardware): PNGs, the harness's and the app's layout checks, contact
#                                   sheets in $CDK_BUILD/host/out (CDK_BUILD default ~/cdk-build/cassette)
# host builds a staged LF copy of the UI alone (src/ui: the UI runs on its synthetic model until the controller is
# bound; the core units need their own include paths and src/core/alsa/tools has a main() of its own) in
# $CDK_BUILD/stage/cassette, and compiles LVGL into $CDK_BUILD/_lvgl-host (LVBUILD; never the shared
# ~/cdk-build/_lvgl-host). The launcher checkout and tree come from LAUNCHER_SRC / LAUNCHER_TREE as for every app.
# No device binary yet (cross): the packaging task adds the device main.
# The core tests alone, without LVGL: wsl -e sh <app>/tests/run.sh (and tests/<unit>/run.sh, tests/ui/run.sh).
set -e
here=$(cd "$(dirname "$0")" && pwd)
mode=all
for a in "$@"; do
    case $a in cross | host | all) mode=$a ;; esac
done
if [ "$mode" = host ]; then
    CDK_BUILD="${CDK_BUILD:-$HOME/cdk-build/cassette}"
    LVBUILD="${LVBUILD:-$CDK_BUILD/_lvgl-host}"
    export CDK_BUILD LVBUILD
    stage="$CDK_BUILD/stage/cassette"
    rm -rf "$stage"
    mkdir -p "$stage"
    (cd "$here" && tar cf - app.json src/ui tests/scenes tests/ui) | (cd "$stage" && tar xf -)
    find "$stage" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name Makefile -o -name '*.scene' \
        -o -name '*.json' \) -exec sed -i 's/\r$//' {} +
    echo "== cassette: UI helper tests"
    make -s -C "$stage/tests/ui" APP="$stage" BUILD="$CDK_BUILD/ui-tests" -j3 test
    exec bash "$here/../_common/build_app.sh" --name Cassette "$stage" "$@"
fi
exec bash "$here/../_common/build_app.sh" --name Cassette "$here" "$@"
