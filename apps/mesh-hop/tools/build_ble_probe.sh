#!/bin/sh
# Builds tools/ble_probe.cpp (the BLE link of src/core/ble against a real board, run on the deck) in WSL:
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/tools/build_ble_probe.sh [cross|host|both] [OUT_DIR]
#   cross: aarch64 for the deck (Ubuntu's aarch64-linux-gnu-g++; libstdc++ linked in, so it needs only the deck's libc;
#          libsystemd.so.0 is opened at run time) -> OUT_DIR/ble_probe
#   host:  the PC, with ASan + UBSan, for tests/ble/run_mock.sh -> OUT_DIR/ble_probe-host
# OUT_DIR defaults to ${CDK_BUILD:-~/cdk-build/mesh-hop}/ble-probe. Copy the cross one to the deck and run it there:
#   ./ble_probe scan; ./ble_probe connect AA:BB:CC:DD:EE:FF --pin 123456
set -e
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
mode=${1:-both}
out=${2:-${CDK_BUILD:-$HOME/cdk-build/mesh-hop}/ble-probe}
mkdir -p "$out"
srcs="$here/ble_probe.cpp $app/src/core/*.cpp $app/src/core/ble/*.cpp"
if [ "$mode" = cross ] || [ "$mode" = both ]; then
    # shellcheck disable=SC2086
    aarch64-linux-gnu-g++ -std=c++17 -Wall -Wextra -O2 -I"$app/src/core" $srcs -static-libstdc++ -static-libgcc -lpthread \
        -o "$out/ble_probe"
    aarch64-linux-gnu-strip "$out/ble_probe"
    echo "== $out/ble_probe: $(stat -c %s "$out/ble_probe") bytes"
    aarch64-linux-gnu-readelf -d "$out/ble_probe" | grep NEEDED || true
fi
if [ "$mode" = host ] || [ "$mode" = both ]; then
    # shellcheck disable=SC2086
    g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
        -I"$app/src/core" $srcs -lpthread -o "$out/ble_probe-host"
    echo "== $out/ble_probe-host"
fi
