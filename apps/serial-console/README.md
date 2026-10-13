# Serial Console

Version 0.3.0 (draft, not published yet), package `serial-console`. Part of [cyberdeck-zero-apps](../../README.md).

Serial Console talks to a USB-serial adapter or a microcontroller board (Arduino, ESP32, Raspberry Pi Pico, a GPS, a
router console...) from the deck. It is a full-screen touch app made for the small screen: instead of dropping you into
a raw terminal it first shows the serial ports it finds, says what is plugged into each one and recommends the settings,
can find the speed by itself, can find out **what is on the other end** (the board or firmware behind a USB-serial
adapter), then opens a terminal page.

## What it does

* **Ports page.** Every USB serial port (`ttyUSB*`, `ttyACM*`) as soon as it is plugged in, refreshed every second
  (plug and unplug a board: the list follows, only the rows that changed are redrawn). Each row names the device
  (for example *CP210x USB-serial*, *Arduino Uno*, *FT232R USB-serial*, or the USB product string of a device the app
  does not know) with its tty at the right; the status line under the title says `ttyUSB0 - 115200 recommended` for
  the highlighted one. **Show on-board UARTs** adds the board's own UARTs: the one on GPIO 14/15 (`/dev/serial0`) shows
  as *On-board UART - GPIO 14/15*. Empty 8250 slots (`ttyS*` with no UART behind) and the Raspberry Pi's Bluetooth UART
  (`/dev/serial1`) are never listed. With no port at all, a card says what to do. A board in its USB boot loader is a
  USB drive, not a serial port: an RP2040 / RP2350 in BOOTSEL mode (`2e8a:0003`, `2e8a:000f`) or a board in a UF2 boot
  loader gets a row (*RP2040 in BOOTSEL mode*; the status line: *A USB drive, not a serial port: replug it without
  BOOT*; Enter: *RP2040 in BOOTSEL mode (a USB drive, not a serial port): unplug and replug without the BOOT button*).
* **Port page** (Enter on a port). *What is connected*: device name, the exact chip where the USB descriptor tells it
  reliably (FTDI FT232R / FT232H / FT2232C/D / FT2232H / FT4232H / FT-X from bcdDevice and the interface count, as
  Linux's `ftdi_sio` decides; WCH CH340 / CH341 / CH9102 / CH343 / CH342 from the product id; Silicon Labs CP2102 /
  CP2102N / CP2104... only when the product string names it), with its release (bcdDevice), maker, the USB product
  string, USB id, USB speed and interface count, driver, serial number, what it typically is (*ESP32, Meshtastic*;
  *Arduino sketches*; *GPS: NMEA sentences*...), the recommended settings and the `/dev` node; the status line explains
  the recommendation (*ESP32 boards use 115200; ESP8266 ROM: 74880*). Then **Identify what is connected** (next
  section). Then the settings, kept per port (by its `/dev/serial/by-id` name, else USB id and serial number): **Speed** 1200 to 921600
  (74880 included, the recommended one marked), **Frame** (8N1, 7E1, 8E1, 8O1, 7O1, 8N2), **Local echo**, **Enter
  sends** (CR, LF, CR LF), **Backspace sends** (DEL 0x7F or BS 0x08), **Log to file**, **Find the speed**, **Send Enter
  while finding**, and **Connect** (highlighted when the page opens).
* **Find the speed.** Listens about 0.7 s at each speed (the recommended one first, then 115200, 9600, 57600, 38400,
  19200, 230400, 460800, 921600, 4800, 2400), shows *Trying 9600... (2 of 10)* and scores what comes: clean
  text wins (*Found 57600 - text looks clean*, the speed is set), noise (NUL, 0xFF, bytes that are not text) loses. It
  sends nothing, unless **Send Enter while finding** is on: then a silent board gets a second pass with one Enter at
  each speed (a router's login prompt answers it). Nothing readable: *No readable text at any speed; the board may be
  silent: try Connect and press its reset button*; the speed is left as it was. Esc (or a tap on the row) stops it.
  It never tries 1200: opening a port at 1200 baud is the "touch" that reboots an Arduino Leonardo / Zero or a Pico
  into its boot loader; 1200 stays in the Speed list to choose by hand.
* **What is on the other end?** A USB-serial adapter (CH340, CP210x, FTDI, PL2303) says nothing about what is wired
  to it: the port page says so (*The adapter cannot tell what is behind it; the probes below ask*) and
  offers three probes, each its own row; Esc (or a tap on its Busy row) stops one at once:
  * **Listen** (level 1, safe: nothing is sent, the DTR / RTS lines are not touched). It listens at the port's speed,
    then 115200, 9600, 74880, 57600, 38400, 19200, 230400, 250000, 460800, 921600, 4800 (about 1 s each, 2 s for the
    first: opening a port restarts an Arduino-style board), and stops at the first speed where something is clearly
    recognised or the text is clean. Never 1200 (the speed whose "touch" reboots some USB boards into their boot
    loader); on a microcontroller's own USB (Pico, ESP32-S3 USB, Leonardo, Teensy...) only one speed, the speed does
    not matter there. Nothing in 3 s at three speeds: *Nothing heard* (it may print only when it starts).
  * **Reset and listen** (level 2, after a confirmation: *This resets the board (DTR/RTS pulse); a running program
    restarts...*). Nothing is sent. The lines: RTS asserted with DTR released for 100 ms (the ESP auto-reset circuit
    pulls EN low with IO0 high), both released 150 ms (the ESP boots its program, IO0 is never low, so never the
    download mode), both asserted (DTR's edge is the Arduino reset), then as they were. Then 3 s of boot output at the
    port's speed. A garbled start on an ESP-style adapter (an ESP8266's ROM prints at 74880) gets a second reset and
    1 s at 74880. A board whose own USB has no reset line (RP2040, CircuitPython boards, Leonardo...) or a port without
    modem lines: nothing is pulsed, it says so and listens while you press the board's reset button.
  * **Ask the device** (level 3): a page of probes, each asks a confirmation that lists exactly what it sends; none
    runs by itself:

    | Probe | Sends | Resets the board |
    | --- | --- | --- |
    | MicroPython | Ctrl-C (`03`, stops the running program), Ctrl-B (`02`, prints the banner); no code runs; the board stays at `>>>` | no |
    | AT modem | `AT` CR, then `ATI` CR | no |
    | Marlin printer | `M115` LF (firmware name, version, machine) | no |
    | GRBL controller | `$I` LF (version, build options) | no |
    | Arduino boot loader | after a DTR reset, STK500 v1: get sync `30 20`, read signature `75 20`, version `41 81 20` / `41 82 20`, leave `51 20` (115200 then 57600); a Mega: STK500 v2 sign-on and signature | yes (DTR), it runs its sketch again after |
    | ESP boot loader | the esptool boot loader entry (DTR / RTS), then SLIP: SYNC, GET_SECURITY_INFO (the chip id of the newer chips), READ_REG of `0x40001000` (the chip's magic value) and of the eFuse MAC words; then the esptool hard reset into its program | yes, and back into its program (said on the card) |

    Read-only, enforced in code: the STK500 and esptool builders refuse every command that is not on their read-only
    list (no flash write, no erase, no register write; the tests check it), and a board taken into its boot loader is
    always reset back into its program, also when the probe is stopped. The lines are always put back as they were.
  * **The card** (it opens when a probe ends): *Device* (the adapter), *Behind it* (the chip or what it is: *ESP32-S3*,
    *RP2040*, *ATmega328P*, *GPS receiver*, *Marlin printer*...), *Chip*, *Firmware* (name and version: *ESP-IDF
    v5.1.2*, *MicroPython v1.22.2*, *Optiboot 8.0*, *NMEA SPG 3.01*), *Board*, *Details* (the ESP-IDF project, the MAC,
    the reset reason...), *Confidence* (*Sure*, *Likely*, *A guess*, and how: listened, after a reset, asked), *Found*
    (the date), **Boot output** and **Evidence** (the lines it printed, the lines that matched: a scrolling page in the
    monospace face), *Recommended* settings, **Apply settings** (sets that speed and 8N1 for the port), **Run again**,
    **Copy summary** (to a text file in `~/serial-logs`). A note row says what the probe has to say (*No DTR/RTS lines:
    you pressed the board's reset button*, *The board was reset into its program*).
  * **What it recognises** (a table of patterns in `src/core/identify.cpp`, plus NMEA checksums and binary framing):
    ESP-IDF programs (boot loader, `I (123) tag:` logs, project name, app and IDF versions, chip from the boot loader's
    tag), the ESP32 family's ROM banner (`ets Jun  8 2016`, `ESP-ROM:esp32s3-...`, reset reason, download mode), the
    ESP8266 ROM (74880), Arduino-ESP32's debug report, Meshtastic (banner, `S:B:` version line, log format, API frames
    `94 C3`), MicroPython and CircuitPython banners (version, board, MCU), Marlin and Prusa firmware (`M115`), Klipper
    (MCU frames with their CRC), GRBL / grblHAL / FluidNC, U-Boot, a Linux console (kernel, systemd, login prompt),
    OpenWrt, MikroTik RouterOS, Cisco IOS, AT modems (SIMCom, Quectel, ESP-AT), GNSS receivers (NMEA with valid
    checksums; u-blox, MediaTek, SiRF, CASIC, Quectel from their proprietary sentences; UBX `B5 62` and RTCM 3 frames),
    MAVLink heartbeats, SLIP frames, protobuf-like streams, plain text (an Arduino sketch when the USB id is an
    Arduino's), noise and silence.
  * **Remembered** per port in the settings file (the result, its evidence, its first output lines and the date): the
    port page shows it as **Behind it** (Enter: its card), and its speed becomes the port's recommended speed (the ports
    page says `ttyUSB1 - 9600 recommended` for a GPS found at 9600). A later probe that finds nothing keeps it.
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
| A probe or Find the speed running | Esc stops it (the page stays) | a tap on its Busy row stops it |
| Confirmations | Enter is the action (*Reset and listen*, *Send*), Esc is Cancel | both buttons |
| Boot output, Evidence | Up / Down, Page Up / Down, Home / End scroll; Esc or Left goes back | a drag scrolls; the header's Esc |
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
| `~/serial-logs/<tty>-identify-<date>-<time>.txt` | a card's summary (Copy summary) |

The identification is kept in the port's section of the settings file as `id_*` keys (`id_kind`, `id_chip`,
`id_firmware`, `id_version`, `id_board`, `id_detail`, `id_framing`, `id_conf`, `id_baud`, `id_level`, `id_probe`,
`id_when`, and repeated `id_ev` / `id_boot` lines); version 0.2.0 ignores them. Delete the file to forget everything.

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
* **A bare adapter cannot be identified from its USB id**: a CH340 or a CP2102 looks the same with an ESP32, a GPS or
  nothing behind it. Only what the device prints (Listen, Reset and listen) or answers (Ask) tells; a silent device
  that answers none of the probes stays unknown.
* **Opening a port raises DTR and RTS** (Linux does it for every program): an Arduino-style board restarts when Listen
  opens its port, as with any serial terminal. Listen sends nothing and changes no line itself.
* Reset and listen assumes the common wiring (the ESP two-transistor auto-reset circuit, the Arduino DTR capacitor). A
  board with RTS wired straight to its reset, or a USB-Serial/JTAG port that re-enumerates at its reset (the app waits
  2.5 s for it to come back), may behave differently; the card shows what came.
* The ESP chip query uses GET_SECURITY_INFO and the magic value at `0x40001000`, as esptool does; it does not read the
  flash id or size (that needs SPI register writes) and not `0x500007FC`. The MAC is read for the ESP8266, ESP32, -S2,
  -S3, -C2, -C3, -C6 and -H2 (esptool's eFuse addresses).
* The Arduino probe speaks STK500 v1 (optiboot, ATmegaBOOT) and v2's sign-on and signature only (Mega 2560); boards
  without a DTR reset (Leonardo, Nano Every, Uno R4) need their reset button. A Klipper MCU is silent until its host
  talks to it; a Meshtastic node sends API frames only to a client that asked: Listen reports them silent.
* The patterns are written from each firmware's documented format; they have been tested on representative samples
  (`tests/fixtures`), not yet on every real board (see `tests/BOARD-TESTS.md`).
* The settings file keeps the launcher's config folder name (see the launcher's NAMING.md).

## Changes

* **0.3.0**: *What is on the other end?*: Listen, Reset and listen, Ask the device (MicroPython, AT, Marlin, GRBL, the
  Arduino and ESP boot loaders, read-only), the result card with its boot output and evidence, remembered per port and
  used for the recommended speed; the exact USB-serial chip, USB product string, speed and interfaces on the port page;
  the BOOTSEL / UF2 drive row on the ports page.
* 0.2.0: a native full-screen app instead of the numbered menu and picocom of 0.1.0: the ports page with hot-plug
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
* The identification: `test_identify.cpp` runs the classifier on `tests/fixtures/*.txt` (one representative sample
  per firmware family, written in its real format: each file says so in its header; NMEA checksums are real) and on
  binary frames built with their real checksums (UBX, RTCM 3, MAVLink, Klipper, Meshtastic, SLIP), and checks the
  settings-file round trip; `test_bootproto.cpp` checks the STK500 v1 / v2 and esptool SLIP framing, checksums, chip
  and MAC tables, the read-only whitelists and the DTR / RTS sequences on the recording fake (order, the ESP circuit
  never put in download mode by Reset and listen, the lines put back after a cancel); `test_probe_pty.cpp` runs every
  probe against the simulated boards of `src/core/sim.cpp` on pseudo-terminals (ESP32, ESP32-S3, ESP8266 in a reboot
  loop, Arduino Uno with optiboot, MicroPython and CircuitPython Picos, a u-blox GPS, GRBL, Marlin, a SIM800 modem, a
  silent board): what was sent (nothing for Listen; exactly the listed bytes for Ask), the boot loader commands seen
  (read-only only), the line trace and its restore, the confirmation rule, Esc.
* `tests/scenes/*.scene` play every page at 640x480, 800x480, 720x720, 1280x720 and 1024x600 in the **demonstration
  world**: `SERIAL_CONSOLE_DEMO=<empty|boards|lost|noaccess> serial-console` makes the app scan a fake `/sys` and
  `/dev` tree in a temporary folder whose boards are pseudo-terminals (an ESP32 on a CP2102 with a colour boot log, an
  Arduino Uno at 9600, an FT232R cable at 57600, the on-board UART; `lost` unplugs the ESP32 board for 3 s, `noaccess`
  refuses it; `identify` has the simulated boards of `src/core/sim.cpp` with their DTR / RTS: an ESP32-S3 on a CH340, a
  u-blox GPS on a PL2303 cable, a MicroPython Pico, an Arduino Uno, and an RP2040 in BOOTSEL mode). It also works on
  the deck, to try the app without a board. Contact sheets:
  `~/cdk-build/serial-console/host/out/sheets`.
* `tests/BOARD-TESTS.md`: the physical tests on the deck and the Pi 3A+.

Sources: `src/main.cpp` (the device runner), `src/ui/` (the pages: `serial_app.*`, `identify_pages.cpp`,
`terminal_view.*`, `app_spec.cpp`), `src/core/` (no LVGL: `devices`, `ports`, `fake_sys`, `settings`, `term`, `baud`,
`keymap`, `serial_io`, `identify` (the classifier), `probe` (the probes), `linectl` (DTR / RTS and the reset
sequences), `bootproto` (STK500, SLIP, esptool), `sim` (the simulated boards), `demo`, `console_core`, `fsutil`).

## Credits and licence

The identification speaks or recognises these protocols and formats, written from their public documentation (no
code copied): the Espressif ROM serial protocol as documented for esptool (Espressif Systems; esptool is GPL-2.0, only
its documented packet format, chip-detect values and eFuse addresses are used), STK500 v1 and v2 (Atmel application
notes AVR061 / AVR068), SLIP (RFC 1055), NMEA 0183, u-blox UBX, RTCM 3 (CRC-24Q), MAVLink (HEARTBEAT, CRC-16/MCRF4XX),
Klipper's MCU framing, Meshtastic's serial API framing, and the boot / log formats of the firmwares listed above.

<!-- branding-allow-begin: credit of the cp0_lvgl runtime linked into the binary (MIT) -->
MIT. The binary contains the launcher's `cp0_lvgl` runtime (M5Stack Technology CO LTD, MIT) and LVGL (LVGL Kft, MIT);
see `root/usr/share/doc/serial-console/copyright`. The fonts (Montserrat, JetBrains Mono) are the launcher's, read at
run time; none is shipped.
<!-- branding-allow-end -->
