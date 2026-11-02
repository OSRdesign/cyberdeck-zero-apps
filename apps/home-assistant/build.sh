#!/bin/sh
# Builds the Home Assistant app in WSL (see apps/_common/build_app.sh -h):
#   sh apps/home-assistant/build.sh [cross|host|all] [--arch arm64|amd64] [--strict] [--sync]
#   sh apps/home-assistant/build.sh integration
#   cross: the aarch64 binary into root/usr/share/APPLaunch/bin/ (x86-64 into root.amd64/ with --arch amd64);
#   host:  the unit tests (core: protocol, model, engine, simulated Home Assistant, files, QR code), the headless scenes
#          (every page with the simulated Home Assistant, HA_DEMO=<world>);
#   integration: the real libcurl client (src/core) against the fake Home Assistant server tests/fake_ha/fake_ha.py
#          (Python 3 stdlib): pairing, sign-in, refresh, 2,000 registry entries, 100 subscribed entities, fragmented
#          messages, service calls, pings, a reset link, an expired token, revoke. Needs a libcurl with WebSocket
#          support: HA_LIBCURL (default ~/ha-spike/curlroot/lib/libcurl.so.4), see tests/integration/run.sh.
set -e
here=$(cd "$(dirname "$0")" && pwd)
if [ "${1:-}" = integration ]; then
    exec sh "$here/tests/integration/run.sh"
fi
exec bash "$here/../_common/build_app.sh" --name HomeAssistant "$here" "$@"
