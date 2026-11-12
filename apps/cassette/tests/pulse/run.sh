#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the Cassette sound-server tests in WSL from a native-filesystem LF copy (never builds on /mnt/c):
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-bt-pulse/apps/cassette/tests/pulse/run.sh [SAN=thread|SAN=address,undefined]
# 1. the tests on the fake server (always), plus the real-library group with whatever the system has (skipped when
#    libpulse.so.0 does not load), and the real-server smoke test real-pulse-test (skipped when libpulse or a server is
#    missing; on a WSLg PC it reaches WSLg's PulseAudio through PULSE_SERVER)
# 2. the same real runs once per entry of PULSE_LIBPATHS="<LD_LIBRARY_PATH> ..." (folders joined by ':')
# 3. the ABI check (check_pulse_abi.cpp) against each include root of PULSE_INCLUDES="<dir> ..." (default: /usr/include
#    when it holds pulse/pulseaudio.h); skipped with a note when there are no headers
# No install is needed for 2 and 3: a private root works (apt-get download libpulse-dev libpulse0 libsndfile1 libasyncns0
# libflac12t64 libogg0 libopus0 libvorbis0a libvorbisenc2 libmpg123-0t64 libmp3lame0, then dpkg-deb -x each .deb into one
# folder; LD_LIBRARY_PATH needs <root>/usr/lib/x86_64-linux-gnu and <root>/usr/lib/x86_64-linux-gnu/pulseaudio).
# Refuses to start when another cmake/make/ninja/scons build is running; uses a fresh build dir; -j2.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/cassette-pulse-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src/core/pulse src/core/alsa src/core/ring.hpp src/core/ring.cpp src/core/frame_sink.hpp \
    tests/pulse tools/pulse_capture_test.cpp) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name Makefile \) -exec sed -i 's/\r$//' {} +
make -C "$work/app/tests/pulse" APP="$work/app" BUILD="$work/build" -j2 "$@" all
echo "== pulse tests (fake server; real libpulse of the system if any)"
"$work/build/test-cassette-pulse"
echo "== real-server smoke test (system libraries)"
"$work/build/real-pulse-test"
for p in ${PULSE_LIBPATHS:-}; do
    echo "== pulse tests with the real library from $p"
    LD_LIBRARY_PATH="$p" "$work/build/test-cassette-pulse" | tail -n 3
    echo "== real-server smoke test with the library from $p"
    LD_LIBRARY_PATH="$p" "$work/build/real-pulse-test" ${PULSE_SMOKE_SOURCE:-} ${PULSE_SMOKE_SECONDS:-}
done
incs=${PULSE_INCLUDES:-}
if [ -z "$incs" ] && [ -f /usr/include/pulse/pulseaudio.h ]; then
    incs=/usr/include
fi
if [ -z "$incs" ]; then
    echo "== ABI check skipped: no pulseaudio headers (libpulse-dev, or PULSE_INCLUDES)"
fi
for i in $incs; do
    echo "== ABI check against $i"
    ${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -I"$i" -I"$work/app/src/core/pulse" \
        "$work/app/tests/pulse/check_pulse_abi.cpp" -o "$work/build/check-pulse-abi"
    "$work/build/check-pulse-abi"
done
