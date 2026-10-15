#!/bin/sh
# Builds Net Toolbox in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/net-toolbox/build.sh [cross|host|all] [--strict] [--sync]
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/; host: unit tests (parsers on the fixtures of the deck,
#   a Pi 3A+ and a Pi 5, DNS packets, ping statistics, the diagnosis on fakes, sockets on loopback), headless scenes.
# Then the package (a draft): python3 tools/make_registry.py --only net-toolbox --out <dir outside the repo>
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name NetToolbox "$here" "$@"
