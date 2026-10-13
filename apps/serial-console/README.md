# Serial Console

Version 0.2.0 (draft, not published yet), package `serial-console`. Part of [cyberdeck-zero-apps](../../README.md).

Serial Console talks to a USB-serial adapter or a microcontroller board (Arduino, ESP32, Raspberry Pi Pico, a GPS, a
router console...) from the deck. It is a full-screen touch app made for the small screen: instead of dropping you into
a raw terminal it first shows the serial ports it finds, says what is plugged into each one and recommends the settings,
can find the speed by itself, then opens a terminal page.

## What it does

* **Ports page.** Every USB serial port (`ttyUSB*`, `ttyACM*`) as soon as it is plugged in, refreshed every second
  (plug and unplug a board: the list follows, only the rows that changed are redrawn). Each row names the device
  (for example *CP210x USB-serial*, *Arduino Uno*, *FT232R USB-serial*, or the USB product string of a device the app
  does not know) with its tty at the right; the status line under the title says `ttyUSB0 - 115200 recommended` for
  the highlighted one. **Show on-board UARTs** adds the board's own UARTs: the one on GPIO 14/15 (`/dev/serial0`) shows
  as *On-board UART - GPIO 14/15*. Empty 8250 slots (`ttyS*` with no UART behind) and the Raspberry Pi's Bluetooth UART
  (`/dev/serial1`) are never listed. With no port at all, a card says what to do.
* **Port page** (Enter on a port). *What is connected*: device name, maker, USB id, driver, serial number, what it
  typically is (*ESP32, Meshtastic*; *Arduino sketches*; *GPS: NMEA sentences*...), the recommended settings and the
  `/dev` node; the status line explains the recommendation (*ESP32 boards use 115200; ESP8266 ROM: 74880*). Then the
  settings, kept per port (by its `/dev/serial/by-id` name, else USB id and serial number): **Speed** 1200 to 921600
  (74880 included, the recommended one marked), **Frame** (8N1, 7E1, 8E1, 8O1, 7O1, 8N2), **Local echo**, **Enter
  sends** (CR, LF, CR LF), **Backspace sends** (DEL 0x7F or BS 0x08), **Log to file**, **Find the speed**, **Send Enter
  while finding**, and **Connect** (highlighted when the page opens).
* **Find the speed.** Listens about 0.7 s at each speed (the recommended one first, then 115200, 9600, 57600, 38400,
  19200, 230400, 460800, 921600, 4800, 2400, 1200), shows *Trying 9600... (2 of 11)* and scores what comes: clean
  text wins (*Found 57600 - text looks clean*, the speed is set), noise (NUL, 0xFF, bytes that are not text) loses. It
  sends nothing, unless **Send Enter while finding** is on: then a silent board gets a second pass with one Enter at
  each speed (a router's login prompt answers it). Nothing readable: *No readable text at any speed; the board may be
  silent: try Connect and press its reset button*; the speed is left as it was. Esc (or a tap on the row) stops it.
* **Terminal page** (Connect). The header shows `ttyUSB0 115200 8N1` (the frame moves to the status line on a 640x480
  screen, where the header has no room for it), the status line a pill (*Connected*, *Port lost: waiting*) and the RX /
  TX byte counts, *Paused*, the log file. The text is the board's output in a monospace face: colour codes and other
  escape sequences are removed, CR, LF, Backspace, Tab and the line-editing codes of a shell prompt are handled, bytes
  that are not UTF-8 show as `.`; a long line wraps; the last 2000 lines are kept. A drag scrolls back (it pauses the
  view; scrolling to the bottom follows again). The toolbar (it scrolls sideways where the screen is narrow): Ctrl-C,
  Ctrl-D, Ctrl-Z, Tab, Send Esc, Enter, Pause / Resume, Clear, Start / Stop log, Reset board.
* **Unplugged during a session**: the text stays, the pill turns to *Port lost: waiting*, and the app reconnects by
  itself, with the same settings, when the same board comes back (even on another ttyUSB number).
* **No permission**: when the user is not in the `dialout` group, the ports page has a *Serial port access - How to
  fix* row, and a refused open shows a dialog with the exact fix (`sudo usermod -aG dialout $USER`, then reboot).

## Keys and touch

| Where | Keyboard | Touch |
| --- | --- | --- |
| Lists | Up / Down move, Enter or Right opens, Esc or Left goes back | a tap highlights a row, a second tap opens it; a drag scrolls |
| Terminal, keyboard connected | every key goes to the board: Enter sends the line ending, Backspace DEL or BS, Ctrl+letter its control code, arrows / Home / End / Page Up / Down / F1-F12 their ANSI sequences | toolbar keys |
| Terminal, app keys | **Esc** leaves the page (the connection closes; a real ESC is the toolbar's Send Esc), Shift+Page Up / Down scroll, Alt+P pause, Alt+X clear, Alt+L log, Alt+R reset the board, Alt+E send ESC | |
| Terminal, no keyboard | | a tap on the line field brings the on-screen keyboard; its Enter (or Send) sends the line; its one-shot Ctrl + a letter sends the control code |

A short Esc on the ports page (or the header's Esc) ends the app; holding Esc 3 s is the launcher's watchdog. The
on-screen keyboard only shows when no physical keyboard is connected (the launcher's rule).

## Devices it knows

USB-serial chips: FTDI FT232R, FT2232, FT4232H, FT232H, FT231X (`0403`); Prolific PL2303 (`067b:2303`, `23a3`);
Silicon Labs CP210x, CP2105, CP2108 (`10c4:ea60`, `ea70`, `ea71`); WCH CH340, CH341, CH9102, CH343 (`1a86:7523`,
`5523`, `55d4`, `55d3`). Boards and probes: Arduino (`2341`, `2a03`: Uno, Mega 2560, Nano Every, Leonardo, Due, Uno
R4), Espressif native USB (`303a:1001` USB-Serial/JTAG, `303a:4001`), Raspberry Pi Pico and Debug Probe (`2e8a`),
Adafruit, SparkFun and Seeed boards, the RAK4631 / Feather nRF52840 (`239a:8029`), Teensy (`16c0:0483`), ST-LINK and
STLINK-V3 virtual COM ports (`0483:374b`, `3752`, `374e`), STM32 virtual COM and Flipper Zero (`0483:5740`, told apart
by the product string), DAPLink, J-Link, u-blox GPS receivers (`1546:01a7`, `01a8`, `01a9`, 9600 NMEA), the Cisco USB
console (`05a6:0009`, 9600). Recommendations: ESP32 / ESP8266 boards and most adapters 115200 (the ESP8266 boot ROM
prints at 74880), Arduino sketches 9600, GPS 9600, switch consoles 9600, native USB ports any speed. An unknown device
works too: named from its USB product string, 115200 8N1 suggested. The table is `src/core/devices.cpp`.

## Files

| File | What |
| --- | --- |
| `~/.config/cardputerzero/serial-console.conf` | the settings: on-board UARTs shown, the last port, one `[port <id>]` section per port (the file of version 0.1.0 is read once and kept: its settings become those of its last port) |
| `~/serial-logs/<tty>-<date>-<time>.log` | the logs: every byte received, raw (nothing is dropped, even when the screen cannot keep up); a new file each time logging is switched on |

## Requirements

* A screen of at least 640x480 (`X-MinSize=640x480`: the tile is hidden on smaller screens; started there anyway, the
  app says it needs 640x480) and a launcher with full-screen app support.
* The user in the `dialout` group: `sudo usermod -aG dialout $USER`, then reboot.
* Packages: `libfreetype6`, `libinput10`, `libxkbcommon0` (picocom is no longer used).

## Known limits

* Not a terminal emulator: no cursor addressing, no colours, no alternate screen. Line-oriented consoles (boot logs,
  shells, REPLs, AT commands, U-Boot, a router CLI) work; full-screen programs (vi, top, menuconfig) do not.
* At 640x480 without a keyboard, the on-screen keyboard leaves room for the line field only: the text shows again when
  it goes (Esc).
* One port at a time. Typed bytes are not logged (only what the board sends).
* Reset board pulses DTR off and RTS on for 100 ms: it resets ESP32 / ESP8266 boards with the usual auto-reset circuit
  and Arduino boards (DTR); a board wired differently may not react; a port without modem lines says so.
* The settings file keeps the launcher's config folder name (see the launcher's NAMING.md).

## Changes

* **0.2.0**: a native full-screen app instead of the numbered menu and picocom of 0.1.0: the ports page with hot-plug
  and device identification, recommended settings, Find the speed, the terminal page with its toolbar, the on-screen
  keyboard line, reconnection after an unplug, settings per port. Needs 640x480 (0.1.0 also ran at 480x320). The 0.1.0
  settings file is migrated.
* 0.1.0: a POSIX sh menu in the launcher's terminal, then picocom (draft, not published).

## Build and test

Built like the other native apps (see [`apps/_common`](../_common/README.md) and the
[template](../cyberdeck-app-template/README.md)), in WSL with the launcher's build tree:

```sh
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/serial-console/build.sh host --strict   # tests + scenes
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/serial-console/build.sh cross --strict  # aarch64 binary
python3 tools/make_registry.py --only serial-console --out C:/somewhere/outside/the/repo       # the .deb
```

* `tests/` holds the unit tests of `src/core` (device table, identification on a fake sysfs tree, the byte filter and
  the ring of lines, the speed scorer, the settings file and its migration, the keys) and the I/O tests on
  pseudo-terminals: a "board" thread on the master side sends text only at its own speed and noise at any other, as a
  UART does; the tests open, configure (74880 too), receive, send, log, flood the port faster than the screen drains it
  (the log keeps every byte), unplug and plug it again, and run Find the speed (found, silent, stopped).
* `tests/scenes/*.scene` play every page at 640x480, 800x480, 720x720, 1280x720 and 1024x600 in the **demonstration
  world**: `SERIAL_CONSOLE_DEMO=<empty|boards|lost|noaccess> serial-console` makes the app scan a fake `/sys` and
  `/dev` tree in a temporary folder whose boards are pseudo-terminals (an ESP32 on a CP2102 with a colour boot log, an
  Arduino Uno at 9600, an FT232R cable at 57600, the on-board UART; `lost` unplugs the ESP32 board for 3 s, `noaccess`
  refuses it). It also works on the deck, to try the app without a board. Contact sheets:
  `~/cdk-build/serial-console/host/out/sheets`.
* `tests/BOARD-TESTS.md`: the physical tests on the deck and the Pi 3A+.

Sources: `src/main.cpp` (the device runner), `src/ui/` (the pages: `serial_app.*`, `terminal_view.*`, `app_spec.cpp`),
`src/core/` (no LVGL: `devices`, `ports`, `fake_sys`, `settings`, `term`, `baud`, `keymap`, `serial_io`, `demo`,
`console_core`, `fsutil`).

## Credits and licence

<!-- branding-allow-begin: credit of the cp0_lvgl runtime linked into the binary (MIT) -->
MIT. The binary contains the launcher's `cp0_lvgl` runtime (M5Stack Technology CO LTD, MIT) and LVGL (LVGL Kft, MIT);
see `root/usr/share/doc/serial-console/copyright`. The fonts (Montserrat, JetBrains Mono) are the launcher's, read at
run time; none is shipped.
<!-- branding-allow-end -->
