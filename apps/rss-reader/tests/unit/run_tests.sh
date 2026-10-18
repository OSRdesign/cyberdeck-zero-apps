#!/bin/sh
# Host tests of the RSS Reader core (src/core: xml, feed, html_text, discover, fetch, catalog, store, refresh, opml). Needs
# g++, python3 and curl; no LVGL, no board, no internet (a local server on 127.0.0.1 stands in for the web). Run alone:
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/tests/unit/run_tests.sh
#   SANITIZE=1 ...   the same under -fsanitize=address,undefined
# Output goes to ${OUT:-$HOME/cdk-build/rss-reader/unit}.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
CORE=$APP/src/core
OUT=${OUT:-$HOME/cdk-build/rss-reader/unit}
mkdir -p "$OUT"
FLAGS="-std=c++17 -Wall -Wextra -O1 -g"
if [ -n "${SANITIZE:-}" ]; then
    FLAGS="$FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
    BIN=$OUT/test_core_asan
else
    BIN=$OUT/test_core
fi
# shellcheck disable=SC2086
g++ $FLAGS -DTEST_DATA_DIR="\"$HERE/../data\"" -DCATALOG_PATH="\"$APP/data/catalog.json\"" -I"$CORE" -I"$HERE" \
    "$HERE"/test_*.cpp "$CORE"/*.cpp -lpthread -o "$BIN"

SERVER_PID=
if command -v python3 >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
    rm -f "$OUT/server.port"
    python3 "$HERE/local_server.py" "$OUT/server.port" &
    SERVER_PID=$!
    trap 'kill $SERVER_PID 2>/dev/null || true' EXIT
    i=0
    while [ ! -s "$OUT/server.port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    if [ -s "$OUT/server.port" ]; then RSS_TEST_SERVER="http://127.0.0.1:$(cat "$OUT/server.port")"; export RSS_TEST_SERVER; fi
fi
"$BIN"
