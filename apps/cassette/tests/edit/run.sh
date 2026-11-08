#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the Cassette edit tests in WSL from a native-filesystem LF copy (never builds on /mnt/c):
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-cassette-edit/apps/cassette/tests/edit/run.sh          (plain, -Werror)
#   wsl -e sh .../run.sh san                                                                    (ASan + UBSan)
# Refuses to start when another cmake/make/ninja/scons build is running; uses a fresh build dir; -j3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
san=""
[ "$1" = "san" ] && san="SAN=1"
work=$(mktemp -d "${TMPDIR:-/tmp}/cassette-edit-build-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app/tests"
(cd "$app" && tar cf - src tests/edit) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name Makefile \) -exec sed -i 's/\r$//' {} +
make -C "$work/app/tests/edit" APP="$work/app" BUILD="$work/build" $san -j3 test
