# SPDX-License-Identifier: MIT
"""Entry point of Mesh Hop's bundled meshcore-cli, run by ./mccli as  python3 -I mccli.py [meshcore-cli arguments].

Upstream (py/, pinned in apps/mesh-hop/cli/requirements.txt) is not modified. This shim only:
  - puts the bundle first on sys.path (-I leaves out PYTHONPATH, the user's site-packages and this folder);
  - blocks bleak, so meshcore-cli and meshcore_py take their no-Bluetooth path even when a system copy exists: the node
    is reached through Mesh Hop's TCP relay, never over Bluetooth LE from Python;
  - points meshcore-cli's typed-line histories at /dev/null and turns its <node>.msgs message log off (report 033,
    decision D5: both would keep typed commands, passwords included). Saved passwords (<key>.pass) are unchanged.
A name it patches that upstream no longer has stops it with exit status 3, so a pin bump cannot silently write those
files again (the smoke flow of build.sh cli runs this file).
"""
import os
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(HERE, "py"))
sys.modules["bleak"] = None                       # import bleak -> ImportError (upstream guards it)

import meshcore_cli.meshcore_cli as upstream      # noqa: E402

HISTORIES = ("MCCLI_HISTORY_FILE", "MCCLI_REPEATER_HISTORY_FILE", "MCCLI_REGION_FILES_HISTORY")


def stop(what):
    sys.stderr.write("mccli: meshcore-cli %s has no %s any more: update mccli.py (report 033, D5)\n"
                     % (getattr(upstream, "VERSION", "?"), what))
    sys.exit(3)


for name in HISTORIES:
    if not isinstance(getattr(upstream, name, None), str):
        stop(name)
    setattr(upstream, name, os.devnull)

if not callable(getattr(upstream, "log_message", None)) or not hasattr(upstream.log_message, "file"):
    stop("log_message")


async def _no_message_log(mc, msg):
    return None


_no_message_log.file = None                       # upstream sets .file at connect; nothing reads it here
upstream.log_message = _no_message_log

sys.exit(upstream.cli())
