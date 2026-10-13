#!/bin/sh
# Builds the System Monitor in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/system-monitor/build.sh [cross|host|all] [--strict] [--sync]
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/; host: unit tests (parsers on the fixtures of a Pi
#   Zero 2 W, a Pi 3A+ and a Pi 5), headless scenes on those fixtures (SYSTEM_MONITOR_FIXTURE), contact sheets.
# Then the package (a draft): python3 tools/make_registry.py --only system-monitor --out <dir outside the repo>
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name SystemMonitor "$here" "$@"
