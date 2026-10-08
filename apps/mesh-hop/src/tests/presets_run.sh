#!/bin/sh
# The headless run of the preset list refresh: a local web server stands in for api.meshcore.nz (MESHHOP_PRESETS_URL), the "online" test is
# forced with MESHHOP_ONLINE. Cases: online and fetched, offline with the saved copy, offline first start, server down, refused list,
# curl missing. Needs the host build. Output: the log and the state line of each case, screenshots in $SHOTS/<case>/.
#   wsl -e sh -c 'SHOTS=/mnt/c/.../presets sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/src/tests/presets_run.sh'
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${SHOTS:-/tmp/mesh-hop-presets}
FEED=$HOME/mesh-hop-feed
rm -rf "$FEED"
mkdir -p "$FEED/ok" "$FEED/odd"
python3 - "$HERE/data/meshcore_config.json" "$FEED" <<'PY'
import json, sys
src, out = sys.argv[1], sys.argv[2]
d = json.load(open(src))
e = d["config"]["suggested_radio_settings"]["entries"]
for x in e:
    if x["title"] == "Brazil":
        x["title"] = "Brazil (test list)"
e.append({"title": "Testland", "description": "x", "frequency": "868.100", "spreading_factor": "9", "bandwidth": "125", "coding_rate": "5"})
json.dump(d, open(out + "/ok/config.json", "w"))
e[-1]["spreading_factor"] = "40"                 # one odd value: the whole list must be refused
json.dump(d, open(out + "/odd/config.json", "w"))
PY
python3 -m http.server 8099 --bind 127.0.0.1 --directory "$FEED" > /dev/null 2>&1 &
WEB=$!
sleep 1
run() {      # run <case> <data dir> <env...>
    CASE=$1; DATA_DIR=$2; shift 2
    echo "######## $CASE"
    env "$@" DATA="$DATA_DIR" SHOTS="$ROOT/$CASE" SCRIPT="$HERE/presets.script" sh "$HERE/ui_smoke.sh" | grep -v "^shot"
}
D1=$HOME/mesh-hop-presets-data1
D2=$HOME/mesh-hop-presets-data2
rm -rf "$D1" "$D2"
mkdir -p "$D1" "$D2"
run 1-online-fetched "$D1" MESHHOP_ONLINE=1 MESHHOP_PRESETS_URL=http://127.0.0.1:8099/ok/config.json
echo "-- saved copy on disk:"; head -c 200 "$D1/presets.jsonl"; echo; wc -l "$D1/presets.jsonl"
run 2-offline-saved-copy "$D1" MESHHOP_ONLINE=0
run 3-offline-first-start "$D2" MESHHOP_ONLINE=0
run 4-server-down "$D2" MESHHOP_ONLINE=1 MESHHOP_PRESETS_URL=http://127.0.0.1:8098/ok/config.json
run 5-list-refused "$D2" MESHHOP_ONLINE=1 MESHHOP_PRESETS_URL=http://127.0.0.1:8099/odd/config.json
run 6-curl-missing "$D2" MESHHOP_ONLINE=1 MESHHOP_CURL=/nonexistent/curl
# a saved copy that is odd must be ignored: damage it, start offline
sed -i 's/"sf":9/"sf":19/' "$D1/presets.jsonl"
run 7-damaged-copy "$D1" MESHHOP_ONLINE=0
# the real address over HTTPS (needs the network of this PC): REAL=1
if [ -n "$REAL" ]; then
    D3=$HOME/mesh-hop-presets-data3
    rm -rf "$D3"
    mkdir -p "$D3"
    run 8-real-https "$D3" MESHHOP_ONLINE=1
    head -c 300 "$D3/presets.jsonl"; echo
    rm -rf "$D3"
fi
kill $WEB 2>/dev/null
rm -rf "$D1" "$D2"
