#!/bin/sh
# Builds the viz1090 intro screen (a native cdk app, src/) in WSL (see apps/_common/build_app.sh -h):
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/viz1090/build.sh [cross|host|all] [--strict] [--sync]
#   cross: the aarch64 binary, moved to root/opt/viz1090/vizsetup (run_viz1090.sh runs it before the map);
#   host: unit tests (NMEA, SDR ids, airport list, settings file, places), headless scenes, contact sheets.
# viz1090, readsb and the display bridge of the map are built on a Pi by build/build_on_pi.sh.
# Then the package: python3 tools/make_registry.py --only viz1090 --out <dir outside packages/>
set -e
here=$(cd "$(dirname "$0")" && pwd)
rc=0
bash "$here/../_common/build_app.sh" --name Viz1090Setup "$here" "$@" || rc=$?
# build_app.sh puts the binary where a launcher app lives (bin/<id>); this one is the intro of the viz1090 tile
bin="$here/root/usr/share/APPLaunch/bin/viz1090"
if [ -f "$bin" ]; then
    mv -f "$bin" "$here/root/opt/viz1090/vizsetup"
    rmdir "$here/root/usr/share/APPLaunch/bin" 2> /dev/null || true
    echo "== $here/root/opt/viz1090/vizsetup: $(stat -c %s "$here/root/opt/viz1090/vizsetup") bytes"
fi
exit $rc
