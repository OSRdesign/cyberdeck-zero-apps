#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the Cassette codec tests in WSL from a native-filesystem LF copy (never builds on /mnt/c):
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-cassette-codecs/apps/cassette/tests/codec/run.sh [SAN=address]
# 1. the tests on the fake libraries (always), plus the real-library group with whatever the system has (skipped when
#    libFLAC / libopusenc do not load)
# 2. the real-library group once per entry of CODEC_LIBPATHS="<LD_LIBRARY_PATH> ..." (each value may hold several
#    folders joined by ':')
# 3. the ABI check (check_codec_abi.cpp) against each include root of CODEC_INCLUDES="<dir> ..." (default: /usr/include
#    when it holds FLAC/stream_encoder.h and opus/opusenc.h); skipped with a note when there are no headers
# No install is needed for 2 and 3: a private root works (apt-get download libflac-dev libflac12t64 libopus-dev libopus0
# libopusenc-dev libopusenc0 libogg0, then dpkg-deb -x each .deb into one folder; docs/FORMATS.md).
# CODEC_MEASURE=1 also runs cassette-codec-measure (synthetic table) with the first CODEC_LIBPATHS entry.
# Refuses to start when another cmake/make/ninja/scons build is running; uses a fresh build dir; -j3.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/cassette-codec-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src/core tests/codec tests/fixture_codecs.hpp tests/fake_decoders.hpp) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name Makefile \) -exec sed -i 's/\r$//' {} +
make -C "$work/app/tests/codec" APP="$work/app" BUILD="$work/build" -j3 "$@" all
echo "== codec tests (fakes; real libraries of the system if any)"
"$work/build/test-codec"
for p in ${CODEC_LIBPATHS:-}; do
    echo "== codec tests with the real libraries from $p"
    LD_LIBRARY_PATH="$p" "$work/build/test-codec"
done
incs=${CODEC_INCLUDES:-}
if [ -z "$incs" ] && [ -f /usr/include/FLAC/stream_encoder.h ] && [ -f /usr/include/opus/opusenc.h ]; then
    incs=/usr/include
fi
if [ -z "$incs" ]; then
    echo "== ABI check skipped: no libFLAC / libopusenc headers (libflac-dev, libopusenc-dev, or CODEC_INCLUDES)"
fi
for i in $incs; do
    echo "== ABI check against $i"
    ${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -I"$i" -I"$i/opus" -I"$work/app/src/core/codec" \
        "$work/app/tests/codec/check_codec_abi.cpp" -o "$work/build/check-codec-abi"
    "$work/build/check-codec-abi"
done
if [ "${CODEC_MEASURE:-0}" = 1 ]; then
    first=$(echo "${CODEC_LIBPATHS:-}" | awk '{print $1}')
    echo "== cassette-codec-measure (libraries from ${first:-the system})"
    LD_LIBRARY_PATH="$first" "$work/build/cassette-codec-measure" --seconds "${CODEC_MEASURE_SECONDS:-30}"
fi
