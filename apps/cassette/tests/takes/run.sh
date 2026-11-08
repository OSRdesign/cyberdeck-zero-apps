#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the Cassette takes tests in WSL from a native-filesystem LF copy (never builds on /mnt/c):
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-cassette-takes/apps/cassette/tests/takes/run.sh [san]
# "san" builds with ASan + UBSan. Refuses to start when another cmake/make/ninja/scons build is running; fresh build dir.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
san=0
[ "$1" = "san" ] && san=1
work=$(mktemp -d "${TMPDIR:-/tmp}/cassette-takes-build-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src tests app.json) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name Makefile -o -name '*.json' \) -exec sed -i 's/\r$//' {} +
make -C "$work/app/tests/takes" APP="$work/app" BUILD="$work/build" SAN="$san" -j3 test
