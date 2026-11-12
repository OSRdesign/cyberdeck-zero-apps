#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Compiles and runs check_decode_abi.cpp against the -dev headers in an include root (default ~/cdk-build/vinyl-devroot/root,
# made with: apt-get download libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libmpg123-dev libvorbis-dev
# libogg-dev; dpkg-deb -x each into root/). A second root (e.g. Debian 13's FFmpeg 7.1 headers) is passed as $1.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
root=${1:-$HOME/cdk-build/vinyl-devroot/root}
work=$(mktemp -d "${TMPDIR:-/tmp}/vinyl-abi-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src/core/decode tests/abi) | (cd "$work/app" && tar xf -)
find "$work/app" -type f -exec sed -i 's/\r$//' {} +
inc="-I$root/usr/include -I$root/usr/include/x86_64-linux-gnu -I$root/usr/include/aarch64-linux-gnu"
${CXX:-g++} -std=c++17 -Wall -Wextra -Wno-deprecated-declarations -Wno-unused-variable -I"$work/app/src/core" $inc \
    "$work/app/tests/abi/check_decode_abi.cpp" "$work/app/src/core/decode/decode_libs.cpp" -ldl -o "$work/abi"
"$work/abi"
