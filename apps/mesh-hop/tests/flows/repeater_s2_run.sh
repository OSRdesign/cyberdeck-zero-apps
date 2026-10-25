#!/bin/sh
# The 0.8.0 / 0.8.1 flows (report 036 slice S2, the read-only view of the Repeater page; 0.8.1: a hub and a screen per section):
# repeater-s2.script (a keyboard: the status by itself after a login, the hub and its summaries, the Status screen and its Refresh,
# Neighbours and Owner reading themselves once, More neighbours, Refresh all from the hub and its stop, More, the owner without a
# login, a flood contact whose guest login brings a route back, the answers lost on the air; the simulator's --neighbours 24
# --owner-info --learn-path, SIMCTL=1 for "drop"), repeater-s2-old.script (--old-repeater: no answer to a guest's neighbours,
# firmware too old for the owner) and repeater-s2-touch.script (touch only), at 640x480 and 800x480.
#   sh repeater_s2_run.sh [RUNNER] [OUT] [SIZES]
#   RUNNER default ${CDK_BUILD:-~/cdk-build/mesh-hop}/host/mesh-hop-script; OUT default /tmp/mesh-hop-repeater-s2-flows; SIZES default
#   "640x480 800x480". The S1 flows of the page: repeater_run.sh.
# Output: OUT/repeater-s2-<WxH>/, OUT/repeater-s2-old-<WxH>/, OUT/repeater-s2-touch-<WxH>/ (shots, dump.txt), OUT/*.out, and the
# failures at the end.
HERE=$(cd "$(dirname "$0")" && pwd)
export MESHHOP_BIN=${1:-${CDK_BUILD:-$HOME/cdk-build/mesh-hop}/host/mesh-hop-script}
OUT=${2:-/tmp/mesh-hop-repeater-s2-flows}
SIZES=${3:-640x480 800x480}
export MESHHOP_ONLINE=0
rm -rf "$OUT"
mkdir -p "$OUT"
for size in $SIZES; do
    echo "== repeater-s2 $size"
    SIZE=$size SIMCTL=1 SIMARGS="--remote-cases --contacts 6 --neighbours 24 --owner-info Hilltop-owner|LoRa-868 --learn-path" \
        SHOTS="$OUT/repeater-s2-$size" SCRIPT="$HERE/repeater-s2.script" sh "$HERE/ui_smoke.sh" > "$OUT/repeater-s2-$size.out" 2>&1
    echo "== repeater-s2-old $size"
    SIZE=$size SIMARGS="--remote-cases --contacts 6 --old-repeater" SHOTS="$OUT/repeater-s2-old-$size" SCRIPT="$HERE/repeater-s2-old.script" \
        sh "$HERE/ui_smoke.sh" > "$OUT/repeater-s2-old-$size.out" 2>&1
    echo "== repeater-s2-touch $size"
    SIZE=$size SIMARGS="--remote-cases --contacts 6 --neighbours 24" SHOTS="$OUT/repeater-s2-touch-$size" SCRIPT="$HERE/repeater-s2-touch.script" \
        sh "$HERE/ui_smoke.sh" > "$OUT/repeater-s2-touch-$size.out" 2>&1
done
echo "shots: $(find "$OUT" -name '*.png' | wc -l)"
echo "PASS lines: $(grep -h ' PASS\|: PASS' "$OUT"/*.out | wc -l)"
echo "failures reported by the runner:"
if grep -H "^script:\|FAIL\|cannot write\|platform init failed" "$OUT"/*.out; then exit 1; fi
echo "  none"
