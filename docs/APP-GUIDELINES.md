# App guidelines for CyberdeckZero Launcher

How to write an app that installs from **Settings > Apps** and runs on every board of the project, on the Pi Zero 2 W
deck first. Read it before you start, tick the [checklist](#13-final-checklist) before you publish. It links the
reference documents instead of copying them; when a number here and the launcher disagree, the launcher wins (read the
tokens at run time, never copy them).

The app contract (environment variables, state files, `.desktop` keys) is specified in the launcher repository:
[APP-CONTRACT.md](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/APP-CONTRACT.md). Hosting and
the Settings > Apps flow: [HOSTING-APPS.md](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/HOSTING-APPS.md).
The starting point of a native app: [`apps/cyberdeck-app-template`](../apps/cyberdeck-app-template/README.md) on the shared code in
[`apps/_common`](../apps/_common/README.md). Repository overview: [README](../README.md).

## 0. The boards and the rule

| Board | Screen (logical) | Input | Notes |
|---|---|---|---|
| Pi Zero 2 W deck (the reference) | 640x480, 512 MB RAM, 4 cores | Bluetooth keyboard + touch | tokens below are the deck's |
| Pi 3A+ + Waveshare 4.3" DSI | 800x480 | touch only (USB keyboard optional) | the touch-only reference. Its earlier 480x320 SPI panel is best effort only |
| HackBerry CM5 | 720x720 square, desktop session | built-in keyboard + touch | the on-screen keyboard rule is forced |
| Pi 5 + HyperPixel 4" | 800x480, rotated | touch (+ keyboard) | rotation and 16/32 bpp handled by the launcher's display backend |

**Owner decision (2026-10-13): new work targets 640x480 as the minimum screen.** 480x320 is best effort. A new native
app declares `X-MinSize=640x480` and is hidden on a smaller screen. If fitting a smaller screen would cost heavy rework,
we do not do it ([section 12](#12-when-not-to-build-it)). Landscape only: the launcher rotates portrait panels for you.

## 1. Which kind of app

| Kind | What it is | Fits when | Example |
|---|---|---|---|
| **A. Native `cp0_lvgl` app** (the default) | C++ app from the template: fullscreen, the launcher's widgets, status strip and on-screen keyboard in its own process | any app with its own screens, forms, lists, live data; touch and keyboard | the [template](../apps/cyberdeck-app-template/README.md) |
| **B. Wrapped terminal / shell app** | a program run by the launcher's terminal page (`Terminal=true` in the `.desktop`) | a text tool that already exists (menu script, `picocom`, a TUI); it must also run on small screens | a serial console menu |
| **C. Wrapped upstream program** | an existing program (SDL, framebuffer) started as `X-Fullscreen=true` with a small shim for display and input, or drawn into the launcher's 320x170 virtual framebuffer | you carry someone else's program and do not rewrite its UI | [viz1090](../apps/viz1090/README.md) (fullscreen shim); [LAN Scan](../apps/lanscan/README.md) and [Wi-Fi Survey](../apps/wifi-survey/README.md) (320x170 compat) |

Choose by this order: **A** unless the program already exists (then **C**) or is plain text (then **B**).

- **A** gives you tokens, widgets, the shared status strip, an on-screen keyboard rule and a headless test harness.
  It costs about 13-15 MB of RAM per app (launcher report 024).
- **B** is free of layout work (the text grid follows the screen) and the on-screen keyboard comes from the launcher, but there is
  no status bar, touch only scrolls, and the terminal has no F-keys, Home/End or Page keys on the on-screen keyboard. A terminal app gets
  no per-launch contract variables. It may skip `X-MinSize` (so it also runs on 480x320).
- **C** draws a fixed picture: a **compat** app (the default without `X-Fullscreen`) always draws 320x170, scaled by the
  launcher, with a toolbar, and **cannot get an on-screen keyboard** in contract 1. Do not start a new interactive app this way.
  A fullscreen wrapper must take the framebuffer, rotation, depth and touch device from the contract
  (`APPLAUNCH_SCREEN_FB`, `APPLAUNCH_SCREEN_ROTATE`, `APPLAUNCH_SCREEN_TOUCH`, `APPLAUNCH_TOUCH_*`), never `/dev/fb0` and `event0` by number.
  Find a touch device by its capabilities (multitouch X/Y, no keyboard keys), retry every second ([decisions](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/dev/decisions.md), 2026-10-04).
- The virtual framebuffer (VFB2) is internal between the launcher and its shim; an app only uses ordinary fbdev calls.

## 2. Screens, sizes, input

### Sizes: tokens, never pixels

The launcher publishes its design tokens for the current panel (`screen.state` and `cp0_ui_metrics_get()`): spacing
s/m/l, radius s/m/l, `header`, `row` (7 mm), `target` (9 mm), `dialog_max_w`, text sizes title/heading/body/caption.
The deck (640x480, 11.3 px/mm) values, for orientation only:

| Token | Deck 640x480 | Meaning |
|---|---|---|
| `space` s/m/l | 8 / 16 / 20 | gaps and margins |
| `header` | 56 | the page header (title, Esc, status strip) |
| `row` | 79 | a list or form row (at least 7 mm) |
| `target` | 102 | a button or touch target (at least 9 mm) |
| `text` title/heading/body/caption | 32 / 28 / 24 / 18 | font sizes |

Rules:

| Do | Don't |
|---|---|
| lay out from `cp0_ui_metrics_get()`, `cp0_ui_roller_page()` and the `cp0::ui` widgets | write `lv_obj_set_size(x, 120, 48)` or any pixel constant |
| size things in millimetres (`ppmm`) when you must draw your own | assume 11.3 px/mm; the HyperPixel is 9.2, the HackBerry 10.0 |
| read the canvas from `APPLAUNCH_CANVAS`, or `screen.state`, or the framebuffer | hard-code 640x480, 320x170 or `/dev/fb0` |
| support all of 640x480 (standard), 800x480 (wide), 720x720 (square), 1280x720 (large), plus 1024x600 | design only for the deck |
| keep every interactive element at least `row` high (buttons `target`) | squeeze controls to fit; scroll instead |
| show "needs 640x480" and a way out when the screen is too small (an older launcher ignores `X-MinSize`) | draw off the edge |

The template does this for you: `AppSpec` carries the minimum, `_common/app.cpp` shows the notice.

### Header, title, status strip

Every page uses `cp0::ui::Page` with `cp0::ui::add_title()` and `cp0::ui::StatusStrip::add(page->header())`. Never draw
your own clock, Wi-Fi or Bluetooth icons and never keep a copy of the strip or the widgets: they come from the pinned
launcher component ([`LAUNCHER_REF`](../apps/_common/LAUNCHER_REF)). The header has the on-screen **Esc** button; keep it.
Colours and fonts come from the widgets (black ground, Montserrat from `/usr/share/APPLaunch/share/font`, read at run
time; do not bundle fonts that the launcher has).

### Lists: Roller or CompactList

Two list widgets, same keys and touch rule (a tap selects a row, a tap on the selected row acts); API and use in the
launcher's [`docs/cp0_lvgl.en.md`](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/cp0_lvgl.en.md),
"Lists" (decision 2026-10-14).

| Use | For |
|---|---|
| `cp0::ui::Roller` | few big items you pick from: a choice list with a check on the current value, as in Settings |
| `cp0::ui::CompactList` | data and app lists (System Monitor, Serial Console, Net Toolbox): top-aligned rows, scrolling only when the highlight reaches the bottom edge, a thin scrollbar that hides itself, section headings in grey capitals that are never selected |

- `ListDensity::Compact` (5.0 mm rows) for dense data, where a tap only moves the highlight; `Comfortable` (7 mm) for
  any list whose rows trigger an action.
- Live data: `set_rows()` changes only the texts that changed and keeps the scroll and the highlight; `reset_rows()`
  for new content.
- **Status is never shown by colour alone.** Green, amber and red (`ValueTone` Ok / Warn / Bad) always come with a
  word or a shape (*Power OK*, *Under-voltage*; a check, a cross, a dash).

### Touch and keyboard: both complete

A touch-only board (Pi 3A+) and a keyboard-only use must each reach **every** function.

| Rule | Detail |
|---|---|
| Keyboard first | Up/Down move, Enter or Right opens, Esc or Left goes back, Tab between fields |
| Touch is complete | tap = move highlight, tap on the highlighted row = open, drag = scroll; no gesture-only action, no hover, no long-press-only action |
| Esc / back everywhere | at a sub-page it goes back; at the root a **short Esc quits the app** (owner decision 2026-10-13; touch-only users need it), by key or by the header's Esc button |
| Hold Esc 3 s | the launcher's watchdog kills a stuck app (SIGTERM, then SIGKILL after a grace). Handle SIGTERM/SIGINT/SIGHUP and exit cleanly; `cdk::run()` does |
| Letter shortcuts | compare the physical key code, not ASCII (W, E, R, T share evdev codes with the arrows); never make a letter the only way to a function |
| Dialogs | Cancel = Esc, primary = Enter, both also buttons |

### On-screen keyboard

The launcher never draws over a fullscreen app, so a native app with text fields draws its own, **only by the shared rule**:
`cp0_app_osk_allowed()` on `input.state` (the template wires it: [`_common/osk_policy`](../apps/_common/README.md)).

| Do | Don't |
|---|---|
| show it only when no physical keyboard is connected (or `vkb=force`, as on the HackBerry); hide it within about 1 s when one connects | add a button, key or gesture that summons it by hand |
| treat a Bluetooth keyboard that went away as asleep (the rule does, 600 s) | show it because "the board is touch only" by your own test |
| keep the field you edit visible above it: at 640x480 only about 180 px remain, so a text page is the header plus one field | put two fields and a long help text on a typing page |
| use `cp0::ui::TextField` and `Dialog` (they follow the keyboard) | build a second keyboard widget |

Never make typing the **only** way in: offer pick lists or presets where you can.

## 3. The launcher contract

Full text: [APP-CONTRACT.md](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/APP-CONTRACT.md). What an app uses:

| Item | Use |
|---|---|
| `APPLAUNCH_CONTRACT=1`, `APPLAUNCH_APP_MODE` (`compat` / `fullscreen`) | set per launch on the app's command line; absent = started without a contract launcher |
| `APPLAUNCH_WINDOW`, `APPLAUNCH_CANVAS`, `APPLAUNCH_CANVAS_SCALE` | where and how many pixels you draw (fullscreen: the whole logical panel, scale 1) |
| `APPLAUNCH_SCREEN_W/H/ROTATE/PPMM/CLASS/FB/TOUCH` | the panel, process-wide; density of your canvas = `PPMM / CANVAS_SCALE` |
| `APPLAUNCH_STATE_DIR` | directory of `screen.state` (tokens, `gen`) and `input.state` (keyboards, `vkb=auto/off/force`); written **only** by the launcher, re-read when `gen` changes (at most once a second) |
| `.desktop` `X-Fullscreen=true` | the app owns the panel until it exits |
| `.desktop` `X-MinSize=WxH` | the tile is hidden (also in Settings > Apps) on a smaller screen; a malformed value means no restriction |

- An app **never writes** `screen.state` or `input.state`. The launcher rewrites `screen.state` after every app exits.
- Ignore unknown keys and variables; names and units never change within contract 1.
- The C reader (`cp0_app_contract.h`, no LVGL) is copy-ready; a native app already uses it through `cp0_lvgl`.

### Started without the launcher (ssh, a script, an older launcher)

Rule: **degrade to your own probe, never crash**. `cp0_app_env_read()` returns -1: read `screen.state` if present, else
the framebuffer size and 11.3 px/mm. Log one line saying so (the template: `no launcher contract (started by hand ...)`),
keep keyboard working, touch axes may be transposed (they come from the service's environment). A headless tool or
daemon part of your app must also run with no display (`--version`, `--help`, a `--headless` or `--report` mode) so it can be unit-tested.

## 4. Naming, branding, credits, licences

| Rule | Detail |
|---|---|
| App id | lowercase letters, digits and `-`; the same in `share_code`, `package`, the folder `apps/<id>`, the `.desktop` file `<id>.desktop`, the icon `<id>.png` |
| Binary | `/usr/share/APPLaunch/bin/<id>`, **named after the app id**. Do not prefix it with the old stock-device prefix ([report 025](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/dev/reports/025-rebranding-plan.md), decision 7): it collided with process names. Note `comm` is cut to 15 characters: `pgrep -x` the short name, never `pkill -f` |
| Product name | write **CyberdeckZero Launcher** (short: CyberdeckZero), in one word |
| Affiliation | never imply that the project, the launcher or your app is made, endorsed or sponsored by the maker of the stock device; no logos or names of it in your tile, icon, README or package description |
| Credit upstream | name every program, library, dataset and protocol you use, its author and licence, in the README and in the package copyright file |
| Our code | MIT, copyright `2026 <your name>`; add no copyright line for anyone who did not write the file |
| Third-party licences | ship each licence text under `/usr/share/doc/<app>/` (`copyright` in Debian format; one `LICENSE.<component>` file per bundled component); GPL parts: give the source URL in the description and the README |
| Technical names that stay | `APPLaunch`, `APPLAUNCH_*`, `applaunch` state paths, `cp0_*`, `~/.config/cardputerzero` (launcher's own): [NAMING.md](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/NAMING.md). Your app has no reason to use them except to read the contract |

<!-- branding-allow-begin: names the old stock-device prefix as the thing not to use -->
Example: the binary is `serial-console`, not the stock prefix plus `-serial-console`. Credits that must name the
upstream (a launcher derived from the stock device's MIT-licensed code, bundled upstream Store) use the approved
phrases ("CardputerZero Store by M5Stack", "CardputerZero launcher by M5Stack") and sit inside a
`branding-allow-begin` / `branding-allow-end` block of a Markdown comment, as the template's README does.
<!-- branding-allow-end -->

Check it: `sh tools/branding/check-branding.sh` (see [`tools/branding`](../tools/branding/check-branding.sh), allow-list
[`branding-allow.txt`](../tools/branding/branding-allow.txt)). New files must pass without new allow-list entries.

## 5. Packaging and registry

Layout of `apps/<id>/` (the template shows every file):

```
app.json                         what the registry shows (below)
icon.png                         100x100 px, square, transparent (see ICON-PALETTE.md)
build.sh                         calls ../_common/build_app.sh (native apps)
src/ tests/                      sources; unit tests and headless scenes
screenshots/*.png                optional; listed in registry.json
DEBIAN/{preinst,postinst,prerm,postrm}   optional maintainer scripts, passed to the .deb unchanged
root/usr/share/APPLaunch/bin/<id>                    executable (anything under bin/ is mode 755)
root/usr/share/APPLaunch/applications/<id>.desktop   the launcher entry
root/usr/share/APPLaunch/share/images/<id>.png       the tile icon
root/usr/share/APPLaunch/share/<id>/                 your data files (not another app's folder)
root/usr/share/doc/<id>/copyright                    licences (section 4)
```

A native app's `.desktop` (`Exec` is an absolute path, no shell tricks; `Icon` is relative to `/usr/share/APPLaunch/`):

```ini
[Desktop Entry]
Name=My App
Exec=/usr/share/APPLaunch/bin/my-app
Terminal=false
Icon=share/images/my-app.png
Type=Application
X-Fullscreen=true
X-MinSize=640x480
```

`app.json` (checked by `tools/make_registry.py`; `share_code`, `package`, `version`, `title`, `summary` are required):

| Field | Rule |
|---|---|
| `share_code` | lowercase id, stable forever (it gives the registry uuid) |
| `package` | Debian package name, = the app id |
| `version` | **raise it for every release**: the deck compares Debian-style (`dpkg --compare-versions`); the same version is not an update |
| `title`, `summary`, `description` | what Settings > Apps shows; the description states needs (hardware, groups, network, minimum screen) |
| `license` | SPDX expression of everything shipped (`MIT`, or `BSD-3-Clause AND GPL-3.0-or-later`) |
| `depends` | Debian packages `apt` must add: **only what the binary links dynamically** (native apps: `libfreetype6, libinput10, libxkbcommon0`), plus tools you run (`curl`, `picocom`). Check the names exist on Debian 13 arm64 |
| `permissions` | `network`, `additional_hardware`, `camera`, `microphone`, `imu`, `background_service`, `external_display` (honest: they are shown to the user) |
| `draft` | `true` keeps the app out of the registry (`make_registry.py` skips it); build a local `.deb` with `--only <id> --out <dir outside packages/>` |
| `categories`, `author`, `source_repo` | shown in the catalogue |

- **libc6 floor.** `tools/build_deb.py` adds `libc6 (>= 2.38)` to every package (the binaries are built for Debian 13); do not
  repeat it in `depends`, and never lower it. Build on the pinned toolchain (`build_app.sh`), not on an older system.
- **arm64 only.** A binary for another CPU installs and does not run.
- **Version bumps.** Change `version`, rebuild (`python3 tools/make_registry.py`), commit the new `.deb` and `registry.json`. After an upgrade the
  tile keeps its place on the grid only if the `.desktop` file name is unchanged: **never rename it**.
- **Publish the package you tested.** Put the exact `.deb` that passed the board tests in `packages/`, remove `draft`,
  and write the registry with `python3 tools/make_registry.py --no-build` (a fresh build is not byte-identical). Check
  that only that app's entry (and `generated_at`) changed; every other entry, and `device_targets`, stay as on `main`.
- **Installing.** Settings > Apps asks for the deck user's **sudo password** (the `.deb` is installed as root; there are no root-free installs) for install,
  update and removal, up to three tries. Do not write a postinst that needs interaction, network or more than a few seconds; a failed
  maintainer script shows as `dpkg error`. The package must be removable cleanly: `prerm` stops your own processes, `postrm` leaves the user's data in `~`.
- **No root at run time.** The app runs as the deck user. If it needs a group (`dialout`, `video`, `bluetooth`), document it (section 6).
- **Large packages.** Above 50 MB attach the `.deb` to a GitHub Release (README of this repository, "Publish").
- **Source of truth.** Never hand-edit `registry.json`; regenerate it.

## 6. Behaviour

| Rule | Why and how |
|---|---|
| **No blocking UI.** Nothing slow on the LVGL thread: no `system()`, no blocking `popen()`, no sleeping, no network call | use [`_common/async_run`](../apps/_common/README.md) (fork + exec in its own process group, lines delivered on the LVGL thread in order, timeout, cancel, `PDEATHSIG`, `shutdown()` at exit) or a worker thread that posts results with `lv_async_call` |
| **Every background job has a cancel and a timeout** | a Busy row with Esc to cancel; a default timeout on every command; kill the whole process group; leave no child behind on SIGTERM (board test T2.5) |
| **Write only where you say** | `~/.config/<id>/` for settings, `~/.local/share/<id>/` or a folder you document (for example `~/Notes`) for data; never in `/usr`, `/etc`, `/tmp` without a unique name, or another app's folder. Write atomically (temporary file, then rename) |
| **Permission failures explain the fix** | "Cannot open /dev/ttyUSB0: your user is not in the dialout group. Run: `sudo usermod -aG dialout $USER`, then log out and in." Never just "error 13" |
| **Hardware absent is a message, not a crash** | no radio board, no RTL-SDR, no Bluetooth: say what is missing, how to plug it, and keep the app usable (or exit cleanly with the reason) |
| **Offline is normal** | a failed fetch shows a short reason (`No network`, `Timed out`) and the cached data, as Settings > Apps does |
| **Resource limits** | Pi Zero 2 W: 512 MB RAM (about 414 MB usable) shared with the launcher (about 20 MB) and the OS; native app **VmRSS under 25 MB**, idle CPU at most a few percent, cap file and list sizes (a note file, a log buffer, a scan result list), no Python for long-running parts, poll at 1 Hz at most |
| **Battery and power** | no busy loops; sleep in the event loop; scans and radios (Wi-Fi, Bluetooth, LoRa, SDR) only while a page needs them, switched off on leave and on exit; a combined Wi-Fi/Bluetooth chip may disturb the Bluetooth keyboard during a scan: short scans with a warning |
| **Clean exit** | on SIGTERM, Esc at the root or the watchdog: stop jobs, close devices, flush files, restore what you changed (the framebuffer is the launcher's to repaint) |
| **Logging** | `stderr`, one line per event with a `[<id>]` tag (`[cdk] ...` in the template); the launcher puts it in its journal (`journalctl --user -u <launcher service>`). Log the contract mode and the canvas at start, every failure with its reason, never a password, key or message body. No log files that grow without bound |
| **Time and locale** | no RTC on the boards: do not trust the clock at boot; use monotonic time for timeouts; parse command output with `LC_ALL=C` |

## 7. Testing

Three layers, all before the owner's board test. Controller-led testing: the Controller checks code, builds and
deploys; the **owner runs the physical steps** from a written protocol.

| Layer | What | Where |
|---|---|---|
| **1. Unit tests** | the app's logic without LVGL (`core/`): navigation, parsers (fixtures of `/proc`, `/sys` and command output), rules, state machines; plus the package files: the `.desktop` has `X-Fullscreen`, `X-MinSize` equal to `AppSpec`, `Exec` and `Icon` resolve to files of `root/`, `app.json` is complete | `tests/test_core.cpp` in the template; `build.sh host` runs it |
| **2. Headless scenes** | the real UI on a plain LVGL display at 640x480, 800x480, 720x720, 1280x720 and 1024x600 (480x320 for the "needs 640x480" notice): scripted keys and taps, PNG shots, layout checks (nothing off-screen, rows and targets at least 7 and 9 mm, nothing under the header or the on-screen keyboard), one contact sheet per scene | `tests/scenes/*.scene`; [`_common/headless/README.md`](../apps/_common/headless/README.md) |
| **3. Board protocol** | a written `tests/BOARD-TESTS.md` in the style of the template's: numbered steps with the exact command, the expected picture or log line, the cleanup | [`BOARD-TESTS.md`](../apps/cyberdeck-app-template/tests/BOARD-TESTS.md) |

Scenes every app has: each page; the form with the on-screen keyboard (`keyboards 0`, then `keyboards 1`, then `osk force`); an error line; a dialog;
a busy row; exit by Esc; the notice under 640x480; a start with no contract (`nocontract`). A layout change that fails a
check is fixed in the layout, never by loosening the check.

**What the owner tests physically** (a protocol, T-numbers as in the template's):

| Test | On |
|---|---|
| Opens fullscreen, looks like the launcher, status strip identical to the home grid's | deck |
| Esc from every source (BT keyboard, USB keyboard, header button by touch); hold Esc 3 s ends it; home grid intact | deck |
| On-screen keyboard appears with no keyboard attached, disappears within about 1 s when one connects, typing works | deck (touch only) |
| Layout and touch accuracy at 720x720 with the keyboard rule forced (`APPLAUNCH_OSK=force`) | HackBerry |
| Tile hidden, and the notice when started by hand | a 480x320 screen (the Pi 3A+ with its SPI panel, or the render harness) |
| VmRSS under 25 MB, idle CPU low, `free -m` | deck |
| Started over ssh without the launcher: degrades cleanly | deck |
| Your own hardware and permission cases (device absent, group missing, no network) | the board that has it |

Do not run commands that kill by pattern (`pkill -f`); the board tests name processes exactly. Remove test installs
afterwards (`dpkg -r <id>`). A change to a board that is not yours (launcher, Store, other apps) is out of scope for your tests.

## 8. README of each app

`apps/<id>/README.md`, in this order (copy from [Mesh Hop](../apps/mesh-hop/README.md) or [LAN Scan](../apps/lanscan/README.md)):

| Section | Content |
|---|---|
| Title and one-paragraph description | what it does, for whom, the version |
| Screenshots | taken on a board or in the headless harness at **640x480** (PNG, `docs/screenshots/<id>-<page>.png`, linked relatively; put the ones the Store should show in `apps/<id>/screenshots/*.png`, which `make_registry.py` lists; the old 320x170 size is stale); at least the main page and one form, and one at 720x720 or 800x480 if the layout differs. Show the real UI, no mock-ups |
| What it does / how to use | pages and functions |
| Keys and touch | one table: every function, its key and its touch action |
| Requirements | minimum screen (`640x480`), hardware, groups, packages, network, launcher version (full-screen support) |
| Install | Settings > Apps flow; manual `dpkg -i` |
| Files the app keeps | every path it writes, and how to reset |
| Known limits | honest list (boards where it does not run, missing features) |
| Credits and licences | upstream names, authors, licences, source URLs; third parties; the independent-project statement where needed |
| Build and test | `build.sh host`, `cross`, the package command, the board protocol |
| Changes | short list per version |

Add the app to the table in the repository [README](../README.md) (name, version, one line).

## 9. Do and don't, in one list

| Do | Don't |
|---|---|
| start from the template; keep logic in `core/` with tests | copy the widgets or the status strip into your app |
| read tokens, canvas and keyboard rule at run time | hard-code sizes, `/dev/fb0`, `event0`, 11.3 px/mm |
| reach every function by touch and by keys | hide an action behind a long press, a gesture or one letter |
| let Esc go back and, at the root, quit | trap the user in a page; swallow Esc |
| show an on-screen keyboard only by the shared rule | add a "keyboard" button |
| run commands through `async_run` with timeout and cancel | block the LVGL thread |
| write one line of log per event, with the reason | log secrets or flood the journal |
| name the binary after the app id, ship licences, credit upstream | borrow the stock device's name, logo or prefix |
| raise `version` for every release | re-upload a `.deb` under the same version |

## 10. Wrapped apps: extra rules

- **B (terminal).** `Exec` is a POSIX `sh` script or a program; no `X-Fullscreen`. Remember choices in `~/.config/<id>`; keep
  menus numbered so the on-screen digits work; tell how to quit (for `picocom`: Ctrl-A then Ctrl-X, typable because Ctrl is one-shot); check the group and
  the device and explain the fix; test with a `socat` pseudo-terminal pair.
- **C (upstream program).** Keep the upstream sources unmodified where you can and say what your shim changes. Ship its licence and source offer
  (GPL parts: the source URL). Read the display and touch from the contract. Provide a close control that works by touch (the shim of viz1090 draws one) and the
  3 s hold-Esc must still end it. Bundle its fonts and data under `/opt/<id>` or `/usr/share/APPLaunch/share/<id>`, with licences.
- **A compat app** (the old 320x170 model) stays supported for the apps that exist; the contract makes it aware of the
  window, scale and density, but it cannot be made responsive. Start no new app this way.

## 11. Existing apps and these guidelines

The first apps predate the template. They are not wrong, but they do not all meet today's rules; do not copy their
gaps. Known gaps (also tracked in their READMEs): LAN Scan and Wi-Fi Survey draw 320x170 (compat) and have no on-screen
keyboard; Mesh Hop types with a physical keyboard only, and its published 0.3.0 does not declare `X-MinSize` (the
source does since apps PR #11); viz1090 reads its framebuffer through its own
shim; some still carry the old binary prefix, to be dropped at their next version (decision 7 of report 025). Fix these when you next
release the app, not as a drive-by.

## 12. When NOT to build it

Do not start (or stop, and say so) when:

- **It needs heavy rework to fit 640x480.** A dense desktop UI (many columns, tiny controls, a mouse-only editor, a
  map with hover) is not squeezed in. Redesign it into the roller/page model or leave it. The same holds for 480x320: not worth
  heavy rework; declare `X-MinSize=640x480`.
- **It is x86-only** (binary blobs, a vendor driver, a Wine app). Packages are arm64; the x86 port of the whole stack is a separate project.
- **It is GPU-heavy** (3D, video decode at high resolution, a browser, an emulator). The Zero 2 W has a software-drawn LVGL
  screen, 512 MB RAM and a thermal limit; the rule is idle CPU a few percent and VmRSS under 25 MB.
- **It needs root at run time**, or a root-free install. The launcher installs with sudo; the app runs as the deck user.
- **It only works with a keyboard, or only with touch.** Both must be complete (section 2).
- **It needs the network all the time** with no offline state, or sends data to a third party you do not name in the description.
- **It duplicates a launcher page** (Wi-Fi, Bluetooth, brightness, date): improve the launcher instead; apps live in this repository, launcher pages in the launcher's.
- **It is not licensable** under MIT, or under a licence you can ship and credit.

Ask the owner before spending effort on a borderline case.

## 13. Final checklist

Copy into the pull request and tick.

| # | Check | Done |
|---|---|---|
| 1 | Kind chosen (A native default / B terminal / C wrapped) and the reason written in the README | [ ] |
| 2 | `.desktop`: `X-Fullscreen=true`, `X-MinSize=640x480` (native), absolute `Exec`, `Icon` relative | [ ] |
| 3 | No pixel constants: layout from tokens and widgets; no hard-coded 640x480 or `/dev/fb0` | [ ] |
| 4 | Header via `Page`, title and shared `StatusStrip`; no own clock or icons | [ ] |
| 5 | Every function reachable by keys and by touch; Esc goes back, short Esc at the root quits | [ ] |
| 6 | On-screen keyboard only by `cp0_app_osk_allowed`, no manual summon, field stays visible | [ ] |
| 7 | SIGTERM, SIGINT, SIGHUP end the app cleanly; hold-Esc works; no child process left | [ ] |
| 8 | Works with no contract (ssh, older launcher): one log line, no crash | [ ] |
| 9 | Under 640x480 shows the "needs 640x480" notice | [ ] |
| 10 | No blocking call on the LVGL thread; every job has a cancel and a timeout | [ ] |
| 11 | Writes only to `~/.config/<id>`, documented dirs; atomic writes | [ ] |
| 12 | Permission, missing-hardware and offline cases give a message with the exact fix | [ ] |
| 13 | VmRSS under 25 MB, idle CPU low on the deck | [ ] |
| 14 | Logs to stderr with a tag, no secrets, bounded | [ ] |
| 15 | Binary named after the app id; no stock-device prefix or logo; "CyberdeckZero Launcher" spelled right | [ ] |
| 16 | Upstream credited, licences in `/usr/share/doc/<id>/copyright` (+ texts), GPL source URL given | [ ] |
| 17 | `app.json` complete: `version` raised, `depends` correct (no libc6 line), `permissions` honest, `license` right | [ ] |
| 18 | `python3 tools/make_registry.py` builds; `dpkg-deb -I` and `-c` look right; `registry.json` regenerated | [ ] |
| 19 | Unit tests green; headless scenes pass at the five sizes; contact sheets reviewed | [ ] |
| 20 | `tests/BOARD-TESTS.md` written; the owner ran it on the deck (and the other boards that apply) | [ ] |
| 21 | README complete (section 8) with 640x480 screenshots; repository README table updated | [ ] |
| 22 | `sh tools/branding/check-branding.sh` clean for the new files | [ ] |
| 23 | Icon follows [ICON-PALETTE.md](ICON-PALETTE.md): 100x100 square transparent PNG, own hue (not green, not shared), own silhouette, two colours, strokes >= 9 px | [ ] |
| 24 | Icon checked at 32 px and in greyscale on the review sheet (`python tools/icons/make_icons.py`), distinct from every other tile | [ ] |
| 25 | Data and app lists on `CompactList`; no status shown by colour alone | [ ] |

> **Icon upgrades:** a launcher older than PR #46 (2026-10-14) reads an icon's size once and keeps it until it restarts, so an app
> update that ships an icon of a different size can look cropped on its tile; a current launcher reloads the tile within about 3 s
> when the icon file changes. Keep the icon at 100x100 from the first version on.

