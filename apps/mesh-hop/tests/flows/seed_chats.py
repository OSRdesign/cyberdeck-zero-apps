#!/usr/bin/env python3
"""Writes a messages.jsonl for the Chats renders (P4) into a Mesh Hop data folder: short and long messages in the channels and the
direct chats of tools/meshcore_sim.py, ours in every state, none read yet (the unread counts show).

  python3 seed_chats.py <data folder> [--big N]

--big N adds N more messages to the Public channel (the deck keeps the newest 200 of a conversation) for the memory runs.
Keys follow tools/meshcore_sim.py: Alice is d:101112131415, Bob d:505152535455. The channels are the sim's Public (c:0) and #test (c:1).
State numbers are MsgState: 0 received, 2 sent, 3 delivered, 5 no ack.
"""
import json
import os
import sys
import time

MIN = 60
now = int(time.time())
LONG = ("Heading up to the ridge repeater this afternoon to swap its battery; if anyone hears it drop for ten minutes around "
        "three, that is me. Bring a spare antenna if you come along.")
# (conversation, outgoing, minutes ago, sender, text, state, heard back)
MSGS = [
    ("c:0", 0, 300, "Zed", "Morning all, the valley repeater is back up", 0, 0),
    ("c:0", 0, 280, "Yan", "Thanks Zed, hearing it at -9 dB here", 0, 0),
    ("c:0", 1, 250, "You", "Same here, two hops from the north side", 2, 2),
    ("c:0", 0, 120, "Xia", LONG, 0, 0),
    ("c:0", 0, 45, "Zed", "Count me in for the ridge", 0, 0),
    ("c:0", 0, 12, "Yan", "Anyone near the bridge with a spare cable?", 0, 0),
    ("c:1", 0, 600, "Zed", "test 1 2 3", 0, 0),
    ("c:1", 1, 590, "You", "loud and clear", 2, 0),
    ("d:101112131415", 0, 200, "Alice", "Hi! Are you coming to the meetup on Saturday?", 0, 0),
    ("d:101112131415", 1, 195, "You", "Yes, I will bring the deck and two boards", 3, 0),
    ("d:101112131415", 0, 190, "Alice", "Great. Could you also bring the solar panel?", 0, 0),
    ("d:101112131415", 1, 185, "You", "Sure. " + LONG, 3, 0),
    ("d:101112131415", 1, 30, "You", "Leaving now", 5, 0),
    ("d:101112131415", 0, 3, "Alice", "See you at the bridge in 20 minutes", 0, 0),
    ("d:505152535455", 0, 900, "Bob", "Did the firmware update work?", 0, 0),
]

folder = sys.argv[1]
big = int(sys.argv[sys.argv.index("--big") + 1]) if "--big" in sys.argv else 0
os.makedirs(folder, exist_ok=True)
lines = []
i = 0
for k in range(big):                     # older than the rest
    i += 1
    ts = now - 2 * 86400 + k * 30
    who = ["Zed", "Yan", "Xia"][k % 3]
    lines.append(json.dumps({"i": i, "k": "c:0", "d": 0, "t": ts, "s": ts, "n": who,
                             "x": "message %d of the long channel history: %s" % (k + 1, LONG[: 40 + (k * 7) % 100]), "st": 0, "h": -1},
                            separators=(",", ":")))
for conv, out, ago, who, text, st, hb in MSGS:
    i += 1
    ts = now - ago * MIN
    o = {"i": i, "k": conv, "d": out, "t": ts, "s": ts, "n": who, "x": text, "st": st, "h": -1}
    if hb:
        o["hb"] = hb
    lines.append(json.dumps(o, separators=(",", ":")))
with open(os.path.join(folder, "messages.jsonl"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("seeded %d messages in %s" % (len(lines), folder))
