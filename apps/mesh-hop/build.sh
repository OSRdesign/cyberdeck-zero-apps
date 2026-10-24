#!/bin/sh
# Builds Mesh Hop in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh [cross|host|all] [--strict] [--sync]
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh flows [WxH]
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build.sh cli
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/mesh-hop;
#   host:  the shared headless harness and its scenes (tests/scenes: the shell and the pages at five sizes, the notice), then
#          Mesh Hop's scripted runner (mesh-hop-script, tests/flows/runner.mk) and the unit tests (tests/unit);
#   flows: the 23 simulator flows of tests/flows with mesh-hop-script (about 17 minutes; needs python3 and a host
#          build), at 640x480 or at WxH ("sizes": the shell, pages and chats scripts at the five sizes and at the HackBerry's
#          density); shots in ${CDK_BUILD:-~/cdk-build/mesh-hop}/flows-<WxH>. 0.7.0's repeater_run.sh (the Repeater page, the
#          Search of Contacts, Tools, MY CONTACT) always runs at 640x480 and 800x480.
#   cli:   the bundled meshcore-cli of the CLI page (report 033, K1; needs python3 with pip, g++; the network once): cli/vendor.py
#          fetches the wheels pinned with their hashes in cli/requirements.txt (cache: $CDK_BUILD/cli-wheels/<arch>, reused
#          offline), checks them, unpacks them into root/opt/mesh-hop/cli/py (arm64) and root.amd64/opt/mesh-hop/cli/py (what
#          differs on amd64) and their licences into root/usr/share/doc/mesh-hop/LICENSE.*; then the smoke flow: that bundle, as
#          installed in $CDK_BUILD/cli-smoke, runs `infos` and `ver` through Mesh Hop's relay against the simulator
#          (tests/unit/test_relay_sim.cpp with MESHHOP_MESHCLI). Run it after changing a pin, then the CLI page's flows with that
#          bundle (tests/flows/cli_run.sh: a session at 640x480 and 800x480 with a keyboard and by touch, and 0.7.0's Open in CLI
#          and To... with the simulator's remote cases; `build.sh flows` runs them too when MESHHOP_MESHCLI is set).
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
if [ "${1:-}" = cli ]; then
    mkdir -p "$build"
    python3 -I "$here/cli/vendor.py" --cache "$build/cli-wheels" --smoke "$build/cli-smoke"
    echo "== host: meshcore-cli smoke on the relay (tests/unit/test_relay_sim.cpp, MESHHOP_MESHCLI)"
    core=$here/src/core
    g++ -std=c++17 -Wall -Wextra -O1 -I"$core" "$here/tests/unit/test_relay_sim.cpp" "$core"/*.cpp "$core"/ble/*.cpp -lpthread \
        -o "$build/test_relay_smoke"
    exec env MESHHOP_SIM="$here/tools/meshcore_sim.py" MESHHOP_MESHCLI="$build/cli-smoke/opt/mesh-hop/cli/mccli" \
        "$build/test_relay_smoke"
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
