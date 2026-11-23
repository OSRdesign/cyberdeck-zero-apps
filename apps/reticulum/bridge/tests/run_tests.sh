#!/bin/sh
# SPDX-License-Identifier: MIT
# The bridge's PC tests in one command (Linux or WSL; Python 3.10 or later):
#
#   sh apps/reticulum/bridge/tests/run_tests.sh [unittest arguments, e.g. -k Integration]
#
# Uses the venv $VENV (default ~/.cache/reticulum-bridge-venv), creating it on first use with --system-site-packages
# and the pinned, hash-checked rns and lxmf wheels of ../requirements.txt (network needed once). cryptography and
# pyserial come from the distribution: sudo apt install python3-venv python3-cryptography python3-serial
# Nothing is installed into the repository; every test works in temporary folders and on 127.0.0.1 only, and never
# touches ~/.reticulum or a real XDG_RUNTIME_DIR.
set -eu
here=$(cd -- "$(dirname -- "$0")/.." && pwd)
VENV=${VENV:-$HOME/.cache/reticulum-bridge-venv}
if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv --system-site-packages "$VENV"
    "$VENV/bin/pip" install -q --require-hashes --no-deps --only-binary=:all: -r "$here/requirements.txt"
fi
"$VENV/bin/python" -c 'import cryptography, serial' 2>/dev/null || {
    echo "missing python3-cryptography or python3-serial (sudo apt install python3-cryptography python3-serial)" >&2
    exit 2
}
"$VENV/bin/python" -c 'import sys, RNS, LXMF; print("python", sys.version.split()[0], "| rns", RNS.__version__, "| lxmf", LXMF.__version__)'
cd "$here"
export PYTHONDONTWRITEBYTECODE=1
exec "$VENV/bin/python" -m unittest discover -s tests -p 'test_*.py' -v "$@"
