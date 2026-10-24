#!/bin/sh
# Host tests of Markdown Notes: the core (src/core: the vault walk, names, the text codec, load and save, the atomic
# save, the changed-on-disk check, the trash, the settings, the ranking, links, tasks, dates), md4c's block model
# (markdown.cpp on third_party/md4c) and the package files (test_*.cpp: test_core); then the UI-independent logic of the
# pages (src/ui: ui_logic, keys, key_bridge; ui_tests.cpp: test_ui). Needs gcc and g++; no LVGL, no board, no network.
# Run alone:
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/markdown-notes/tests/unit/run_tests.sh
#   SANITIZE=1 ...   the same under -fsanitize=address,undefined
# Output goes to ${OUT:-$HOME/cdk-build/markdown-notes/unit}.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
CORE=$APP/src/core
UI=$APP/src/ui
OUT=${OUT:-$HOME/cdk-build/markdown-notes/unit}
mkdir -p "$OUT"
FLAGS="-std=c++17 -Wall -Wextra -O1 -g"
CFLAGS_C="-std=gnu11 -Wall -Wextra -O1 -g"
BIN=$OUT/test_core
UIBIN=$OUT/test_ui
if [ -n "${SANITIZE:-}" ]; then
    FLAGS="$FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
    CFLAGS_C="$CFLAGS_C -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
    BIN=$OUT/test_core_asan
    UIBIN=$OUT/test_ui_asan
fi
# md4c through the app's one-line wrapper (the same file the device and harness builds compile)
# shellcheck disable=SC2086
gcc $CFLAGS_C -c "$CORE/md4c_impl.c" -o "$BIN.md4c.o"
# shellcheck disable=SC2086
g++ $FLAGS -DAPP_DIR="\"$APP\"" -I"$CORE" -I"$HERE" "$HERE"/test_*.cpp "$CORE"/*.cpp "$BIN.md4c.o" -o "$BIN"
# shellcheck disable=SC2086
g++ $FLAGS -DAPP_DIR="\"$APP\"" -I"$CORE" -I"$UI" -I"$HERE" "$HERE/ui_tests.cpp" "$UI/ui_logic.cpp" "$UI/keys.cpp" \
    "$UI/key_bridge.cpp" "$CORE"/*.cpp "$BIN.md4c.o" -o "$UIBIN"
cd "$OUT"
rc=0
"$BIN" || rc=1
"$UIBIN" || rc=1
exit $rc
