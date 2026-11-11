#!/bin/sh
# Regenerates src/ui/fonts/caveat_semibold_<px>.c, the handwriting face of the cassette label (Caveat SemiBold 600,
# SIL Open Font License 1.1, see src/ui/fonts/OFL-Caveat.txt), as LVGL bitmap fonts (4 bpp, uncompressed, ASCII
# 0x20-0x7E and Latin-1 supplement 0xA0-0xFF, kerning kept). The TTF is NOT committed: this script downloads it.
#
#   sh tools/make_handwriting_font.sh            (from apps/cassette; needs curl, python3 with fontTools, and node with
#                                                 lv_font_conv on PATH or in $LV_FONT_CONV)
#
# Source (pinned): https://github.com/googlefonts/caveat commit 59745e818ef7973e11e70cb1358d0e902b56c5fc (2020-10-01),
# fonts/variable/Caveat[wght].ttf, sha256 0bdb6b660482d31531b3945849fba5916b3ef8695da7024a9e6b9ee3c4157988. The
# repository has no static SemiBold, so the variable font is pinned to wght=600 with fontTools' instancer (4.68.0); the
# resulting TTF's sha256 is printed but changes from run to run (a timestamp in its head table), the glyph data does not. OFL.txt of the same commit:
# sha256 1f9d81d094273d82f3898a1ee8b598a717d050ecbf5ff7bede105b704880157b.
#
# The sizes are the rungs of the label's size ladder (ui/handwriting.cpp): 15 (long names), 22 (480x320), 29 (640x480),
# 31 (320x480). Tested with lv_font_conv 1.5.3 (output compiled against LVGL 9.5.0).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/src/ui/fonts"
work="${TMPDIR:-/tmp}/cassette-font.$$"
commit=59745e818ef7973e11e70cb1358d0e902b56c5fc
var_sha=0bdb6b660482d31531b3945849fba5916b3ef8695da7024a9e6b9ee3c4157988
ofl_sha=1f9d81d094273d82f3898a1ee8b598a717d050ecbf5ff7bede105b704880157b
conv="${LV_FONT_CONV:-lv_font_conv}"
sizes="${SIZES:-15 22 29 31}"
PY="${PYTHON:-python3}"

mkdir -p "$work" "$out"
trap 'rm -rf "$work"' EXIT
base="https://raw.githubusercontent.com/googlefonts/caveat/$commit"
curl -fsSL "$base/fonts/variable/Caveat%5Bwght%5D.ttf" -o "$work/Caveat-VF.ttf"
curl -fsSL "$base/OFL.txt" -o "$work/OFL.txt"
echo "$var_sha  $work/Caveat-VF.ttf" | sha256sum -c -
echo "$ofl_sha  $work/OFL.txt" | sha256sum -c -
"$PY" -I - "$work/Caveat-VF.ttf" "$work/Caveat-SemiBold.ttf" << 'EOF'
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
f = TTFont(sys.argv[1])
g = instancer.instantiateVariableFont(f, {"wght": 600})
g.save(sys.argv[2])
EOF
sha256sum "$work/Caveat-SemiBold.ttf"
cp "$work/OFL.txt" "$out/OFL-Caveat.txt"
for px in $sizes; do
    "$conv" --bpp 4 --size "$px" --font "$work/Caveat-SemiBold.ttf" -r 0x20-0x7E -r 0xA0-0xFF --format lvgl \
        --lv-font-name "caveat_semibold_$px" --no-compress --lv-include lvgl/lvgl.h -o "$out/caveat_semibold_$px.c"
    # keep the generated header free of this machine's temporary paths
    sed -i -e "s#--font [^ ]*#--font Caveat-SemiBold.ttf#" -e "s# -o [^ ]*# -o caveat_semibold_$px.c#" "$out/caveat_semibold_$px.c"
    echo "caveat_semibold_$px.c: $(stat -c %s "$out/caveat_semibold_$px.c") bytes of C"
done
