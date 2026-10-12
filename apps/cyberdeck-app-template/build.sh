#!/bin/sh
# Builds the App Template in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/cyberdeck-app-template/build.sh [cross|host|all] [--strict] [--sync]
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/; host: unit tests, headless scenes, contact sheets.
# Then the package (a draft, never published): python3 tools/make_registry.py --only cyberdeck-app-template --out <dir>
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name CdkAppTemplate "$here" "$@"
