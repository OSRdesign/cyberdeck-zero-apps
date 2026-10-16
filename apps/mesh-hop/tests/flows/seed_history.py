#!/usr/bin/env python3
"""Writes a messages.jsonl with messages of every age into a Mesh Hop data folder (for the headless history runs).

  python3 seed_history.py <data folder>

Keys follow tools/meshcore_sim.py: Alice is d:101112131415, Bob d:505152535455. The channels are the sim's Public (c:0) and #test (c:1).
"""
import json
import os
import sys
import time

DAY = 86400
now = int(time.time())
# (conversation, outgoing, age in days, sender, text, state)
MSGS = [
    ("c:0", 0, 100, "Zed", "very old public message", 0),
    ("c:0", 0, 45, "Yan", "old public message", 0),
    ("c:0", 0, 0.05, "Xia", "a recent public message", 0),
    ("c:1", 0, 10, "Zed", "test channel, 10 days ago", 0),
    ("c:1", 1, 2, "You", "test channel, my message 2 days ago", 2),
    ("d:101112131415", 0, 40, "Alice", "Alice, 40 days ago", 0),
    ("d:101112131415", 1, 21, "You", "me to Alice, 21 days ago", 3),
    ("d:101112131415", 0, 1, "Alice", "Alice, yesterday", 0),
    ("d:505152535455", 0, 15, "Bob", "Bob, 15 days ago", 0),
    ("d:505152535455", 0, 8, "Bob", "Bob, 8 days ago", 0),
]

folder = sys.argv[1]
os.makedirs(folder, exist_ok=True)
lines = []
for i, (conv, out, age, who, text, st) in enumerate(MSGS, start=1):
    ts = int(now - age * DAY)
    lines.append(json.dumps({"i": i, "k": conv, "d": out, "t": ts, "s": ts, "n": who, "x": text, "st": st, "h": -1}, separators=(",", ":")))
with open(os.path.join(folder, "messages.jsonl"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("seeded %d messages in %s" % (len(lines), folder))
