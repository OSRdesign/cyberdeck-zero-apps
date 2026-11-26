#!/bin/sh
# SPDX-License-Identifier: MIT
# End to end on this PC with TWO REAL BRIDGES (RNS + LXMF from the bridge tests' venv) linked by a TCP connection on
# 127.0.0.1, the headless app on bridge A and a script standing for the other person on bridge B. Nothing touches ~/.reticulum or
# the real XDG_RUNTIME_DIR; the real systemctl is replaced by tests/fake_systemctl.sh.
#
#   sh apps/reticulum/tests/run_real_chat.sh        (after build.sh host; the venv of bridge/tests/run_tests.sh must exist; about 6 minutes)
#
# Part 1 (real-chat-1.scene): add Bob by his typed address, open the conversation, Bob sends "Hi Alex" (it arrives while the page
# is open), the app sends "Hello Bob" and it is delivered. Part 2: Bob's bridge is stopped, the app sends "Are you there?", it
# fails after LXMF's tries and shows Retry. Part 3: Bob's bridge is started again, the message is sent again by hand and is
# delivered, the conversation is deleted. Afterwards the script asks Bob's bridge what it received.
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
tmp=$(mktemp -d /tmp/ret-realchat.XXXXXX)
fail=0
ok() { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; fail=1; }
port=$("$py" -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
for who in a b; do mkdir -p "$tmp/$who/cfg" "$tmp/$who/run" "$tmp/$who/data"; chmod 700 "$tmp/$who/run"; done
cat > "$tmp/a/cfg/config" <<CFG
[reticulum]
  enable_transport = No
  share_instance = Yes
  instance_name = retchatA$port
[logging]
  loglevel = 3
[interfaces]
  [[Default Interface]]
    type = AutoInterface
    enabled = no
    outgoing = False
  [[Link]]
    type = TCPServerInterface
    enabled = yes
    outgoing = True
    listen_ip = 127.0.0.1
    listen_port = $port
CFG
cat > "$tmp/b/cfg/config" <<CFG
[reticulum]
  enable_transport = No
  share_instance = Yes
  instance_name = retchatB$port
[logging]
  loglevel = 3
[interfaces]
  [[Default Interface]]
    type = AutoInterface
    enabled = no
    outgoing = False
  [[Link]]
    type = TCPClientInterface
    enabled = yes
    outgoing = True
    target_host = 127.0.0.1
    target_port = $port
CFG
client() { # client WHO VERB [JSON]
    who=$1; shift
    PYTHONPATH="$app/bridge" "$py" -m reticulum_bridge.client --sock "$tmp/$who/run/bridge.sock" "$@"
}
field() { # field PATH.OF.KEYS (reads the reply line on stdin)
    "$py" "$here/real_field.py" "$1"
}
start_bridge() { # start_bridge WHO
    (
        unset XDG_RUNTIME_DIR
        export PYTHONPATH="$app/bridge" PYTHONUNBUFFERED=1
        exec "$py" -m reticulum_bridge --config "$tmp/$1/cfg" --data "$tmp/$1/data" --run "$tmp/$1/run"
    ) >> "$tmp/$1.log" 2>&1 &
    eval "pid_$1=$!"
    n=0; while [ ! -S "$tmp/$1/run/bridge.sock" ] && [ $n -lt 200 ]; do sleep 0.2; n=$((n + 1)); done
    [ -S "$tmp/$1/run/bridge.sock" ] || { bad "bridge $1 has no socket"; cat "$tmp/$1.log"; }
}
start_bridge a
start_bridge b
client a identity.name '{"name":"Alex"}' > /dev/null
client b identity.name '{"name":"Bob"}' > /dev/null
addr_a=$(client a status | field result.lxmf_address)
addr_b=$(client b status | field result.lxmf_address)
[ ${#addr_a} -eq 32 ] && [ ${#addr_b} -eq 32 ] && ok "two bridges, addresses $(echo "$addr_a" | cut -c1-8)... and $(echo "$addr_b" | cut -c1-8)..." || bad "no addresses"
sleep 6  # the TCP link between them comes up

printf '#!/bin/sh\nexec sh "%s" "$@"\n' "$here/fake_systemctl.sh" > "$tmp/systemctl"
chmod +x "$tmp/systemctl"
: > "$tmp/systemctl.log"
power=${APPLAUNCH_POWER_SUPPLY_ROOT-$build/power_supply}
mkdir -p "$power" "$build/host/out-real"
run_scene() { # run_scene NAME   (the app on bridge A; the harness's output in $tmp/NAME.out)
    sed "s/@ADDR@/$addr_b/g" "$here/real-scenes/$1.scene" > "$tmp/$1.scene"
    (
        export APPLAUNCH_POWER_SUPPLY_ROOT="$power" RETICULUM_BRIDGE_RUN="$tmp/a/run" RETICULUM_SYSTEMCTL="$tmp/systemctl"
        export FAKE_SYSTEMCTL_LOG="$tmp/systemctl.log" FAKE_UNIT_DIR="$tmp"
        "$bin" --scene "$tmp/$1.scene" --out "$tmp/out" --resources "$res"
    ) > "$tmp/$1.out" 2>&1
}

# ---- part 1
run_scene real-chat-1 &
scene=$!
# the app's own log is written late, the bridge's at once: Bob writes after the contact was added and the conversation is open
n=0; while ! grep -q "contact added" "$tmp/a.log" 2> /dev/null && [ $n -lt 600 ]; do sleep 0.5; n=$((n + 1)); done
sleep 10
client b messages.send "{\"address\":\"$addr_a\",\"text\":\"Hi Alex\"}" | cut -c1-160 | sed 's/^/      bob sends: /'
wait $scene && ok "part 1: the app added Bob, received \"Hi Alex\", sent \"Hello Bob\" and saw it delivered" || { bad "part 1 failed"; tail -n 25 "$tmp/real-chat-1.out"; }
client b messages.history "{\"address\":\"$addr_a\"}" > "$tmp/b-history.json"
grep -q "Hello Bob" "$tmp/b-history.json" && ok "Bob's bridge received \"Hello Bob\"" || bad "Bob's bridge did not receive \"Hello Bob\""

[ "${PARTS:-3}" = 1 ] && { for who in a b; do client $who service.stop > /dev/null 2>&1; done; wait 2> /dev/null; cp -r "$tmp/out"/* "$build/host/out-real/" 2> /dev/null; echo "part 1 only: $tmp"; [ -n "${KEEP:-}" ] || rm -rf "$tmp"; exit $fail; }
# ---- part 2: Bob goes away
client b service.stop > /dev/null 2>&1
wait $pid_b 2> /dev/null
sleep 3
run_scene real-chat-2 && ok "part 2: the message to the offline contact failed and Retry showed; nothing was sent again by itself" || { bad "part 2 failed"; tail -n 25 "$tmp/real-chat-2.out"; }
rm -f "$tmp/b/run/bridge.sock"

# ---- part 3: Bob is back, the retry is by hand
start_bridge b
sleep 12
run_scene real-chat-3 && ok "part 3: Retry by hand delivered it, the conversation was deleted on this deck" || { bad "part 3 failed"; tail -n 25 "$tmp/real-chat-3.out"; }
client b messages.history "{\"address\":\"$addr_a\"}" > "$tmp/b-history.json"
grep -q "Are you there?" "$tmp/b-history.json" && ok "Bob's bridge received \"Are you there?\" after the retry" || bad "Bob's bridge did not receive the retried message"
client a messages.conversations | grep -q "$addr_b" && bad "the conversation is still on deck A" || ok "the conversation is gone from deck A; Bob's copy stays"
grep -q "Hello Bob" "$tmp/b-history.json" && ok "Bob's copy is untouched by the delete on deck A" || bad "Bob's copy changed"
client a contacts.list | grep -q "$addr_b" && ok "Bob is still a contact on deck A (the conversation was deleted, not the contact)" || bad "Bob was removed"

# ---- the logs
grep -i -q "Hello Bob\|Are you there\|Hi Alex" "$tmp/a.log" "$tmp/b.log" && bad "message text in a bridge log" || ok "no message text in the bridge logs"
grep -q "announce" "$tmp/real-chat-1.out" "$tmp/real-chat-2.out" "$tmp/real-chat-3.out" && bad "the app announced" || ok "the app never announced"
for who in a b; do client $who service.stop > /dev/null 2>&1; done
wait 2> /dev/null
cp -r "$tmp/out"/* "$build/host/out-real/" 2> /dev/null
for who in a b; do echo "      bridge $who log (tail):"; tail -n 6 "$tmp/$who.log" | cut -c1-160 | sed 's/^/      /'; done
[ -n "${KEEP:-}" ] && echo "kept: $tmp" || rm -rf "$tmp"
[ $fail -eq 0 ] && echo "REAL BRIDGES (chat): ALL PASS" || echo "REAL BRIDGES (chat): FAILURES"
exit $fail
