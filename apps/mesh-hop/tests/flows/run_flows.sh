#!/bin/sh
# The 22 scripted simulator flows of Mesh Hop (0.5: nodes, the start page over TCP; 0.5.1: hidden and shown again), one after the other (the simulator's link /tmp/ttyMHSIM is shared), each
# with the simulator options its header names; the preset flows through presets_run.sh (its cases 1-7 and the 60 s
# retry). About 15 minutes. Needs python3 and the host build (../../build.sh host).
#   sh run_flows.sh [RUNNER] [OUT] [WxH | sizes]
#   RUNNER default ~/cdk-build/mesh-hop/host/mesh-hop-script, OUT default /tmp/mesh-hop-flows, WxH default 640x480;
#   "sizes" (or "stage", its name before P4): shell.script (the shell: Home, header, caption, hint line, dialogs), pages.script
#   (P3: the pages with 193 contacts) and chats.script (P4: Chats, the conversation, the on-screen keyboard; chats_run.sh) at
#   640x480, 800x480, 720x720, 1024x600 and 1280x720, and the three at 720x720 with the HackBerry's density (PPMM 670) (about 12
#   minutes).
# Output: OUT/<flow>/*.png and dump.txt, OUT/<flow>.out (the run's stdout and stderr), a summary at the end: the shots
# (145 `shot` lines + 4 shots at the end of a hold at 640x480, the preset flow counted once) and every line a runner
# reported as a failure.
HERE=$(cd "$(dirname "$0")" && pwd)
export MESHHOP_BIN=${1:-${CDK_BUILD:-$HOME/cdk-build/mesh-hop}/host/mesh-hop-script}
OUT=${2:-/tmp/mesh-hop-flows}
export SIZE=${3:-640x480}
export MESHHOP_ONLINE=0
T=$HERE
rm -rf "$OUT"
mkdir -p "$OUT"
t0=$(date +%s)
one() { # name, then environment assignments and the command
    n=$1
    shift
    echo "== $n ($(($(date +%s) - t0)) s)"
    env "$@" > "$OUT/$n.out" 2>&1
}
if [ "$SIZE" = stage ] || [ "$SIZE" = sizes ]; then
    for s in 640x480 800x480 720x720 1024x600 1280x720 720x720@670; do
        size=${s%@*}
        ppmm=
        [ "$s" != "$size" ] && ppmm=${s#*@}
        c=$size${ppmm:+-ppmm$ppmm}       # (not n: one() sets n)
        one "shell-$c" SIZE=$size PPMM=$ppmm SHOTS="$OUT/shell-$c" SCRIPT="$T/shell.script" sh "$T/ui_smoke.sh"
        grep -h "^fill \|^check \|^shell \|^hint \|^expect \|^script:" "$OUT/shell-$c.out" | grep -v " PASS" | sed "s/^/  /"
        one "pages-$c" SIZE=$size PPMM=$ppmm SHOTS="$OUT/pages-$c" SIMARGS="--contacts 193 --manual-add --new-nodes 3 --heard-back 2" SCRIPT="$T/pages.script" sh "$T/ui_smoke.sh"
        grep -h "^check \|^shell \|^hint \|^expect \|^script:" "$OUT/pages-$c.out" | grep -v " PASS" | sed "s/^/  /"
        one "chats-$c" SIZE=$size PPMM=$ppmm SHOTS="$OUT/chats-$c" DATA="$OUT/chats-data" sh "$T/chats_run.sh"
        grep -h "^fill \|^check \|^shell \|^hint \|^expect \|^script:" "$OUT/chats-$c.out" | grep -v " PASS" | sed "s/^/  /"
        rm -rf "$OUT/chats-data"
        echo "  $(grep -h ' PASS' "$OUT/shell-$c.out" "$OUT/pages-$c.out" "$OUT/chats-$c.out" | wc -l) PASS lines, $(grep -h ' FAIL\|^script:' "$OUT/shell-$c.out" "$OUT/pages-$c.out" "$OUT/chats-$c.out" | wc -l) failures"
    done
    echo "== done in $(($(date +%s) - t0)) s: $(find "$OUT" -name '*.png' | wc -l) shots"
    exit 0
fi
one smoke SHOTS="$OUT/smoke" DATA="$OUT/smoke-data" SCRIPT="$T/smoke.script" sh "$T/ui_smoke.sh"
one offline SHOTS="$OUT/offline" DATA="$OUT/smoke-data" NOSIM=1 SCRIPT="$T/offline.script" sh "$T/ui_smoke.sh"
rm -rf "$OUT/smoke-data"
one bigcontacts SHOTS="$OUT/bigcontacts" SIMARGS="--contacts 157" SCRIPT="$T/bigcontacts.script" sh "$T/ui_smoke.sh"
one chan-silent SHOTS="$OUT/chan-silent" SIMARGS="--chan-reply none" SCRIPT="$T/chan-silent.script" sh "$T/ui_smoke.sh"
one firmware SHOTS="$OUT/firmware" SCRIPT="$T/firmware.script" sh "$T/ui_smoke.sh"
one heard-back SHOTS="$OUT/heard-back" SIMARGS="--heard-back 2 --heard-back-direct" SCRIPT="$T/heard-back.script" sh "$T/ui_smoke.sh"
one packet-log SHOTS="$OUT/packet-log" SIMARGS="--heard-back 2 --new-nodes 2" SCRIPT="$T/packet-log.script" sh "$T/ui_smoke.sh"
one position SHOTS="$OUT/position" SCRIPT="$T/position.script" sh "$T/ui_smoke.sh"
one position-nogps SHOTS="$OUT/position-nogps" SIMARGS="--no-gps-var --share-location" SCRIPT="$T/position-nogps.script" sh "$T/ui_smoke.sh"
one p2-contacts SHOTS="$OUT/p2-contacts" SIMARGS="--contacts 8 --manual-add --new-nodes 3 --slow-remove 0.4" SCRIPT="$T/p2-contacts.script" sh "$T/ui_smoke.sh"
one p2-nearby SHOTS="$OUT/p2-nearby" SIMARGS="--manual-add --new-nodes 3" SCRIPT="$T/p2-nearby.script" sh "$T/ui_smoke.sh"
one p2-oldfw SHOTS="$OUT/p2-oldfw" SIMARGS="--fw-level 8 --no-autoadd --no-stats --no-discover --no-factory-reset" SCRIPT="$T/p2-oldfw.script" sh "$T/ui_smoke.sh"
one p2-settings SHOTS="$OUT/p2-settings" SCRIPT="$T/p2-settings.script" sh "$T/ui_smoke.sh"
one history SHOTS="$OUT/history" sh "$T/history_run.sh"
one p2-search SHOTS="$OUT/p2-search" sh "$T/p2_run.sh" p2-search.script
one b-noboard SHOTS="$OUT/b-noboard" NOSIM=1 sh "$T/p2_run.sh" b-noboard.script
one b-reset SHOTS="$OUT/b-reset" sh "$T/p2_run.sh" b-reset.script "--reset-format-delay 4"
one b-swap SHOTS="$OUT/b-swap" SIMCTL=1 sh "$T/p2_run.sh" b-swap.script
one presets SHOTS="$OUT/presets" sh "$T/presets_run.sh"
one nodes SHOTS="$OUT/nodes" sh "$T/nodes_run.sh"
one lifecycle SHOTS="$OUT/lifecycle" SIMCTL=1 SCRIPT="$T/lifecycle.script" sh "$T/ui_smoke.sh"
echo "== done in $(($(date +%s) - t0)) s at $SIZE"
main=0
for d in "$OUT"/*/; do
    [ "$(basename "$d")" = presets ] && continue
    main=$((main + $(ls "$d" | grep -c '\.png$')))
done
p1=$(ls "$OUT/presets/1-online-fetched" 2> /dev/null | grep -c '\.png$')
echo "shots: $((main + p1)) ($main in 19 flows + $p1 of the preset flow's first case); preset cases in all: $(find "$OUT/presets" -name '*.png' | wc -l)"
echo "failures reported by the runner:"
grep -H "^script:\|FAIL\|cannot write\|platform init failed" "$OUT"/*.out | grep -v "^.*: *#" || echo "  none"
