#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build_app.sh: build one cdk app (a native fullscreen app on the launcher's cp0_lvgl) in WSL.
#
#   apps/_common/build_app.sh --name ProjName <app dir> [cross|host|all] [--strict] [--sync]
#
#   cross  aarch64 binary for the boards: copies the app (src/), apps/_common and the launcher's LVGL FreeType glue into
#          a scratch project <launcher tree>/projects/<ProjName>, runs scons there under the shared build lock, strips
#          the result into <app>/root/usr/share/APPLaunch/bin/M5CardputerZero-<app id> and writes
#          <app>/root/usr/share/doc/<app id>/build-info; prints its size and its NEEDED libraries.
#   host   the PC side: the app's headless harness and unit tests (make -C apps/_common/headless, LVGL from the
#          launcher tree's SDK), then every test (test-common, test-<app id>) and every scene of <app>/tests/scenes
#          (PNG shots, layout checks, contact sheets in ~/cdk-build/<app id>/host/out/sheets).
#   all    both (default).
#
# The launcher: LAUNCHER_SRC (default /mnt/c/CLAUDE/zero7/launcher) is the git checkout the pin is checked against;
# LAUNCHER_TREE (default ~/zero7/<its folder name>) is its WSL build tree, whose SDK/ holds LVGL and the aarch64
# sysroot (tools/dev/wsl-build.sh of the launcher keeps it in sync and seeds it).
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
STRICT=0
SYNC=0
while (($#)); do
    case $1 in
        --name) NAME=${2:?--name needs a project name}; shift 2 ;;
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
APP=$(cd -- "$APP" && pwd)
ID=$(basename -- "$APP")
[[ -f $APP/app.json && -d $APP/src ]] || { echo "$APP: no app.json or src/" >&2; exit 2; }

LAUNCHER_SRC=${LAUNCHER_SRC:-/mnt/c/CLAUDE/zero7/launcher}
LAUNCHER_TREE=${LAUNCHER_TREE:-$HOME/zero7/$(basename -- "$LAUNCHER_SRC")}
VENV=${VENV:-$HOME/zero7/venv}
LOCK=/tmp/wsl-build.lock
BUILD=${CDK_BUILD:-$HOME/cdk-build/$ID}
GEN=$BUILD/gen
[[ -d $LAUNCHER_TREE/SDK/github_source/lvgl/lvgl_9_5 ]] || {
    echo "no LVGL in $LAUNCHER_TREE/SDK: build the launcher once there (tools/dev/wsl-build.sh)" >&2
    exit 2
}

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

# the build tree must hold the checkout's cp0_lvgl and FreeType glue
tree_differs() {
    ! diff -rq -x build -x dist "$LAUNCHER_SRC/ext_components/cp0_lvgl" "$LAUNCHER_TREE/ext_components/cp0_lvgl" > /dev/null 2>&1 ||
        ! diff -rq "$LAUNCHER_SRC/projects/APPLaunch/main/lvgl" "$LAUNCHER_TREE/projects/APPLaunch/main/lvgl" > /dev/null 2>&1
}
if [[ -d $LAUNCHER_SRC/ext_components ]] && tree_differs; then
    if ((SYNC)); then
        echo "== sync $LAUNCHER_SRC -> $LAUNCHER_TREE (the launcher's tools/dev/lib.sh, under the build lock)"
        (
            # shellcheck disable=SC1091
            . "$LAUNCHER_SRC/tools/dev/lib.sh"
            take_build_lock
            sync_tree "$LAUNCHER_SRC" "$LAUNCHER_TREE"
        )
    else
        echo "ERROR: $LAUNCHER_TREE does not hold the checkout's cp0_lvgl: run the launcher's tools/dev/wsl-build.sh" \
            "or pass --sync" >&2
        exit 1
    fi
fi

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
    local proj=$LAUNCHER_TREE/projects/$NAME
    local bin=$APP/root/usr/share/APPLaunch/bin/M5CardputerZero-$ID
    stamp aarch64
    echo "== cross: $proj"
    mkdir -p "$proj"
    rm -rf "$proj/main"
    mkdir -p "$proj/main/app" "$proj/main/common/stub" "$proj/main/lvgl_ovr"
    cp -r "$APP/src/." "$proj/main/app/"
    cp "$COMMON"/*.hpp "$COMMON"/*.cpp "$proj/main/common/"
    cp "$COMMON"/stub/*.hpp "$COMMON"/stub/*.cpp "$proj/main/common/stub/"
    cp "$GEN/cdk_build_info.h" "$proj/main/common/"
    cp "$LAUNCHER_TREE"/projects/APPLaunch/main/lvgl/lv_freetype* "$proj/main/lvgl_ovr/"
    cp "$COMMON/scons/SConstruct" "$proj/SConstruct"
    cp "$COMMON/scons/main.SConstruct" "$proj/main/SConstruct"
    cp "$COMMON/scons/cross_config_defaults.mk" "$proj/linux_x86_cross_cp0_config_defaults.mk"
    (
        cd "$proj"
        # shellcheck disable=SC1091
        [[ -f $VENV/bin/activate ]] && . "$VENV/bin/activate"
        export APPLAUNCH_HW=pizero2w CONFIG_REPO_AUTOMATION=y CONFIG_DEFAULT_FILE=linux_x86_cross_cp0_config_defaults.mk
        flock "$LOCK" scons -j"$(nproc)"
    )
    mkdir -p "$(dirname "$bin")"
    cp "$proj/dist/$NAME" "$bin"
    aarch64-linux-gnu-strip "$bin"
    chmod 755 "$bin"
    local doc=$APP/root/usr/share/doc/$ID
    mkdir -p "$doc"
    {
        echo "$ID $VERSION"
        echo "launcher $HEAD ($PIN against $REF)"
        echo "built $(date -u +%F) aarch64"
    } > "$doc/build-info"
    echo "== $bin: $(stat -c %s "$bin") bytes stripped"
    file "$bin" 2> /dev/null || true
    aarch64-linux-gnu-readelf -d "$bin" | grep NEEDED || true
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
