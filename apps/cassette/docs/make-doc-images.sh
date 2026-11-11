#!/bin/sh
# Renders the images of USER-GUIDE.md from the scene files and puts them in docs/img (see MAKING-SCREENSHOTS.md).
#
#   wsl -e sh <repo>/apps/cassette/docs/make-doc-images.sh
#
# What it does:
#   1. stages the app like build.sh does (a Linux, LF copy under $CDK_BUILD/stage-docs), keeping only the scenes that
#      docs/doc-images.tsv names, so it runs about 20 scenes and not all 160;
#   2. builds the headless host (the first run also builds LVGL: a few minutes) and runs those scenes;
#   3. copies each wanted PNG to docs/img/<name> (docs/doc-images.tsv says which);
#   4. runs docs/finish-images.py (needs Python with Pillow): shrinks the PNGs, draws the numbered markers on the
#      annot-* images and makes layouts.png. Without Pillow the raw PNGs stay in place and the script says so.
#
# Environment (as for build.sh): LAUNCHER_SRC and LAUNCHER_TREE point at the launcher tree; CDK_BUILD is the build
# folder (default ~/cdk-build/cassette-docs-img); LVBUILD the LVGL host build (default $CDK_BUILD/_lvgl-host).
# Writes only to $CDK_BUILD and to apps/cassette/docs/img. No device, no network.
set -e
here=$(cd "$(dirname "$0")" && pwd)   # .../apps/cassette/docs
app=$(dirname "$here")                # .../apps/cassette
common="$app/../_common"
CDK_BUILD="${CDK_BUILD:-$HOME/cdk-build/cassette-docs-img}"
LVBUILD="${LVBUILD:-$CDK_BUILD/_lvgl-host}"
export CDK_BUILD LVBUILD
tsv="$here/doc-images.tsv"
img="$here/img"
[ -f "$tsv" ] || { echo "missing $tsv" >&2; exit 1; }
[ -n "$LAUNCHER_TREE" ] || { echo "set LAUNCHER_SRC and LAUNCHER_TREE (the launcher tree the apps build against)" >&2; exit 1; }

# The header's clock (HH:MM) comes from the launcher's toolkit with no option to fix it: freeze the wall clock for the
# build and the scene run (an LD_PRELOAD shim, docs/freeze-clock.c, 2026-11-11 12:00 UTC) so a re-render changes an
# image only where the UI changed. Without a C compiler the images show the time of the run.
if command -v cc > /dev/null 2>&1; then
    mkdir -p "$CDK_BUILD"
    if cc -shared -fPIC -o "$CDK_BUILD/freeze-clock.so" "$here/freeze-clock.c"; then
        export LD_PRELOAD="$CDK_BUILD/freeze-clock.so"
    fi
fi
[ -n "$LD_PRELOAD" ] || echo "NOTE: clock not frozen: the header clock in the images shows the time of this run." >&2

# 1. stage (the same files as build.sh host stages), then drop every scene the table does not need
st="$CDK_BUILD/stage-docs/cassette"
rm -rf "$st"
mkdir -p "$st"
extra=
[ -f "$app/cp0_config.mk" ] && extra=cp0_config.mk
# shellcheck disable=SC2086
(cd "$app" && tar cf - --exclude=src/core/alsa/tools app.json src $extra) | (cd "$st" && tar xf -)
(cd "$app" && tar cf - tests/scenes tests/ui tests/bind) | (cd "$st" && tar xf -)
cp "$st"/tests/bind/scenes/*.scene "$st/tests/scenes/"
find "$st" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name Makefile -o -name '*.scene' \
    -o -name '*.json' -o -name '*.txt' -o -name '*.sh' -o -name '*.mk' \) -exec sed -i 's/\r$//' {} +
scenes=$(grep -v '^#' "$tsv" | awk -F'\t' 'NF >= 4 { print $2 }' | sort -u)
for f in "$st"/tests/scenes/*.scene; do
    n=$(basename "$f" .scene)
    echo "$scenes" | grep -qx "$n" || rm -f "$f"
done
for n in $scenes; do
    [ -f "$st/tests/scenes/$n.scene" ] || { echo "scene $n.scene not found (docs/doc-images.tsv)" >&2; exit 1; }
done

# 2. build the host and run the kept scenes (build_app.sh runs every scene left in the stage); the scene's own checks
# (expect ...) fail the run when a layout or a text is wrong
echo "== building and rendering $(echo "$scenes" | wc -w) scenes into $CDK_BUILD/host/out"
bash "$common/build_app.sh" --name Cassette "$st" host

# 3. copy the PNGs
mkdir -p "$img"
missing=0
list="$CDK_BUILD/doc-images.list"
grep -v '^#' "$tsv" | awk -F'	' 'NF >= 4' > "$list"
while IFS='	' read -r name scene size shot; do
    src="$CDK_BUILD/host/out/$scene/$size/$shot.png"
    if [ -f "$src" ]; then cp "$src" "$img/$name"; else echo "MISSING $src" >&2; missing=1; fi
done < "$list"
[ "$missing" = 0 ] || exit 1
echo "== copied to $img"

# 4. shrink, annotate, layouts strip
for py in python3 python python.exe py.exe; do
    command -v "$py" > /dev/null 2>&1 || continue
    "$py" -c 'import PIL' > /dev/null 2>&1 || continue
    arg="$here/finish-images.py"
    case $py in *.exe) arg=$(wslpath -w "$arg") ;; esac
    "$py" "$arg" "$tsv" "$img" && exit 0
done
echo "NOTE: no Python with Pillow found: the PNGs in $img are raw (large, no markers, no layouts.png)." >&2
echo "      Run: python docs/finish-images.py docs/doc-images.tsv docs/img   (any machine with Pillow)" >&2
