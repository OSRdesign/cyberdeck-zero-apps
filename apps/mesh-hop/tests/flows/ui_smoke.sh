#!/bin/sh
# Scripted run of the app (mesh-hop-script, the shared headless harness's platform) against the simulated radio:
# screenshots of every screen in $SHOTS and the `dump` lines. Needs the host build:   wsl -e sh .../apps/mesh-hop/build.sh host
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/tests/flows/ui_smoke.sh
# Variables: SIMARGS (more options for the simulator, e.g. "--contacts 157 --chan-reply none"), SHOTS (screenshot folder), SCRIPT (default smoke.script), DATA (keep the history folder, default: a new one that is
# deleted), NOSIM=1 (no simulated board: the offline case), SIMCTL=1 (the script may use `sim <line>` to type into the simulator: "swap", "unplug"...),
# SIZE=WxH (the screen; the script's coordinates are the 640x480 stage's, moved to where the stage is), MESHHOP_BIN (the runner),
# CDK_HEADLESS_RESOURCES (the launcher tree's projects/APPLaunch/APPLaunch: fonts of the shared widgets).
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${MESHHOP_BIN:-${CDK_BUILD:-$HOME/cdk-build/mesh-hop}/host/mesh-hop-script}
export CDK_HEADLESS_RESOURCES=${CDK_HEADLESS_RESOURCES:-$HOME/zero7/launcher/projects/APPLaunch/APPLaunch}
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
    if [ -n "$SIMCTL" ]; then
        FIFO=$(mktemp -u /tmp/mhsim.XXXXXX); mkfifo "$FIFO"; exec 3<>"$FIFO"
        export MESHHOP_SIMIN=$FIFO
        python3 "$SIM" --link "$LINK" --echo $SIMARGS <&3 > "$SHOTS/sim.log" 2>&1 &
    else
        python3 "$SIM" --link "$LINK" --echo $SIMARGS < /dev/null > "$SHOTS/sim.log" 2>&1 &
    fi
    SIMPID=$!
    sleep 1
else
    export MESHHOP_PORT=/nonexistent/ttyMH
fi
"$BIN" --script "${SCRIPT:-$HERE/smoke.script}" --shot-dir "$SHOTS" --size "${SIZE:-640x480}" | tee "$SHOTS/dump.txt"
RC=$?
if [ -f "$DATA/mesh-hop.log" ]; then echo "== the app log (mesh-hop.log) =="; cat "$DATA/mesh-hop.log"; fi
[ -n "$SIMPID" ] && kill $SIMPID 2>/dev/null
[ -n "$FIFO" ] && rm -f "$FIFO"
[ -z "$KEEP" ] && rm -rf "$DATA"
echo "exit code $RC; screenshots in $SHOTS"
exit $RC
