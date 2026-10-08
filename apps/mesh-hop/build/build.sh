#!/bin/sh
# Builds the Mesh Hop app and installs the binary into the package tree.
#   aarch64 (the deck), run from Windows:
#     wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build/build.sh
#   x86-64 host build for the PC (headless mode, screenshots, scripted tests), binary in ${MESHHOP_HOST_OUT:-$HOME/mesh-hop-host}/mesh-hop:
#     wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/build/build.sh host
#
# 1. copies src/ into a scratch project of the launcher tree (projects/MeshHop, projects/MeshHopHost), kept out of git
#    through .git/info/exclude: the launcher repo never tracks app code (the build only needs its SDK: LVGL, toolchain);
# 2. runs scons there, under the shared WSL build lock (one build at a time);
# 3. aarch64: copies dist/MeshHop to root/usr/share/APPLaunch/bin/M5CardputerZero-mesh-hop.
set -e
MODE=${1:-cross}
APP=$(cd "$(dirname "$0")/.." && pwd)
LAUNCHER=${LAUNCHER:-/mnt/c/CLAUDE/zero7/launcher}
if [ "$MODE" = host ]; then NAME=MeshHopHost; else NAME=MeshHop; fi
PROJ=$LAUNCHER/projects/$NAME
BIN=$APP/root/usr/share/APPLaunch/bin/M5CardputerZero-mesh-hop

[ -d "$LAUNCHER/projects/APPLaunch" ] || { echo "launcher tree not found at $LAUNCHER" >&2; exit 1; }

EXCLUDE=$LAUNCHER/.git/info/exclude
grep -qx "projects/$NAME/" "$EXCLUDE" 2>/dev/null || echo "projects/$NAME/" >> "$EXCLUDE"

# fresh copy of the sources (build/ and dist/ of the scratch project stay for incremental builds)
mkdir -p "$PROJ"
rm -rf "$PROJ/main"
cp -r "$APP/src/main" "$PROJ/main"
for f in SConstruct config_defaults.mk linux_x86_cross_cp0_config_defaults.mk \
         linux_x86_sdl2_config_defaults.mk mac_cross_cp0_config_defaults.mk linux_x86_host_config_defaults.mk; do
    cp "$APP/src/$f" "$PROJ/$f"
done

cd "$PROJ"
export PATH=$LAUNCHER/.venv-pizero2w/bin:$PATH
export APPLAUNCH_HW=pizero2w CONFIG_REPO_AUTOMATION=y
if [ "$MODE" = host ]; then
    export CONFIG_DEFAULT_FILE=linux_x86_host_config_defaults.mk     # native compiler, system FreeType, no SDL window
else
    export CONFIG_DEFAULT_FILE=linux_x86_cross_cp0_config_defaults.mk
fi
flock /tmp/wsl-build.lock scons -j"$(nproc)"

if [ "$MODE" = host ]; then
    mkdir -p ${MESHHOP_HOST_OUT:-$HOME/mesh-hop-host}
    cp "dist/$NAME" ${MESHHOP_HOST_OUT:-$HOME/mesh-hop-host}/mesh-hop
    echo "host binary: ${MESHHOP_HOST_OUT:-$HOME/mesh-hop-host}/mesh-hop"
else
    mkdir -p "$(dirname "$BIN")"
    cp "dist/$NAME" "$BIN"
    aarch64-linux-gnu-strip "$BIN" 2>/dev/null || true       # the symbols are only needed for gdb: keep the package small
    chmod 755 "$BIN"
    file "$BIN" 2>/dev/null || true
    echo "installed $BIN"
fi
