# SPDX-License-Identifier: MIT
"""A plain RNS node for the integration test (not a bridge): a TCP server interface on 127.0.0.1.

    python3 peer.py CONFIG_DIR PORT
Commands on stdin, one JSON answer per line on stdout: "announce TEXT", "stats", "heard", "quit".
"""
import json
import os
import sys

cfg, port = sys.argv[1], int(sys.argv[2])
os.makedirs(cfg, exist_ok=True)
with open(os.path.join(cfg, "config"), "w") as f:
    f.write("[reticulum]\n  enable_transport = No\n  share_instance = No\n[logging]\n  loglevel = 2\n"
            "[interfaces]\n  [[Peer server]]\n    type = TCPServerInterface\n    enabled = yes\n"
            "    listen_ip = 127.0.0.1\n    listen_port = %d\n" % port)

import RNS  # noqa: E402

reticulum = RNS.Reticulum(configdir=cfg)
heard = []


class Handler:
    aspect_filter = None

    def received_announce(self, destination_hash, announced_identity, app_data):
        heard.append({"destination": destination_hash.hex(), "app_data": (app_data or b"").hex()})


RNS.Transport.register_announce_handler(Handler())
dest = RNS.Destination(RNS.Identity(), RNS.Destination.IN, RNS.Destination.SINGLE, "bridgetest", "peer")


def say(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


say({"ready": True, "destination": dest.hash.hex()})
for line in sys.stdin:
    cmd, _, arg = line.strip().partition(" ")
    if cmd == "announce":
        dest.announce(app_data=arg.encode())
        say({"announced": True})
    elif cmd == "stats":
        say([{k: i.get(k) for k in ("short_name", "status", "clients", "rxb", "txb")}
             for i in reticulum.get_interface_stats()["interfaces"]])
    elif cmd == "heard":
        say(heard)
    elif cmd == "quit":
        break
os._exit(0)
