#!/bin/sh
# The CLI page's flows (report 033, P5) with the real bundled meshcore-cli on Mesh Hop's relay against the simulated board, at
# 640x480 and 800x480: cli.script (a keyboard: Starting..., infos, to the repeater, login with the password masked, ver, Help, All
# commands, Esc; Chats afterwards), cli-touch.script (touch only: the on-screen keyboard, Help by its button, no To... (0.7.2), the
# header's Esc) and cli-repeater.script (0.7.0, report 036 S1: an admin login on the Repeater page, More > Open in CLI, ver with no
# login typed; 0.7.2: Open in CLI from the pages of two of the same name, spaces and a hex-looking name; the simulator's
# --remote-cases).
#   sh cli_run.sh [RUNNER] [OUT] [MCCLI] [SIZES]
#   RUNNER default ${CDK_BUILD:-~/cdk-build/mesh-hop}/host/mesh-hop-script; OUT default /tmp/mesh-hop-cli-flows; MCCLI: the mccli
#   wrapper of a bundle installed for this machine (default $MESHHOP_MESHCLI; build.sh cli makes one in $CDK_BUILD/cli-smoke);
#   SIZES default "640x480 800x480". meshcore-cli's .pyc files go to OUT/cache (the first start compiles them).
# Output: OUT/cli-<WxH>/ and OUT/cli-touch-<WxH>/ (shots, dump.txt), OUT/*.out, and the failures at the end.
HERE=$(cd "$(dirname "$0")" && pwd)
export MESHHOP_BIN=${1:-${CDK_BUILD:-$HOME/cdk-build/mesh-hop}/host/mesh-hop-script}
OUT=${2:-/tmp/mesh-hop-cli-flows}
export MESHHOP_MESHCLI=${3:-$MESHHOP_MESHCLI}
SIZES=${4:-640x480 800x480}
if [ -z "$MESHHOP_MESHCLI" ] || [ ! -x "$MESHHOP_MESHCLI" ]; then
    echo "cli_run.sh: no mccli (MESHHOP_MESHCLI or the third argument; build.sh cli installs one in \$CDK_BUILD/cli-smoke)" >&2
    exit 2
fi
export MESHHOP_ONLINE=0 XDG_CACHE_HOME=$OUT/cache
rm -rf "$OUT"
mkdir -p "$OUT"
for size in $SIZES; do
    echo "== cli $size"
    SIZE=$size SIMCTL=1 SHOTS="$OUT/cli-$size" SCRIPT="$HERE/cli.script" sh "$HERE/ui_smoke.sh" > "$OUT/cli-$size.out" 2>&1
    echo "== cli-touch $size"
    SIZE=$size SHOTS="$OUT/cli-touch-$size" SCRIPT="$HERE/cli-touch.script" sh "$HERE/ui_smoke.sh" > "$OUT/cli-touch-$size.out" 2>&1
    echo "== cli-repeater $size"
    SIZE=$size SIMCTL=1 SIMARGS="--remote-cases" SHOTS="$OUT/cli-repeater-$size" SCRIPT="$HERE/cli-repeater.script" sh "$HERE/ui_smoke.sh" \
        > "$OUT/cli-repeater-$size.out" 2>&1
done
echo "shots: $(find "$OUT" -name '*.png' | wc -l)"
echo "PASS lines: $(grep -h ' PASS\|: PASS' "$OUT"/*.out | wc -l)"
echo "failures reported by the runner:"
if grep -H "^script:\|FAIL\|cannot write\|platform init failed" "$OUT"/*.out; then exit 1; fi
echo "  none"
