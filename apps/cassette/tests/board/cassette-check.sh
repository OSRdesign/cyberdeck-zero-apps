#!/bin/sh
# cassette-check.sh: read-only checks for the Cassette board tests (apps/cassette/tests/BOARD-TESTS.md).
# Runs on the board as the normal user. No sudo, writes nothing, changes no mixer control.
#
#   sh cassette-check.sh cards            sound cards, /dev/snd/by-id, capture devices, mixer controls
#   sh cassette-check.sh take [FILE]      the newest take (or FILE): size, WAV header, length, its journal
#   sh cassette-check.sh journal          every journal entry in <folder>/.journal and leftover .tmp-* files
#   sh cassette-check.sh watch [S] [N]    CPU % of the cassette process, memory, temperature: a line every S s (5), N lines (120)
#   sh cassette-check.sh usb [MIN]        kernel messages about USB and sound of the last MIN minutes (15)
#
# The recordings folder: $CASSETTE_FOLDER, else "folder" from ~/.config/cassette/settings.json, else ~/Recordings.

LC_ALL=C
export LC_ALL

folder() {
    if [ -n "$CASSETTE_FOLDER" ]; then
        echo "$CASSETTE_FOLDER"
        return
    fi
    f=
    s="$HOME/.config/cassette/settings.json"
    if [ -r "$s" ]; then
        f=$(sed -n 's/.*"folder"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$s" | head -n 1)
    fi
    [ -n "$f" ] || f="~/Recordings"
    case $f in
    "~") f=$HOME ;;
    "~/"*) f="$HOME/${f#\~/}" ;;
    esac
    echo "$f"
}

# u32 FILE OFFSET: little-endian unsigned 32-bit number (the boards and PCs are little-endian, od reads host order)
u32() { od -An -tu4 -j "$2" -N 4 "$1" 2>/dev/null | tr -d ' '; }
u16() { od -An -tu2 -j "$2" -N 2 "$1" 2>/dev/null | tr -d ' '; }
tag() { dd if="$1" bs=1 skip="$2" count=4 2>/dev/null | tr -c 'A-Za-z0-9 ' '?'; }

# jget FILE KEY: a number or a string value of "KEY" in a journal file (one line per key, as Cassette writes it)
jget() { sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\{0,1\}\([^\",}]*\)\"\{0,1\}.*/\1/p" "$1" | head -n 1; }

cmd_cards() {
    echo "== /proc/asound/cards"
    cat /proc/asound/cards 2>/dev/null || echo "(not readable)"
    echo "== /dev/snd/by-id"
    ls -l /dev/snd/by-id 2>/dev/null || echo "(none: no USB sound device)"
    echo "== capture devices"
    if command -v arecord >/dev/null 2>&1; then
        arecord -l 2>&1
    else
        ls /proc/asound/card*/pcm*c 2>/dev/null || echo "(arecord not installed)"
    fi
    echo "== USB audio devices"
    for d in /sys/bus/usb/devices/*; do
        [ -r "$d/idVendor" ] || continue
        if grep -qs '^01$' "$d"/*:*/bInterfaceClass; then
            echo "$(cat "$d/idVendor"):$(cat "$d/idProduct") $(cat "$d/manufacturer" 2>/dev/null) $(cat "$d/product" 2>/dev/null) (port $(basename "$d"))"
        fi
    done
    echo "== mixer controls (read only)"
    if command -v amixer >/dev/null 2>&1; then
        for c in /proc/asound/card[0-9]*; do
            [ -d "$c" ] || continue
            n=${c##*card}
            echo "-- card $n: $(cat "$c/id" 2>/dev/null)"
            amixer -c "$n" scontents 2>&1 | grep -E "Simple mixer|Capabilities|Limits|Capture|Mono|Front|dB" | head -n 40
        done
    else
        echo "(amixer not installed: alsa-utils; cassette-probe shows the controls too)"
    fi
}

cmd_take() {
    dir=$(folder)
    f=$1
    if [ -z "$f" ]; then
        f=$(ls -t "$dir"/*.wav "$dir"/*.WAV 2>/dev/null | head -n 1)
    fi
    if [ -z "$f" ] || [ ! -r "$f" ]; then
        echo "no take found in $dir"
        return 1
    fi
    size=$(wc -c <"$f" | tr -d ' ')
    echo "file   $f"
    echo "size   $size bytes, modified $(date -r "$f" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)"
    riff=$(tag "$f" 0)
    wave=$(tag "$f" 8)
    echo "header $riff ... $wave, RIFF size $(u32 "$f" 4)"
    rate=0 ch=0 bits=0 data_at=0 data_size=0 ds64_data=
    off=12
    i=0
    while [ "$off" -lt "$size" ] && [ $i -lt 32 ]; do
        id=$(tag "$f" "$off")
        len=$(u32 "$f" $((off + 4)))
        [ -n "$len" ] || break
        case $id in
        "fmt ")
            ch=$(u16 "$f" $((off + 10)))
            rate=$(u32 "$f" $((off + 12)))
            bits=$(u16 "$f" $((off + 22)))
            ;;
        ds64)
            lo=$(u32 "$f" $((off + 16)))
            hi=$(u32 "$f" $((off + 20)))
            ds64_data=$((hi * 4294967296 + lo))
            ;;
        data)
            data_at=$((off + 8))
            data_size=$len
            break
            ;;
        esac
        echo "chunk  $id at $off, $len bytes"
        off=$((off + 8 + len + (len % 2)))
        i=$((i + 1))
    done
    echo "format $rate Hz, $ch channel(s), $bits-bit"
    if [ "$data_at" -eq 0 ]; then
        echo "data   NOT FOUND (damaged header?)"
        return 1
    fi
    [ -n "$ds64_data" ] && [ "$data_size" -eq 4294967295 ] && data_size=$ds64_data
    echo "data   at $data_at, declared $data_size bytes, in the file $((size - data_at)) bytes"
    ba=$((ch * bits / 8))
    if [ "$rate" -gt 0 ] && [ "$ba" -gt 0 ]; then
        decl=$((data_size / ba))
        real=$(((size - data_at) / ba))
        echo "length declared $((decl / rate)).$(((decl % rate) * 10 / rate)) s, in the file $((real / rate)).$(((real % rate) * 10 / rate)) s"
        if [ "$data_size" -gt $((size - data_at)) ]; then
            echo "WARNING the header declares more audio than the file holds (not repaired yet)"
        elif [ $((size - data_at - data_size)) -gt "$ba" ]; then
            echo "note   the file holds more audio than declared (a take still recording, or not repaired yet)"
        else
            echo "ok     header and file size agree"
        fi
    fi
    base=$(basename "$f")
    hit=
    for j in "$dir"/.journal/*.json; do
        [ -r "$j" ] || continue
        if grep -q "$base" "$j"; then
            hit=$j
            break
        fi
    done
    if [ -n "$hit" ]; then
        show_journal "$hit"
    else
        echo "journal (none names this file: an edit result, or a take from elsewhere)"
    fi
}

show_journal() {
    j=$1
    r=$(jget "$j" rate)
    [ -n "$r" ] && [ "$r" -gt 0 ] 2>/dev/null || r=48000
    fr=$(jget "$j" frames)
    fs=$(jget "$j" frames_synced)
    dr=$(jget "$j" dropped_frames)
    lo=$(jget "$j" lost_frames)
    pa=$(jget "$j" paused_frames)
    echo "journal $(basename "$j"): state $(jget "$j" state), take $(jget "$j" take)"
    echo "        frames ${fr:-?} ($(( ${fr:-0} / r )) s), synced ${fs:-?}, paused ${pa:-0}, DROPPED ${dr:-0} ($(( ${dr:-0} * 1000 / r )) ms), LOST ${lo:-0} ($(( ${lo:-0} * 1000 / r )) ms)"
    rs=$(jget "$j" reason)
    [ -n "$rs" ] && echo "        reason: $rs"
    echo "        device $(jget "$j" name) $(jget "$j" by_id)"
}

cmd_journal() {
    dir=$(folder)
    echo "== folder $dir"
    df -h "$dir" 2>/dev/null | tail -n 1
    found=0
    for j in "$dir"/.journal/*.json; do
        [ -r "$j" ] || continue
        found=1
        show_journal "$j"
    done
    [ $found = 1 ] || echo "no journal entries"
    echo "== leftover temporary files (.tmp-*; Cassette removes them at its next start)"
    ls -la "$dir"/.tmp-* 2>/dev/null || echo "none"
    echo "== takes, newest first"
    ls -lt "$dir"/*.wav "$dir"/*.WAV 2>/dev/null | head -n 20 || echo "none"
}

cmd_watch() {
    every=${1:-5}
    n=${2:-120}
    hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
    echo "time      cassette-cpu%  rss-MB  temp-C  load  throttled"
    k=0
    while [ $k -lt "$n" ]; do
        pid=$(pgrep -x cassette | head -n 1)
        t1=0
        [ -n "$pid" ] && t1=$(awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null)
        sleep "$every"
        cpu="-" rss="-"
        if [ -n "$pid" ] && [ -r "/proc/$pid/stat" ]; then
            t2=$(awk '{print $14 + $15}' "/proc/$pid/stat")
            cpu=$(awk -v a="$t1" -v b="$t2" -v h="$hz" -v s="$every" 'BEGIN { printf "%.1f", (b - a) * 100 / h / s }')
            rss=$(awk '/VmRSS/ { printf "%.1f", $2 / 1024 }' "/proc/$pid/status")
        fi
        temp="-"
        if [ -r /sys/class/thermal/thermal_zone0/temp ]; then
            temp=$(awk '{ printf "%.1f", $1 / 1000 }' /sys/class/thermal/thermal_zone0/temp)
        fi
        thr="-"
        command -v vcgencmd >/dev/null 2>&1 && thr=$(vcgencmd get_throttled 2>/dev/null | sed 's/.*=//')
        printf '%s  %12s  %6s  %6s  %4s  %s\n' "$(date +%H:%M:%S)" "$cpu" "$rss" "$temp" "$(cut -d' ' -f1 /proc/loadavg)" "$thr"
        k=$((k + 1))
    done
}

cmd_usb() {
    min=${1:-15}
    pat='usb|snd|audio|xhci|dwc'
    if journalctl -k --since "${min} min ago" -q --no-pager >/dev/null 2>&1; then
        journalctl -k --since "${min} min ago" -q --no-pager | grep -i -E "$pat" | tail -n 60
    elif dmesg >/dev/null 2>&1; then
        dmesg -T 2>/dev/null | grep -i -E "$pat" | tail -n 60
    else
        echo "kernel log not readable as this user (needs the adm or systemd-journal group); ask the controller"
    fi
}

case $1 in
cards) cmd_cards ;;
take) shift; cmd_take "$@" ;;
journal) cmd_journal ;;
watch) shift; cmd_watch "$@" ;;
usb) shift; cmd_usb "$@" ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
