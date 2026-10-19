#!/bin/sh
# The Mesh Hop Bluetooth LE backend (src/core/ble: BluezBackend over sd-bus, BleLink, BleTransport) against a mock BlueZ
# (mock_bluez.py) on a private D-Bus, driven by tools/ble_probe built for the PC with ASan + UBSan. No radio, no board.
# Needs dbus-daemon, python3 with dbus-python and PyGObject, and libsystemd.so.0 (WSL Ubuntu has them). About 30 s.
#   wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/mesh-hop/tests/ble/run_mock.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
APP=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/mesh-hop-ble-mock}
ADDR=EC:EB:45:39:28:2D
rm -rf "$OUT"
mkdir -p "$OUT"
sh "$APP/tools/build_ble_probe.sh" host "$OUT" > "$OUT/build.log" 2>&1 || { tail -n 30 "$OUT/build.log"; exit 1; }
PROBE=$OUT/ble_probe-host
cat > "$OUT/bus.conf" << EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$OUT/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
dbus-daemon --config-file="$OUT/bus.conf" --nofork --nopidfile > "$OUT/daemon.log" 2>&1 &
DAEMON=$!
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$OUT/bus"
export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1
i=0
while [ ! -S "$OUT/bus" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done

pass=0
fail=0
MOCK=
start_mock() {
    rm -f "$OUT/report.txt"
    python3 "$HERE/mock_bluez.py" --report "$OUT/report.txt" "$@" > "$OUT/mock.log" 2>&1 &
    MOCK=$!
    i=0
    until grep -q "ready" "$OUT/mock.log" 2> /dev/null || [ $i -ge 50 ]; do sleep 0.1; i=$((i + 1)); done
}
stop_mock() {
    kill -TERM "$MOCK" 2> /dev/null
    wait "$MOCK" 2> /dev/null
}
ok() { pass=$((pass + 1)); echo "PASS $1"; }
ko() { fail=$((fail + 1)); echo "FAIL $1"; }
expect() { # what file pattern
    if grep -q -- "$3" "$2"; then ok "$1"; else ko "$1: no '$3' in $2"; fi
}
expect_not() {
    if grep -q -- "$3" "$2"; then ko "$1: '$3' in $2"; else ok "$1"; fi
}
probe() { # name args...
    n=$1
    shift
    timeout 60 "$PROBE" "$@" > "$OUT/$n.out" 2>&1
    echo "rc=$?" >> "$OUT/$n.out"
}
sane() { # name: no sanitizer report in the probe's output
    expect_not "$1: no ASan / UBSan / LSan report" "$OUT/$1.out" "Sanitizer\|runtime error"
}

echo "== info"
start_mock
probe info info
stop_mock
expect "info: the adapter" "$OUT/info.out" "adapter /org/bluez/hci0 powered"
expect "info: our agent, not the default one" "$OUT/info.out" "agent registered (not the default one)"
expect "info: the board is marked" "$OUT/info.out" "\* $ADDR"
expect "info: rc 0" "$OUT/info.out" "rc=0"
expect "info: KeyboardOnly" "$OUT/report.txt" "agent_capability=KeyboardOnly"
expect "info: never the default agent" "$OUT/report.txt" "default_agent_requested=0"
sane info

echo "== scan"
start_mock
probe scan scan --seconds 2
stop_mock
expect "scan: the board seen with its RSSI" "$OUT/scan.out" "seen  $ADDR random rssi -52"
expect "scan: one MeshCore board" "$OUT/scan.out" "1 MeshCore board(s) known"
expect_not "scan: the Meshtastic node is not listed" "$OUT/scan.out" "Meshtastic"
expect "scan: LE filter" "$OUT/report.txt" "filter=Transport=le"
expect "scan: rc 0" "$OUT/scan.out" "rc=0"
sane scan

echo "== pair with the PIN, then the companion handshake"
start_mock
probe pair connect $ADDR --pin 123456 --log
stop_mock
expect "pair: the PIN is asked" "$OUT/pair.out" "sending the PIN"
expect "pair: connected" "$OUT/pair.out" "CONNECTED MeshCore-Mock, MTU 247, notify by AcquireNotify (socket), paired now"
expect "pair: DEVICE_INFO decoded" "$OUT/pair.out" "DEVICE_INFO: firmware level 11, Seeed Xiao-nrf52, v1.15.0-dee3e26"
expect "pair: SELF_INFO decoded" "$OUT/pair.out" "SELF_INFO: \"Mock Board\", key a0a1a2a3a4a5"
expect "pair: PASS" "$OUT/pair.out" "^ *[0-9.]* PASS"
expect "pair: rc 0" "$OUT/pair.out" "rc=0"
expect "pair: BlueZ asked OUR agent" "$OUT/report.txt" "passkey_via=caller"
expect "pair: the PIN typed" "$OUT/report.txt" "passkey=123456"
expect "pair: write with response" "$OUT/report.txt" "write_types=request"
expect "pair: two commands, one write each" "$OUT/report.txt" "writes=2"
expect "pair: never the default agent" "$OUT/report.txt" "default_agent_requested=0"
sane pair

echo "== a paired board: no pairing"
start_mock --paired
probe bonded connect $ADDR
stop_mock
expect "bonded: connected" "$OUT/bonded.out" "already paired"
expect "bonded: PASS" "$OUT/bonded.out" "rc=0"
expect "bonded: no Pair call" "$OUT/report.txt" "pairs=0"
expect "bonded: one Connect" "$OUT/report.txt" "connects=1"
sane bonded

echo "== a wrong PIN"
start_mock
probe wrongpin connect $ADDR --pin 111111
stop_mock
expect "wrongpin: refused" "$OUT/wrongpin.out" "FAIL Pairing failed (wrong PIN?)"
expect "wrongpin: rc 1" "$OUT/wrongpin.out" "rc=1"
expect "wrongpin: the PIN that was typed" "$OUT/report.txt" "passkey=111111"
sane wrongpin

echo "== AcquireNotify refused: StartNotify"
start_mock --paired --no-acquire
probe startnotify connect $ADDR
stop_mock
expect "startnotify: the fallback" "$OUT/startnotify.out" "notify by StartNotify (PropertiesChanged)"
expect "startnotify: PASS" "$OUT/startnotify.out" "rc=0"
sane startnotify

echo "== MTU 23: every answer cut in 20 byte notifications (nRF52 Bluefruit)"
start_mock --paired --mtu 23
probe mtu23 connect $ADDR
stop_mock
expect "mtu23: MTU" "$OUT/mtu23.out" "MTU 23"
expect "mtu23: both answers joined" "$OUT/mtu23.out" "joined 2"
expect "mtu23: PASS" "$OUT/mtu23.out" "rc=0"
sane mtu23

echo "== the board walks away for 3 s and comes back: the link reconnects by itself"
start_mock --paired --drop-after 1 --away 3
probe reconnect connect $ADDR --listen 12 --reconnect
stop_mock
expect "reconnect: the loss is seen" "$OUT/reconnect.out" "link lost"
expect "reconnect: a failed try while away" "$OUT/reconnect.out" "state reconnecting: org.bluez.Error.Failed: le-connection-abort-by-local"
expect "reconnect: back" "$OUT/reconnect.out" "RECONNECTED after"
if [ "$(grep -c 'DEVICE_INFO (82 bytes)' "$OUT/reconnect.out")" -ge 2 ]; then ok "reconnect: DEVICE_INFO again"; else ko "reconnect: DEVICE_INFO again"; fi
expect "reconnect: rc 0" "$OUT/reconnect.out" "rc=0"
expect "reconnect: one drop" "$OUT/report.txt" "drops=1"
sane reconnect

echo "== an address nobody advertises"
start_mock
probe notfound connect 11:22:33:44:55:66
stop_mock
expect "notfound: not found" "$OUT/notfound.out" "FAIL Board not found"
expect "notfound: rc 1" "$OUT/notfound.out" "rc=1"
sane notfound

kill "$DAEMON" 2> /dev/null
wait "$DAEMON" 2> /dev/null
echo "ble mock: $pass passed, $fail failed (outputs in $OUT)"
[ $fail -eq 0 ]
