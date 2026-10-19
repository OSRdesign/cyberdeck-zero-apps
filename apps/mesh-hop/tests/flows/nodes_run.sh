#!/bin/sh
# Mesh Hop 0.5: the start page against the simulator over TCP (tools/meshcore_sim.py --tcp), with tests/flows/nodes.script:
# no USB port and no Bluetooth (MESHHOP_DEMO=nobt), the node added by IP address through the page. Needs the host build.
#   SIZE=WxH SHOTS=dir sh nodes_run.sh
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${MESHHOP_BIN:-${CDK_BUILD:-$HOME/cdk-build/mesh-hop}/host/mesh-hop-script}
export CDK_HEADLESS_RESOURCES=${CDK_HEADLESS_RESOURCES:-$HOME/zero7/launcher/projects/APPLaunch/APPLaunch}
SHOTS=${SHOTS:-/tmp/mesh-hop-nodes}
SIM=${MESHHOP_SIM:-$HERE/../../tools/meshcore_sim.py}
DATA=$(mktemp -d /tmp/mesh-hop-data.XXXXXX)
mkdir -p "$SHOTS"
rm -f "$SHOTS"/*.png
unset MESHHOP_PORT
export MESHHOP_DATA=$DATA MESHHOP_DEMO=nobt MESHHOP_KEYMAP=us MESHHOP_ONLINE=0
python3 "$SIM" --tcp 15007 --echo < /dev/null > "$SHOTS/sim.log" 2>&1 &
SIMPID=$!
sleep 1
"$BIN" --script "$HERE/nodes.script" --shot-dir "$SHOTS" --size "${SIZE:-640x480}" ${PPMM:+--ppmm $PPMM} | tee "$SHOTS/dump.txt"
RC=$?
if [ -f "$DATA/mesh-hop.log" ]; then echo "== the app log (mesh-hop.log) =="; cat "$DATA/mesh-hop.log"; fi
kill $SIMPID 2> /dev/null
rm -rf "$DATA"
echo "exit code $RC; screenshots in $SHOTS"
exit $RC
