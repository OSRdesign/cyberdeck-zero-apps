# Serial Console 0.4.0: board tests

For the Controller and the user (controller-led testing: the Controller deploys and reads logs, the user does the
physical steps). `deck.py` is `C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`; `DECK_HOST=<ip>`
points it at another board). The app's process name is `serial-console`.

Expected pictures: the contact sheets of `build.sh host` in `\\wsl.localhost\Ubuntu\home\osrde\cdk-build\serial-console\host\out\sheets\`
(`ports.png`, `empty.png`, `port.png`, `detect.png`, `terminal.png`, `lost.png`, `access.png`, `notice.png`,
`identify.png`, `identify-more.png`, `longname.png`; PDA mode: `portrait.png`, `portrait-identify.png`,
`portrait-band.png`).

Hardware wanted: a USB-serial board that prints at boot (an ESP32 dev board on CP2102 / CH340 / CH9102 is ideal), and
if possible a second one (an Arduino Uno, a GPS, an FTDI cable). For the identification (S8 to S12): an ESP32 board,
a Raspberry Pi Pico (RP2040) with MicroPython (and its BOOT button), an Arduino Uno (or a Nano clone on a CH340), and a
GPS module on a USB-serial adapter, if available. Write down for each board what the card says (photo or the
Copy summary file in `~/serial-logs`).

## 0. Deploy

```
python tools/make_registry.py --only serial-console --out C:/CLAUDE/zero7/pkg-out
python deck.py put C:/CLAUDE/zero7/pkg-out/serial-console_0.6.0_arm64.deb /tmp/serial-console.deb
python deck.py sudo 'dpkg -i /tmp/serial-console.deb'
python deck.py run '/usr/share/APPLaunch/bin/serial-console --version; id -nG'
```

`--version`: `Serial Console (serial-console) 0.6.0`, the launcher commit `5f54d15...` (`cp0_lvgl pinned`), `needs a
screen of at least 640x480, in portrait 480x640`. `id -nG` must list `dialout` (else S6 first). Logs: `python deck.py run 'journalctl --user -n 80 --no-pager | grep -E "cdk|serial-console"'`.

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
   The row shows *Trying 115200... (1 of 10)* etc.; press the board's reset (EN) button during the first second.
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
python deck.py put C:/CLAUDE/zero7/pkg-out/serial-console_0.6.0_arm64.deb /tmp/serial-console.deb
python deck.py sudo 'dpkg -i /tmp/serial-console.deb'
```

1. The tile shows (800x480 is above X-MinSize). Open it by touch; the ports page as on `ports.png` 800x480.
2. Plug a board: it appears; tap it twice: the port page; tap Connect twice.
3. No keyboard: the terminal page has the line field and Send. Tap the field: the on-screen keyboard; the field stays
   above it; type `help` and its Enter: the line goes to the board, the keyboard stays; its Esc closes it.
4. On-screen Ctrl then C: Ctrl-C reaches the board. Toolbar keys by touch: they are as high as the header Esc (the
   9 mm toolbar key, taller than Send) and the received text above them is not covered. The header Esc leaves; the
   header Esc again twice ends the app.

## S8. Identify an ESP32 board (deck, BT keyboard)

The probes' log lines: `python deck.py run 'journalctl --user -n 40 --no-pager | grep "serial-console\] identify"'`
(`identify ttyUSB0: listen -> esp-idf (45)`).

1. Port page of the ESP32 board: the rows *Chip* (*CP2102N (rev 1.00)* / *CH340 (rev 2.54)*, whatever the board has),
   *Product*, *USB* (*12 Mbit/s, 1 interface*), then *Identify what is connected* and the note *The adapter cannot tell
   what is behind it; the probes below ask*. Note the Chip value and compare with the chip printed on
   the board.
2. **Listen** (Enter): the row turns *Listening at 115200 (1/11)*; nothing else happens on the board (no reset: its
   program keeps running). A board that logs (ESP-IDF, Meshtastic) gives a card *Behind it: An ESP32 board* or the
   family, *Likely (listened)*; a silent board (MicroPython, CircuitPython): *Nothing heard*, the note *Many boards stay silent until asked: try the rows
   below* and the rows *Try Ask the device* (opens the Ask page, as the port page's row does) and *Reset and listen*; no
   text may end with dots on the card. Esc while it listens: *Stopped* at once. On the ports page the names are whole
   (*ESP32 native USB*, *ttyACM0* at the right); a long USB product string ends with dots, never the port name.
3. **Reset and listen**: the dialog *Reset the board?*; Cancel does nothing. Enter: the board restarts (watch its LED),
   the card says the chip (*ESP32*, *ESP32-S3*...), *Firmware ESP-IDF v...* or *Arduino-ESP32*, *Sure (after a reset)*;
   *Boot output* shows the ROM banner (`ets Jun  8 2016...` or `ESP-ROM:esp32s3-...`) and the boot. Then Connect: the
   program runs normally (it was not left in the download mode). An ESP8266 board: the card says *ESP8266* and the
   boot output has a part *at 74880*.
4. **Ask the device > ESP boot loader > Send**: the board restarts twice (into its ROM, then back); the card says the
   chip and *Details: MAC xx:xx:...* (compare with `esptool.py chip_id` on a PC if you have it), and the note *The
   board was reset into its program*. Connect: the program runs. Nothing was written: the board's program is the same
   after (same boot output).
5. **Apply settings** on a card: the port's Speed becomes the recommended one; back on the ports page the status line
   says the new recommended speed. Restart the app: the port page shows *Behind it* with the same result.
6. **Copy summary**: `python deck.py run 'ls ~/serial-logs; cat ~/serial-logs/*identify*.txt'` shows the card as text.

## S9. Raspberry Pi Pico (RP2040), MicroPython

1. Hold the Pico's BOOTSEL button and plug it in: the ports page shows the row *RP2040 in BOOTSEL mode* (no tty); the
   status line *A USB drive, not a serial port: replug it without BOOT*; Enter shows the whole sentence. Unplug, plug
   again without the button: the row goes, *Raspberry Pi Pico (MicroPython)* / `ttyACM0` comes.
2. Port page: no adapter note (its own USB); **Listen**: one speed only, about 3 s; it shows the program's output
   (Text) or *Nothing heard* at the prompt. The Pico must not reboot into BOOTSEL (no 1200-baud touch).
3. **Reset and listen**: the dialog says the board's own USB has no reset line; Start: nothing is pulsed, the app
   listens 6 s (a bare Pico has no reset button: nothing restarts, that is expected); the card's note says so.
4. **Ask the device > MicroPython > Send**: a running `main.py` stops; the card says *Behind it RP2040*, *Firmware
   MicroPython v1.xx*, *Board Raspberry Pi Pico*. Connect: the REPL prompt `>>>` is there (Ctrl-D in the terminal
   restarts the program). With CircuitPython: *CircuitPython 9.x*.

## S10. Arduino Uno / Nano

1. Port page of the Uno (`2341:0043`, or a Nano clone on CH340: the adapter note shows).
2. **Listen** at its speed (9600 for most sketches): opening the port restarts it (normal for Arduino), its output
   gives *Arduino sketch* (Uno) or *Text, unknown* (clone), *Likely*.
3. **Ask the device > Arduino boot loader > Send**: the board resets (LED blinks), the card says *Behind it ATmega328P*,
   *Firmware Optiboot 8.0* (an old Nano: *STK500 boot loader* at 57600), *Details boot loader at 115200*. After it,
   Connect: the sketch runs as before (nothing written). A Mega 2560: *STK500 v2 boot loader* and *ATmega2560*.
4. **Ask the device > ESP boot loader** on the Uno: *No ESP boot loader answered*; the sketch runs after.

## S11. A GPS module on a USB-serial adapter

1. **Listen**: it tries 115200 (noise), then 9600: the card *Behind it GPS receiver*, *Chip u-blox NEO-...* (only when
   the module printed its TXT lines during the listen: a u-blox prints them at power-up; unplug-replug the GPS just
   before Listen to see them), *Firmware NMEA*, *Details* the constellations, *Recommended 9600 8N1*.
2. Apply settings, Connect: NMEA sentences readable at once.
3. A GPS set to 38400 or to UBX binary: Listen finds 38400; binary: *Framing UBX (B5 62)*.

## S12. Other devices (whatever is at hand)

A router or a Pi console (U-Boot / Linux / OpenWrt), a 3D printer (Marlin: Ask > Marlin printer: *Ender-3...*), a GRBL
CNC (Ask > GRBL controller), an AT modem (Ask > AT modem: the model), a Meshtastic node (Listen with serial debug on).
Note the card. A probe that does not fit the device must do no harm: AT on an ESP32 prints an error at most; nothing
else.

## S13. PDA mode (0.5.0; launcher v0.8.9 or later, Settings > Screen > PDA mode On rotate left)

On the Pi 3A+ or Pi 4 B DSI board (480x800, touch only) and, if at hand, the FZ-M1 tablet (800x1280, amd64 package).
A board that prints (the ESP32 of S1), or a loopback: a USB-serial adapter (CP2102, CH340, FT232) with a wire from TX
to RX, Local echo Off, so every line typed comes back as received text.

1. **Upright start.** The home grid shows the **Serial Console** tile; a tap starts it **upright** (not turned): the
   ports page as `portrait/480x800/ports.png` of the sheets (on the tablet `portrait/800x1280/ports.png`); the header
   reads *Serial Console*, the Esc pill and the status strip do not overlap it. On the 480 px wide screen the port
   page's header reads *ttyUSB0* (the last part of the path).
2. **Touch under rotation.** A tap on a port's row highlights the row under the finger (not another one: the touch
   follows the portrait rotation), a second tap opens the port page; a drag scrolls the list the way the finger moves;
   Speed by two taps, a value by two taps; the header Esc goes back. Note any tap that lands on the wrong row.
3. **Terminal, touch only.** Connect: the header `ttyUSB0 115200 8N1`, the green *Connected* pill, the key bar and the
   line field with Send at the bottom; the board's text fills the height (about 30 rows at 480x800) at a size you can
   read (note it: too small, fine, too large). A drag on the key bar scrolls it to Reset board.
4. **Keyboard up.** Tap the line field: the on-screen keyboard across the bottom (Esc, Tab, Ctrl and the arrows over
   the letters), inside the screen, nothing cut at the sides; the board's text stays above the field (about 19 rows),
   the newest line right above it. Type `help` and the keyboard's Enter: the line goes to the board (loopback: it comes
   back as text), the keyboard stays. Ctrl then C: a Ctrl-C goes to the board (TX + 1). The keyboard's Esc closes it:
   the status line and the key bar come back.
5. **A keyboard connects** (Bluetooth or USB): the line field goes, every key goes to the board at once; the text
   takes the room of the field. Esc leaves the terminal, Esc, Esc ends the app.
6. **Loopback flood** (optional, an adapter with TX wired to RX at 921600): hold a key on the keyboard; the text
   scrolls, the counters run, Esc answers at once.
7. Settings > Screen > PDA mode Off (the launcher restarts): the app is the landscape app of 0.4.5, unchanged.

## S14. PDA S4: the handwriting band in the app (build at the launcher pin v0.8.11 / 5f54d15, launcher with the S4 part)

The launcher must carry PDA S4 phase 1 (release v0.8.11 or later), PDA mode On rotate
left. Boards: the **Pi 4 B** (DSI 800x480 panel, portrait 480x800, finger or stylus, arm64 package) and the **FZ-M1
tablet** (800x1280 portrait, finger, amd64 package). The board or loopback of S13; no physical keyboard connected
unless a step says so. Expected pictures: `portrait-band.png` of the sheets (480x800, 480x640, 800x1280).

1. **Band on the line field.** Settings > System > On-screen keyboard = **Handwriting + keyboard**. Start Serial
   Console (upright), Connect, tap the line field: the **band** comes up at the bottom (the key strip Keyb, Tab, Ctrl,
   Alt, arrows, Enter over the letters and digits boxes), the same look as the launcher's; no on-screen keyboard. The
   board's text keeps its rows above the field, the field right above the band. Esc (header) or the board's text: as
   before.
2. **Writing.** Write `help` in the letters box: the letters appear in the line field; write `42` in the digits box:
   the digits appear. Strip **Enter**: the line goes to the board (loopback: it comes back as text, TX grows), the band
   stays for the next line.
3. **Strip keys = the on-screen keyboard's keys.** Ctrl on the strip, then a written `c`: a Ctrl-C goes to the board
   (TX + 1), as the keyboard's Ctrl then C. Tab, Up, Down, Left, Right behave as the on-screen keyboard's Tab and arrows
   on the line field (no new rule: Left / Right move in the line). **Keyb**: the standard keyboard with a close cross in
   the band's place; type a key; the cross brings the band back.
4. **Key bar.** Esc (the strip has none: the header Esc or Keyb's keyboard Esc) ends the editing: the band goes, the
   status line and the key bar come back; Ctrl-C, Tab, Esc of the key bar still go to the board (TX grows).
5. **Setting Auto.** Leave the app with the switcher (or Home), Settings > System > On-screen keyboard = **Auto**, back
   in the app, tap the line field: the **standard on-screen keyboard** as in S13 step 4, no band. Back to Handwriting +
   keyboard: the band again.
6. **A keyboard connects:** the field and the band go, every key to the board (S13 step 5).
7. Tablet only: steps 1, 2 and 4 (the band is 478 px high, taller than the keyboard: about 26 rows of text above the
   field instead of 34; note if too few).
8. Note the band's memory if easy: `free -m` before and after starting the app.

## Unverified until a board run

| # | Assumption |
| --- | --- |
| U-1 | Real USB unplug ends a read with EIO / hang-up (tested with pseudo-terminals only) and the by-id link comes back for the reconnect |
| U-2 | `TIOCEXCL` / `TIOCNXCL` and the DTR/RTS pulse work on cp210x, ch341, ftdi_sio, cdc_acm |
| U-3 | 74880 through termios2 is accepted by the cp210x / ch341 drivers |
| U-4 | Rendering at 10 frames a second during a flood stays smooth on the Zero 2 W |
| U-5 | The deck's BT keyboard delivers Ctrl / Alt / Shift with the keys (keymap.cpp reads keysym, text and code) |
| U-6 | `/dev/serial1` is the Bluetooth UART on the Pi Zero 2 W / 3A+ (it is hidden on that basis) |
| U-7 | TIOCMSET sets DTR and RTS together on cp210x, ch341, ftdi_sio, pl2303 and cdc_acm, and the ESP auto-reset circuit boots normally with Reset and listen (tested on the recording fake and simulated boards only) |
| U-8 | The esptool sequences (entry, SYNC, GET_SECURITY_INFO, READ_REG, hard reset) work on a real ESP32 / ESP32-S3 / ESP8266; the eFuse MAC addresses taken from esptool are right |
| U-9 | optiboot on a real Uno answers 0x30 0x20 / 0x75 0x20 / 0x41 0x81 within the timeouts after the DTR reset; the sketch restarts after 0x51 0x20 |
| U-10 | The sysfs `bcdDevice`, `bNumInterfaces`, `speed` and `/sys/bus/usb/devices` are read as on a PC; an RP2040 in BOOTSEL mode is `2e8a:0003` with no tty |
| U-11 | The text patterns match real boards' output (they were written from the formats and tested on representative samples only) |
| U-12 | A USB-Serial/JTAG ESP32-S3 (`303a:1001`) that re-enumerates at a reset is found again within 2.5 s |
| U-13 | In PDA mode the launcher starts the app upright with the portrait canvas (`APPLAUNCH_CANVAS=480x800`, the board's rotation inherited) and the touch, the on-screen keyboard's keys included, follows that rotation (seen only in the harness, which has no rotation) |
| U-14 | The portrait densities of the harness (8.5 px/mm on the 4.3" DSI, 9.0 on a 3.5" panel, 8.6 on the tablet) are the boards'; the terminal's 14 px text (1.6 mm, the deck's size) reads well on the DSI panel |
| U-15 | In PDA mode the launcher publishes the usual keyboard rule (`input.state vkb=auto`) while the app is in front, also with Settings > On-screen keyboard = Handwriting + keyboard, so the app's on-screen keyboard shows when no keyboard is connected |
| U-16 | PDA S4: the app's display backend builds the band with `APPLAUNCH_PDA=1` on the rotated panel (touch of the strip and the writing zone follow the rotation; the band is drawn in the app's framebuffer output, 16 / 32 bpp), and the recogniser reads the shipped tables and the user's glyphs from disk at start (the harness drew the band on a plain display without rotation, shipped tables only) |
