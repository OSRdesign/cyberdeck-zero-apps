#!/bin/sh
# The headless history run: seeds a data folder with messages of every age, runs history.script against the simulated board and prints
# what is left on disk. Needs the host build (build.sh host).
#   wsl -e sh -c 'SHOTS=/mnt/c/.../history sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/src/tests/history_run.sh'
HERE=$(cd "$(dirname "$0")" && pwd)
export MESHHOP_ONLINE=${MESHHOP_ONLINE:-0}
export DATA=${DATA:-$HOME/mesh-hop-history-data}
rm -rf "$DATA"
python3 "$HERE/seed_history.py" "$DATA"
echo "messages.jsonl before: $(wc -l < "$DATA/messages.jsonl") lines"
SCRIPT="$HERE/history.script" sh "$HERE/ui_smoke.sh"
RC=$?
# since 0.2.2 the history of a board lives in boards/<first 12 hex digits of its key>/ (the seeded old-layout file is moved there at the first SELF_INFO)
B=$(ls -d "$DATA"/boards/*/ | head -1)
echo "messages.jsonl after: $(wc -l < "${B}messages.jsonl") lines (the original is kept as messages.jsonl.pre-boards.bak)"
echo "contacts.jsonl: $(wc -l < "${B}contacts.jsonl") lines; channels.jsonl: $(wc -l < "${B}channels.jsonl") lines"
cat "${B}prefs.txt" 2>/dev/null
exit $RC
