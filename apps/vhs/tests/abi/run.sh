#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The FFmpeg ABI check of record/av_libs.hpp (brief 15.2), in WSL from a native-filesystem LF copy:
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-vhs-p1/apps/vhs/tests/abi/run.sh [ROOT ...]
# ROOT holds usr/include/libavformat ... (a private root, as Vinyl's): the 61 set (Debian 13, FFmpeg 7.1) and the 62
# set (Ubuntu 26.04, FFmpeg 8.0). Without arguments: every root of the list below that exists, and the build box's own
# headers when installed (the 60 set, FFmpeg 6.1). Making the roots without installing anything:
#   61: mkdir -p ~/cdk-build/vinyl-devroot71 && cd ~/cdk-build/vinyl-devroot71 && apt-get download (Debian 13's
#       libavformat-dev libavcodec-dev libavutil-dev ...) && dpkg-deb -x each into root
#   62: the FFmpeg 8.0 release tarball's headers (ffmpeg.org) under ~/cdk-build/vhs/ffmpeg62/root/usr/include, with the
#       generated libavutil/avconfig.h and ffversion.h a configure run writes
# A failure (a mismatch, a missing root that was named) exits 1: it blocks the gate.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$(dirname "$here")")
if [ $# -eq 0 ]; then
    set --
    for r in "$HOME/cdk-build/vinyl-devroot71/root" "$HOME/cdk-build/vhs/ffmpeg62/root" "$HOME/cdk-build/vinyl-devroot/root"; do
        [ -f "$r/usr/include/libavformat/avformat.h" ] || [ -f "$r/usr/include/x86_64-linux-gnu/libavformat/avformat.h" ] && set -- "$@" "$r"
    done
    [ -f /usr/include/x86_64-linux-gnu/libavformat/avformat.h ] || [ -f /usr/include/libavformat/avformat.h ] && set -- "$@" /
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/vhs-abi-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src/core/record/av_libs.hpp src/core/record/av_libs.cpp tests/abi) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' \) -exec sed -i 's/\r$//' {} +
fail=0
n=0
for root in "$@"; do
    n=$((n + 1))
    echo "== $root"
    if [ ! -f "$root/usr/include/libavformat/avformat.h" ] && [ ! -f "$root/usr/include/x86_64-linux-gnu/libavformat/avformat.h" ]; then
        echo "no FFmpeg headers under $root"
        fail=1
        continue
    fi
    inc="-I$root/usr/include -I$root/usr/include/x86_64-linux-gnu"
    [ "$root" = "/" ] && inc=""
    # shellcheck disable=SC2086
    if ! g++ -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-ignored-attributes -I"$work/app/src/core" $inc \
        "$work/app/tests/abi/check_av_abi.cpp" "$work/app/src/core/record/av_libs.cpp" -ldl -o "$work/abi$n"; then
        echo "abi: does not compile against $root (a signature differs: see the static_assert above)"
        fail=1
        continue
    fi
    "$work/abi$n" || fail=1
done
[ "$n" -gt 0 ] || { echo "abi: no header root found"; exit 1; }
exit $fail
