# viz1090 for the cyberdeck: sources of the package

The package (`packages/viz1090_*.deb`) is the [viz1090](https://github.com/nmatsuda/viz1090) ADS-B display running
full screen on the Raspberry Pi Zero 2W deck and the other boards of the launcher, fed by an RTL-SDR dongle. This
folder holds what is specific to the cyberdeck; viz1090 and the decoder are built from their upstream repositories by
`build_on_pi.sh`.

| File | What it is |
| --- | --- |
| `viz_fb_shim.c` | Display and input bridge preloaded into SDL programs: software renderer, copy of every frame to the panel's framebuffer (turned and converted for rotated and 16 bpp panels), touch and keyboard turned into SDL events, the launcher's status bar, and the close button. |
| `viz_screen.h` | Which screen the app runs on (size, density, rotation, framebuffer and touch device), from the launcher's app contract, else `screen.state`, else the framebuffer, else 640x480; and viz1090's `--uiscale` for that density. Shared by the bridge and the intro screen. |
| `cp0_fb_output.[ch]` | The launcher's framebuffer output stage (rotation 0/90/180/270, 16/32 bpp, buffer-oriented touch), copied unchanged from `cyberdeck-zero-launcher/ext_components/cp0_lvgl/src/cp0` (an SDK file of the app contract). |
| `cp0_statusbar.[ch]` | The launcher's top bar (clock, Wi-Fi, Bluetooth), copied from `cyberdeck-zero-launcher/ext_components/cp0_lvgl` so that it looks exactly like the home grid. Keep in sync. |
| `vizsetup.cpp` | The intro screen: SDR and GPS check, airport code / position entry, Start. Lays out from the screen size (the 640x480 design, stretched); a notice below 640x480. `vizsetup --screen` prints the screen for `run_viz1090.sh`. |
| `run_viz1090.sh` | What the tile runs: intro screen, then a map window cut around your position, the decoder and viz1090 at the screen's size. |
| `extract_map.py`, `make_world.py` | Cut a window out of the world map / build the world map from open data (`make_world.py` needs a PC). |
| `build_on_pi.sh` | Builds everything on a Pi and stages it in `../root/` (`--bridge-only`: only the bridge, the intro screen and the scripts). |

Needs a launcher with **full-screen app support** (`X-Fullscreen=true` in the `.desktop` file): the launcher stops
drawing and gives the whole panel to the app.

## Testing without a screen

The bridge has test hooks that the package never sets: `VIZ_FAKE_FB=WxHx16|32` draws into memory instead of a
framebuffer, `VIZ_FAKE_TAPS="x,y;x,y"` plays taps (logical pixels) through the same touch mapping as a real panel,
and `VIZ_DUMP=file.bmp` writes the framebuffer (as the panel stores it, before the rotation is undone) after
`VIZ_DUMP_FRAME` frames and exits. With the contract variables (`APPLAUNCH_CONTRACT=1 APPLAUNCH_CANVAS=1280x720
APPLAUNCH_SCREEN_PPMM=1130 APPLAUNCH_SCREEN_ROTATE=0 APPLAUNCH_TOUCH_ORIENT=buffer`), `SDL_VIDEODRIVER=offscreen` and
`LD_PRELOAD=libviz_fb.so`, this renders the intro screen or the map at any size on any Pi. `VIZ_LAYOUT_REPORT=1`
makes the intro screen print every touch target and its size in millimetres.

## Writing another full-screen app

Any SDL2 program can run the same way: start it with `SDL_VIDEODRIVER=offscreen`, `LD_PRELOAD=libviz_fb.so`,
`VIZ_STATUS_BAR=1` (the launcher's top bar) and, if the app has no way to quit by itself, `VIZ_CLOSE_BUTTON=1`
(an X in the top right corner that ends the program). Touch arrives as SDL finger events (0..1 of the program's
window) and the keyboard as key and text events. Make the window the screen's size (`vizsetup --screen`, or the
contract variables): a smaller window is centred, a bigger one is cut. Add `X-Fullscreen=true` to the `.desktop` file.
