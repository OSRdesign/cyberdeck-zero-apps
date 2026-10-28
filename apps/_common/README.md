# apps/_common: shared code of the native fullscreen apps

Source shared by every native app of this repository (report 024 of the launcher docs: platform (a), a native
`X-Fullscreen=true` app linking the launcher's `cp0_lvgl`). It is not an app: `tools/make_registry.py` skips it (no
`app.json`). An app compiles these files with its own (`build_app.sh` copies them into the app's scratch project), so
nothing here is a shared library and there is no ABI to keep: the launcher contract (`docs/APP-CONTRACT.md` in the
launcher repo) is the interface.

Rules and good practice for every app: [`docs/APP-GUIDELINES.md`](../../docs/APP-GUIDELINES.md).

The starting point of a new app is `apps/cyberdeck-app-template` (a draft that uses every piece).

| File | What |
|---|---|
| `app.{hpp,cpp}` | `AppSpec` (id, title, minimum screen = `X-MinSize`, minimum portrait canvas = `X-Portrait` when the app has a portrait layout, UI builder), `start()` / `stop()` (what both runners do once the display exists), `request_quit()`, the keypad input group, the "needs WxH" notice for a screen under the minimum (an older launcher ignores `X-MinSize`), `hidden()` / `leave_to_home()` (multitasking) |
| `platform.{hpp,cpp}` | the device runner `cdk::run()`: cp0_lvgl's display backend switched to the native display, every keyboard read, SIGTERM / SIGINT / SIGHUP end the loop cleanly, `--version`; started by hand it still uses the launcher's backend (`APPLAUNCH_DISPLAY=dpi-scaled`); with the launcher's control socket, the multitasking lifecycle (`cp0_app_lifecycle_device_start()`: hide / show / close, below) |
| `osk_policy.{hpp,cpp}` | the app-side on-screen keyboard rule: `cp0_app_osk_allowed()` on `input.state`, looked at once a second at most and parsed again only when the file changed (a new `gen`); never shown by hand, and off for 600 s after a Bluetooth keyboard went away (tests and the Settings line to show the rule: [guidelines, section 14](../../docs/APP-GUIDELINES.md#14-lessons-learned)); without the file `APPLAUNCH_OSK` and the keyboards of `/proc/bus/input/devices` |
| `async_run.{hpp,cpp}` | background commands: fork + exec in their own process group, lines and the end delivered on the LVGL thread in order (`lv_async_call` under `lv_lock()`), timeout, cancel, PDEATHSIG, `shutdown()` at exit |
| `build_info.hpp` | the version stamp (`cdk_build_info.h` from `build_app.sh`) |
| `build_app.sh` | cross (aarch64, stripped, into the app's `root/`; with `--arch amd64` x86-64, native, into `root.amd64/`) and host (headless harness, unit tests, scenes, contact sheets) builds in WSL, the launcher pin check, the stamp |
| `LAUNCHER_REF` | the launcher commit the apps build against ([the launcher pin](#the-launcher-pin)) |
| `scons/` | the scratch project's scons files: the SDK's project `SConstruct`, the component (`main.SConstruct`: cp0_lvgl + LVGL static, the launcher's FreeType glue, `-O2`, `--as-needed`) and the cross configuration (cp0_lvgl with the filesystem service only) |
| `headless/` | the PC harness (see its README): a plain LVGL display at any size, scripted keys and taps, PNG shots, layout checks, contact sheets |
| `tests/test_common.cpp` | unit tests of the keyboard rule, the command runner, the minimum-size rule and the lifecycle (off without a socket) |

## Rules every app follows

- `.desktop`: `X-Fullscreen=true`, `X-MinSize=640x480` (decision 2026-10-13), the same numbers in `AppSpec`. An app
  with a portrait layout (PDA mode) also declares `X-Portrait=WxH` and the same numbers in `AppSpec.portrait_w/h`
  ([guidelines, portrait layout](../../docs/APP-GUIDELINES.md#portrait-layout-x-portrait)); without it the launcher
  runs the app turned on a portrait panel.
- Layout from the layout service only (`cp0_ui_metrics_get()`, `cp0_ui_roller_page()`, the widgets): no pixel constant.
- Keyboard first, every action by keys and by touch; a short Esc at the root page ends the app; the 3 s Esc hold is the
  launcher's.
- Commands through `async_run` (never `system()` or a blocking `popen()` on the LVGL thread).
- Multitasking: `X-Background=freeze` (stopped while hidden) or `run` (keeps working, draws nothing) in the `.desktop`
  file, or nothing (the app owns the panel until it exits); the choice and its reason in the README.
- Build: `build.sh` of the app calls `build_app.sh`; the binary links cp0_lvgl and LVGL statically and needs only
  libinput, libxkbcommon, libfreetype, libstdc++ and libc at run time.

## amd64

The same source builds for x86-64 PCs (report 028 of the launcher docs, X8): `build.sh cross --arch amd64` (or
`build.sh --arch amd64` for the host tests as well).

- **Where.** A native build in the launcher's amd64 WSL tree (`LAUNCHER_TREE_AMD64`, default
  `~/zero7/<launcher folder>-amd64`, which the launcher's `tools/dev/wsl-build.sh --arch amd64` syncs and seeds), with
  the cross configuration minus the toolchain prefix and the NEON lines (written as
  `linux_x86_64_native_cp0_config_defaults.mk` in the scratch project, as the launcher's own native configuration).
- **Needs.** gcc, g++ and the -dev packages found by pkg-config (`sudo apt install libinput-dev libxkbcommon-dev
  libudev-dev libdbus-1-dev libglib2.0-dev libfreetype-dev`). A private root of them works too: `PKG_CONFIG_PATH` and
  `CONFIG_TOOLCHAIN_FLAGS` (`-isystem <root>/usr/include ... -L<root>/usr/lib/x86_64-linux-gnu
  -Wl,-rpath-link,<root>/usr/lib/x86_64-linux-gnu`) in the environment.
- **Output.** The stripped binary in `<app>/root.amd64/usr/share/APPLaunch/bin/<app id>` and
  `<app>/root.amd64/usr/share/doc/<app id>/build-info` (`built <date> x86_64`; `--version` says `x86_64 build`).
  `root/` keeps the arm64 binary. `tools/make_registry.py` lays `root.amd64/` over `root/` for the amd64 package
  when `app.json` lists `"arch": ["arm64", "amd64"]` (the repository README, section amd64).
- **Same pin, same tests.** One `LAUNCHER_REF` for both architectures; the host build (harness, unit tests, scenes)
  is the same whatever `--arch` says (it always uses `LAUNCHER_TREE`). The binary needs glibc 2.38 or newer and the
  same libraries as on arm64.
- **Check before marking an app amd64.** Nothing Pi-only on its main path (vcgencmd, `/proc/device-tree`, GPIO, the
  Pi's UARTs): such a source must be optional, as System Monitor's firmware flags are.

## Status strip and title

Not copied here: apps use the launcher's own `cp0::ui::StatusStrip::add(page->header())`, `cp0::ui::add_title` (the
`Page` title factory) and `cp0::ui::status_strip_left()` (the `strip_left` of `cp0_ui_roller_page()`) from
`cp0_ui_widgets.hpp`, pinned by `LAUNCHER_REF`. The strip reads the battery itself (a `/sys/class/power_supply`
battery, else the fuel gauge named by `$APPLAUNCH_GAUGE`, the HackBerry's MAX17048), then the launcher's `status.state`
feed while the launcher process is alive (from v0.8.5: Wi-Fi, Bluetooth and battery as the home grid shows them), else
sysfs / procfs (the Wi-Fi interface whatever its name, not only `wlan0`); fonts come from the launcher's
`/usr/share/APPLaunch/share/font`. Header controls stay left of `status_strip_left()`: the battery widens the strip.

The strip's drawing and reading code are compiled into each binary at the pin, so **values the feed carries reach the
apps without a rebuild, a change to the drawing does not**: moving `LAUNCHER_REF` moves the strip; rebuild every app
and compare the header shots with the launcher's. Details, and what else the launcher publishes for apps
(`board.state`, `launcher.sock`, the volume and brightness keys):
[guidelines, section 3](../../docs/APP-GUIDELINES.md#3-the-launcher-contract).

## The launcher pin

`LAUNCHER_REF` is the **v0.8.9** commit `25927c3`, moved from v0.8.5 (`d0cb456`) for System Monitor 0.3.0, the first
app with a portrait layout: on the 480 px wide portrait canvases the v0.8.8 toolkit draws the narrow page header (an
18 px title, the last part of a breadcrumb, the status strip at 72 %), which a v0.8.5 build lacks ("Syste..." in the
header). At 640x480 and every other landscape size the toolkit draws the same pixels as at v0.8.5 (System Monitor's
landscape shots are byte-identical). The pin also brings the v0.8.6 readers and the socket client of `cp0_lvgl`
(`cp0_app_board_read()`, `cp0_app_client.h`). System Monitor 0.3.0 and Serial Console 0.5.0 are built at it (Serial
Console's landscape shots are the same pictures as at v0.8.5 too); every other app still carries a v0.8.5 build and
moves to this pin at its next rebuild (both architectures). `_common` has no launcher-request
wrapper yet.

Launcher **v0.8.7** (media keys of non-keyboard devices, the Windows key opening the app switcher, the sound-player
crash fix) lives in the launcher process: it needs no re-pin and no rebuild.
What needs a re-pin is a change to the drawing of the shared widgets (the status strip), not a change to the data or the
keys the launcher publishes ([guidelines, launcher versions](../../docs/APP-GUIDELINES.md#launcher-versions-and-what-each-added-for-apps)).

## Multitasking (hidden and shown again)

The launcher's toolkit does the app side of APP-CONTRACT section 13 (`cp0_app_lifecycle_lvgl.h`, pinned by
`LAUNCHER_REF`); `platform.cpp` starts it after the app's UI exists, only when the launcher gave a control socket
(`APPLAUNCH_LIFECYCLE=1`, `APPLAUNCH_CONTROL_FD`). From the run loop, without a thread: **hide** = no framebuffer write
and nothing rendered (LVGL's invalidation off), the press in progress cancelled, the keyboards let go (held keys and Alt
released), the key queue emptied, `ack`; **show** = the keyboards and the touch screen read again from a clean state, what was typed meanwhile
dropped, a full repaint, `ack`; **close** = `request_quit(0)`, the same exit as Esc at the root. Alt+Tab is the
launcher's: Tab with Alt held never reaches the app. A `run` app checks `cdk::hidden()` to skip work that only feeds the
screen. The headless harness plays the launcher in a scene with `lifecycle` (`headless/README.md`).
