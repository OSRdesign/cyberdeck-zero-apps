#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the Cassette app tests (task T6a) in WSL from a native-filesystem LF copy (never builds on /mnt/c):
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-cassette-app/apps/cassette/tests/app/run.sh [SAN=thread|SAN=address,undefined]
# ARGS=--quick leaves out the slow real-time deck tests. Refuses to start when another cmake/make/ninja/scons build is
# running; uses a fresh build dir; -j3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/cassette-app-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src/core tests/alsa tests/playback tests/app tests/fixture_codecs.hpp tests/fake_decoders.hpp) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name Makefile \) -exec sed -i 's/\r$//' {} +
make -C "$work/app/tests/app" APP="$work/app" BUILD="$work/build" -j3 "$@" test
