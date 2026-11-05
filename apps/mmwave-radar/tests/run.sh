#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the mmWave Radar core tests in WSL from a native-filesystem LF copy (never builds on /mnt/c):
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-mmwave/apps/mmwave-radar/tests/run.sh
# Refuses to start when another cmake/make/ninja/scons build is running; uses a fresh build dir; -j3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/mmw-build-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src tests) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name Makefile -o -name '*.csv' \) -exec sed -i 's/\r$//' {} +
make -C "$work/app/tests" APP="$work/app" BUILD="$work/build" -j3 test
