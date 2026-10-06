#!/bin/sh
# Host tests of the MeshZero app's protocol / model / store / client layers and of the serial transport
# against the simulated radio. Needs g++ and python3; no LVGL, no board.
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/meshzero/src/tests/run_tests.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$HERE/../main/core
OUT=${TMPDIR:-/tmp}/meshzero-tests
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -O1 -I"$CORE" "$HERE/test_core.cpp" "$CORE"/*.cpp -lpthread -o "$OUT/test_core"
"$OUT/test_core"
g++ -std=c++17 -Wall -Wextra -O1 -I"$CORE" "$HERE/test_sim.cpp" "$CORE"/*.cpp -lpthread -o "$OUT/test_sim"
MESHZERO_SIM="$HERE/../../tools/meshcore_sim.py" "$OUT/test_sim"
