#!/bin/sh
# SPDX-License-Identifier: MIT
# Stages and packages the `reticulum` service package (no compiled code: pure Python, one tree for arm64 and amd64).
#   sh apps/reticulum/build.sh vendor     fetch the two pinned wheels into $CDK_BUILD/wheels (network once), check their
#                                         sha256, unpack them into root/opt/reticulum/py, write LICENSE.rns / LICENSE.lxmf,
#                                         copy bridge/reticulum_bridge into root/opt/reticulum/bridge (run after a pin
#                                         change or a bridge edit; commit the result)
#   sh apps/reticulum/build.sh check      the committed root/ equals what `vendor` would write (no network once cached)
#   sh apps/reticulum/build.sh package    both .deb files into ${OUT:-$CDK_BUILD/pkg-out}, never into packages/
#   sh apps/reticulum/build.sh test       package, then install the amd64 deb with dpkg -x into a scratch root and run
#                                         tests/package_test.sh against it (this machine must be x86-64 Linux or WSL)
# $CDK_BUILD defaults to ~/cdk-build/reticulum. SOURCE_DATE_EPOCH defaults to 2026-11-23 so that the same tree gives the
# same bytes. Publishing (packages/, the registries) is a separate step: python3 tools/make_registry.py --no-build.
set -eu
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/../.." && pwd)
build=${CDK_BUILD:-$HOME/cdk-build/reticulum}
out=${OUT:-$build/pkg-out}
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1795392000}
mode=${1:-package}

case $mode in
    vendor) exec python3 -I "$here/tools/vendor.py" --cache "$build/wheels" ;;
    check) exec python3 -I "$here/tools/vendor.py" --cache "$build/wheels" --check ;;
    package | test)
        python3 -I "$here/tools/vendor.py" --cache "$build/wheels" --check
        # tools/build_deb.py gives 755 to every file with an exec bit: on a Windows drive seen from WSL (/mnt/c) all
        # files have one, so the modes would be wrong. Build there with Windows Python instead (tools/README.md).
        if [ -n "$(find "$here/root" -type f -perm -u+x ! -path '*/py/*' -print -quit)" ]; then
            echo "files with the exec bit under root/ (a /mnt/c checkout?): build with Windows Python:" >&2
            echo "  python tools/make_registry.py --only reticulum --arch arm64 --out <folder>   (and --arch amd64)" >&2
            exit 1
        fi
        for arch in arm64 amd64; do
            python3 "$repo/tools/make_registry.py" --owner "${OWNER:-OSRdesign}" --repo "${REPO:-cyberdeck-zero-apps}" --only reticulum --arch "$arch" --out "$out"
        done
        ls -l "$out"/reticulum_*.deb
        if [ "$mode" = test ]; then
            exec sh "$here/tests/package_test.sh" "$out"/reticulum_*_amd64.deb
        fi
        ;;
    *) echo "usage: build.sh vendor|check|package|test" >&2; exit 2 ;;
esac
