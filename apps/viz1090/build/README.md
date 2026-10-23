# viz1090 for the cyberdeck: sources of the package

The package (`packages/viz1090_*.deb`) is the [viz1090](https://github.com/nmatsuda/viz1090) ADS-B display running
full screen on the Raspberry Pi Zero 2W deck and the other boards of the launcher, fed by an RTL-SDR dongle. This
folder holds what wraps the upstream programs (kind C of the [app guidelines](../../../docs/APP-GUIDELINES.md));
viz1090 and the decoder are built from their upstream repositories by `build_on_pi.sh`. The intro screen is a native
app of the launcher's `cp0_lvgl` (kind A) with its own sources in [`../src`](../src) and [`../tests`](../tests),
cross-built in WSL by [`../build.sh`](../build.sh).

| File | What it is |
| --- | --- |
| `viz_fb_shim.c` | Display and input bridge preloaded into viz1090: software renderer, copy of every frame to the panel's framebuffer (turned and converted for rotated and 16 bpp panels) under the app header of the launcher's native pages (title, the on-screen **Esc**, the status strip; `VIZ_TITLE`), touch and keyboard turned into SDL events (the Esc button, the Esc key, SIGTERM / SIGINT / SIGHUP end the program). The X close button of earlier versions is gone: the header's Esc replaces it. |
| `../src/core/viz_screen.h` | Which screen viz1090 runs on (size, density, rotation, framebuffer and touch device), from the launcher's app contract, else `screen.state`, else the framebuffer, else 640x480; and viz1090's `--uiscale` for that density. Shared by the bridge (built with `-I ../src/core`) and `vizsetup --screen`, which prints it for `run_viz1090.sh`. |
| `cp0_fb_output.[ch]` | The launcher's framebuffer output stage (rotation 0/90/180/270, 16/32 bpp, buffer-oriented touch), copied unchanged from `cyberdeck-zero-launcher/ext_components/cp0_lvgl/src/cp0` (an SDK file of the app contract). |
| `cp0_statusbar.[ch]` | The launcher's top bar (clock, Wi-Fi, Bluetooth, battery), copied from `cyberdeck-zero-launcher/ext_components/cp0_lvgl` so that it looks exactly like the home grid. Keep in sync (`docs/dev/deck/check_statusbar_drift.py` in the launcher repo checks it; exit 0 = identical). |
| `cp0_power_supply.[ch]`, `cp0_app_contract.[ch]` | What the shim's status strip reads, as `cp0::ui::StatusStrip` does: the battery from `/sys/class/power_supply` (`cp0_power_supply_battery`) and the launcher's `status.state` feed while its process is alive (`cp0_app_status_read`), else the sysfs reader of `cp0_statusbar.c` (the Wi-Fi interface whatever its name). Copied unchanged (pure C) from the launcher at the commit of `LAUNCHER_REF`. |
| `cp0_ui_metrics.[ch]`, `cp0_ui_roller.[ch]`, `cp0_ui_status_strip.[ch]` | The launcher's layout service (design tokens per screen class and density), the native page's header geometry (`cp0_ui_roller_page`: header height, title, Esc button, strip) and the strip's placement, copied unchanged (pure C) from `cyberdeck-zero-launcher/ext_components/cp0_lvgl` at the commit of [`../../_common/LAUNCHER_REF`](../../_common/LAUNCHER_REF), so that the map's header is computed by the same calls as the intro screen's. Keep in sync with `cp0_statusbar.[ch]` when `LAUNCHER_REF` moves (last synced at d0cb456 = launcher v0.8.5: the battery in the strip, the status feed). |
| `run_viz1090.sh` | What the tile runs: the intro screen (`/opt/viz1090/vizsetup`: exit 0 = start, 1 = back to the launcher), then a map window cut around your position, the decoder and viz1090 under the app header (the map's size asked from the bridge: `VIZ_PRINT_MAP=1`). |
| `extract_map.py`, `make_world.py` | Cut a window out of the world map / build the world map from open data (`make_world.py` needs a PC). |
| `build_on_pi.sh` | Builds viz1090, readsb and the bridge on a Pi and stages them in `../root/` (`--bridge-only`: only the bridge and the scripts). |

Needs a launcher with **full-screen app support** (`X-Fullscreen=true` in the `.desktop` file): the launcher stops
drawing and gives the whole panel to the app.

## Testing without a screen

The bridge has test hooks that the package never sets: `VIZ_FAKE_FB=WxHx16|32` draws into memory instead of a
framebuffer, `VIZ_FAKE_TAPS="x,y;x,y"` plays taps (logical pixels of the whole screen, header included) through the
same touch mapping as a real panel and logs where each one landed (the Esc button, the header, or the program with its
window coordinates), and `VIZ_DUMP=file.bmp` writes the framebuffer (as the panel stores it, before the rotation is
undone, header included) after `VIZ_DUMP_FRAME` frames and exits. With the contract variables (`APPLAUNCH_CONTRACT=1
APPLAUNCH_CANVAS=1280x720 APPLAUNCH_SCREEN_PPMM=1130 APPLAUNCH_SCREEN_ROTATE=0 APPLAUNCH_TOUCH_ORIENT=buffer`),
`SDL_VIDEODRIVER=offscreen`, `VIZ_TITLE=viz1090` and `LD_PRELOAD=libviz_fb.so`, this renders the map at any size on
any Pi: ask the map's size first (`VIZ_PRINT_MAP=1 ... viz1090` prints it), then start viz1090 with
`--screensize` set to it. A tap on the Esc button (`VIZ_FAKE_TAPS` at its centre) ends viz1090 with status 0; an
evdev event file holding a KEY_ESC press as `VIZ_KEYBOARD` tests the Esc key. The intro screen is tested on the PC
instead: `../build.sh host` runs its unit tests and its headless scenes at every screen size (see the app's README).

## Writing another full-screen app

Any SDL2 program can run the same way: start it with `SDL_VIDEODRIVER=offscreen`, `LD_PRELOAD=libviz_fb.so` and
`VIZ_TITLE=<its name>` (the app header of the native pages: the title, the on-screen **Esc** that sends `SDL_QUIT`
when tapped, the status strip; without `VIZ_TITLE` the program's window fills the screen with no header). Touch
arrives as SDL finger events (0..1 of the program's window; taps on the header never reach it) and the keyboard as
key and text events (Esc as `SDLK_ESCAPE`: the program must quit on it, or on `SDL_QUIT`). Make the window the size
the bridge prints with `VIZ_PRINT_MAP=1 LD_PRELOAD=libviz_fb.so <the program>` (width, height under the header,
header height): a smaller window is centred under the header, a bigger one is cut at the right and bottom. Add
`X-Fullscreen=true` to the `.desktop` file.
A new app with its own screens is better written as a native app from
[`apps/cyberdeck-app-template`](../../cyberdeck-app-template/README.md).
