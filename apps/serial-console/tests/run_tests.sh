#!/bin/sh
# Host tests of the serial console menu (POSIX sh; run in WSL or any Linux):
#   sh apps/serial-console/tests/run_tests.sh
# Unit part: sources the script with SERIAL_CONSOLE_TEST=1 and checks its functions on fixture directories.
# End-to-end part: a socat PTY pair, the menu driven through stdin, picocom started for real (skipped when socat or
# picocom are missing; SOCAT=/path PICOCOM=/path point to unpacked binaries).
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/../root/usr/share/APPLaunch/bin/M5CardputerZero-serial-console
T=$(mktemp -d)
trap 'rm -rf "$T"; [ -n "$SOCAT_PID" ] && kill "$SOCAT_PID" 2>/dev/null' EXIT
FAIL=0; PASS=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); printf 'FAIL %s\n  expected: %s\n  actual:   %s\n' "$1" "$2" "$3"; fi
}

SERIAL_CONSOLE_TEST=1 . "$SCRIPT"

# ---- fixtures: a /dev with two USB ports (one with a by-id link), two UARTs, a by-id link to a non-serial node
mkdir -p "$T/dev" "$T/byid"
for n in ttyUSB0 ttyACM0 ttyS0 ttyAMA0 sda; do : > "$T/dev/$n"; done
ln -s ../dev/ttyACM0 "$T/byid/usb-Arduino__www.arduino.cc__Uno_8573-if00"
ln -s ../dev/sda "$T/byid/usb-Not_A_Serial-if00-port0"
ln -s ../dev/ttyACM0 "$T/byid/usb-Duplicate_Link-if01"

out=$(sc_list_ports "$T/byid" "$T/dev" 0 | sed "s|$T|T|g" | tr '\n' ';')
check "list usb ports" "T/dev/ttyACM0|Arduino__www.arduino.cc__Uno_8|usb-Arduino__www.arduino.cc__Uno_8573-if00;T/dev/ttyUSB0|-|;" "$out"
out=$(sc_list_ports "$T/byid" "$T/dev" 1 | sed "s|$T|T|g" | cut -d'|' -f1 | tr '\n' ' ')
check "list all ports" "T/dev/ttyACM0 T/dev/ttyUSB0 T/dev/ttyS0 T/dev/ttyAMA0 " "$out"
mkdir -p "$T/empty"
check "no ports" "" "$(sc_list_ports "$T/nonexistent" "$T/empty" 0)"
check "label cp2102" "Silicon_Labs_CP2102_0001" "$(sc_label usb-Silicon_Labs_CP2102_0001-if00-port0)"
check "label ftdi" "FTDI_FT232R_UART_A50285BI" "$(sc_label usb-FTDI_FT232R_UART_A50285BI-if00-port0)"

check "next baud" "230400" "$(sc_next 115200 "$SC_BAUDS")"
check "next baud wraps" "1200" "$(sc_next 921600 "$SC_BAUDS")"
check "next unknown" "1200" "$(sc_next 74880 "$SC_BAUDS")"
check "frame 8N1" "-d 8 -y n -p 1" "$(sc_frame_opts 8N1)"
check "frame 7E1" "-d 7 -y e -p 1" "$(sc_frame_opts 7E1)"
check "frame 8O2" "-d 8 -y o -p 2" "$(sc_frame_opts 8O2)"
sc_frame_opts 9X1 >/dev/null; check "frame bad" "1" "$?"

check "args default" "-b 115200 -d 8 -y n -p 1 --imap lfcrlf /dev/ttyUSB0 " \
    "$(sc_build_args /dev/ttyUSB0 115200 8N1 off CR off /l 20260101-000000 | tr '\n' ' ')"
check "args all options" "-b 9600 -d 7 -y e -p 1 --imap lfcrlf --omap crcrlf -c --logfile /l/ttyACM0-20260101-000000.log /dev/ttyACM0 " \
    "$(sc_build_args /dev/ttyACM0 9600 7E1 on CRLF on /l 20260101-000000 | tr '\n' ' ')"
check "args LF" "--omap crlf" "$(sc_build_args /dev/x 1200 8N1 off LF off /l t | grep -A1 omap | tr '\n' ' ' | sed 's/ $//')"

# menu answers
SC_BAUD=115200; SC_FRAME=8N1; SC_ECHO=off; SC_ENDING=CR; SC_LOG=off; SC_ALL=0
sc_apply 2; check "answer digit" "connect 2" "$SC_ACT"
sc_apply ''; check "answer enter" "connect 0" "$SC_ACT"
sc_apply q; check "answer quit" "quit" "$SC_ACT"
sc_apply zz; check "answer bad" "bad" "$SC_ACT"; sc_apply 1x; check "answer 1x" "bad" "$SC_ACT"
sc_apply b; sc_apply b; check "b cycles" "460800" "$SC_BAUD"
sc_apply b74880; check "b<number>" "74880" "$SC_BAUD"
sc_apply f; check "f cycles" "7E1" "$SC_FRAME"
sc_apply e; check "e toggles" "on" "$SC_ECHO"
sc_apply n; check "n cycles" "LF" "$SC_ENDING"
sc_apply l; check "l toggles" "on" "$SC_LOG"
sc_apply a; check "a toggles" "1" "$SC_ALL"

# config round trip, and bad values are ignored
SC_PORT=usb-X-if00; sc_save_conf "$T/cfg/sub/serial-console.conf"
SC_PORT=; SC_BAUD=1; SC_FRAME=; SC_ECHO=; SC_ENDING=; SC_LOG=; SC_ALL=
sc_load_conf "$T/cfg/sub/serial-console.conf"
check "conf round trip" "usb-X-if00 74880 7E1 on LF on 1" "$SC_PORT $SC_BAUD $SC_FRAME $SC_ECHO $SC_ENDING $SC_LOG $SC_ALL"
printf 'baud=abc\nframe=9Z9\necho=maybe\nending=XX\n$(touch %s/pwned)=1\n' "$T" > "$T/bad.conf"
sc_load_conf "$T/bad.conf"
check "conf bad values ignored" "74880 7E1 on LF" "$SC_BAUD $SC_FRAME $SC_ECHO $SC_ENDING"
check "conf never executed" "no" "$([ -e "$T/pwned" ] && echo yes || echo no)"

# ---- the whole menu, with a fake picocom: argv and the saved config
cat > "$T/fakepicocom" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$FAKE_OUT"
EOF
chmod +x "$T/fakepicocom"
run_menu() { # input, extra env via caller
    printf "$1" | FAKE_OUT=$T/argv SC_BYID_DIR=$T/byid SC_DEV_DIR=$T/dev SC_CONF=$T/m/serial-console.conf \
        SC_LOG_DIR=$T/logs SC_PICOCOM=$T/fakepicocom HOME=$T sh "$SCRIPT" > "$T/screen" 2>&1
}
chmod 666 "$T/dev/ttyACM0" "$T/dev/ttyUSB0"
rm -f "$T/argv"; run_menu 'b\nb\n2\n'
check "menu: port 2 at 460800" "-b 460800 -d 8 -y n -p 1 --imap lfcrlf $T/dev/ttyUSB0 " "$(tr '\n' ' ' < "$T/argv" 2>/dev/null)"
check "menu: exit hint shown" "1" "$(grep -c 'Ctrl-A, Ctrl-X' "$T/screen" | head -1 | sed 's/^[2-9]$/1/')"
grep -q '^port=/.*ttyUSB0$' "$T/m/serial-console.conf"; check "menu: remembers port" "0" "$?"
rm -f "$T/argv"; run_menu '\n'
check "menu: Enter = last port" "$T/dev/ttyUSB0" "$(tail -1 "$T/argv" 2>/dev/null)"
check "menu: Enter shows default" "1" "$(grep -c 'Enter=2' "$T/screen" | sed 's/^[2-9]$/1/')"
rm -f "$T/argv"; run_menu 'q\n'
check "menu: quit" "no" "$([ -e "$T/argv" ] && echo yes || echo no)"
rm -f "$T/argv"; printf '' | FAKE_OUT=$T/argv SC_BYID_DIR=$T/none SC_DEV_DIR=$T/empty SC_CONF=$T/n.conf \
    SC_PICOCOM=$T/fakepicocom HOME=$T sh "$SCRIPT" > "$T/screen" 2>&1
check "menu: no port message" "1" "$(grep -c 'No serial port' "$T/screen" | sed 's/^[2-9]$/1/')"
if [ "$(id -u)" != 0 ]; then
    chmod 600 "$T/dev/ttyUSB0"; chmod 000 "$T/dev/ttyUSB0"
    rm -f "$T/argv"; run_menu '2\n\nq\n'
    if id -Gn | grep -qw dialout; then want='No access'; else want='dialout group'; fi
    check "menu: no access message" "1" "$(grep -c "$want" "$T/screen" | sed 's/^[2-9]$/1/')"
    check "menu: picocom not run without access" "no" "$([ -e "$T/argv" ] && echo yes || echo no)"
fi
printf 'q\n' | SC_PICOCOM=/nonexistent/picocom SC_CONF=$T/n.conf HOME=$T sh "$SCRIPT" > "$T/screen" 2>&1
check "menu: picocom missing" "1" "$(grep -c 'apt install picocom' "$T/screen")"

# ---- end to end: socat PTY pair, real picocom
SOCAT=${SOCAT:-$(command -v socat)}; [ -n "$SOCAT_LIBS" ] && export LD_LIBRARY_PATH=$SOCAT_LIBS; PICOCOM=${PICOCOM:-$(command -v picocom)}
if [ -x "$SOCAT" ] && [ -x "$PICOCOM" ]; then
    "$SOCAT" -d -d pty,raw,echo=0,link="$T/dev/ttyUSB7" pty,raw,echo=0,link="$T/peer" 2> "$T/socat.log" &
    SOCAT_PID=$!
    i=0; while [ ! -e "$T/peer" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    # the peer answers "pong" to anything; the menu picks the port by its number and runs picocom for 1.5 s
    ( sleep 0.3; cat "$T/peer" > "$T/peer_rx" ) &
    CAT_PID=$!
    ( sleep 0.6; printf 'pong\n' > "$T/peer" ) &
    mkdir -p "$T/e2e"; rm -rf "$T/byid2"; mkdir "$T/byid2"; : > "$T/e2e/placeholder"
    n=$(SERIAL_CONSOLE_TEST=1 sh -c '. "$1"; sc_list_ports "$2" "$3" 0' _ "$SCRIPT" "$T/byid2" "$T/dev" | grep -n ttyUSB7 | cut -d: -f1)
    cat > "$T/pico_wrap" <<EOF
#!/bin/sh
exec "$PICOCOM" --exit-after 1500 "\$@"
EOF
    chmod +x "$T/pico_wrap"
    { printf 'l\n%s\n' "$n"; sleep 0.4; printf 'ping\r'; sleep 2; } | SC_BYID_DIR=$T/byid2 SC_DEV_DIR=$T/dev \
        SC_CONF=$T/e2e/c.conf SC_LOG_DIR=$T/logs SC_PICOCOM=$T/pico_wrap HOME=$T sh "$SCRIPT" > "$T/e2e_screen" 2>&1
    sleep 0.3; kill "$CAT_PID" 2>/dev/null
    check "e2e: picocom opened the pty" "1" "$(grep -c 'Terminal ready' "$T/e2e_screen" | sed 's/^[2-9]$/1/')"
    check "e2e: board received ping" "1" "$(grep -c ping "$T/peer_rx" 2>/dev/null | sed 's/^[2-9]$/1/')"
    check "e2e: pong shown" "1" "$(grep -c pong "$T/e2e_screen" | sed 's/^[2-9]$/1/')"
    check "e2e: pong logged" "1" "$(cat "$T"/logs/*.log 2>/dev/null | grep -c pong)"
    [ "$FAIL" = 0 ] || { echo "--- e2e screen"; cat -v "$T/e2e_screen"; echo "--- socat"; cat "$T/socat.log"; }
else
    echo "SKIP e2e: socat or picocom not found (SOCAT=$SOCAT PICOCOM=$PICOCOM)"
fi

echo "passed $PASS, failed $FAIL"
[ "$FAIL" = 0 ]
