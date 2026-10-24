#!/bin/sh
# Host tests of Mesh Hop: the protocol / model / store / client layers (with the remote login queue and the password store of
# report 036, test_remote.inc), the Bluetooth LE link over a fake BlueZ
# (core/ble, test_ble.inc) and the serial transport against the simulated radio (core/), the CLI relay against it over TCP and the
# pty (test_relay_sim.cpp), the CLI page's child on a pseudo-terminal (core/pty_process.*, test_pty.inc), and the UI-independent logic
# (ui/ui_logic.*, ui/ui_phase2.cpp, the key bridge ui/key_bridge.*, the CLI page's ui/cli_logic.*, test_cli_ui.inc; 0.7.0: Home,
# Tools, the Search and Options of Contacts, MY CONTACT, the Repeater page's texts, ui/ui_repeater.*, test_repeater_ui.inc). Needs g++ and
# python3; no LVGL, no board. Run by ../../build.sh host, or alone:
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/tests/unit/run_tests.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$HERE/../../src/core
UI=$HERE/../../src/ui
OUT=${OUT:-${TMPDIR:-/tmp}/mesh-hop-tests}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -O1 -DTEST_DATA_DIR="\"$HERE/../data\"" -I"$CORE" "$HERE/test_core.cpp" "$CORE"/*.cpp "$CORE"/ble/*.cpp \
    -lpthread -o "$OUT/test_core"
"$OUT/test_core"
g++ -std=c++17 -Wall -Wextra -O1 -I"$CORE" "$HERE/test_sim.cpp" "$CORE"/*.cpp "$CORE"/ble/*.cpp -lpthread -o "$OUT/test_sim"
MESHHOP_SIM="$HERE/../../tools/meshcore_sim.py" "$OUT/test_sim"
# the simulator's remote repeaters and rooms (report 036: guest password, one pending login, lost answers, restarts, names), no port
python3 "$HERE/sim_remote_check.py" "$HERE/../../tools/meshcore_sim.py"
# the CLI relay (report 033) end to end: the simulator over TCP and over the pty, a console on the relay (relay_console.py)
g++ -std=c++17 -Wall -Wextra -O1 -I"$CORE" "$HERE/test_relay_sim.cpp" "$CORE"/*.cpp "$CORE"/ble/*.cpp -lpthread -o "$OUT/test_relay_sim"
MESHHOP_SIM="$HERE/../../tools/meshcore_sim.py" MESHHOP_CONSOLE="$HERE/relay_console.py" "$OUT/test_relay_sim"
g++ -std=c++17 -Wall -Wextra -O1 -I"$CORE" -I"$UI" "$HERE/test_ui.cpp" "$UI/ui_logic.cpp" "$UI/ui_phase2.cpp" "$UI/ui_pages.cpp" "$UI/png_writer.cpp" \
    "$UI/key_bridge.cpp" "$UI/ui_nodes.cpp" "$UI/cli_logic.cpp" "$UI/ui_repeater.cpp" "$CORE"/*.cpp "$CORE"/ble/*.cpp -lpthread -o "$OUT/test_ui"
# (the CLI page's Help list and password rule are checked against the bundled meshcore-cli of phase P4: root/opt/mesh-hop/cli)
MESHHOP_CLI_PY="$HERE/../../root/opt/mesh-hop/cli/py/meshcore_cli/meshcore_cli.py" "$OUT/test_ui"
