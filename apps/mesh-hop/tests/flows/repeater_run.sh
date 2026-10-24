#!/bin/sh
# The 0.7.0 flows (report 036 slice S1 and the regrouped Home): repeater.script (a keyboard: Home, the Search of Contacts, the Repeater
# page, its login box with a wrong password, Guest, Log out, an admin login with Remember, the saved password, More, a room, a chat
# and a sensor tapped, Tools, Settings' MY CONTACT) and repeater-touch.script (touch only: the on-screen keyboard for the Search and
# the password only, the 3 s hold's Options, Tools), against the simulator with its remote cases (--remote-cases --contacts 6), at
# 640x480 and 800x480.
#   sh repeater_run.sh [RUNNER] [OUT] [SIZES]
#   RUNNER default ${CDK_BUILD:-~/cdk-build/mesh-hop}/host/mesh-hop-script; OUT default /tmp/mesh-hop-repeater-flows; SIZES default
#   "640x480 800x480". The CLI's part of the slice (Open in CLI, To...) needs meshcore-cli: cli_run.sh (cli-repeater.script).
# Output: OUT/repeater-<WxH>/ and OUT/repeater-touch-<WxH>/ (shots, dump.txt), OUT/*.out, and the failures at the end.
HERE=$(cd "$(dirname "$0")" && pwd)
export MESHHOP_BIN=${1:-${CDK_BUILD:-$HOME/cdk-build/mesh-hop}/host/mesh-hop-script}
OUT=${2:-/tmp/mesh-hop-repeater-flows}
SIZES=${3:-640x480 800x480}
export MESHHOP_ONLINE=0
rm -rf "$OUT"
mkdir -p "$OUT"
for size in $SIZES; do
    echo "== repeater $size"
    SIZE=$size SIMCTL=1 SIMARGS="--remote-cases --contacts 6" SHOTS="$OUT/repeater-$size" SCRIPT="$HERE/repeater.script" sh "$HERE/ui_smoke.sh" \
        > "$OUT/repeater-$size.out" 2>&1
    echo "== repeater-touch $size"
    SIZE=$size SIMARGS="--remote-cases --contacts 6" SHOTS="$OUT/repeater-touch-$size" SCRIPT="$HERE/repeater-touch.script" sh "$HERE/ui_smoke.sh" \
        > "$OUT/repeater-touch-$size.out" 2>&1
done
echo "shots: $(find "$OUT" -name '*.png' | wc -l)"
echo "PASS lines: $(grep -h ' PASS\|: PASS' "$OUT"/*.out | wc -l)"
echo "failures reported by the runner:"
if grep -H "^script:\|FAIL\|cannot write\|platform init failed" "$OUT"/*.out; then exit 1; fi
echo "  none"
