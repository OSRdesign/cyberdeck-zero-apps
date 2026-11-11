#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the Cassette ALSA-layer tests in WSL from a native-filesystem LF copy (never builds on /mnt/c):
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-cassette-alsa/apps/cassette/tests/alsa/run.sh [SAN=thread|SAN=address]
# Refuses to start when another cmake/make/ninja/scons build is running; uses a fresh build dir; -j3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/cassette-alsa-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src/core/frame_sink.hpp src/core/ring.hpp src/core/ring.cpp src/core/alsa src/core/pulse tests/alsa \
    tests/pulse/fake_pulse.hpp tests/pulse/fake_pulse.cpp) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name Makefile \) -exec sed -i 's/\r$//' {} +
make -C "$work/app/tests/alsa" APP="$work/app" BUILD="$work/build" -j3 "$@" test
