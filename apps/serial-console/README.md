# Serial Console

Version 0.1.0, package `serial-console`. Part of [cyberdeck-zero-apps](../../README.md).

Serial Console talks to a USB-serial adapter or a microcontroller board (Arduino, ESP32, a router console...) from
the deck. A short numbered menu picks the port and the settings, then [picocom](https://github.com/npat-efault/picocom)
opens the port in the launcher's terminal.

## Features

* Lists the USB serial ports (`ttyUSB*`, `ttyACM*`) with the name of the adapter or board from
  `/dev/serial/by-id`; the on-board UARTs (`ttyS*`, `ttyAMA*`) on request.
* Baud rates 1200 to 921600 (115200 by default), or any rate typed by hand.
* Frames 8N1, 7E1, 8E1, 8O1, 7O1, 8N2; local echo; the line ending sent by Enter (CR, LF or CR LF).
  Received LF line endings are shown as new lines.
* An optional log of the session in `~/serial-logs/<port>-<date>-<time>.log`.
* Remembers the last port and settings in `~/.config/cardputerzero/serial-console.conf`.
* Text mode: works on every screen (the 480x320 Pi 3A+ included), with a keyboard or with the on-screen keyboard.

## Install

On the deck: **Settings > Apps > Sources > + Add a GitHub source**, enter `OSRdesign/cyberdeck-zero-apps`, go to the
**Apps** tab, pick **Serial Console** and press Enter. Installing asks for the deck user's sudo password and also
installs `picocom`. See the [repository README](../../README.md) for details.

## Use

Type an answer and press Enter:

| Answer | Action |
| --- | --- |
| a number | Open that port |
| Enter alone | Open the port marked `*` (the last one used, or the only one) |
| `b` | Next baud rate; `b74880` sets any rate |
| `f` | Next frame (8N1, 7E1, ...) |
| `e` | Local echo on or off |
| `n` | Line ending sent by Enter: CR, LF, CRLF |
| `l` | Log file on or off |
| `a` | Show the on-board UARTs too, or hide them |
| `r` | Refresh the list (after plugging a board) |
| `q` | Quit |

In picocom, **Ctrl-A then Ctrl-X** quits (on the on-screen keyboard Ctrl is a one-shot key: Ctrl, A, Ctrl, X).
Ctrl-A then Ctrl-H shows picocom's other commands.

## Requirements

* `picocom` (installed with the package).
* The deck user must be in the `dialout` group: `sudo usermod -aG dialout $USER`, then reboot. The menu says so when
  a port cannot be opened.

## Known limits

* With the on-screen keyboard up the terminal has only about 5 lines: the port list scrolls when there are many ports.
* The on-screen keyboard has no F-keys, Home/End or Page keys.
* After an upgrade through the Apps menu the tile moves to the end of the launcher grid.

## Build from source

Nothing to compile: the app is the shell script
`root/usr/share/APPLaunch/bin/serial-console` (named after the app id). Host tests (WSL or any Linux):

```
sh apps/serial-console/tests/run_tests.sh
```

The end-to-end part uses `socat` and `picocom` when they are installed (`SOCAT=`, `PICOCOM=` and `SOCAT_LIBS=` point to
unpacked binaries); it is skipped otherwise.

## Credits and licence

MIT. Uses picocom (Nick Patavalis and contributors, GPL-2.0-or-later), installed from Debian as a dependency and not
included in the package.
