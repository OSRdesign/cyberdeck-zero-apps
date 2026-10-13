#!/bin/sh
# Builds the Serial Console in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/serial-console/build.sh [cross|host|all] [--strict] [--sync]
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/; host: unit tests (with the pseudo-terminal I/O
#   tests), headless scenes in the demonstration world (SERIAL_CONSOLE_DEMO), contact sheets.
# Then the package (a draft): python3 tools/make_registry.py --only serial-console --out <dir outside the repo>
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name SerialConsole "$here" "$@"
