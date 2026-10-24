#!/bin/sh
# The scripted flows of Markdown Notes (tests/flows/*.script) with markdown-notes-script: each flow in a fresh folder (the
# vault, a home folder, the settings), the fixtures of tests/data, the clock fixed at 2026-10-24 14:03 UTC (a Saturday).
#   sh run_flows.sh [RUNNER] [OUT] [WxH | sizes | both] [flow ...]
#   RUNNER default ~/cdk-build/markdown-notes/host/markdown-notes-script, OUT default /tmp/markdown-notes-flows, WxH default
#   640x480; "both": 640x480 and 800x480; "sizes": 640x480, 800x480, 720x720 and 1280x800 (the FZ-M1's density). Flows: all
#   of them, or the ones named.
# Output: OUT/<WxH>/<flow>/*.png, OUT/<WxH>/<flow>.out (the run's output); a summary of the failures at the end.
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
RUNNER=${1:-${CDK_BUILD:-$HOME/cdk-build/markdown-notes}/host/markdown-notes-script}
OUT=${2:-/tmp/markdown-notes-flows}
SIZE=${3:-640x480}
[ $# -gt 3 ] && shift 3 || set --
FLOWS=${*:-"create reopen tick links today delete conflict osk osklate help limits screens"}
export CDK_HEADLESS_RESOURCES=${CDK_HEADLESS_RESOURCES:-$HOME/zero7/launcher/projects/APPLaunch/APPLaunch}
export NOTES_TEST_DATA=$APP/tests/data NOTES_TEST_NOW=1792850580 TZ=UTC MDNOTES_KEYMAP=us
# the PC's own battery must not change the header the shots show (build_app.sh does the same for the scenes)
export APPLAUNCH_POWER_SUPPLY_ROOT=${APPLAUNCH_POWER_SUPPLY_ROOT-/tmp/markdown-notes-flows-no-battery}
mkdir -p "$APPLAUNCH_POWER_SUPPLY_ROOT"
case $SIZE in
    both) SIZES="640x480 800x480" ;;
    sizes) SIZES="640x480 800x480 720x720 1280x800" ;;
    *) SIZES=$SIZE ;;
esac
if [ "$SIZES" != "$SIZE" ]; then        # the sizes side by side (one process each), then the summary
    for s in $SIZES; do NOTES_FLOWS_CHILD=1 sh "$0" "$RUNNER" "$OUT" "$s" $FLOWS > "$OUT.$s.log" 2>&1 & done
    wait
    for s in $SIZES; do cat "$OUT.$s.log"; done
    echo "== $(find "$OUT" -name "*.png" | wc -l) shots"
    echo "failures:"
    grep -H "FAIL\|^script:\|cannot write\|platform init failed\|under ROW" "$OUT"/*/*.out || echo "  none"
    exit 0
fi
t0=$(date +%s)
mkdir -p "$OUT"
for f in $FLOWS; do
    d=$(mktemp -d /tmp/mdnotes-flow.XXXXXX)
    mkdir -p "$d/vault" "$d/home" "$d/config" "$OUT/$SIZE"
    rm -rf "${OUT:?}/$SIZE/$f"
    NOTES_VAULT=$d/vault HOME=$d/home XDG_CONFIG_HOME=$d/config "$RUNNER" --script "$HERE/$f.script" --shot-dir "$OUT/$SIZE/$f" \
        --size "$SIZE" > "$OUT/$SIZE/$f.out" 2>&1
    rc=$?
    echo "$SIZE $f: rc $rc, $(grep -c ' PASS' "$OUT/$SIZE/$f.out") PASS, $(grep -c 'FAIL' "$OUT/$SIZE/$f.out") FAIL, $(ls "$OUT/$SIZE/$f" 2> /dev/null | grep -c '\.png$') shots"
    rm -rf "$d"
done
echo "== $SIZE done in $(($(date +%s) - t0)) s"
[ -n "${NOTES_FLOWS_CHILD:-}" ] && exit 0
echo "failures:"
grep -H "FAIL\|^script:\|cannot write\|platform init failed\|under ROW" "$OUT"/*/*.out || echo "  none"
