#!/bin/sh
# Builds Mesh Hop in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh [cross|host|all] [--strict] [--sync]
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh flows [WxH]
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/mesh-hop;
#   host:  the shared headless harness and its scenes (tests/scenes: the stage at five sizes, the notice), then
#          Mesh Hop's scripted runner (mesh-hop-script, tests/flows/runner.mk) and the unit tests (tests/unit);
#   flows: the 20 simulator flows of tests/flows with mesh-hop-script (about 15 minutes; needs python3 and a host
#          build), at 640x480 or at WxH; shots in ${CDK_BUILD:-~/cdk-build/mesh-hop}/flows-<WxH>.
# Then the package: python3 tools/make_registry.py --only mesh-hop --out <dir outside packages/>
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${CDK_BUILD:-$HOME/cdk-build/mesh-hop}
src=${LAUNCHER_SRC:-/mnt/c/CLAUDE/zero7/launcher}
tree=${LAUNCHER_TREE:-$HOME/zero7/$(basename -- "$src")}

if [ "${1:-}" = flows ]; then
    size=${2:-640x480}
    exec sh "$here/tests/flows/run_flows.sh" "$build/host/mesh-hop-script" "$build/flows-$size" "$size"
fi

rc=0
bash "$here/../_common/build_app.sh" --name MeshHop "$here" "$@" || rc=$?
mode=all
for a in "$@"; do case $a in cross | host | all) mode=$a ;; esac; done
if [ "$mode" != cross ]; then
    echo "== host: mesh-hop-script (tests/flows/runner.mk)"
    flock /tmp/wsl-build.lock make -f "$here/tests/flows/runner.mk" -j"$(nproc)" APP="$here" LAUNCHER="$tree" \
        BUILD="$build/host" GEN="$build/gen" > "$build/runner-make.log" 2>&1 || { tail -n 40 "$build/runner-make.log"; exit 1; }
    echo "== $build/host/mesh-hop-script"
    echo "== host: unit tests (tests/unit/run_tests.sh)"
    if OUT="$build/unit" sh "$here/tests/unit/run_tests.sh" > "$build/unit.log" 2>&1; then
        grep -E "passed|failed|PASS|FAIL" "$build/unit.log" | tail -n 6
    else
        tail -n 30 "$build/unit.log"
        rc=1
    fi
fi
exit $rc
