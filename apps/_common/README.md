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
| `app.{hpp,cpp}` | `AppSpec` (id, title, minimum screen = `X-MinSize`, UI builder), `start()` / `stop()` (what both runners do once the display exists), `request_quit()`, the keypad input group, the "needs WxH" notice for a screen under the minimum (an older launcher ignores `X-MinSize`) |
| `platform.{hpp,cpp}` | the device runner `cdk::run()`: cp0_lvgl's display backend switched to the native display, every keyboard read, SIGTERM / SIGINT / SIGHUP end the loop cleanly, `--version`; started by hand it still uses the launcher's backend (`APPLAUNCH_DISPLAY=dpi-scaled`) |
| `osk_policy.{hpp,cpp}` | the app-side on-screen keyboard rule: `cp0_app_osk_allowed()` on `input.state`, looked at once a second at most and parsed again only when the file changed (a new `gen`); without the file `APPLAUNCH_OSK` and the keyboards of `/proc/bus/input/devices` |
| `async_run.{hpp,cpp}` | background commands: fork + exec in their own process group, lines and the end delivered on the LVGL thread in order (`lv_async_call` under `lv_lock()`), timeout, cancel, PDEATHSIG, `shutdown()` at exit |
| `build_info.hpp` | the version stamp (`cdk_build_info.h` from `build_app.sh`) |
| `build_app.sh` | cross (aarch64, stripped, into the app's `root/`) and host (headless harness, unit tests, scenes, contact sheets) builds in WSL, the launcher pin check, the stamp |
| `LAUNCHER_REF` | the launcher commit the apps build against |
| `scons/` | the scratch project's scons files: the SDK's project `SConstruct`, the component (`main.SConstruct`: cp0_lvgl + LVGL static, the launcher's FreeType glue, `-O2`, `--as-needed`) and the cross configuration (cp0_lvgl with the filesystem service only) |
| `headless/` | the PC harness (see its README): a plain LVGL display at any size, scripted keys and taps, PNG shots, layout checks, contact sheets |
| `tests/test_common.cpp` | unit tests of the keyboard rule, the command runner and the minimum-size rule |

## Rules every app follows

- `.desktop`: `X-Fullscreen=true`, `X-MinSize=640x480` (decision 2026-10-13), the same numbers in `AppSpec`.
- Layout from the layout service only (`cp0_ui_metrics_get()`, `cp0_ui_roller_page()`, the widgets): no pixel constant.
- Keyboard first, every action by keys and by touch; a short Esc at the root page ends the app; the 3 s Esc hold is the
  launcher's.
- Commands through `async_run` (never `system()` or a blocking `popen()` on the LVGL thread).
- Build: `build.sh` of the app calls `build_app.sh`; the binary links cp0_lvgl and LVGL statically and needs only
  libinput, libxkbcommon, libfreetype, libstdc++ and libc at run time.

## Status strip and title

Not copied here: apps use the launcher's own `cp0::ui::StatusStrip::add(page->header())`, `cp0::ui::add_title` (the
`Page` title factory) and `cp0::ui::status_strip_left()` (the `strip_left` of `cp0_ui_roller_page()`) from
`cp0_ui_widgets.hpp`, pinned by `LAUNCHER_REF`. The strip's default source reads sysfs / procfs; fonts come from the
launcher's `/usr/share/APPLaunch/share/font`. Moving `LAUNCHER_REF` moves the strip: rebuild every app and compare the
header shots with the launcher's.
