#!/bin/sh
# Host tests of Mesh Hop: the protocol / model / store / client layers and the serial transport against the simulated radio
# (core/), and the UI-independent logic (ui/ui_logic.*). Needs g++ and python3; no LVGL, no board.
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/src/tests/run_tests.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$HERE/../main/core
UI=$HERE/../main/ui
OUT=${TMPDIR:-/tmp}/mesh-hop-tests
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -O1 -DTEST_DATA_DIR="\"$HERE/data\"" -I"$CORE" "$HERE/test_core.cpp" "$CORE"/*.cpp -lpthread -o "$OUT/test_core"
"$OUT/test_core"
g++ -std=c++17 -Wall -Wextra -O1 -I"$CORE" "$HERE/test_sim.cpp" "$CORE"/*.cpp -lpthread -o "$OUT/test_sim"
MESHHOP_SIM="$HERE/../../tools/meshcore_sim.py" "$OUT/test_sim"
g++ -std=c++17 -Wall -Wextra -O1 -I"$CORE" -I"$UI" "$HERE/test_ui.cpp" "$UI/ui_logic.cpp" "$UI/ui_phase2.cpp" "$UI/png_writer.cpp" "$CORE"/*.cpp -lpthread -o "$OUT/test_ui"
"$OUT/test_ui"
