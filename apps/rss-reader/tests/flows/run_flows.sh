#!/bin/sh
# The scripted flows of RSS Reader (tests/flows/*.script) with rss-reader-script: each flow in a fresh data folder (the store
# and a home folder), the fake network of routes.txt answering from tests/data/ui, the clock fixed at 2026-10-18 16:00 UTC.
#   sh run_flows.sh [RUNNER] [OUT] [WxH | sizes] [flow ...]
#   RUNNER default ~/cdk-build/rss-reader/host/rss-reader-script, OUT default /tmp/rss-reader-flows, WxH default 640x480;
#   "sizes": 640x480, 800x480, 720x720, 1024x600 and 1280x720. Flows: all of them, or the ones named.
# Output: OUT/<WxH>/<flow>/*.png, OUT/<WxH>/<flow>.out (the run's output); a summary of the failures at the end.
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
RUNNER=${1:-${CDK_BUILD:-$HOME/cdk-build/rss-reader}/host/rss-reader-script}
OUT=${2:-/tmp/rss-reader-flows}
SIZE=${3:-640x480}
[ $# -gt 3 ] && shift 3 || set --
FLOWS=${*:-"home favorites refresh browse add articles reader settings saved images offline savedpage capfull"}
export CDK_HEADLESS_RESOURCES=${CDK_HEADLESS_RESOURCES:-$HOME/zero7/launcher/projects/APPLaunch/APPLaunch}
export RSS_FAKE_ROUTES=$HERE/routes.txt RSS_FAKE_ROOT=$APP/tests/data/ui RSS_TEST_NOW=1792339200 TZ=UTC RSSREADER_KEYMAP=us
export RSSREADER_CATALOG=$APP/data/catalog.json
SIZES=$SIZE
if [ "$SIZE" = sizes ]; then        # the five sizes side by side (one process each), then the summary
    for s in 640x480 800x480 720x720 1024x600 1280x720; do RSS_FLOWS_CHILD=1 sh "$0" "$RUNNER" "$OUT" "$s" $FLOWS > "$OUT.$s.log" 2>&1 & done
    wait
    cat "$OUT".*.log
    echo "== $(find "$OUT" -name "*.png" | wc -l) shots"
    echo "failures:"
    grep -H "FAIL\|^script:\|cannot write\|platform init failed\|under ROW" "$OUT"/*/*.out || echo "  none"
    exit 0
fi
t0=$(date +%s)
mkdir -p "$OUT"
for s in $SIZES; do
    for f in $FLOWS; do
        d=$(mktemp -d /tmp/rss-flow.XXXXXX)
        mkdir -p "$d/store" "$d/home" "$OUT/$s"
        rm -rf "${OUT:?}/$s/$f"
        RSSREADER_DATA=$d/store HOME=$d/home "$RUNNER" --script "$HERE/$f.script" --shot-dir "$OUT/$s/$f" --size "$s" > "$OUT/$s/$f.out" 2>&1
        rc=$?
        echo "$s $f: rc $rc, $(grep -c ' PASS' "$OUT/$s/$f.out") PASS, $(grep -c 'FAIL' "$OUT/$s/$f.out") FAIL, $(ls "$OUT/$s/$f" 2> /dev/null | grep -c '\.png$') shots"
        rm -rf "$d"
    done
done
echo "== $SIZE done in $(($(date +%s) - t0)) s"
[ -n "${RSS_FLOWS_CHILD:-}" ] && exit 0
echo "failures:"
grep -H "FAIL\|^script:\|cannot write\|platform init failed\|under ROW" "$OUT"/*/*.out || echo "  none"
