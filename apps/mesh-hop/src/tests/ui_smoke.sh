#!/bin/sh
# Scripted run of the headless build against the simulated radio: screenshots of every screen in $SHOTS and the `dump` lines.
# Needs the host build:   wsl -e sh .../apps/mesh-hop/build/build.sh host
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/src/tests/ui_smoke.sh
# On the deck (the aarch64 binary, no panel used; copy this file, smoke.script, offline.script and tools/meshcore_sim.py to one folder):
#   MESHHOP_BIN=/usr/share/APPLaunch/bin/M5CardputerZero-mesh-hop sh ui_smoke.sh
# Variables: SIMARGS (more options for the simulator, e.g. "--contacts 157 --chan-reply none"), SHOTS (screenshot folder), SCRIPT (default smoke.script), DATA (keep the history folder, default: a new one that is
# deleted), NOSIM=1 (no simulated board: the offline case).
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${MESHHOP_BIN:-${MESHHOP_HOST_OUT:-$HOME/mesh-hop-host}/mesh-hop}
SHOTS=${SHOTS:-/tmp/mesh-hop-shots}
SIM=${MESHHOP_SIM:-$HERE/../../tools/meshcore_sim.py}
[ -f "$SIM" ] || SIM=$HERE/meshcore_sim.py        # copied next to the script (on the deck)
LINK=/tmp/ttyMHSIM
if [ -n "$DATA" ]; then KEEP=1; else DATA=$(mktemp -d /tmp/mesh-hop-data.XXXXXX); fi
mkdir -p "$SHOTS"
rm -f "$SHOTS"/*.png
export MESHHOP_DATA=$DATA MESHHOP_PORT=$LINK MESHHOP_KEYMAP=us MESHHOP_LOADING_MS=${LOADING_MS:-400}
SIMPID=
if [ -z "$NOSIM" ]; then
    python3 "$SIM" --link "$LINK" --echo $SIMARGS < /dev/null > "$SHOTS/sim.log" 2>&1 &
    SIMPID=$!
    sleep 1
else
    export MESHHOP_PORT=/nonexistent/ttyMH
fi
"$BIN" --headless --script "${SCRIPT:-$HERE/smoke.script}" --shot-dir "$SHOTS" | tee "$SHOTS/dump.txt"
RC=$?
if [ -f "$DATA/mesh-hop.log" ]; then echo "== the app log (mesh-hop.log) =="; cat "$DATA/mesh-hop.log"; fi
[ -n "$SIMPID" ] && kill $SIMPID 2>/dev/null
[ -z "$KEEP" ] && rm -rf "$DATA"
echo "exit code $RC; screenshots in $SHOTS"
exit $RC
