#!/bin/sh
# Builds mmWave Radar in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mmwave-radar/build.sh [cross|host|all] [--arch arm64|amd64] [--strict] [--sync]
#   cross: the binary into root/ (arm64) or root.amd64/ (amd64); host: unit tests (decoder on the recorded fixtures,
#   detector and link on fake ports, tracker, settings, geometry, the scripted ports, the package files) and the headless
#   scenes (scripted ports, no serial device).
# The core tests alone, without LVGL: wsl -e sh <app>/tests/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
exec bash "$here/../_common/build_app.sh" --name MmwaveRadar "$here" "$@"
