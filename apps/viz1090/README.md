# viz1090

Version 0.2.0, package `viz1090`. Part of [cyberdeck-zero-apps](../../README.md).

The real [viz1090](https://github.com/nmatsuda/viz1090) ADS-B display (by Nathan Matsuda) on the deck, full screen,
with its own RTL-SDR decoder, an intro screen (type the nearest airport code, or use a GPS) and a world map.
Since 0.2.0 it fills any screen from 640x480 up (the deck, the 800x480 panels, 720x720, 1024x600, 1280x720).

## Features

* Live aircraft from an RTL-SDR dongle on a world map, in viz1090's own display.
* Its own decoder: readsb built with RTL-SDR support. It runs only while viz1090 is on screen.
* An intro screen that shows whether the SDR dongle and a USB GPS receiver are found, takes your position (the
  nearest airport code typed on the keyboard, the GPS position by tapping its panel, or latitude and longitude on
  a touch keypad), saves it and starts the display.
* The launcher's top bar (clock, Wi-Fi, Bluetooth) and a close button (X, top right) on the map.
* Your position and options are kept in `~/.config/cardputerzero/viz1090.conf`
  (`LAT`, `LON`, `UISCALE`, `METRIC`, `RADIUS`).

## Screens

The screen comes from the launcher's app contract (`APPLAUNCH_CANVAS`, `APPLAUNCH_SCREEN_*`), else the launcher's
`screen.state`, else the framebuffer itself (640x480 when nothing answers). The intro screen, the map
(`viz1090 --screensize`), the top bar and the close button all use that size; the picture is turned and converted for
rotated panels and 16 bpp framebuffers as the launcher does (`APPLAUNCH_SCREEN_ROTATE`, `APPLAUNCH_TOUCH_ORIENT=buffer`).

| Screen | Intro screen | Map text (`UISCALE`) |
| --- | --- | --- |
| 640x480 (the deck) | as in 0.1.x, pixel for pixel | 2 |
| 800x480 (Pi 3A+ DSI, HyperPixel) | wider left column, same keypad | 2 |
| 720x720 (HackBerry) | taller rows and keys, text x1.125 | 2 |
| 1024x600 | everything x1.25, wider left column | 1 on a 7" panel (6.6 px/mm), 2 at the default 11.3 px/mm |
| 1280x720 | everything x1.5, wider left column | 2 (1 below 7.5 px/mm) |
| smaller (480x320) | a "needs a 640x480 screen" notice with Back | - |

`UISCALE` (viz1090's text size, 12 px x uiscale) follows the screen's density: 1 below 7.5 px/mm, 2 up to 14 px/mm,
3 above, and never more than one step per 240 screen rows. A `UISCALE=` line in `viz1090.conf` still wins. The map
window cut around your position (`RADIUS`, 500 nautical miles) is the same on every screen: viz1090 maps its zoom to
half the longer side, so the window covers the whole screen, square or wide, at any zoom up to that distance.

## Install

On the deck: **Settings > Apps > Sources > + Add a GitHub source**, enter `OSRdesign/cyberdeck-zero-apps`, go to the
**Apps** tab, pick **viz1090** and press Enter. Installing asks for the deck user's sudo password; the package depends on
the libraries it needs (SDL2, librtlsdr, libusb and others, listed in `app.json`). See the [repository README](../../README.md) for details.
If an older version is installed, the deck offers 0.2.0 as an update in the same menu.

## Use

Start **viz1090** from the launcher. The intro screen comes first.

* Intro screen: type the airport code (or tap the GPS panel when it has a fix, or fill latitude and longitude on
  the keypad), then **Start**. Keys: Enter starts, Tab / Down / Up change the field, Backspace deletes, Esc (or
  **Back**) cancels and returns to the launcher. Without a keyboard (the touch-only Pi 3A+) the position is entered
  on the keypad or taken from the GPS; the airport code needs a keyboard (there is no on-screen letter keyboard).
* Map: touch pans (one finger) and zooms (two fingers), tried with a finger on the deck; the close button (X) or Esc returns to the launcher; `=`
  zooms in with the keyboard.

## Screenshots

The intro screen, with the SDR receiver detected and the keypad for the position:

![viz1090, intro screen](../../docs/screenshots/viz1090-intro.png)

The map with aircraft, the status line and the close button:

![viz1090, map](../../docs/screenshots/viz1090-map.png)

## Requirements

* A screen of at least **640x480** (see [Screens](#screens)).
* An **RTL-SDR dongle** with a 1090 MHz antenna.
* A launcher with **full-screen app support** (`X-Fullscreen=true` in the `.desktop` file): the launcher stops
  drawing and gives the whole panel to the app.
* `python3` and the libraries listed in `app.json` (installed with the package).
* Optional: a USB GPS receiver (serial NMEA).

## Known limits

* After an upgrade through the Apps menu the tile moves to the end of the launcher grid.
* The intro screen's touch areas (each control with its share of the gaps and its label) do not all reach the 9 mm of
  the guidelines: on the deck (640x480, 11.3 px/mm assumed) the keys are 8.5 x 7.1 mm and the GPS panel 5.8 mm high;
  on the Pi 3A+ panel (800x480, 8.5 px/mm) everything is 9.3 mm or more except the GPS panel (7.6 mm); on 720x720
  (10 px/mm) everything is 9.7 mm or more. At 1024x600 and 1280x720 it depends on the panel (at the launcher's
  default 11.3 px/mm the smallest, the GPS panel, is 7.2 and 8.6 mm).
* The keyboard is read from `/dev/input/bt-keyboard` (or `VIZ_KEYBOARD`) only: a keyboard without that name (a USB
  keyboard, the HackBerry's built-in one) does not reach the intro screen or the map; touch, the Back button and the
  close button still work.
* The tile is not hidden on a screen smaller than 640x480 (no `X-MinSize` yet): the intro screen shows a notice there.

## Changes

* **0.2.0**: runs on every screen from 640x480 up instead of only 640x480. The screen size, rotation, depth and
  density come from the launcher's app contract (or `screen.state`, or the framebuffer); the intro screen lays itself
  out from the window size (unchanged at 640x480) with touch areas that include the gaps and labels around each
  control; viz1090 gets `--screensize` and a `--uiscale` chosen from the density (a `UISCALE=` line in the config
  still wins); the top bar and close button follow the window; rotated panels (90/180/270) and 16 bpp
  framebuffers are drawn through the launcher's own output stage (`cp0_fb_output.c`), and buffer-oriented touch
  (`APPLAUNCH_TOUCH_ORIENT=buffer`) is mapped through the rotation. The touch device named by the launcher
  (`APPLAUNCH_SCREEN_TOUCH`) is tried before the scan. Below 640x480 the intro screen shows a "needs a 640x480 screen"
  notice. Checked headless at 640x480, 800x480 (16 bpp, and rotated 90 like the HyperPixel), 720x720, 1024x600,
  1280x720 and 480x320 on a Pi; not yet on a board with an SDR.
* **0.1.1**: touch works again when the Bluetooth keyboard is connected. 0.1.0 opened a fixed
  `/dev/input/event1`, which on the deck is sometimes the infrared node of the RTL-SDR dongle and not the screen.
  The display layer now looks through `/dev/input/event*` for the touch screen (a device that reports multitouch
  positions and is not a keyboard, the Goodix panel first) and tries again every second if it is absent or goes away.
  `VIZ_TOUCH=/dev/input/eventN` still forces a device. On the deck the right device was opened at every launch,
  the keyboard, Esc and the close button kept working, and touch recovered by itself after an interruption.

## Build from source

The package is built from upstream sources plus the deck-specific files of [`build`](build). On a Raspberry Pi
(64-bit Raspberry Pi OS, Debian 13):

```
sh apps/viz1090/build/build_on_pi.sh
```

It builds viz1090 and readsb from their upstream repositories at the tested commits, builds the display bridge
(`libviz_fb.so`) and the intro screen (`vizsetup`), and stages everything in `root/`. `--bridge-only` rebuilds only
the bridge, the intro screen and the scripts and leaves the viz1090 and readsb binaries in `root/` as they are. Then, at the root of this
repository, run `python3 tools/make_registry.py`. `build/make_world.py` builds the world map from open data (it
needs a PC). The files of `build/` and this page are not part of the `.deb`; only `root/` is. The files in `build/`
are described in [build/README.md](build/README.md), which also explains how to run another SDL2 program full
screen the same way.

## Credits and licence

The package bundles several programs and data sets, each under its own licence (the full notices are in the
package's `copyright` file, [`root/usr/share/doc/viz1090/copyright`](root/usr/share/doc/viz1090/copyright)):

* **viz1090**: BSD licence. Source <https://github.com/nmatsuda/viz1090>, commit
  `fb7bf016d5d95ddca5b27f260e871484864b5c96`; built with one change (three constants in `AircraftLabel.h`, see
  `build/build_on_pi.sh`). Copyright Nathan Matsuda, Malcolm Robb and Salvatore Sanfilippo.
* **readsb** (the decoder): GNU General Public License 3 or later. Source <https://github.com/wiedehopf/readsb>,
  commit `094720939c01943de82b14df6f42f67fff1cd514`, built unmodified with `make RTLSDR=yes`. To get the source
  from the packager as well, open an issue at <https://github.com/OSRdesign/cyberdeck-zero-apps>.
* **Display bridge and intro screen** (`libviz_fb.so`, `vizsetup`, `run_viz1090.sh`, `extract_map.py`): MIT, sources
  in [`build`](build). `cp0_statusbar.[ch]` and `cp0_fb_output.[ch]` come from
  [cyberdeck-zero-launcher](https://github.com/OSRdesign/cyberdeck-zero-launcher) (MIT).
* **Fonts**: Terminus TTF, Montserrat and the Font Awesome 5 Free font files are under the SIL Open Font License 1.1;
  Font Awesome Free icons are under CC BY 4.0.
* **Map data**: Natural Earth and OurAirports (public domain), converted by `build/make_world.py`.
