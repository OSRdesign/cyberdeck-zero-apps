#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Runs the Vinyl unit tests in WSL from a native-filesystem LF copy (never builds on /mnt/c), -Werror, -j2, nice 15:
#   wsl -e sh /mnt/c/CLAUDE/zero7/apps-vinyl/apps/vinyl/tests/run.sh [SAN=address,undefined]
# Then makes the real-library fixtures with the build box's encoders (ffmpeg, lame, flac, oggenc, opusenc; any missing
# tool leaves its files out and its case skipped) and runs the binary again with VINYL_FIXTURES. VINYL_CJK_MP3 may name
# the study's S_cjk-title.mp3 (report 059, 14.2) for the CJK tag check.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(dirname "$here")
if pgrep -x 'cmake|make|ninja|scons' > /dev/null 2>&1; then
    echo "another build is running: try later" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/vinyl-tests-XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/app"
(cd "$app" && tar cf - src tests third_party) | (cd "$work/app" && tar xf -)
find "$work/app" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name Makefile \) -exec sed -i 's/\r$//' {} +
nice -n 15 make -s -C "$work/app/tests" APP="$work/app" BUILD="$work/build" -j2 "$@" all
fx="$work/fixtures"
mkdir -p "$fx"
T="Vinyl test sine"
have() { command -v "$1" > /dev/null 2>&1; }
if have ffmpeg; then
    q="-hide_banner -loglevel error -y"
    ffmpeg $q -f lavfi -i "sine=frequency=440:duration=6:sample_rate=44100" -ac 2 -metadata title="$T" "$fx/sine.wav"
    ffmpeg $q -f lavfi -i "sine=frequency=1000:duration=6:sample_rate=96000" -ac 2 -c:a flac -sample_fmt s32 \
        -bits_per_raw_sample 24 -metadata title="$T" "$fx/sine24.flac"
    ffmpeg $q -i "$fx/sine.wav" -c:a aac -b:a 128k -metadata title="$T" "$fx/sine.m4a"
    ffmpeg $q -i "$fx/sine.wav" -c:a alac -metadata title="$T" "$fx/sine-alac.m4a"
    ffmpeg $q -i "$fx/sine.wav" -c:a flac "$fx/whole.flac"
    ffmpeg $q -i "$fx/sine.wav" -af atrim=end_sample=145530 -c:a flac "$fx/half1.flac"
    ffmpeg $q -i "$fx/sine.wav" -af atrim=start_sample=145530,asetpts=PTS-STARTPTS -c:a flac "$fx/half2.flac"
    have lame && lame --quiet --tt "$T" "$fx/sine.wav" "$fx/sine.mp3"
    have flac && flac --silent -f -T TITLE="$T" -o "$fx/sine.flac" "$fx/sine.wav"
    have oggenc && oggenc --quiet -t "$T" -o "$fx/sine.ogg" "$fx/sine.wav"
    have opusenc && opusenc --quiet --title "$T" "$fx/sine.wav" "$fx/sine.opus"
fi
echo "== vinyl unit tests (fakes; the real libraries on the fixtures of $fx)"
VINYL_FIXTURES="$fx" "$work/build/test-vinyl"
