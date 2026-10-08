#!/bin/sh
# Runs a phase 2 script of the headless build against the simulated board, with the seeded history of seed_history.py
# (10 messages from 100 days to 1 hour old, plus the 2 the simulator sends). In the script the words @D60@, @D30@ ... are replaced by the
# local date (YYYY-MM-DD) that many days ago, so date ranges can be typed.
#   wsl -e sh .../src/tests/p2_run.sh p2-search.script "--contacts 8"
# Needs the host build (build.sh host). Variables as in ui_smoke.sh (SHOTS, ...); NOSEED=1 starts with an empty history.
HERE=$(cd "$(dirname "$0")" && pwd)
export MESHHOP_ONLINE=${MESHHOP_ONLINE:-0}
export DATA=${DATA:-$HOME/mesh-hop-p2-data}
rm -rf "$DATA"
[ -z "$NOSEED" ] && python3 "$HERE/seed_history.py" "$DATA"
TMP=$(mktemp /tmp/p2script.XXXXXX)
cp "$HERE/$1" "$TMP"
for d in 1 2 7 30 45 50 60 90; do
    day=$(date -d "-$d days" +%Y-%m-%d)
    sed -i "s/@D$d@/$day/g" "$TMP"
done
SCRIPT="$TMP" SIMARGS="$2" sh "$HERE/ui_smoke.sh"
RC=$?
rm -f "$TMP"
exit $RC
