#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Deck Atlas: the optional real-tile check (the legibility and import-speed checks of report 054), not part of build.sh host.
#   DECKATLAS_TILES=<a z/x/y tile folder, read only> wsl -e sh <app>/tests/real/run.sh
# Skips (exit 0, one line) when DECKATLAS_TILES is not set or not a folder. Otherwise:
#   1. builds import_bench.c against the engine sources of LAUNCHER_TREE (default ~/zero7/launcher; cp0_lvgl src/map
#      without the widget), as the engine's own tests do;
#   2. imports the folder into a new store in a scratch folder (never writes to the source): prints the phases, the
#      tiles per second, the output's size, and the tiles in the MBTiles against the files in the folder;
#   3. when the app's headless harness is built (build.sh host; HEADLESS=<path> to point at it), renders the scenes of
#      tests/real/scenes over that MBTiles (read in place) into <scratch>/shots: the Map tab at zoom 13, 15 and 17 at
#      640x480, 480x320, 320x480 and 480x800, and the legibility shots at the zero8 density (6.3 px/mm) at 1x, 1.5x
#      (the automatic choice there) and 2x, and at the deck's (11.3 px/mm, 2x).
# Environment: DECKATLAS_REAL_OUT=<folder> (default a new folder under $TMPDIR), LAUNCHER_TREE, HEADLESS, RESOURCES.
set -e
if [ -z "${DECKATLAS_TILES:-}" ]; then
    echo "SKIP real-tile check: set DECKATLAS_TILES to a z/x/y tile folder (it is only read)"
    exit 0
fi
if [ ! -d "$DECKATLAS_TILES" ]; then
    echo "SKIP real-tile check: $DECKATLAS_TILES is not a folder"
    exit 0
fi
here=$(cd "$(dirname "$0")" && pwd)
LAUNCHER_TREE=${LAUNCHER_TREE:-$HOME/zero7/launcher}
CP0=$LAUNCHER_TREE/ext_components/cp0_lvgl
[ -f "$CP0/include/cp0_map.h" ] || { echo "no map engine in $CP0 (LAUNCHER_TREE at the pin?)" >&2; exit 2; }
OUT=${DECKATLAS_REAL_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/deck-atlas-real-XXXXXX")}
mkdir -p "$OUT"
rm -rf "$OUT/store" "$OUT/shots"
HEADLESS=${HEADLESS:-$HOME/cdk-build/deck-atlas/host/deck-atlas-headless}
RES=${RESOURCES:-$LAUNCHER_TREE/projects/APPLaunch/APPLaunch}

echo "== build import_bench ($CP0)"
srcs=$(ls "$CP0"/src/map/*.c | grep -v cp0_map_widget)
# shellcheck disable=SC2086
cc -std=c11 -O2 -pthread -I"$CP0/include" -I"$CP0/src/map" $srcs "$CP0/src/cp0_app_contract.c" \
    "$CP0/src/cp0_ui_metrics.c" "$here/import_bench.c" -ldl -lm -o "$OUT/import_bench"

files=$(find "$DECKATLAS_TILES" -type f | wc -l)
tiles=$(find "$DECKATLAS_TILES" -type f \( -name '*.png' -o -name '*.jpg' -o -name '*.jpeg' \) | wc -l)
bytes=$(du -sb "$DECKATLAS_TILES" | cut -f1)
echo "== source $DECKATLAS_TILES: $files files, $tiles image files, $bytes bytes"
echo "== import into $OUT/store"
"$OUT/import_bench" "$DECKATLAS_TILES" "$OUT/store" | tee "$OUT/import.log"
mb=$(ls "$OUT"/store/archives/*.mbtiles | head -n1)
python3 - "$mb" "$tiles" "$OUT/import.log" << 'EOF'
import re, sqlite3, sys
path, tiles, log = sys.argv[1], int(sys.argv[2]), open(sys.argv[3]).read()
db = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
n = db.execute("select count(*) from tiles").fetchone()[0]
per = db.execute("select zoom_level, count(*) from tiles group by zoom_level order by zoom_level").fetchall()
meta = dict(db.execute("select name, value from metadata").fetchall())
conv = float(re.search(r"convert_s=([0-9.]+)", log).group(1))
total = float(re.search(r"total_s=([0-9.]+)", log).group(1))
print("== MBTiles: %d tiles (%d image files in the folder), per zoom %s" % (n, tiles, per))
print("== metadata: %s" % {k: meta[k] for k in sorted(meta) if k in ("name", "format", "bounds", "minzoom", "maxzoom", "attribution")})
print("== rate: %.0f tiles/s over the conversion (%.2f s), %.0f tiles/s over the whole import (%.2f s)" %
      (n / conv if conv else 0, conv, n / total if total else 0, total))
EOF
ls -l "$mb"

if [ ! -x "$HEADLESS" ]; then
    echo "SKIP the real-tile scenes: no harness at $HEADLESS (build.sh host first, or HEADLESS=<path>)"
    exit 0
fi
echo "== scenes over $mb"
export DECKATLAS_MBTILES="$mb"
export APPLAUNCH_POWER_SUPPLY_ROOT="$OUT/power_supply"
mkdir -p "$APPLAUNCH_POWER_SUPPLY_ROOT"
rc=0
for s in "$here"/scenes/*.scene; do
    "$HEADLESS" --scene "$s" --out "$OUT/shots" --resources "$RES" || rc=1
done
echo "== shots in $OUT/shots (contact sheets in $OUT/shots/sheets)"
exit $rc
