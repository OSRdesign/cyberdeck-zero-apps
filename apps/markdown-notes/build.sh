#!/bin/sh
# Builds and tests Markdown Notes in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/markdown-notes/build.sh [cross|host|all] [--arch amd64] [--strict] [--sync]
#   wsl -e sh .../build.sh test | asan | sim [WxH | both | sizes] [flow ...] | ui
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/markdown-notes (--arch amd64: the x86-64 one into
#          root.amd64/); --strict stops when the launcher is not the pinned one (apps/_common/LAUNCHER_REF);
#   host:  the shared headless harness and its scenes (tests/scenes, on a copy of the fixture vault tests/data/vault in
#          /tmp/markdown-notes-scene-vault), the scripted runner (markdown-notes-script, tests/flows/runner.mk), then the
#          unit tests;
#   test:  the unit tests only (tests/unit/run_tests.sh: the core, md4c's block model, the UI logic, the package files);
#   asan:  the same under AddressSanitizer + UBSan;
#   sim:   the scripted flows (tests/flows/*.script: seeded vaults, a fixed clock) at 640x480, WxH, "both" (640x480 and
#          800x480) or "sizes" (also 720x720 and 1280x800); needs a host build; shots in
#          ${NOTES_SHOTS:-${CDK_BUILD:-~/cdk-build/markdown-notes}/flows};
#   ui:    sim at the four sizes.
# Build folder: ${CDK_BUILD:-~/cdk-build/markdown-notes}. The launcher: LAUNCHER_SRC / LAUNCHER_TREE (build_app.sh -h).
# Then the package (a draft): python3 tools/make_registry.py --only markdown-notes --out <dir outside packages/>
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${CDK_BUILD:-$HOME/cdk-build/markdown-notes}
src=${LAUNCHER_SRC:-/mnt/c/CLAUDE/zero7/launcher}
tree=${LAUNCHER_TREE:-$HOME/zero7/$(basename -- "$src")}
shots=${NOTES_SHOTS:-$build/flows}
export CDK_HEADLESS_RESOURCES=${CDK_HEADLESS_RESOURCES:-$tree/projects/APPLaunch/APPLaunch}

case ${1:-} in
    test) OUT="$build/unit" exec sh "$here/tests/unit/run_tests.sh" ;;
    asan) SANITIZE=1 OUT="$build/unit" exec sh "$here/tests/unit/run_tests.sh" ;;
    sim)
        shift
        size=${1:-640x480}
        [ $# -gt 0 ] && shift
        exec sh "$here/tests/flows/run_flows.sh" "$build/host/markdown-notes-script" "$shots" "$size" "$@" ;;
    ui) exec sh "$here/tests/flows/run_flows.sh" "$build/host/markdown-notes-script" "$shots" sizes ;;
esac

# The scenes' vault: a fresh copy of the fixture vault (the scenes only read it: every size runs at once; portrait-band.scene
# writes, each size into its own note, in a second copy, /tmp/markdown-notes-scene-vault-band), its notes
# aged as tests/data/vault.ages says; two settings folders: the default one and one with open_in=editor (editor.scene,
# keyboard.scene: the scenes have no hold and no Ctrl key to reach the editor from the preview).
scene_vault() {
    v=/tmp/markdown-notes-scene-vault
    vb=/tmp/markdown-notes-scene-vault-band
    rm -rf "$v" "$vb" /tmp/markdown-notes-scene-config /tmp/markdown-notes-scene-config-editor
    mkdir -p /tmp/markdown-notes-scene-config-editor/markdown-notes
    printf 'vault=~/Notes\ntext_size=medium\nopen_in=editor\n' > /tmp/markdown-notes-scene-config-editor/markdown-notes/settings.conf
    mkdir -p "$v" "$vb"
    cp -r "$here/tests/data/vault/." "$v/"
    cp -r "$here/tests/data/vault/." "$vb/"
    now=$(date +%s)
    grep -v '^#' "$here/tests/data/vault.ages" | while read -r line; do
        [ -n "$line" ] || continue
        secs=${line##* }
        rel=${line% *}
        touch -d "@$((now - secs))" "$v/$rel"
        touch -d "@$((now - secs))" "$vb/$rel"
    done
}

rc=0
mode=all
for a in "$@"; do case $a in cross | host | all) mode=$a ;; esac; done
[ "$mode" != cross ] && scene_vault
bash "$here/../_common/build_app.sh" --name MarkdownNotes "$here" "$@" || rc=$?
if [ "$mode" != cross ]; then
    echo "== host: markdown-notes-script (tests/flows/runner.mk)"
    flock /tmp/wsl-build.lock make -f "$here/tests/flows/runner.mk" -j"$(nproc)" APP="$here" LAUNCHER="$tree" \
        BUILD="$build/host" GEN="$build/gen" ${LVBUILD:+LVBUILD="$LVBUILD"} > "$build/runner-make.log" 2>&1 ||
        { tail -n 40 "$build/runner-make.log"; exit 1; }
    echo "== $build/host/markdown-notes-script"
    echo "== host: unit tests (tests/unit/run_tests.sh)"
    mkdir -p "$build"
    if OUT="$build/unit" sh "$here/tests/unit/run_tests.sh" > "$build/unit.log" 2>&1; then
        grep -E "checks|FAIL" "$build/unit.log" | tail -n 6
    else
        tail -n 30 "$build/unit.log"
        rc=1
    fi
fi
exit $rc
