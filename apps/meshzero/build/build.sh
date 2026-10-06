#!/bin/sh
# Builds the MeshZero app (aarch64) and installs the binary into the package tree.
# Run from Windows:  wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/meshzero/build/build.sh
#
# 1. copies src/ into the launcher tree projects/MeshZero (build scratch only, kept out of git through
#    .git/info/exclude: the launcher repo never tracks app code);
# 2. runs scons there, under the shared WSL build lock;
# 3. copies dist/MeshZero to root/usr/share/APPLaunch/bin/M5CardputerZero-meshzero.
set -e
APP=$(cd "$(dirname "$0")/.." && pwd)
LAUNCHER=${LAUNCHER:-/mnt/c/CLAUDE/zero7/launcher}
PROJ=$LAUNCHER/projects/MeshZero
BIN=$APP/root/usr/share/APPLaunch/bin/M5CardputerZero-meshzero

[ -d "$LAUNCHER/projects/APPLaunch" ] || { echo "launcher tree not found at $LAUNCHER" >&2; exit 1; }

# keep the scratch project out of the launcher repo
EXCLUDE=$LAUNCHER/.git/info/exclude
grep -qx 'projects/MeshZero/' "$EXCLUDE" 2>/dev/null || echo 'projects/MeshZero/' >> "$EXCLUDE"

# fresh copy of the sources (build/ and dist/ of the scratch project stay for incremental builds)
mkdir -p "$PROJ"
rm -rf "$PROJ/main"
cp -r "$APP/src/main" "$PROJ/main"
for f in SConstruct config_defaults.mk linux_x86_cross_cp0_config_defaults.mk \
         linux_x86_sdl2_config_defaults.mk mac_cross_cp0_config_defaults.mk; do
    cp "$APP/src/$f" "$PROJ/$f"
done

cd "$PROJ"
export PATH=$LAUNCHER/.venv-pizero2w/bin:$PATH
export APPLAUNCH_HW=pizero2w CONFIG_REPO_AUTOMATION=y CONFIG_DEFAULT_FILE=linux_x86_cross_cp0_config_defaults.mk
flock /tmp/wsl-build.lock scons -j"$(nproc)"

mkdir -p "$(dirname "$BIN")"
cp dist/MeshZero "$BIN"
chmod 755 "$BIN"
file "$BIN" 2>/dev/null || true
echo "installed $BIN"
