#!/bin/sh
# Builds and tests RSS Reader in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/build.sh [cross|host|all] [--strict] [--sync]
#   wsl -e sh .../build.sh test | asan | sim [WxH] | ui
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/rss-reader (and the catalog copy into
#          root/usr/share/APPLaunch/share/rss-reader); --strict stops when the launcher is not the pinned one
#          (apps/_common/LAUNCHER_REF);
#   host:  the shared headless harness and its scenes (tests/scenes), RSS Reader's scripted runner (rss-reader-script,
#          tests/flows/runner.mk) and the unit tests;
#   test:  the unit tests only (tests/unit/run_tests.sh: the core and the UI logic, about a minute);  asan: the same under
#          AddressSanitizer + UBSan;
#   sim:   the scripted flows against the fake network (tests/flows/*.script, fixtures in tests/data/ui, a fixed clock) at
#          640x480 or WxH; needs a host build; shots in ${CDK_BUILD:-~/cdk-build/rss-reader}/flows-<WxH>;
#   ui:    the flows at the five sizes (640x480, 800x480, 720x720, 1024x600, 1280x720): the render sheets' shots.
# Then the package: python3 tools/make_registry.py --only rss-reader --out <dir outside packages/> (app.json is a draft).
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${CDK_BUILD:-$HOME/cdk-build/rss-reader}
src=${LAUNCHER_SRC:-/mnt/c/CLAUDE/zero7/launcher}
tree=${LAUNCHER_TREE:-$HOME/zero7/$(basename -- "$src")}

case ${1:-} in
    test) OUT="$build/unit" exec sh "$here/tests/unit/run_tests.sh" ;;
    asan) SANITIZE=1 OUT="$build/unit" exec sh "$here/tests/unit/run_tests.sh" ;;
    sim) exec sh "$here/tests/flows/run_flows.sh" "$build/host/rss-reader-script" "$build/flows-${2:-640x480}" "${2:-640x480}" ;;
    ui) exec sh "$here/tests/flows/run_flows.sh" "$build/host/rss-reader-script" "$build/flows-sizes" sizes ;;
esac

rc=0
mode=all
for a in "$@"; do case $a in cross | host | all) mode=$a ;; esac; done
if [ "$mode" != host ]; then
    mkdir -p "$here/root/usr/share/APPLaunch/share/rss-reader"
    cmp -s "$here/data/catalog.json" "$here/root/usr/share/APPLaunch/share/rss-reader/catalog.json" ||
        cp "$here/data/catalog.json" "$here/root/usr/share/APPLaunch/share/rss-reader/catalog.json"
fi
bash "$here/../_common/build_app.sh" --name RssReader "$here" "$@" || rc=$?
if [ "$mode" != cross ]; then
    echo "== host: rss-reader-script (tests/flows/runner.mk)"
    flock /tmp/wsl-build.lock make -f "$here/tests/flows/runner.mk" -j"$(nproc)" APP="$here" LAUNCHER="$tree" \
        BUILD="$build/host" GEN="$build/gen" > "$build/runner-make.log" 2>&1 || { tail -n 40 "$build/runner-make.log"; exit 1; }
    echo "== $build/host/rss-reader-script"
    echo "== host: unit tests (tests/unit/run_tests.sh)"
    if OUT="$build/unit" sh "$here/tests/unit/run_tests.sh" > "$build/unit.log" 2>&1; then
        grep -E "checks|FAIL" "$build/unit.log" | tail -n 6
    else
        tail -n 30 "$build/unit.log"
        rc=1
    fi
fi
exit $rc
