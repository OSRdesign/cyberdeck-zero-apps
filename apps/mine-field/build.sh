#!/bin/sh
# Builds Mine Field in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mine-field/build.sh [cross|host|all] [--arch arm64|amd64] [--strict] [--sync]
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/; host: unit tests (the core engine), headless scenes.
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name MineField "$here" "$@"
