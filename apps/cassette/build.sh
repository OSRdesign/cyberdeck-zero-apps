#!/bin/sh
# Builds Cassette in WSL (see apps/_common/build_app.sh -h). Only the core exists so far (task T1: no UI, no package):
# the core tests alone, without LVGL and without sound hardware:  wsl -e sh <app>/tests/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name Cassette "$here" "$@"
