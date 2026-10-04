# viz1090 for the cyberdeck: sources of the package

The package (`packages/viz1090_*.deb`) is the [viz1090](https://github.com/nmatsuda/viz1090) ADS-B display running
full screen on the Raspberry Pi Zero 2W deck, fed by an RTL-SDR dongle. This folder holds what is specific to the
deck; viz1090 and the decoder are built from their upstream repositories by `build_on_pi.sh`.

| File | What it is |
| --- | --- |
| `viz_fb_shim.c` | Display and input bridge preloaded into SDL programs: software renderer, copy of every frame to `/dev/fb0`, touch and keyboard turned into SDL events, the launcher's status bar, and the close button. |
| `cp0_statusbar.[ch]` | The launcher's top bar (clock, Wi-Fi, Bluetooth), copied from `cyberdeck-zero-launcher/ext_components/cp0_lvgl` so that it looks exactly like the home grid. Keep in sync. |
| `vizsetup.cpp` | The intro screen: SDR and GPS check, airport code / position entry, Start. |
| `run_viz1090.sh` | What the tile runs: intro screen, then a map window cut around your position, the decoder and viz1090. |
| `extract_map.py`, `make_world.py` | Cut a window out of the world map / build the world map from open data (`make_world.py` needs a PC). |
| `build_on_pi.sh` | Builds everything on a Pi and stages it in `../root/`. |

Needs a launcher with **full-screen app support** (`X-Fullscreen=true` in the `.desktop` file): the launcher stops
drawing and gives the whole panel to the app.

## Writing another full-screen app

Any SDL2 program can run the same way: start it with `SDL_VIDEODRIVER=offscreen`, `LD_PRELOAD=libviz_fb.so`,
`VIZ_STATUS_BAR=1` (the launcher's top bar) and, if the app has no way to quit by itself, `VIZ_CLOSE_BUTTON=1`
(an X in the top right corner that ends the program). Touch arrives as SDL finger events and the keyboard as key and
text events. Add `X-Fullscreen=true` to the `.desktop` file.
