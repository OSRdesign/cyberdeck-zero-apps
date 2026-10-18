#!/bin/sh
# RSS Reader, phase 1 (core library only): builds and runs the host unit tests in WSL.
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/rss-reader/build.sh [test|asan]
#   test (default): tests/unit/run_tests.sh;  asan: the same with -fsanitize=address,undefined.
# The UI, the cross build and the package come with phases 2 and 3 (see apps/mesh-hop/build.sh for the shape they will take).
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=${CDK_BUILD:-$HOME/cdk-build/rss-reader}
mode=${1:-test}
case $mode in
    test) OUT="$build/unit" sh "$here/tests/unit/run_tests.sh" ;;
    asan) SANITIZE=1 OUT="$build/unit" sh "$here/tests/unit/run_tests.sh" ;;
    *) echo "usage: $0 [test|asan]" >&2; exit 2 ;;
esac
