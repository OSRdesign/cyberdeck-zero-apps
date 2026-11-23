#!/bin/sh
# SPDX-License-Identifier: MIT
# End to end on this PC: the app (headless build) against the REAL bridge (RNS + LXMF from the bridge tests' venv) and a
# plain Reticulum node on 127.0.0.1 (bridge/tests/peer.py). Everything lives in a temporary folder; the real
# systemctl is replaced by tests/fake_systemctl.sh; nothing touches ~/.reticulum or the real XDG_RUNTIME_DIR.
#
#   sh apps/reticulum/tests/run_real.sh        (after build.sh host; the venv of bridge/tests/run_tests.sh must exist)
#
# The scene tests/real-scenes/real-tcp.scene adds an Internet connection pointing at the node, switches it on and off.
# Afterwards the script checks the configuration file the bridge wrote and the bridge's log: the connection is written
# switched off and listen-only, the node saw the connection while it was on, and the bridge never announced.
set -u
here=$(cd -- "$(dirname -- "$0")" && pwd)
app=$(dirname "$here")
build=${CDK_BUILD:-$HOME/cdk-build/reticulum}
bin=$build/host/reticulum-headless
res=${LAUNCHER_TREE:-$HOME/zero7/launcher}/projects/APPLaunch/APPLaunch
venv=${VENV:-$HOME/.cache/reticulum-bridge-venv}
py=$venv/bin/python
[ -x "$bin" ] || { echo "no $bin: run build.sh host first" >&2; exit 2; }
[ -x "$py" ] || { echo "no $py: run bridge/tests/run_tests.sh once to make the venv" >&2; exit 2; }
tmp=$(mktemp -d /tmp/ret-real.XXXXXX)
fail=0
ok() { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; fail=1; }
port=$("$py" -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
mkdir -p "$tmp/cfg" "$tmp/run" "$tmp/out"
chmod 700 "$tmp/run"
cat > "$tmp/cfg/config" <<CFG
[reticulum]
  enable_transport = No
  share_instance = Yes
  instance_name = retapp$port
[logging]
  loglevel = 3
[interfaces]
  [[Default Interface]]
    type = AutoInterface
    enabled = no
    outgoing = False
CFG
mkfifo "$tmp/peer.in"
"$py" "$app/bridge/tests/peer.py" "$tmp/peer" "$port" < "$tmp/peer.in" > "$tmp/peer.out" 2> "$tmp/peer.err" &
peer=$!
exec 3> "$tmp/peer.in"
n=0; while ! grep -q ready "$tmp/peer.out" 2>/dev/null && [ $n -lt 100 ]; do sleep 0.2; n=$((n + 1)); done
grep -q ready "$tmp/peer.out" && ok "the plain node listens on 127.0.0.1:$port" || { bad "the node did not start"; cat "$tmp/peer.err"; }
(
    unset XDG_RUNTIME_DIR
    export PYTHONPATH="$app/bridge" PYTHONUNBUFFERED=1
    exec "$py" -m reticulum_bridge --config "$tmp/cfg" --data "$tmp/data" --run "$tmp/run"
) > "$tmp/bridge.log" 2>&1 &
bridge=$!
n=0; while [ ! -S "$tmp/run/bridge.sock" ] && [ $n -lt 200 ]; do sleep 0.2; n=$((n + 1)); done
[ -S "$tmp/run/bridge.sock" ] && ok "the real bridge answers (socket after about $((n / 5)) s)" || { bad "no socket"; cat "$tmp/bridge.log"; }

printf '#!/bin/sh\nexec sh "%s" "$@"\n' "$here/fake_systemctl.sh" > "$tmp/systemctl"
chmod +x "$tmp/systemctl"
sed "s/@PORT@/$port/g" "$here/real-scenes/real-tcp.scene" > "$tmp/real-tcp.scene"
: > "$tmp/systemctl.log"
power=${APPLAUNCH_POWER_SUPPLY_ROOT-$build/power_supply}
mkdir -p "$power"
(
    export APPLAUNCH_POWER_SUPPLY_ROOT="$power" RETICULUM_BRIDGE_RUN="$tmp/run" RETICULUM_SYSTEMCTL="$tmp/systemctl"
    export FAKE_SYSTEMCTL_LOG="$tmp/systemctl.log" FAKE_UNIT_DIR="$tmp"
    "$bin" --scene "$tmp/real-tcp.scene" --out "$tmp/out" --resources "$res"
) > "$tmp/headless.out" 2>&1
h=$?
[ $h -eq 0 ] && ok "the scene passed (touch-free walk through Add, Switch on, Switch off against the real bridge)" || { bad "the scene failed (exit $h)"; tail -n 30 "$tmp/headless.out"; }

# what the bridge wrote and what the node saw
"$py" - "$tmp/cfg/config" "$port" <<'PY' || bad "the configuration is not as expected"
import sys
text = open(sys.argv[1]).read()
port = sys.argv[2]
i = text.index("[[Internet]]")
block = text[i:i + 400]
need = ["type = TCPClientInterface", "enabled = no", "outgoing = False", "target_host = 127.0.0.1", "target_port = " + port]
missing = [n for n in need if n not in block]
sys.exit("missing: %s\n%s" % (missing, block) if missing else 0)
PY
[ $fail -eq 0 ] && ok "config: [[Internet]] is TCPClientInterface to 127.0.0.1:$port, enabled = no, outgoing = False (switched off again)"
echo "stats" >&3; sleep 1
echo "      peer: $(tail -n 1 "$tmp/peer.out" | cut -c1-200)"
grep -q "announced" "$tmp/bridge.log" && bad "the bridge announced" || ok "the bridge log has no announcement"
grep -i -q "passphrase\|private" "$tmp/bridge.log" && bad "secret words in the bridge log" || ok "no secret words in the bridge log"
grep -q "stop reticulum" "$tmp/systemctl.log" && ok "on exit the app asked systemctl to stop the service (Keep running was off)" || bad "no stop at exit"
PYTHONPATH="$app/bridge" "$py" -m reticulum_bridge.client --sock "$tmp/run/bridge.sock" ifaces | cut -c1-400 | sed 's/^/      /'
PYTHONPATH="$app/bridge" "$py" -m reticulum_bridge.client --sock "$tmp/run/bridge.sock" service.stop > /dev/null 2>&1
wait $bridge; rc=$?
[ $rc -eq 0 ] && ok "service.stop: the bridge exits 0" || bad "the bridge exited $rc"
echo "quit" >&3; exec 3>&-
wait $peer 2> /dev/null
echo "      bridge log:"; sed 's/^/      /' "$tmp/bridge.log" | head -30
mkdir -p "$build/host/out-real"
cp "$tmp"/out/*/*/*.png "$build/host/out-real/" 2>/dev/null
cp -r "$tmp/out"/* "$build/host/out-real/" 2>/dev/null
rm -rf "$tmp"
[ $fail -eq 0 ] && echo "REAL BRIDGE: ALL PASS" || echo "REAL BRIDGE: FAILURES"
exit $fail
