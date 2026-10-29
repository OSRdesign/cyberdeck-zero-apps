#!/bin/sh
# Builds Klondike Solitaire in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/klondike-solitaire/build.sh [cross|host|all] [--arch arm64|amd64] [--strict] [--sync]
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/; host: unit tests (the core engine), headless scenes.
# Then the package (a draft while the UI is in development): python3 tools/make_registry.py --only klondike-solitaire --out <dir>
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name KlondikeSolitaire "$here" "$@"
