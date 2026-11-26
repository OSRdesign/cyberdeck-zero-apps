#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs the scenes of tests/bridge-scenes against the FAKE bridge (tests/fake_bridge.py) and the fake systemctl: no radio,
# no real service, nothing started on this machine. Needs the headless build first (build.sh host; WSL or Linux, python3).
#
#   sh apps/reticulum/tests/run_scenes.sh [--sizes WxH,WxH] [scene name ...]
#
# For each scene it reads its header lines (comments the harness ignores):
#   # fake-scenario: mixed        the world of fake_bridge.py (fresh, mixed, listenonly, radio, busyport, chat, chatblocked, chatempty, notice; default fresh)
#   # fake-delay: 1.5             every answer waits this long (a slow bridge)
#   # fake-deliver-after: 2.5     a message you send stays "sending" this long before it is delivered or fails (default 0.7)
#   # fake-heard-after: 4        (chat) Camp base is heard this long after the app subscribes (a `heard` event)
#   # fake-appear-after: 3        the service's sockets do not exist for this long (still starting)
#   # fake-start-rc: 1            the exit status of `systemctl start` (and FAKE_START_MSG: # fake-start-msg: ...)
#   # fake-unit: enabled          the unit is enabled when the app first looks
# starts one fake bridge with a socket per size, runs the scene at those sizes (one harness process per size, in
# parallel, all in $CDK_BUILD/host/out-bridge) and then check_requests.py on what the app asked (no transmit-enabling
# request without a scene that says so; the scene's own expectations). Exit status 0 when every scene passed.
set -u
here=$(cd -- "$(dirname -- "$0")" && pwd)
app=$(dirname "$here")
build=${CDK_BUILD:-$HOME/cdk-build/reticulum}
bin=$build/host/reticulum-headless
res=${LAUNCHER_TREE:-$HOME/zero7/launcher}/projects/APPLaunch/APPLaunch
out=$build/host/out-bridge
power=${APPLAUNCH_POWER_SUPPLY_ROOT-$build/power_supply}
sizes_arg=
names=
while [ $# -gt 0 ]; do
    case $1 in
        --sizes) sizes_arg=$2; shift 2 ;;
        *) names="$names $1"; shift ;;
    esac
done
[ -x "$bin" ] || { echo "no $bin: run build.sh host first" >&2; exit 2; }
mkdir -p "$power" "$out"
export APPLAUNCH_POWER_SUPPLY_ROOT=$power
rc=0
count=0
for scene in "$here"/bridge-scenes/*.scene; do
    base=$(basename "$scene" .scene)
    if [ -n "$names" ]; then case " $names " in *" $base "*) ;; *) continue ;; esac; fi
    hdr() { sed -n "s/^# $1: *//p" "$scene" | head -n1; }
    scenario=$(hdr fake-scenario); scenario=${scenario:-fresh}
    delay=$(hdr fake-delay); delay=${delay:-0}
    deliver=$(hdr fake-deliver-after); deliver=${deliver:-0.7}
    heard=$(hdr fake-heard-after); heard=${heard:-0}
    appear=$(hdr fake-appear-after); appear=${appear:-0}
    start_rc=$(hdr fake-start-rc); start_rc=${start_rc:-0}
    start_msg=$(hdr fake-start-msg)
    unit=$(hdr fake-unit); unit=${unit:-disabled}
    sizes=$sizes_arg
    [ -n "$sizes" ] || sizes=$(sed -n 's/^sizes *//p' "$scene" | head -n1 | tr -s ' ' ',')
    [ -n "$sizes" ] || sizes=640x480
    tmp=$(mktemp -d /tmp/ret-scene.XXXXXX)
    python3 "$app/tests/fake_bridge.py" --base "$tmp" --names "$sizes" --scenario "$scenario" --delay "$delay" --deliver-after "$deliver" --heard-after "$heard" \
        --appear-after "$appear" > "$tmp/fake.out" 2>&1 &
    fake=$!
    if [ "$appear" = 0 ]; then
        i=0
        while [ $i -lt 100 ] && ! grep -q ready "$tmp/fake.out" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
    fi
    : > "$tmp/systemctl.log"
    # a wrapper, so that no executable bit is needed on the script in a checkout
    printf '#!/bin/sh
exec sh "%s" "$@"
' "$here/fake_systemctl.sh" > "$tmp/systemctl"
    chmod +x "$tmp/systemctl"
    (
        export RETICULUM_BRIDGE_RUN="$tmp/{canvas}" RETICULUM_SYSTEMCTL="$tmp/systemctl"
        export FAKE_SYSTEMCTL_LOG="$tmp/systemctl.log" FAKE_UNIT_DIR="$tmp" FAKE_UNIT_INITIAL="$unit"
        export FAKE_START_RC="$start_rc" FAKE_START_MSG="$start_msg"
        [ "$(hdr fake-enable-rc)" ] && export FAKE_ENABLE_RC=$(hdr fake-enable-rc)
        "$bin" --scene "$scene" --out "$out" --resources "$res" --sizes "$sizes"
    ) > "$tmp/headless.out" 2>&1
    h=$?
    touch "$tmp/stop"
    wait "$fake" 2>/dev/null
    c=0
    python3 "$here/check_requests.py" "$tmp/requests.log" "$tmp/systemctl.log" "$scene" "$sizes" || c=1
    if [ $h -ne 0 ] || [ $c -ne 0 ]; then
        rc=1
        echo "FAIL  $base (harness exit $h, request checks $c); output:"
        tail -n 25 "$tmp/headless.out"
        cp "$tmp/requests.log" "$out/$base.requests.log" 2>/dev/null
        cp "$tmp/systemctl.log" "$out/$base.systemctl.log" 2>/dev/null
    else
        echo "PASS  $base  ($sizes; fake scenario $scenario)"
        cp "$tmp/requests.log" "$out/$base.requests.log" 2>/dev/null
        cp "$tmp/systemctl.log" "$out/$base.systemctl.log" 2>/dev/null
    fi
    cp "$tmp/requests.log" "$out/$base.requests.log" 2>/dev/null
    rm -rf "$tmp"
    count=$((count + 1))
done
echo "fake-bridge scenes: $count run, $([ $rc -eq 0 ] && echo all passed || echo FAILED)"
exit $rc
