#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build_app.sh: build one cdk app (a native fullscreen app on the launcher's cp0_lvgl) in WSL.
#
#   apps/_common/build_app.sh --name ProjName <app dir> [cross|host|all] [--arch arm64|amd64] [--strict] [--sync]
#
#   cross  the device binary: copies the app (src/), apps/_common and the launcher's LVGL FreeType glue into a scratch
#          project <launcher tree>/projects/<ProjName>, runs scons there under the shared build lock, strips the result
#          into <app>/<root>/usr/share/APPLaunch/bin/<app id> and writes <app>/<root>/usr/share/doc/<app id>/build-info
#          (with the architecture); prints its size and its NEEDED libraries. By --arch:
#            arm64 (default)  aarch64 for the boards, cross-built with the SDK sysroot; <root> = root
#            amd64            x86-64 for PCs (report 028), a native build in the launcher's amd64 tree with the host gcc
#                             and the build box's -dev packages (pkg-config: libinput xkbcommon libudev dbus-1 gio-2.0
#                             freetype2); <root> = root.amd64, the overlay tools/make_registry.py lays over root/
#   host   the PC side: the app's headless harness and unit tests (make -C apps/_common/headless, LVGL from the
#          launcher tree's SDK), then every test (test-common, test-<app id>) and every scene of <app>/tests/scenes
#          (PNG shots, layout checks, contact sheets in ~/cdk-build/<app id>/host/out/sheets). The same for both
#          architectures (it always uses LAUNCHER_TREE).
#   all    both (default).
#
# The launcher: LAUNCHER_SRC (default /mnt/c/CLAUDE/zero7/launcher) is the git checkout the pin is checked against;
# LAUNCHER_TREE (default ~/zero7/<its folder name>) is its WSL build tree, whose SDK/ holds LVGL and the aarch64
# sysroot (tools/dev/wsl-build.sh of the launcher keeps it in sync and seeds it); LAUNCHER_TREE_AMD64 (default
# ~/zero7/<its folder name>-amd64, the tree of tools/dev/wsl-build.sh --arch amd64) is the one of the amd64 builds.
# Without the -dev packages installed, a private root of them works through PKG_CONFIG_PATH and CONFIG_TOOLCHAIN_FLAGS
# (-isystem <root>/usr/include ... -L<root>/usr/lib/x86_64-linux-gnu -Wl,-rpath-link,...) in the environment.
#   pin       apps/_common/LAUNCHER_REF. "pinned" = the checkout's HEAD is that commit; "same-tree" = another commit
#             with identical ext_components/cp0_lvgl, projects/APPLaunch/main/lvgl and the render harness LVGL config;
#             "differs" = a warning (an error with --strict). Uncommitted changes in the checkout are not looked at
#             (git status over /mnt/c from WSL takes minutes): build from a clean checkout.
#   tree      the build tree's cp0_lvgl and FreeType glue must equal the checkout's: an error, or --sync copies the
#             checkout into the tree first (the launcher's own tools/dev/lib.sh sync_tree, under the build lock).
# Every compile runs under flock /tmp/wsl-build.lock (one build at a time on the build box).
set -euo pipefail

usage() { awk 'NR > 3 && /^#/ { sub(/^# ?/, ""); print; next } NR > 3 { exit }' "$0"; }

COMMON=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
NAME=""
APP=""
MODE=all
ARCH=arm64
STRICT=0
SYNC=0
while (($#)); do
    case $1 in
        --name) NAME=${2:?--name needs a project name}; shift 2 ;;
        --arch) ARCH=${2:?--arch needs arm64 or amd64}; shift 2 ;;
        --strict) STRICT=1; shift ;;
        --sync) SYNC=1; shift ;;
        cross | host | all) MODE=$1; shift ;;
        -h | --help) usage; exit 0 ;;
        -*) echo "unknown option $1 (see -h)" >&2; exit 2 ;;
        *) APP=$1; shift ;;
    esac
done
[[ -n $APP && -n $NAME ]] || { usage >&2; exit 2; }
[[ $NAME =~ ^[A-Za-z][A-Za-z0-9_]*$ ]] || { echo "--name: letters, digits and _ only (a scons project)" >&2; exit 2; }
case $ARCH in
    arm64 | amd64) ;;
    *) echo "--arch $ARCH: arm64 or amd64" >&2; exit 2 ;;
esac
APP=$(cd -- "$APP" && pwd)
ID=$(basename -- "$APP")
[[ -f $APP/app.json && -d $APP/src ]] || { echo "$APP: no app.json or src/" >&2; exit 2; }

LAUNCHER_SRC=${LAUNCHER_SRC:-/mnt/c/CLAUDE/zero7/launcher}
LAUNCHER_TREE=${LAUNCHER_TREE:-$HOME/zero7/$(basename -- "$LAUNCHER_SRC")}
LAUNCHER_TREE_AMD64=${LAUNCHER_TREE_AMD64:-$HOME/zero7/$(basename -- "$LAUNCHER_SRC")-amd64}
VENV=${VENV:-$HOME/zero7/venv}
LOCK=/tmp/wsl-build.lock
BUILD=${CDK_BUILD:-$HOME/cdk-build/$ID}
GEN=$BUILD/gen
# the trees this run uses: the device build's (by --arch) and the host build's (always LAUNCHER_TREE)
DEVICE_TREE=$LAUNCHER_TREE
[[ $ARCH == amd64 ]] && DEVICE_TREE=$LAUNCHER_TREE_AMD64
TREES=()
if [[ $MODE != host ]]; then TREES+=("$DEVICE_TREE"); fi
if [[ $MODE != cross && $DEVICE_TREE != "$LAUNCHER_TREE" || $MODE == host ]]; then TREES+=("$LAUNCHER_TREE"); fi
for tree in "${TREES[@]}"; do
    if [[ ! -d $tree/SDK/github_source/lvgl/lvgl_9_5 ]]; then
        hint=
        if [[ $tree == "$DEVICE_TREE" && $ARCH == amd64 ]]; then hint=" --arch amd64"; fi
        echo "no LVGL in $tree/SDK: build the launcher once there (tools/dev/wsl-build.sh$hint)" >&2
        exit 2
    fi
done
if [[ $ARCH == amd64 && $MODE != host ]]; then
    [[ $(uname -m) == x86_64 ]] || { echo "--arch amd64 builds natively: run it on an x86_64 host" >&2; exit 1; }
    MODS="libinput xkbcommon libudev dbus-1 gio-2.0 freetype2"
    # shellcheck disable=SC2086
    if ! command -v gcc > /dev/null || ! command -v g++ > /dev/null || ! pkg-config --exists $MODS 2> /dev/null; then
        echo "--arch amd64: gcc, g++, pkg-config and the -dev packages are needed (pkg-config: $MODS):" >&2
        echo "  sudo apt install gcc g++ pkg-config libinput-dev libxkbcommon-dev libudev-dev libdbus-1-dev" \
            "libglib2.0-dev libfreetype-dev" >&2
        exit 1
    fi
fi

VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$APP/app.json")

# ---------------------------------------------------------------- the launcher pin
PINNED_PATHS=(ext_components/cp0_lvgl projects/APPLaunch/main/lvgl projects/APPLaunch/tests/render/config)
REF=$(grep -Eo '^[0-9a-f]{40}' "$COMMON/LAUNCHER_REF" | head -n1)
HEAD=unknown
PIN=unchecked
if git -C "$LAUNCHER_SRC" rev-parse --git-dir > /dev/null 2>&1; then
    HEAD=$(git -C "$LAUNCHER_SRC" rev-parse HEAD)
    if [[ $HEAD == "$REF" ]]; then
        PIN=pinned
    elif git -C "$LAUNCHER_SRC" cat-file -e "$REF^{commit}" 2> /dev/null &&
        git -C "$LAUNCHER_SRC" diff --quiet "$REF" "$HEAD" -- "${PINNED_PATHS[@]}"; then
        PIN=same-tree
    else
        PIN=differs
    fi
fi
echo "== $ID $VERSION: launcher $LAUNCHER_SRC at ${HEAD:0:7}, pin ${REF:0:7}: $PIN"
if [[ $PIN == differs || $PIN == unchecked ]]; then
    echo "WARNING: the launcher's cp0_lvgl is not the pinned one (apps/_common/LAUNCHER_REF)" >&2
    ((STRICT)) && { echo "--strict: stopping" >&2; exit 1; }
fi

# each build tree used must hold the checkout's cp0_lvgl and FreeType glue
tree_differs() { # tree
    ! diff -rq -x build -x dist "$LAUNCHER_SRC/ext_components/cp0_lvgl" "$1/ext_components/cp0_lvgl" > /dev/null 2>&1 ||
        ! diff -rq "$LAUNCHER_SRC/projects/APPLaunch/main/lvgl" "$1/projects/APPLaunch/main/lvgl" > /dev/null 2>&1
}
for tree in "${TREES[@]}"; do
    if [[ -d $LAUNCHER_SRC/ext_components ]] && tree_differs "$tree"; then
        if ((SYNC)); then
            echo "== sync $LAUNCHER_SRC -> $tree (the launcher's tools/dev/lib.sh, under the build lock)"
            (
                # shellcheck disable=SC1091
                . "$LAUNCHER_SRC/tools/dev/lib.sh"
                take_build_lock
                sync_tree "$LAUNCHER_SRC" "$tree"
            )
        else
            echo "ERROR: $tree does not hold the checkout's cp0_lvgl: run the launcher's tools/dev/wsl-build.sh" \
                "or pass --sync" >&2
            exit 1
        fi
    fi
done

# ---------------------------------------------------------------- version stamp
mkdir -p "$GEN"
stamp() { # target
    cat > "$GEN/cdk_build_info.h.new" << EOF
/* written by apps/_common/build_app.sh */
#define CDK_APP_VERSION "$VERSION"
#define CDK_LAUNCHER_REF "$HEAD"
#define CDK_LAUNCHER_PIN "$PIN"
#define CDK_BUILD_DATE "$(date -u +%F)"
#define CDK_BUILD_TARGET "$1"
EOF
    # unchanged contents keep the old file (and make's timestamps)
    if cmp -s "$GEN/cdk_build_info.h.new" "$GEN/cdk_build_info.h"; then rm "$GEN/cdk_build_info.h.new"
    else mv "$GEN/cdk_build_info.h.new" "$GEN/cdk_build_info.h"; fi
}

build_cross() {
    local proj=$DEVICE_TREE/projects/$NAME
    local root=root target=aarch64 config=linux_x86_cross_cp0_config_defaults.mk tools=aarch64-linux-gnu-
    if [[ $ARCH == amd64 ]]; then
        # native: the cross configuration without the toolchain prefix and the NEON lines, under a name without
        # "cross" (SConstruct then uses no sysroot), as the launcher's linux_x86_64_native_cp0_config_defaults.mk
        root=root.amd64 target=x86_64 config=linux_x86_64_native_cp0_config_defaults.mk tools=
    fi
    local bin=$APP/$root/usr/share/APPLaunch/bin/$ID
    stamp "$target"
    echo "== cross ($ARCH, $target): $proj"
    mkdir -p "$proj"
    rm -rf "$proj/main"
    mkdir -p "$proj/main/app" "$proj/main/common" "$proj/main/lvgl_ovr"
    cp -r "$APP/src/." "$proj/main/app/"
    # vendored sources an app includes as ../../third_party/... from its src/ (a header-only library: not compiled by itself)
    if [[ -d $APP/third_party ]]; then mkdir -p "$proj/main/third_party"; cp -r "$APP/third_party/." "$proj/main/third_party/"; fi
    cp "$COMMON"/*.hpp "$COMMON"/*.cpp "$proj/main/common/"
    cp "$GEN/cdk_build_info.h" "$proj/main/common/"
    cp "$DEVICE_TREE"/projects/APPLaunch/main/lvgl/lv_freetype* "$proj/main/lvgl_ovr/"
    cp "$COMMON/scons/SConstruct" "$proj/SConstruct"
    cp "$COMMON/scons/main.SConstruct" "$proj/main/SConstruct"
    if [[ $ARCH == amd64 ]]; then
        sed 's/\r$//' "$COMMON/scons/cross_config_defaults.mk" |
            grep -v -e '^CONFIG_TOOLCHAIN_PREFIX=' -e 'LV_DRAW_SW_ASM_NEON' -e 'LV_USE_DRAW_SW_ASM' > "$proj/$config"
    else
        cp "$COMMON/scons/cross_config_defaults.mk" "$proj/$config"
    fi
    (
        cd "$proj"
        # shellcheck disable=SC1091
        [[ -f $VENV/bin/activate ]] && . "$VENV/bin/activate"
        export APPLAUNCH_HW=pizero2w CONFIG_REPO_AUTOMATION=y CONFIG_DEFAULT_FILE=$config
        flock "$LOCK" scons -j"$(nproc)"
    )
    mkdir -p "$(dirname "$bin")"
    cp "$proj/dist/$NAME" "$bin"
    "${tools}strip" "$bin"
    chmod 755 "$bin"
    local doc=$APP/$root/usr/share/doc/$ID
    mkdir -p "$doc"
    {
        echo "$ID $VERSION"
        echo "launcher $HEAD ($PIN against $REF)"
        echo "built $(date -u +%F) $target"
    } > "$doc/build-info"
    echo "== $bin: $(stat -c %s "$bin") bytes stripped"
    file "$bin" 2> /dev/null || true
    "${tools}readelf" -d "$bin" | grep NEEDED || true
}

build_host() {
    stamp host
    echo "== host: make -C $COMMON/headless (build $BUILD)"
    flock "$LOCK" make -C "$COMMON/headless" -j"$(nproc)" APP="$APP" LAUNCHER="$LAUNCHER_TREE" BUILD="$BUILD/host" \
        GEN="$GEN" > "$BUILD/host-make.log" 2>&1 || { tail -n 40 "$BUILD/host-make.log"; exit 1; }
    local failed=0 t s
    for t in "$BUILD/host/test-common" "$BUILD/host/test-$ID"; do
        [[ -x $t ]] || continue
        if "$t" > "$BUILD/$(basename "$t").log" 2>&1; then echo "PASS $(tail -n1 "$BUILD/$(basename "$t").log")"
        else echo "FAIL $(basename "$t") (log $BUILD/$(basename "$t").log)"; tail -n 20 "$BUILD/$(basename "$t").log"; failed=1; fi
    done
    rm -rf "$BUILD/host/out"
    for s in "$APP"/tests/scenes/*.scene; do
        [[ -f $s ]] || continue
        "$BUILD/host/$ID-headless" --scene "$s" --out "$BUILD/host/out" \
            --resources "$LAUNCHER_TREE/projects/APPLaunch/APPLaunch" || failed=1
    done
    if [[ -n ${WSL_DISTRO_NAME:-} && -d $BUILD/host/out/sheets ]]; then
        echo "sheets from Windows: \\\\wsl.localhost\\$WSL_DISTRO_NAME${BUILD//\//\\}\\host\\out\\sheets"
    fi
    return $failed
}

mkdir -p "$BUILD"
rc=0
case $MODE in
    cross) build_cross ;;
    host) build_host || rc=1 ;;
    all) build_cross; build_host || rc=1 ;;
esac
exit $rc
