# SPDX-License-Identifier: MIT
"""python3 -m reticulum_bridge [--config DIR] [--data DIR] [--run DIR] [--no-lxmf]: runs the bridge in the foreground.

Exit status: 0 stopped cleanly, 1 could not start (reason on stderr), 2 usage, 3 a bridge already runs for this user.
"""
import argparse
import os
import sys

from . import __version__


def default_data():
    return os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "reticulum")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="reticulum_bridge", description="Reticulum shared instance, LXMF router and "
                                 "the JSON-lines socket of the CyberdeckZero apps (docs/dev/reticulum-bridge.md).")
    ap.add_argument("--config", default=os.environ.get("RETICULUM_BRIDGE_CONFIG"),
                    help="Reticulum config folder (default: RNS's search order, else ~/.reticulum)")
    ap.add_argument("--data", default=os.environ.get("RETICULUM_BRIDGE_DATA") or default_data(),
                    help="identity and LXMF store (default: ~/.local/share/reticulum)")
    ap.add_argument("--run", default=os.environ.get("RETICULUM_BRIDGE_RUN"),
                    help="socket and state file (default: $XDG_RUNTIME_DIR/reticulum)")
    ap.add_argument("--no-lxmf", action="store_true", help="do not start the LXMF router")
    ap.add_argument("--version", action="version", version="reticulum_bridge " + __version__)
    a = ap.parse_args(argv)
    run = a.run or (os.path.join(os.environ["XDG_RUNTIME_DIR"], "reticulum") if os.environ.get("XDG_RUNTIME_DIR")
                    else None)
    if not run:
        sys.stderr.write("[reticulum] XDG_RUNTIME_DIR is not set (no /tmp fallback): start the bridge from the "
                         "user session, or pass --run DIR\n")
        return 2
    from . import rnsconfig
    from .service import Bridge
    return Bridge(rnsconfig.resolve_dir(a.config), os.path.abspath(a.data), os.path.abspath(run),
                  use_lxmf=not a.no_lxmf).run()


if __name__ == "__main__":
    sys.exit(main())
