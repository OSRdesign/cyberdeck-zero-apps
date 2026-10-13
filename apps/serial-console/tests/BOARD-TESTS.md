# Serial Console 0.2.0: board tests

For the Controller and the user (controller-led testing: the Controller deploys and reads logs, the user does the
physical steps). `deck.py` is `C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`; `DECK_HOST=<ip>`
points it at another board). The app's process name is `serial-console`.

Expected pictures: the contact sheets of `build.sh host` in `\\wsl.localhost\Ubuntu\home\osrde\cdk-build\serial-console\host\out\sheets\`
(`ports.png`, `empty.png`, `port.png`, `detect.png`, `terminal.png`, `lost.png`, `access.png`, `notice.png`).

Hardware wanted: a USB-serial board that prints at boot (an ESP32 dev board on CP2102 / CH340 / CH9102 is ideal), and
if possible a second one (an Arduino Uno, a GPS, an FTDI cable).

## 0. Deploy

```
python tools/make_registry.py --only serial-console --out C:/CLAUDE/zero7/pkg-out
python deck.py put C:/CLAUDE/zero7/pkg-out/serial-console_0.2.0_arm64.deb /tmp/serial-console.deb
python deck.py sudo 'dpkg -i /tmp/serial-console.deb'
python deck.py run '/usr/share/APPLaunch/bin/serial-console --version; id -nG'
```

`--version`: `Serial Console (serial-console) 0.2.0`, the launcher commit, `cp0_lvgl pinned`. `id -nG` must list
`dialout` (else S6 first). Logs: `python deck.py run 'journalctl --user -n 80 --no-pager | grep -E "cdk|serial-console"'`.

## S1. Deck (640x480, Bluetooth keyboard): ports, hot-plug, identification

1. Open Serial Console with no board plugged: the card "No serial adapter found..." above "Show on-board UARTs".
2. Plug the ESP32 board: within about a second its row appears and is highlighted (e.g. *CP210x USB-serial* /
   `ttyUSB0`); the status line says `ttyUSB0 - 115200 recommended`. Unplug: the row goes. Plug again.
3. Enter: the port page with Connect highlighted. Up through the rows: Device, Maker, USB ID (matches `lsusb`), Driver
   (`cp210x` / `ch341`), Serial no, Typically, Recommended, Port. The status line explains the recommendation while the
   highlight is on a setting.
4. Show on-board UARTs on: *On-board UART - GPIO 14/15* appears (only if `enable_uart=1`); no `ttyS0` slot, no
   Bluetooth UART. Switch it off again. The choice survives a restart of the app.

## S2. Deck: find the speed

1. On the ESP32's port page set Speed to 9600 (it now says `9600`, Connect `9600 8N1`), go to Find the speed, Enter.
   The row shows *Trying 115200... (1 of 11)* etc.; press the board's reset (EN) button during the first second.
   Expect *Found 115200 - text looks clean* and Speed back to *115200 (recommended)*.
2. Find the speed again without resetting the board (silent board): after about 9 s the notice *No readable text at
   any speed...*; OK closes it; Speed unchanged.
3. Start it again and press Esc after 2 s: it stops at once (*Stopped*), the page stays.

## S3. Deck: terminal with the Bluetooth keyboard

1. Connect. Header `ttyUSB0 115200` (the frame `8N1` starts the status line at 640x480), green *Connected* pill, RX
   counting. Press the board's reset button: the ESP-IDF log appears without colour codes or garbage (the ROM's 74880
   lines show as dots, that is expected), long lines wrap.
2. Type on the BT keyboard: keys go to the board at once (a MicroPython / ESP-IDF console echoes them); Enter sends CR;
   Ctrl+C interrupts a running MicroPython program; arrows recall history where the board supports it.
3. Toolbar by touch: drag it sideways, Pause (the text stops, *Paused*), Resume, Clear, Start log (status shows the
   file), Stop log; check `~/serial-logs/ttyUSB0-*.log` holds the received text. Reset board: an ESP32 / Arduino board
   restarts (DTR/RTS pulse); note the result for your board.
4. Drag the text down: older lines, *Paused*; drag back to the bottom: it follows again.
5. Esc on the BT keyboard: back to the port page (the connection is closed). Esc, Esc: the app ends.

## S4. Deck: unplug during a session

Connect, then pull the USB cable: within a second the pill turns red *Port lost: waiting*, the text stays. Plug it back
(any port): *Connected* again by itself, the same settings, the text goes on. Log: no crash, no SIGKILL.

## S5. Deck: flood

With a board that prints fast (or a loop-back cable on an FT232 / CP2102 at 921600: TX to RX, then paste a long text or
hold a key), the UI stays responsive (Up/Down, Esc work at once), the counters run, `top -b -n 2 -d 3 -p $(pgrep -x
serial-console)` stays reasonable, and with the log on the file size equals the RX count.

## S6. Permission

If the deck user is in `dialout`, test on a user without it, or temporarily: `sudo gpasswd -d $USER dialout`, reboot.
Expect the ports page row *Serial port access - How to fix* and its dialog with `sudo usermod -aG dialout $USER`, and
Connect refused with the dialog *No permission for ttyUSB0*. Restore with `sudo usermod -aG dialout $USER`, reboot.

## S7. Pi 3A+ (800x480, touch only)

```
set DECK_HOST=<pi3a ip>
python deck.py put C:/CLAUDE/zero7/pkg-out/serial-console_0.2.0_arm64.deb /tmp/serial-console.deb
python deck.py sudo 'dpkg -i /tmp/serial-console.deb'
```

1. The tile shows (800x480 is above X-MinSize). Open it by touch; the ports page as on `ports.png` 800x480.
2. Plug a board: it appears; tap it twice: the port page; tap Connect twice.
3. No keyboard: the terminal page has the line field and Send. Tap the field: the on-screen keyboard; the field stays
   above it; type `help` and its Enter: the line goes to the board, the keyboard stays; its Esc closes it.
4. On-screen Ctrl then C: Ctrl-C reaches the board. Toolbar keys by touch. The header Esc leaves; the header Esc again
   twice ends the app.

## Unverified until a board run

| # | Assumption |
| --- | --- |
| U-1 | Real USB unplug ends a read with EIO / hang-up (tested with pseudo-terminals only) and the by-id link comes back for the reconnect |
| U-2 | `TIOCEXCL` / `TIOCNXCL` and the DTR/RTS pulse work on cp210x, ch341, ftdi_sio, cdc_acm |
| U-3 | 74880 through termios2 is accepted by the cp210x / ch341 drivers |
| U-4 | Rendering at 10 frames a second during a flood stays smooth on the Zero 2 W |
| U-5 | The deck's BT keyboard delivers Ctrl / Alt / Shift with the keys (keymap.cpp reads keysym, text and code) |
| U-6 | `/dev/serial1` is the Bluetooth UART on the Pi Zero 2 W / 3A+ (it is hidden on that basis) |
