# App Template (`cyberdeck-app-template`, draft)

The starting point of a native full-screen app for the deck, and the proof that the shared code in
[`apps/_common`](../_common/README.md) works. It is a **draft** (`"draft": true` in `app.json`): `tools/make_registry.py`
never publishes it.

It is a native `X-Fullscreen=true` app linking the launcher's `cp0_lvgl` (report 024, platform (a)): the launcher's
own display backend, keyboard reader, widgets and on-screen keyboard run inside the app's process, so the app looks and
behaves like the launcher's Settings on every screen from 640x480 up.

Rules and good practice (with a checklist to tick before publishing): [`docs/APP-GUIDELINES.md`](../../docs/APP-GUIDELINES.md).

## What it shows

| Piece | Where |
|---|---|
| Page with the title, the shared status strip and the on-screen Esc (`cp0::ui::Page`, `cp0::ui::StatusStrip`, `cp0::ui::add_title`, all from cp0_lvgl) | every page |
| Roller with Nav, Toggle, Info and Busy rows (`cp0::ui::Roller`) | root |
| InfoTable: what the app learnt from the launcher contract and the layout service | Device info |
| TextField with the app-side on-screen keyboard (shown only when no physical keyboard is connected, the launcher's rule; `_common/osk_policy`) | Name |
| Dialog with Cancel (Esc) and a primary button (Enter) | Reset |
| A background command whose lines arrive live in a Busy row, with a timeout (`_common/async_run`) | Run a command |
| The "needs a 640x480 screen" notice on a smaller screen (an older launcher ignores `X-MinSize`) | at start |

Keys: Up / Down move, Enter or Right opens, Esc or Left goes back; at the root a short Esc (or the header's Esc) ends
the app. Holding Esc 3 s is the launcher's watchdog. Touch: a tap on a row moves the highlight, a tap on the
highlighted row opens it, a drag scrolls.

Everything is laid out from the launcher's tokens (`cp0_ui_metrics_get()`, `cp0_ui_roller_page()`): no pixel constant
in `src/`.

## Layout

```
app.json                      "draft": true, version, depends (libfreetype6, libinput10, libxkbcommon0)
build.sh                      calls ../_common/build_app.sh (cross | host | all)
src/main.cpp                  cdk::run(cdk::app_spec(), argc, argv): the device runner
src/ui/app_spec.cpp           id, title, minimum screen (= X-MinSize), the UI builder
src/ui/template_app.*         the pages (LVGL)
src/core/template_core.*      the logic without LVGL: navigation, the name rule, the command state, the info rows
tests/test_core.cpp           unit tests of core/ and of the package files (.desktop keys, app.json)
tests/scenes/*.scene          headless scenes: every widget at 5 sizes, the notice under 640x480, a start without
                              the launcher
root/                         the package tree (the binary is written by build.sh cross)
```

## Build

In WSL (the launcher build tree `~/zero7/launcher` with its SDK; see `_common/build_app.sh -h`):

```sh
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/cyberdeck-app-template/build.sh host    # tests + contact sheets
wsl -e sh /mnt/c/CLAUDE/zero7/cyberdeck-zero-apps/apps/cyberdeck-app-template/build.sh cross   # aarch64 binary into root/
python3 tools/make_registry.py --only cyberdeck-app-template --out C:/somewhere/outside/packages   # the .deb (draft)
```

`--version` of the binary prints the app version, the launcher commit it was built against and whether that matches
`apps/_common/LAUNCHER_REF`.

## Multitasking

The template declares `X-Background=freeze` (0.1.1), the cheap default: under a launcher with multitasking on it can be
left open in the background, stopped, and brought back as it was; `_common` does the rest (APP-CONTRACT section 13 of the launcher; [guidelines](../../docs/APP-GUIDELINES.md#multitasking-hidden-and-shown-again-x-background)).
`tests/scenes/lifecycle.scene` plays the launcher. Choose `run` only for work that must go on while hidden, and none
for an app a pause would falsify.

## What the launcher gives the app

The status strip comes from `cp0_lvgl` at the pin (`apps/_common/LAUNCHER_REF`) with Wi-Fi, Bluetooth and the battery,
read from the launcher's `status.state` feed (the template sets no source; its header keeps its controls left of
`status_strip_left()`). The template uses nothing else of what launcher v0.8.6 added (`board.state`, the level keys of
`status.state`, beeps and toasts over `launcher.sock`); an app that wants them reads
[guidelines, section 3](../../docs/APP-GUIDELINES.md#3-the-launcher-contract) (the launcher's APP-CONTRACT.md is the
normative text) and needs a pin at the v0.8.6 commit or later for the `cp0_lvgl` readers and client.

## Making a new app from it

1. Copy the folder to `apps/<id>`, rename the `.desktop` file, the icon and the doc folder after `<id>`, and set
   `share_code`, `package`, `title`, `version` in `app.json` (keep `"draft": true` until it is reviewed).
2. Change `--name` in `build.sh` (a scons project name: letters and digits) and `id` / `title` in `src/ui/app_spec.cpp`.
3. Replace `src/ui` and `src/core` with the app; keep the logic in `core/` with its tests.
4. Write scenes for its pages; `build.sh host` must pass at every size before a board test.

## Credits and licences

<!-- branding-allow-begin: credit of the cp0_lvgl runtime linked into the binary (MIT) -->
MIT. The binary contains the launcher's `cp0_lvgl` runtime (M5Stack Technology CO LTD, MIT) and LVGL (LVGL Kft, MIT);
see `root/usr/share/doc/cyberdeck-app-template/copyright`. The fonts are the launcher's, read at run time; none is
shipped. The icon is a placeholder.
<!-- branding-allow-end -->
