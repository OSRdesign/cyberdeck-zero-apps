#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Builds vinyl-mixer-probe (see mixer_probe.cpp) in WSL, from a native-filesystem LF copy, nice 15:
#   wsl -e sh <app>/tools/mixer-probe/build.sh [OUT]
# OUT (default ~/cdk-build/vinyl-mixer-probe) gets arm64/vinyl-mixer-probe (aarch64, against the launcher SDK's sysroot:
# LAUNCHER_TREE, default ~/zero7/launcher) and amd64/vinyl-mixer-probe (the build box's compiler). libasound.so.2 is
# loaded at run time; libstdc++ and libgcc are linked in, so a board needs nothing else.
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/../.." && pwd)
out=${1:-$HOME/cdk-build/vinyl-mixer-probe}
tree=${LAUNCHER_TREE:-$HOME/zero7/launcher}
sysroot=$(ls -d "$tree"/SDK/github_source/static_lib_v* 2> /dev/null | grep -v '\.tar' | tail -n 1)
work=$(mktemp -d "${TMPDIR:-/tmp}/vinyl-probe-XXXXXX")
trap 'rm -rf "$work"' EXIT
(cd "$app" && tar cf - src/core/alsa src/core/volume tools/mixer-probe) | (cd "$work" && tar xf -)
find "$work" -type f \( -name '*.cpp' -o -name '*.hpp' \) -exec sed -i 's/\r$//' {} +
srcs="$work/tools/mixer-probe/mixer_probe.cpp $work/src/core/alsa/alsa_api.cpp $work/src/core/alsa/real_alsa.cpp $work/src/core/volume/output_volume.cpp"
flags="-std=c++17 -O2 -Wall -Wextra -Werror -I$work/src/core -static-libstdc++ -static-libgcc"
mkdir -p "$out/arm64" "$out/amd64"
if [ -n "$sysroot" ]; then
    # shellcheck disable=SC2086
    nice -n 15 aarch64-linux-gnu-g++ --sysroot="$sysroot" $flags -o "$out/arm64/vinyl-mixer-probe" $srcs -ldl -lpthread
    aarch64-linux-gnu-strip "$out/arm64/vinyl-mixer-probe"
    echo "== $out/arm64/vinyl-mixer-probe ($sysroot)"
else
    echo "no SDK sysroot under $tree/SDK/github_source: arm64 skipped" >&2
fi
# shellcheck disable=SC2086
nice -n 15 g++ $flags -o "$out/amd64/vinyl-mixer-probe" $srcs -ldl -lpthread
strip "$out/amd64/vinyl-mixer-probe"
echo "== $out/amd64/vinyl-mixer-probe"
