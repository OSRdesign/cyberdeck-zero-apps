# Headless harness of the cdk apps

Runs an app on the PC (WSL) with no board, no framebuffer and no launcher: a plain LVGL display at any size, the
layout service's metrics for that size, the app built by its own `AppSpec` (`cdk::start()`, the same call as on the
deck), scripted keys and taps, PNG shots, layout checks after every shot, one contact sheet per scene. The approach is
the launcher's render harness (`projects/APPLaunch/tests/render`): LVGL 9.5 from the launcher tree's SDK with the
deck's LVGL configuration and the launcher's FreeType glue, cp0_lvgl's widgets and keyboard bridge compiled unchanged;
only the libinput reader (`headless_keyboard.c`), the display and the pointer (`headless_platform.cpp`) are the
harness's own.

```sh
wsl -e sh /mnt/c/.../apps/<app>/build.sh host        # builds, runs the unit tests and every tests/scenes/*.scene
# or by hand, after a build:
~/cdk-build/<app>/host/<app>-headless --scene apps/<app>/tests/scenes/x.scene --out out \
    --resources ~/zero7/launcher/projects/APPLaunch/APPLaunch [--sizes 640x480,720x720]
```

Output: `out/<scene>/<WxH>/<shot>.png`, `out/<scene>/<WxH>/report.txt` (each check, PASS / FAIL with the reason),
`out/<scene>/<WxH>.log` (the app's stderr), `out/sheets/<scene>.png` (a row per shot, a column per size).
Exit status 0 when every size passed.

Default sizes: 640x480 (deck, 11.3 px/mm), 800x480 (9.2), 720x720 (10.0), 1280x720 (11.6), 1024x600 (11.3), 480x320.
The portrait canvases an app with `X-Portrait` gets in PDA mode run at the densities of the launcher's PDA scenes
when a scene names them: 480x800 (8.5, the 4.3" DSI panel), 480x640 (9.0, a 3.5" panel), 800x1280 (8.6, the FZ-M1
tablet).
The densities are the launcher harness's, so both compute the same tokens. Each size runs in its own process, all at
once. The app sees the per-launch environment of a fullscreen app (`APPLAUNCH_CONTRACT=1`, `APPLAUNCH_APP_MODE=fullscreen`,
window and canvas = the size) and an `input.state` fixture (no keyboard, rule `auto`) in `out/<scene>/<WxH>/runtime`.

## Scene scripts

One command per line, `#` comments, `"quoted words"`; `@WxH[,WxH]` in front of a line runs it at those sizes only.
After the app asks to quit, only `expect` lines are read.

| Command | Meaning |
|---|---|
| `title TEXT`, `sizes WxH ...` | sheet title; sizes (the command line's `--sizes` wins) |
| `nocontract` | started by hand: no contract variables and no `input.state` (APP-CONTRACT section 7) |
| `env NAME VALUE` | an environment variable set before the app starts (an app's own test fixture, e.g. the Serial Console's `SERIAL_CONSOLE_DEMO`) |
| `wait MS` | run the app for MS ms (real time: background commands are real processes) |
| `key NAME` | a press and release through cp0_lvgl's keyboard queue: `esc enter up down left right tab backspace delete space home end pgup pgdn`, a letter or a digit |
| `type TEXT` | the characters, each as a key with its text (`cp0_keyboard_inject_key`, the on-screen keyboard's path) |
| `tap TEXT` / `tapkey TEXT` | a tap on the visible label with exactly this text (its nearest clickable parent): on the page / on the on-screen keyboard |
| `tapxy FX FY`, `drag FX1 FY1 FX2 FY2` | a tap / a drag at fractions (0..1) of the screen |
| `keyboards N`, `osk auto\|off\|force` | rewrite `input.state` (gen + 1): physical keyboards present, the keyboard rule |
| `shot NAME` / `check NAME` | settle 250 ms, save the PNG (shot only), run the layout checks |
| `expect quit\|noquit`, `expect osk on\|off`, `expect text "T"`, `expect notext "T"` | assertions |
| `expect key "T" toolbar\|target` | the button labelled T is exactly the 9 mm toolbar key (`cp0_ui_toolbar_key_px`) / TARGET high and at least as wide; its size goes in the report |
| `say TEXT` | a line in the log |
| `lifecycle` | (scene option, like `nocontract`) the harness plays a multitasking launcher: a control socket in `APPLAUNCH_LIFECYCLE` / `APPLAUNCH_CONTROL_FD` and the toolkit's helper (`cp0_app_lifecycle_lvgl.h`) on the harness's display and pointer |
| `hide`, `show`, `close` | the launcher's message; `hide` / `show` must be answered within 300 ms (the time goes in the report), `close` must make the app quit within 300 ms |
| `stale KEY` | a press and release queued without running the app: what was typed while it was frozen (the next `show` must drop it) |
| `expect hidden\|visible` | the app's lifecycle state (`cdk::hidden()`) |
| `expect nowrite` | no flush at all while the app was hidden (the helper turns LVGL's invalidation off; the harness writes nothing then anyway) |
| `expect repaint` | every pixel of the screen was flushed since the last `show` (run a `wait` first) |

## Layout checks (after every shot)

Interactive element = a visible clickable object that is a button / switch / slider / checkbox or listens to a press
or click, and has no such object inside it (a scroll area that only drags is not one).

| Check | Rule |
|---|---|
| on-screen | no object is cut by the screen edge (only a scrolling container may hide part of an object) |
| size | page rows and fields >= ROW (7 mm), buttons >= TARGET (7 mm), the header's Esc >= min(TARGET, header - 2 x status_top) (the layout service's header rule), keys of the on-screen keyboard >= `CP0_UI_OSK_KEY_MIN_PX`; a row inside a vertically scrolling list may be LIST_ROW (5 mm: `cp0::ui::CompactList`'s compact density, where a tap only moves the highlight) |
| header | no interactive element of the body reaches under the header |
| keyboard | while the on-screen keyboard is up, the field it is for is visible above it and no element of the page lies under it |
| notice | under the app's minimum screen, the notice is the only thing to press |

The report line of each shot also gives the smallest row, button, header element and key it found (and the smallest
compact list row when there is one).
