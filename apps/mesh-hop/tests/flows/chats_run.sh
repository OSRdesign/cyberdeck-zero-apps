#!/bin/sh
# Runs chats.script (P4: Chats, the conversation, the compose view and the editors with the on-screen keyboard) against the simulated
# board with the history of seed_chats.py (short and long messages, none read yet). Needs the host build (build.sh host).
#   wsl -e sh -c 'SIZE=800x480 SHOTS=/tmp/chats sh .../tests/flows/chats_run.sh'
# Variables as in ui_smoke.sh (SHOTS, SIZE, PPMM, MESHHOP_BIN...); BIG=N adds N messages to the Public channel.
HERE=$(cd "$(dirname "$0")" && pwd)
export MESHHOP_ONLINE=${MESHHOP_ONLINE:-0}
export DATA=${DATA:-$HOME/mesh-hop-chats-data}
rm -rf "$DATA"
python3 "$HERE/seed_chats.py" "$DATA" ${BIG:+--big $BIG}
SCRIPT="${SCRIPT:-$HERE/chats.script}" sh "$HERE/ui_smoke.sh"
