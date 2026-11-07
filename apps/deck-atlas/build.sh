#!/bin/sh
# Builds Deck Atlas in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/deck-atlas/build.sh [cross|host|all] [--arch arm64|amd64] [--strict] [--sync]
#   cross: the binary into root/ (arm64) or root.amd64/ (amd64), with the launcher toolkit's map engine switched on for
#   this app only (cp0_config.mk: CONFIG_CP0_LVGL_MAP=y); host: unit tests (settings, labels, the import, source and
#   file-browser models, the package files) and the headless scenes (a demonstration store built from tests/fixtures).
# The optional real-tile check (import speed, legibility shots) needs a tile folder: see tests/real/run.sh.
# The test tiles are synthetic: tests/tools/gen_fixture_tiles.py writes tests/fixtures/tiles (never real map data).
set -e
here=$(cd "$(dirname "$0")" && pwd)
python3 "$here/tests/tools/gen_fixture_tiles.py" "$here/tests/fixtures/tiles" > /dev/null
python3 "$here/tests/tools/gen_nmea.py" "$here/tests/gps-feeds"
exec bash "$here/../_common/build_app.sh" --name DeckAtlas "$here" "$@"
