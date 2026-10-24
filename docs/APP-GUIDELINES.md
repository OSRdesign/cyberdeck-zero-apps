# App guidelines for CyberdeckZero Launcher

How to write an app that installs from **Settings > Apps** and runs on every board of the project, on the Pi Zero 2 W
deck first. Read it before you start, tick the [checklist](#13-final-checklist) before you publish; [section 14](#14-lessons-learned) has what went wrong on the deck. It links the
reference documents instead of copying them; when a number here and the launcher disagree, the launcher wins (read the
tokens at run time, never copy them).

The app contract (environment variables, state files, `.desktop` keys, requests to the launcher) is specified in the
launcher repository, and that text is normative:
[APP-CONTRACT.md](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/APP-CONTRACT.md); what the
launcher gives apps as of v0.8.7 is summarised in [section 3](#3-the-launcher-contract). Hosting and
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
| Panasonic FZ-M1 tablet (x86-64 PC, amd64) | 1280x800, desktop session (XFCE) | touch + USB or Bluetooth keyboard | the PC reference; runs only apps with an amd64 package ([README](../README.md#amd64-x86-64-pcs)); the touch panel reports at most 2 contacts (no three-finger gesture) |

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
s/m/l, radius s/m/l, `header`, `row` (7 mm), `target` (7 mm, the same size as `row`), `dialog_max_w`, text sizes title/heading/body/caption.
The deck (640x480, 11.3 px/mm) values, for orientation only:

| Token | Deck 640x480 | Meaning |
|---|---|---|
| `space` s/m/l | 8 / 16 / 20 | gaps and margins |
| `header` | 56 | the page header (title, Esc, status strip) |
| `row` | 79 | a list or form row (at least 7 mm) |
| `target` | 79 | a button or touch target (at least 7 mm; 102 px / 9 mm with a launcher before 550465f) |
| `text` title/heading/body/caption | 32 / 28 / 24 / 18 | font sizes |

Rules:

| Do | Don't |
|---|---|
| lay out from `cp0_ui_metrics_get()`, `cp0_ui_roller_page()` and the `cp0::ui` widgets | write `lv_obj_set_size(x, 120, 48)` or any pixel constant |
| size things in millimetres (`ppmm`) when you must draw your own | assume 11.3 px/mm; the HyperPixel is 9.2, the HackBerry 10.0, the FZ-M1 tablet 8.6 |
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

- **What the strip shows.** The clock, the Wi-Fi bars, the Bluetooth glyph and, on a board that has one, the battery
  (level glyph and percent, green while charging) exactly as the home grid shows them. Where the values come from, and
  which change needs a rebuild: [section 3, the status strip](#the-status-strip-data-and-drawing).
- **Keep your header controls left of `cp0::ui::status_strip_left()`** (the `strip_left` argument of
  `cp0_ui_roller_page()`; the template and every app here do it). Never assume a strip width: the battery makes the
  strip wider on a board that has one (the HackBerry, the FZ-M1 tablet), and the strip is measured once, at the first
  call, with the battery the app itself can read (below).

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

#### Row kinds: what can be tapped

A touch-only board (the Pi 3A+) has no focus highlight, so the user must see all the time which rows do something. The
launcher's list widgets draw that cue from one optional tag per row, `cp0::ui::RowAffordance` (the last argument of
`ListRow::item(label, value, tone, id, affordance)`); the table of the tags is in the launcher's
[`docs/cp0_lvgl.en.md`](https://github.com/OSRdesign/cyberdeck-zero-launcher/blob/main/docs/cp0_lvgl.en.md), "Row
affordances" (decision 2026-10-16). It is purely visual: keys, taps and row heights do not change.

| Kind (tag) | Example | Look |
|---|---|---|
| **Open** | *All processes*, *Diagnose my connection*: the row opens another page | a chevron `›` at the right edge |
| **Edit** | *Sort by*, *Network*, *Latitude*: the row changes a value, in place or through a picker or editor | the value keeps its tone, then the chevron |
| **Action** | *Save log*, *Listen*: the row does something right there | normal label and a dot in the chevron's slot, white when idle |
| **Action, running** (`ActionRunning`) | the same *Listen* row, or a *Stop* row, while it runs | the dot turns blue (`#5AAAFF`) |
| **Info** | *MAC address*, a reading: read-only | no marker; a plain value is drawn grey (Muted); an explicit Ok / Warn / Bad tone keeps its colour |

- **Every list row of a native app is tagged.** An untagged row (`Unspecified`) still draws as before, but it tells the
  user nothing; do not leave one. Do not colour a label (blue, underline, accent) to mean "tappable": the marker is the
  only cue, and colour stays for status (see above).
- **Running state** is the same row with another tag. Give the row `Action` or `ActionRunning` from your state each time
  you build the rows and hand them to the normal refresh (`set_rows()`, or `update_row()` / `set_value()`): only the
  dot's colour changes, the scroll and the selection stay.
- **A list of your own** (rows of any height, wrapped text, a chain of hops) uses the shared helpers of
  `cp0_ui_list.h` (`cp0_ui_list_has_chevron()`, `_has_dot()`, `_label_rgb()`, `_value_rgb()`, `_dot_rgb()`,
  `_trail_w()`, `_chevron_y()`) so that it looks the same as `CompactList`; reserve the trailing width only for rows
  that carry a marker, and make the value text fit beside the marker (it is never cut under it).
- **Not lists:** a bar of buttons (Start / Stop / Save cells) and a tile grid are controls that look tappable by
  themselves; they carry no row marker.

### Touch and keyboard: both complete

A touch-only board (Pi 3A+) and a keyboard-only use must each reach **every** function.

| Rule | Detail |
|---|---|
| Keyboard first | Up/Down move, Enter or Right opens, Esc or Left goes back, Tab between fields |
| Touch is complete | tap = move highlight, tap on the highlighted row = open, drag = scroll; no gesture-only action, no hover, no long-press-only action (a 3 s hold that opens a box listing every action is allowed: [section 14](#14-lessons-learned)) |
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
| `APPLAUNCH_STATE_DIR` | directory of the state files `screen.state`, `input.state`, `status.state` and `board.state` (below); written **only** by the launcher, re-read when `gen` changes (at most once a second); without it `$XDG_RUNTIME_DIR/applaunch/` |
| `$XDG_RUNTIME_DIR/applaunch/launcher.sock` | requests to the launcher (beep, toast, volume, brightness, dim), from launcher 0.8.6 ([below](#asking-the-launcher-launchersock)) |
| `.desktop` `X-Fullscreen=true` | the app owns the panel until it exits |
| `.desktop` `X-MinSize=WxH` | the tile is hidden (also in Settings > Apps) on a smaller screen; a malformed value means no restriction |
| `.desktop` `X-Background=freeze\|run` | multitasking: the app can be hidden and shown again instead of owning the panel until it exits (below) |

- An app **never writes** `screen.state` or `input.state`. The launcher rewrites `screen.state` after every app exits.
- Ignore unknown keys and variables; names and units never change within contract 1.
- The C reader (`cp0_app_contract.h`, no LVGL) is copy-ready; a native app already uses it through `cp0_lvgl`.
- An app **never writes** `status.state` or `board.state` either, and never creates `launcher.sock`.

APP-CONTRACT.md is the normative text; the sections below summarise what changed for apps up to launcher **v0.8.7**
and how to use it. A native app gets a feature only when it is built against a launcher commit that has it
([launcher versions](#launcher-versions-and-what-each-added-for-apps)).

### What an app can read

| Source | Keys | Since | On an older launcher, or with no launcher |
|---|---|---|---|
| per-launch environment (`APPLAUNCH_CONTRACT`, `APPLAUNCH_APP_MODE`, `APPLAUNCH_WINDOW`, `APPLAUNCH_CANVAS`, `APPLAUNCH_CANVAS_SCALE`, `APPLAUNCH_STATE_DIR`) and panel environment (`APPLAUNCH_SCREEN_*`) | above; contract section 3 | contract 1 | absent: `cp0_app_env_read()` returns -1, use your own probe ([below](#started-without-the-launcher-ssh-a-script-an-older-launcher)) |
| board profile environment (`APPLAUNCH_BOARD`, `APPLAUNCH_GAUGE`, `APPLAUNCH_BACKLIGHT`, `APPLAUNCH_HINT_*`, `APPLAUNCH_TOUCH_*`) | the board's `board.conf`, inherited by every child of the launcher service | - | values of the board, not computed by the launcher; absent over ssh |
| `screen.state` | `w`, `h`, `rot`, `ppmm`, `class`, `compat`, `scale`, `fb`, `bpp`, `touch`, the tokens, `gen`; `touch_slots` (multitasking) | contract 1 | missing: `cp0_app_screen_compute()` gives the same tokens from a size and a density |
| `input.state` | `keyboards`, `bt_keyboards`, `last_seen`, `kb_gone`, `bt_gone`, `vkb`, `pointer`, `gen` | contract 1 | no `vkb` = `auto`, no `pointer` = `off` |
| `status.state` | `pid`, `gen`, `wifi`, `wifi_pct`, `eth`, `bt` (`off` / `on` / `connected`), `battery` (-1 = none), `battery_state` | **0.8.5** | `cp0_app_status_read()` returns -1: read sysfs yourself (the strip does, below) |
| `status.state` | `volume` (-1 = no sound output), `mute`, `brightness_pct` (-1 = no backlight to drive) | **0.8.6** | key missing: the reader gives -1 (`mute` 0); show no level bar |
| `board.state` | `pid`, `gen`, `launcher` (version), `board`, `arch`, `ram_mb`, `battery` (0 none, 1 present, 2 not set up), `battery_source` (`power_supply:<name>` or `gauge:max17048`), `backlight` (`none` / `onoff` / `levels`), `backlight_steps`, `sound` (0 / 1 / 2), `sound_kind` (`alsa` / `server` / `bluetooth`), `sound_guess` (1 = `bluetooth` guessed from the sink's name), `requests` (the verbs `launcher.sock` answers on this board), `battery_note`, `backlight_note`, `sound_note` (hint text) | **0.8.6** | `cp0_app_board_read()` returns -1: assume nothing, hide what depends on it (no sound option, no beeps) |

Rules for every row: **a missing key means its default** (the readers fill -1 or `""`), unknown keys and variables
are ignored, and in `board.state` a fact the launcher does not know is an **absent key, never `0`** (right after the
launcher starts, `sound` can be absent until its first sound-server probe). `status.state` and `board.state` are
trusted only while their `pid` (the launcher) is alive (`kill(pid, 0)`; the readers check it): a launcher that crashed
leaves a file nobody should believe. The launcher writes `status.state` only when a shown value changes (checked every
2 s); re-read it on a new `gen`, at most once a second. Readers: `cp0_app_contract.h` (`cp0_app_status_read()`,
`cp0_app_board_read()`, `cp0_app_board_has_request()`), plain C, part of `cp0_lvgl` and copy-ready for an app without
it. Contract sections 4, 5, 5b, 5c.

### The status strip: data and drawing

`cp0::ui::StatusStrip` is part of `cp0_lvgl`, compiled **statically** into your binary from the pinned launcher commit,
together with its renderer (`cp0_statusbar.c`) and its reading code. Every 2 s while its screen is active, a strip added
without a source of its own:

1. reads the battery itself (`cp0_power_supply_battery()`): a Linux `power_supply` battery (pin of 0.8.3 or later),
   else the fuel gauge that `$APPLAUNCH_GAUGE` names (the HackBerry's MAX17048 over i2c, pin of 0.8.4 or later; the
   app inherits the variable from the board profile);
2. reads the launcher's feed (`cp0_app_status_read()` on `status.state`, pin of 0.8.5 or later): Wi-Fi, Bluetooth
   powered / connected as the launcher's BlueZ sees it, and the battery when the feed has one;
3. without a good feed (no file, or its `pid` gone): sysfs and procfs (`cp0_statusbar_read_state()`), which since 0.8.5
   finds the Wi-Fi interface under any name (`wlan0`, `wlp2s0`, `wlx...`) instead of assuming `wlan0`. sysfs cannot
   tell "Bluetooth powered", so the glyph can differ from the home grid's on that path only.

What an app **must** do: add the strip with no source (`StatusStrip::add(page->header())`) and keep its header
controls left of `status_strip_left()`. What it **need not** do: read Wi-Fi, Bluetooth or the battery for the strip,
or read `status.state` for it. What it **must not** do: draw its own clock or icons, pass its own source without a
reason, keep a copy of the strip.

**Battery width.** `status_strip_left()` is measured once, at its first call, by drawing the strip in its fullest state
with the battery the app's own `cp0_power_supply_battery()` sees (not the feed's). So lay the header out from it, never
from a fixed width. From the source (not seen on a board): an app started where it cannot read the battery itself but
the launcher's feed has one (over ssh, without the board profile's `$APPLAUNCH_GAUGE`) draws the battery without having
reserved its width, and the strip can overlap the header's controls.

**What reaches a published app without a rebuild, and what does not:**

| Change in the launcher | Reaches an app |
|---|---|
| a value the feed already carries (Wi-Fi, Bluetooth, battery: a new battery source, a fixed Bluetooth state) | **at once**, no rebuild, for an app pinned at v0.8.5 or later |
| a new key of the feed, or a new reading rule in the strip | only after a re-pin and rebuild |
| the strip's **drawing** (`cp0_statusbar.c`: layout, glyphs, colours, sizes) | only after a re-pin and rebuild of **every** app, then a publish |

Checklist line for whoever changes the strip's drawing in the launcher: **open an apps re-pin** (move
[`LAUNCHER_REF`](../apps/_common/LAUNCHER_REF), rebuild every native app on both architectures, compare the header shots
with the launcher's home grid, raise the versions, publish). A non-LVGL app that draws its own bar (viz1090's map shim,
`apps/viz1090/build`) keeps a copy of `cp0_statusbar.[ch]`; the launcher's `docs/dev/deck/check_statusbar_drift.py`
compares those copies with the launcher's tree (exit 0 identical, 1 drifted or a copy inside an app that links
`cp0_lvgl`, 2 a file or the apps repository missing) and is a step of the launcher's release checklist. An LVGL app
never has a copy.

### Asking the launcher (`launcher.sock`)

From launcher **0.8.6** an app asks the launcher for what the launcher owns (volume, backlight, sounds, notices)
through one Unix stream socket, `$XDG_RUNTIME_DIR/applaunch/launcher.sock` (mode 0600, the launcher's user; no `/tmp`
fallback). One request line, one reply line, at most 254 bytes. Replies are `ok ...` or `err <word>` (`unknown`, `bad`,
`nosound`, `absent`, `limit`, `busy`; `denied` reserved). Contract section 14.

| Request | Does |
|---|---|
| `hello 1` | `ok 1 launcher=<version> app=<process name> verbs=<list>` |
| `caps` | `ok sound=... backlight=... dim=... verbs=...` |
| `beep [notify\|done\|warn]` | a short built-in sound at the volume in force; `err nosound` on a board without sound output |
| `toast <text>` | the launcher's own toast (cut at 160 characters); it waits and **shows when the launcher is in front again** (after your app exits or is hidden), at most 4 waiting |
| `volume get\|up\|down\|mute\|unmute\|set N` | as the volume keys; for media apps only (`get` is in `status.state` already) |
| `brightness get\|set N` | levels backlights only; `err absent` otherwise |
| `dim allow\|deny` | lets the launcher dim the idle panel while your app is in front (battery boards with brightness levels); ends when the launcher takes the panel back: send it again after a `show` |

- **Client.** `cp0_app_client.h` / `cp0_app_client.c` (plain C, part of `cp0_lvgl` at a v0.8.6 pin and copy-ready):
  `cp0_app_client_call()`, `cp0_app_beep()`, `cp0_app_toast()`. They return 0 for `ok`, 1 for `err`, -1 for no service,
  within 200 ms, and print nothing.
- **Degrade.** No socket (an older launcher, no launcher, no `XDG_RUNTIME_DIR`) = `-1` at once: skip the beep or the
  toast silently, show no error. **Never block more than about 200 ms**, and **never call it on the LVGL thread**: use a
  worker, and remember a failure for a few seconds instead of retrying per event.
- **Check before offering.** Show a sound option only when `board.state` says `sound=1` and `requests` has `beep`
  (`cp0_app_board_has_request()`); a `beep` on a board with `sound=0` answers `err nosound`.
- **Limits.** Per client process: `beep` and `toast` one per second, `volume`, `brightness` and `dim` ten per second,
  the rest fifty (`err limit`). Every request is logged by the launcher with the process name.
- **Permissions.** None per app yet: every process of the user may ask. `X-Permissions` in the `.desktop` file is
  **reserved** (a later launcher may answer `err denied` to undeclared verbs); do not rely on it staying open.
- **Test from ssh** with `cyberdeckzero-ctl` (installed by the launcher in `/usr/share/APPLaunch/bin`, linked in
  `/usr/local/bin`): `cyberdeckzero-ctl hello`, `beep done`, `toast "2 new messages"`, `volume get`, `board` (prints
  `board.state`); exit status 0 ok, 1 refused, 2 usage, 3 no launcher service. A terminal app or a script uses the same
  command.

### Sound, volume and brightness

- **Sounds.** The launcher's own interface sounds are off and there is no setting for app beeps. An app may ask for a
  `beep` (above); it plays at the current volume, so a muted or very quiet volume is silent: that is the user's choice.
  Without sound output (`board.state` `sound=0`) expect `err nosound`.
- **The volume, mute and brightness keys are the launcher's** while your app is in front (`KEY_VOLUMEUP`,
  `KEY_VOLUMEDOWN`, `KEY_MUTE`, `KEY_BRIGHTNESSUP`, `KEY_BRIGHTNESSDOWN`; launcher 0.8.6, no rebuild of the app needed).
  The keys still reach the app: **do not act on them too**, or the step doubles.
- **Media-key devices (launcher 0.8.7).** The launcher also reads input devices that carry these five keys but cannot
  type (on the FZ-M1 tablet "Panasonic Laptop Support", "Video Bus" and "Intel Virtual Buttons"; the nodes read on other
  boards are listed in the launcher's 0.8.7 release notes), and acts on them while an app is in front. Only those five
  keys are taken from such a device; it never counts as a keyboard (`input.state` and the on-screen keyboard rule do not
  change), is never grabbed, and a press seen on two devices counts once. No rebuild of the app is needed.
- **The Windows (Super) key is the launcher's too** (contract sections 5 and 13). A tap alone goes home (hides the
  app); from 0.8.7 a double tap (second press within 0.45 s) or a hold of 0.6 s opens the app switcher. Give Super no
  meaning of your own. `KEY_F14` is reserved (the FZ-M1's rotation-lock button sends Super+F14 in one BIOS mode): do not
  use it either.
- **Showing the level.** The launcher draws nothing over a fullscreen app. An app that wants to show the level draws
  its own bar from `status.state` (`volume`, `mute`, `brightness_pct`), polled with the rest of the status, not through
  `launcher.sock`.
- Not verified on boards (launcher 0.8.6 notes): the keys inside apps and `dim allow`. `board.state`, `beep` and
  `toast` were checked over ssh on the HackBerry and the FZ-M1 tablet. Launcher 0.8.7 notes: on the FZ-M1 the owner
  checked the volume buttons and the Windows key double tap; the new devices on the HackBerry and the deck were not.

### Launcher versions and what each added for apps

The apps build against one launcher commit, [`apps/_common/LAUNCHER_REF`](../apps/_common/LAUNCHER_REF) (today the
**v0.8.5** commit `d0cb456`; the launcher itself is at **v0.8.7**, which changed nothing an app is built with). Launcher features that live in the launcher process (the feed's values, the media keys,
the socket's server) work with any build; features that live in `cp0_lvgl` (the strip's drawing and reading, the
readers, the client) reach an app only at a pin that has them. **Using a 0.8.6 feature from `cp0_lvgl` needs a re-pin
to the v0.8.6 commit `aef009b`** (and then the rebuild of every app, as above); an app can also copy the plain-C
files of the contract and the client.

| Launcher | Added for apps | Reaches an app |
|---|---|---|
| 0.8.1 (`2b1a070`, the previous pin) | amd64 launcher; the apps' amd64 builds use the same pin | - |
| 0.8.2 | Settings > Apps reads `registry-amd64.json` on a PC | no rebuild |
| 0.8.3 | the strip shows a `power_supply` battery | re-pin |
| 0.8.4 | the strip reads a fuel gauge (`$APPLAUNCH_GAUGE`, HackBerry MAX17048); the launcher detects battery, backlight and sound at run time | re-pin |
| 0.8.5 (`d0cb456`, **the current pin**) | `status.state` (`wifi`, `wifi_pct`, `eth`, `bt`, `battery`, `battery_state`) and `cp0_app_status_read()`; the strip reads the feed; Wi-Fi interface found under any name | re-pin (done: the six apps rebuilt at 0.8.5) |
| 0.8.6 (`aef009b`) | `board.state`; `status.state` gains `volume`, `mute`, `brightness_pct`; `launcher.sock`, `cp0_app_client.[ch]`, `cyberdeckzero-ctl`; volume and brightness keys of keyboards acted on by the launcher while an app is in front; `X-Permissions` reserved | the files and the keys: no rebuild; the readers and the client in `cp0_lvgl`: re-pin. The strip did not change, so no forced re-pin |
| 0.8.7 ([release page](https://github.com/OSRdesign/cyberdeck-zero-launcher/releases/tag/v0.8.7)) | the five media keys are also read from input devices that are not keyboards and act while an app is in front; the Windows key double tap or a 0.6 s hold opens the app switcher (apps give it no meaning); `KEY_F14` reserved; fix of a launcher crash at start (sound-player race, also in 0.8.6). Apps do not change | nothing to rebuild, no new pin (the strip's drawing did not change) |

**What needs a re-pin, in short.** An app pins a launcher commit. A change to the **drawing** of the shared widgets (the
status strip) needs a re-pin and a rebuild of every app. A change to the **data** the launcher publishes (state-file
values) or to the **keys** the launcher reads (media keys, Super) lives in the launcher process and reaches every app
at once. A new reader or client in `cp0_lvgl` reaches an app only at a pin that has it.

### Started without the launcher (ssh, a script, an older launcher)

Rule: **degrade to your own probe, never crash**. `cp0_app_env_read()` returns -1: read `screen.state` if present, else
the framebuffer size and 11.3 px/mm. Log one line saying so (the template: `no launcher contract (started by hand ...)`),
keep keyboard working, touch axes may be transposed (they come from the service's environment). A headless tool or
daemon part of your app must also run with no display (`--version`, `--help`, a `--headless` or `--report` mode) so it can be unit-tested.

### Multitasking: hidden and shown again (`X-Background`)

A launcher with multitasking on (APP-CONTRACT section 13) can hide an app that declares `X-Background` (Super, Alt+Tab,
three fingers held 1 s, a long-press on the clock) and bring it back from its switcher. **Opting in is one line** in the
`.desktop` file, once the app is built against a `cp0_lvgl` that has the helper (`_common/LAUNCHER_REF`): `_common`
starts it (`platform.cpp`), and it does the protocol, no drawing while hidden, the input drop and the full repaint on
return, the launcher's `close` (= your normal exit, as Esc at the root). Without the launcher's control socket (an older
launcher, multitasking off, started by hand) nothing changes.

| Mode | While hidden | Choose it when |
|---|---|---|
| `X-Background=freeze` | the whole process group is **stopped** (no CPU; memory kept; timers jump on return) | the default: nothing must go on (a monitor, a reader, a form) |
| `X-Background=run` | the app **keeps running**, nothing is drawn, input is ignored | work that must not stop: reading a serial port (a stopped tty overflows its kernel buffer in seconds), receiving radio messages |
| none | (today) the app owns the panel until it exits; three fingers offer "Close <app>?" | wrapped apps, an app whose results a pause would falsify and that has no hook for it yet |

What an opted-in app must (and must not) do:

- **Never write the framebuffer yourself while hidden.** On `cp0_lvgl` nothing to do: the helper suspends the display and
  turns LVGL's invalidation off (nothing is rendered). An app that draws by itself (a shim) stops its copies on `hide`.
- **`run`: only your own work while hidden.** Use `cdk::hidden()` to skip what only feeds the screen (rebuilding a page,
  formatting a list) and refresh it when it turns false; keep timers at their usual rate or lower; no new polling. Mesh
  Hop: the radio and the model go on, no page is rebuilt and nothing counts as read while hidden.
- **Hardware.** A frozen app still holds what it opened (a serial port opened exclusive, an SDR). Either `run` and keep
  using it, or close it on hide and open it on show (no app does that today).
- **Save state on exit, and as you go.** The launcher closes hidden apps under memory pressure (frozen ones first) and
  at its cap of four; `close` gives you your normal exit, the kernel's OOM killer gives nothing.
- **A pause must not lie.** A measurement that spans a freeze (a speed test, ping timings, a walk test) is wrong after it:
  such an app stays without `X-Background` until it can cancel or mark the measurement (Net Toolbox today).
- **Esc at the root still quits** (owner decision D5); `cdk::leave_to_home()` exists for an app that wants a "minimise"
  control, none has one.
- **Test it** with a `lifecycle` scene ([`_common/headless/README.md`](../apps/_common/headless/README.md)): hide, keys and
  taps while hidden, `expect nowrite`, `stale` keys, show, `expect repaint`, `close`, `expect quit`.

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
X-Background=freeze
```

(`X-Background`: section 3, multitasking; leave it out for an app that must not be paused.)

`app.json` (checked by `tools/make_registry.py`; `share_code`, `package`, `version`, `title`, `summary` are required):

| Field | Rule |
|---|---|
| `share_code` | lowercase id, stable forever (it gives the registry uuid) |
| `package` | Debian package name, = the app id |
| `version` | **raise it for every release**: the deck compares Debian-style (`dpkg --compare-versions`); the same version is not an update |
| `title`, `summary`, `description` | what Settings > Apps shows; the description states needs (hardware, groups, network, minimum screen) |
| `license` | SPDX expression of everything shipped (`MIT`, or `BSD-3-Clause AND GPL-3.0-or-later`) |
| `depends` | Debian packages `apt` must add: **only what the binary links dynamically** (native apps: `libfreetype6, libinput10, libxkbcommon0`), plus tools you run (`curl`, `picocom`). Check the names exist on Debian 13 arm64 |
| `recommends` | optional: Debian packages a feature uses when they are there (apt installs them by default; the app must work without them), e.g. Mesh Hop's `bluez` |
| `permissions` | `network`, `additional_hardware`, `camera`, `microphone`, `imu`, `background_service`, `external_display` (honest: they are shown to the user) |
| `draft` | `true` keeps the app out of the registry (`make_registry.py` skips it); build a local `.deb` with `--only <id> --out <dir outside packages/>` |
| `arch` | optional, default `["arm64"]`; `["arm64", "amd64"]` also packages the app for x86-64 PCs from `root.amd64/` into `registry-amd64.json` (README, amd64) |
| `categories`, `author`, `source_repo` | shown in the catalogue |

- **libc6 floor.** `tools/build_deb.py` adds `libc6 (>= 2.38)` to every package (the binaries are built for Debian 13); do not
  repeat it in `depends`, and never lower it. Build on the pinned toolchain (`build_app.sh`), not on an older system.
- **arm64 first.** A binary for another CPU installs and does not run. An amd64 package is optional and comes from the
  same app folder: `"arch": ["arm64", "amd64"]` and the x86-64 binary in `root.amd64/` (`build.sh cross --arch amd64`;
  README, amd64). `build_deb.py` refuses a package holding an ELF file of the wrong architecture.
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
| **2. Headless scenes** | the real UI on a plain LVGL display at 640x480, 800x480, 720x720, 1280x720 and 1024x600 (480x320 for the "needs 640x480" notice): scripted keys and taps, PNG shots, layout checks (nothing off-screen, rows and targets at least 5/7 mm, nothing under the header or the on-screen keyboard), one contact sheet per scene | `tests/scenes/*.scene`; [`_common/headless/README.md`](../apps/_common/headless/README.md) |
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
| An app that lists amd64: the amd64 package installs from Settings > Apps, opens, and the main path works by touch and keys | FZ-M1 tablet |

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
| ask the launcher for a beep or a toast from a worker, and carry on without it | drive the volume or the backlight yourself (`pactl`, sysfs), or act on their keys |
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
keyboard; viz1090 reads its framebuffer through its own
shim; some still carry the old binary prefix, to be dropped at their next version (decision 7 of report 025). Fix these when you next
release the app, not as a drive-by.

Mesh Hop (0.4.0) follows these rules, the on-screen keyboard rule included, with these exceptions, kept on purpose: its own
virtual row list for the long contact and message lists (only the rows in view are widgets) instead of `CompactList`; its
own key bridge typing into the launcher's text fields instead of `cp0::ui::TextField` (its US and AZERTY tables and the
editors' filters); buttons at the 7 mm `row` height instead of `target`; a chat's options by a 3 s hold on its row (or
Ctrl+O); a short Esc of the keyboard at Home says "Hold Esc 3 s to exit" (the header's Esc there asks *Quit Mesh Hop?*).

## 12. When NOT to build it

Do not start (or stop, and say so) when:

- **It needs heavy rework to fit 640x480.** A dense desktop UI (many columns, tiny controls, a mouse-only editor, a
  map with hover) is not squeezed in. Redesign it into the roller/page model or leave it. The same holds for 480x320: not worth
  heavy rework; declare `X-MinSize=640x480`.
- **It is x86-only** (binary blobs, a vendor driver, a Wine app). Every app runs on the decks (arm64); an amd64 build for PCs comes in addition, never instead.
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
| 26 | List rows tagged Open / Edit / Action / Info; no marker on pure read-outs ([row kinds](#row-kinds-what-can-be-tapped)) | [ ] |
| 27 | Multitasking chosen and written in the README: `X-Background=freeze`, `run` (and why), or none (and why); a `lifecycle` scene passes ([section 3](#multitasking-hidden-and-shown-again-x-background)) | [ ] |
| 28 | Header controls left of `status_strip_left()`, nothing placed from a fixed strip width; the app does not act on the volume, mute or brightness keys ([section 3](#sound-volume-and-brightness)) | [ ] |
| 29 | Launcher requests (if any) off the LVGL thread, at most about 200 ms, silent without the socket; sound options only with `board.state` `sound=1`; a missing state key means its default ([section 3](#asking-the-launcher-launchersock)) | [ ] |
| 30 | Built against the current [`LAUNCHER_REF`](../apps/_common/LAUNCHER_REF) (`--version` says it matches); a launcher change to the strip's drawing means a re-pin and rebuild of every app ([section 3](#the-status-strip-data-and-drawing)) | [ ] |

> **Icon upgrades:** a launcher older than PR #46 (2026-10-14) reads an icon's size once and keeps it until it restarts, so an app
> update that ships an icon of a different size can look cropped on its tile; a current launcher reloads the tile within about 3 s
> when the icon file changes. Keep the icon at 100x100 from the first version on.

## 14. Lessons learned

Problems met on the deck, 2026-10-22 to 2026-10-25 (Mesh Hop 0.7.x, Markdown Notes 0.1.x). Each one is a rule now.

### The on-screen keyboard: the rule, and showing its state

Physical keyboard first, never summoned by hand ([section 2](#on-screen-keyboard)). For 600 s after a Bluetooth keyboard
goes away the on-screen keyboard stays off (the keyboard is probably asleep); after that it shows by itself in the field
being typed into. To test it: **Settings > System > On-screen keyboard**, and `input.state` in `$APPLAUNCH_STATE_DIR`
(`keyboards`, `bt_gone`, `vkb`); in the headless harness the scene commands `keyboards 0|1` and `osk force`. An app with
text fields should show the rule's state in its own Settings, so that a user who sees no keyboard can tell why. Markdown
Notes has an *On-screen keyboard* line there ("allowed: no", "a keyboard is connected", "Bluetooth keyboard asleep 8 min
more") and is the pattern to copy. A tap on a field asks for the keyboard each time; the shared rule still decides.

### Options by a 3 s hold, instead of a toolbar

On 640x480 a bar of 7 mm buttons costs rows of the page. Mesh Hop (a chat's or a contact's options) and Markdown Notes
(the preview and the editor) open instead a message box listing **every action** after a 3 s hold on a row or on the
text (a bar fills while the finger is down), and show each action's Ctrl key in grey for keyboard users.

| Use a bar when | Use the hold box when |
|---|---|
| a few actions are used all the time (Send, Save) | many actions, each used now and then |
| the page is a form or a list that needs all its height anyway | the page is text to read or write, and its height is the point |

The trade-off: a hold is slow and not discoverable, and the rule of section 2 is *no long-press-only action*. The owner
decided it for the Notes app (2026-10-24): all the actions are in the box, so touch reaches every one, and keyboards have a
Ctrl key for each. Keep to that: a hold is **never the only way for an essential action** unless the box lists all of them;
a line under the title tells how to open it ("Hold 3 s: options" without a keyboard, "Ctrl+O: options" with one); a tap
and a drag keep their normal meaning (a hold never moves the cursor); Esc closes the box.

### No heavy work at start on a Pi Zero 2 W

Do not start CPU-heavy background work (Python bytecode compilation, hashing, indexing, a cache build) when the app
connects to hardware. Mesh Hop 0.6.0 and 0.7.0 ran the CLI's warm-up (`meshcore-cli -v`, Python compiling its files for
10-20 s) as soon as a node connected on USB, and the whole deck reset; 0.5.3, without it, connected fine. Why the board
reset under that load (power, USB or both) is not known. So: start such work **on demand**, when the page that needs it
opens; at idle priority (`nice`, `ionice`); and with an **environment switch that turns it off** for diagnostics
(`MESHHOP_NO_WARMUP=1`). A board that resets right after a connect: try the switch first.

### After a hard reset, do not trust the last log lines

An app logs to stderr and nothing is fsynced. When the board resets hard (power, brown-out), the last seconds before the
reset are usually missing from the log, so its last lines are **not** where it went wrong. Look at what started in the
20 to 30 s before them, and test again with the suspected work switched off.

### Learned names and keys: the device, not the port

What an app remembers about a USB device (the name the board answered with, a setting, a key) is stored with the device's
identity (USB id and serial number), never with the port path (`/dev/ttyACM0`): the same path is another board tomorrow.
Show the remembered value only while that device is plugged in. Mesh Hop 0.7.1 fixed a node list that showed the name of
the board that had been on the port before.

### Text size: LVGL's textarea lays the whole text out again at every key

`lv_textarea` (and `cp0::ui::TextField` with `multiline`) sets the whole text and lays it out again on every key, so the
cost grows with the note. Markdown Notes caps the editor at **128 KB** and reads **1 MB** in the preview only; larger files
are not opened. The cap is to be tuned on the board (the typing speed in a large note on the Zero 2 W is its board test
B4); a custom editor on a line buffer is the fallback if it lags. Cap every text you edit, and write the cap in the README.

### Building and publishing the packages

- **The amd64 binary.** Rebuild it with every release (the arm64 build does not touch `root.amd64/`). The steps, with a
  private root of the -dev packages when `sudo apt install` is not possible, are in the
  [Mesh Hop README](../apps/mesh-hop/README.md), paragraph *Building the amd64 package*.
- **Publishing.** Use `python3 tools/make_registry.py --no-build` to ship the `.deb` files that were tested, and splice
  only your own entry into `registry.json` and `registry-amd64.json`: a plain run rebuilds every package and rewrites every
  `updated_at` (section 5).
- **File modes.** Packaging from WSL on `/mnt/c` gives mode 755 to every file. Package with **Windows Python** from the
  checkout, so that the files get 644 and the binaries 755.

