# Net Walk (`net-walk`)

A network-wiring puzzle for the cyberdeck, played by touch or keyboard: turn the tiles until the server's power reaches every
computer. Native fullscreen app on the launcher's `cp0_lvgl` (like Mine Field and Klondike Solitaire), MIT, no network, no
hardware. Written from scratch: no code, art or text of other programs of its kind. Drawn in the look of Palm OS 4 colour
(2001) inside the launcher's shared page header: a #CCCCCC window with white and #666666 bevels, bold bitmap text, navy dialog
title bars; the computers are beige CRT monitors and the server is a grey rack tower (see Devices).

## Screenshots

`screenshots/` (rendered by the PC harness, `build.sh host`): `01-deck-480x320`, `02-powered-640x480`, `03-win-800x480`,
`04-size-480x320`, `05-scores-640x480`, `06-expert-1024x600`, `07-portrait-480x800`, `08-portrait-win-480x640`.

## What it does

- **The board.** Tiles of four kinds, each with a fixed type: a computer (one cable end), a straight, a corner and a T. The server
  is in the middle and is a T or a cross. Playing only turns tiles; no tile ever changes type.
- **Power flows live.** From the server through every cable end that the next tile answers: reached cables are green and reached
  computers light up (blue to lime), the rest stay dark grey.
- **The win** is a property of the board state, not a comparison with a stored solution: every tile is reached by the power (so every
  computer is connected and every tile is used) and no cable end points at nothing or at a tile that does not answer it. Loops are
  allowed, so any valid wiring wins, not only the one the generator started from. A short flash of the cables, then the "Network up!"
  window with the moves and the time, and OK for the next board.
- **Sizes.** Easy 5 x 5, Normal 7 x 7, Hard 9 x 9, Expert 9 x 9 with wrap (the edges meet: a cable off the right edge comes in at the
  left; the frame is dashed). The app opens on the size you played last.
- **Moves, time, best.** Moves counts every quarter turn that changes a tile (a locked tile does not turn; a cross turns into itself
  and is not counted). The clock starts with the first move and stops at the win. The panel shows the best moves of the size; the
  Scores window shows the best moves and the best time of each size (the two records are independent).
- **Padlock.** A tile you are sure of can be locked: it does not turn until unlocked.
- **Generator.** A seeded random spanning tree grown from the server (growing-tree: the newest cell or a random one, with a bias against
  long straight runs; the server has three or four cables, any other tile at most three) gives the solved orientation of every tile,
  which the game keeps (for a later hint). Then every tile is turned by 0 to 3 quarter turns (rotation only) and the board is
  scrambled again if it happens to be a win already.

### Touch and keys

| Action | Touch | Keys |
|---|---|---|
| Turn clockwise | tap the tile | Enter or Space on the ring |
| Turn counter-clockwise | press the tile 0.35 s or more, then let go | C |
| Padlock | hold the tile 0.6 s | L |
| Move the ring | | arrow keys |
| New game | New game button | N |
| Size, Scores | the buttons | |
| Close a window | OK, Cancel | Esc |
| Leave | the header's Esc button | Esc on the board (the header's Esc button does the same: it closes an open window first) |

A finger that moves more than a few pixels is a drag and does nothing. In the Size list a tap acts at once, and arrows move
a highlight, Enter acts. Right and middle mouse buttons are **not** bound: the toolkit's pointer path carries only the left button;
the long press and the hold (and C and L) do the same.

### Row markers

The Size window follows the launcher's row-marker rule: every row, which does something, carries a dot (a white dot in a grey
ring while idle); the current size has a check. The Scores window is information only: its values are grey and its rows carry no
marker. (There is no menu any more: New game, Size and Scores are buttons, Quit is the header's Esc.)

### Devices

The computers and the server are drawn as small pixel-art devices in the middle of their tile, always upright; only the cable
turns, and it runs behind the device so the stub that leaves the device shows which sides are wired (`src/core/sprites.*`, unit
tested). A **computer** is a beige CRT monitor (bevelled grey bezel, dark outline, a stand): its screen is **blue** with no power
and **lime** with two text lines, a glare pixel and a bright LED once the power reaches it. The **server** is a dark rack tower
with a row of status LEDs (green, one amber), a red power button (the only red), vent slots, two drive bays with handles and
feet. Two detail levels by the tile size: under 19 logical pixels the small devices (monitor 9 x 8, server 7 x 10, the 9 x 9
board at 480x320), from 19 the full ones (13 x 14 and 11 x 13), repeated 2x, 3x on a tile with room for it. The padlock and the
key cursor are drawn over the devices.

### Layout, landscape and portrait

The launcher's **shared page header** (the Esc button, the title "Net Walk" and the status strip with the clock, battery,
Bluetooth and Wi-Fi: `cp0::ui::Page` and `StatusStrip`, the same as in Mine Field and Klondike Solitaire) is on top; its height
is the layout service's (40 px at 480x320, 56 px at 640x480, 800x480, 1024x600 and the portrait canvases, 64 px at 720x720 and
1280x800). The **body** under it is one object that draws everything from a **logical canvas** scaled by a whole number, so every
edge is crisp; there is no font file and no image (tiles, bevels, devices and the bold text are rectangles). The scale is the
largest whole number at which the canvas still holds the body minimum (landscape 240 x 136, portrait 160 x 200 logical pixels);
the canvas takes the whole body (under one logical pixel of grey letterbox per side) and the tiles are the largest whole number of
logical pixels that fits. A near-square body (within 5:4, the 720x720 screen) takes the orientation that gives the larger tiles.

| Screen | Header | Body | Scale | Canvas | 9 x 9 tile | before the header |
|---|---|---|---|---|---|---|
| 480x320 | 40 | 480x280 | 2 | 240x140 | 14 (28 px) | 15 (30 px) |
| 640x480 | 56 | 640x424 | 2 | 320x212 | 22 (44 px) | 24 (48 px) |
| 800x480 | 56 | 800x424 | 3 | 266x141 | 14 (42 px) | 15 (45 px) |
| 1024x600 | 56 | 1024x544 | 4 | 256x136 | 14 (56 px) | 19 (57 px) |
| 1280x800 | 64 | 1280x736 | 5 | 256x147 | 15 (75 px) | 15 (75 px) |
| 720x720 | 64 | 720x656 | 3 | 240x218, portrait layout | 17 (51 px) | |
| 480x800 portrait | 56 | 480x744 | 3 | 160x248 | 16 (48 px) | 16 (48 px) |
| 480x640 portrait | 56 | 480x584 | 2 | 240x292 | 25 (50 px) | 15 (45 px) |
| 800x1280 portrait | 56 | 800x1224 | 5 | 160x244 | 16 (80 px) | 16 (48 px at scale 3) |

At 480x320 the header takes 40 of 320 px, so the body is 280 px high and no longer holds the earlier 240 x 160 canvas: the
minimum is now 240 x 136 and the 9 x 9 tile went from 15 to 14 logical px (30 to 28 px). The sizes are listed as measured by the
harness (`@hdr`, `@body`, `@scale`, `@tile` probes). At 480x320 the buttons stay 40 px high and the 9 x 9 tile loses 2 px (28 px,
about 2.5 mm on a 4.3" panel).

Landscape: the board at the left, the panel (level, Moves, Time, Best, then New game, Size, Scores) at the right. Portrait
(`X-Portrait=480x640`): the panel under the board in two lines, the three buttons side by side. The layout arithmetic is in
`src/core/layout.*` and unit tested. The launcher's volume and brightness keys are not used by the app.

## The engine (`src/core/`, no LVGL, unit tested)

| File | What |
|---|---|
| `board.*` | tiles as 4-bit masks, rotation, kinds, neighbours with wrap, the power flood from the server, the win check by state, the generator |
| `game.*` | the levels, one game: moves, locks, the clock, the frozen board after the win |
| `scores.*` | best moves and best time per size, `scores.json` in the data folder, the last size; a damaged file reads as empty |
| `layout.*` | the whole-number scale, the tile size, every rectangle of the body, hit testing |
| `sprites.*` | the pixel art of the monitor and the server at two detail levels, their colours (monitor blue / lime) |
| `pixfont.*` | the bold bitmap font (own glyphs on a 5 x 8 grid, made bold by a one-pixel smear; advance 7) |
| `rng.hpp`, `fsutil.*` | SplitMix64; atomic file writes |

## The UI (`src/ui/`)

`netwalk_app.*` is the screen: the launcher's page header (`cp0::ui::Page`, its Esc button calls the same `back()` as the Esc key)
and, under it, one body object that draws everything and takes the pointer and key events (it starts below the header, so
nothing interactive reaches under it); `painter.hpp` holds the Palm OS 4 palette and the rectangle
primitives; `app_spec.cpp` gives the runners the id, title, minimum screen (`X-MinSize=480x320`) and portrait canvas
(`X-Portrait=480x640`).

## Files the app keeps

`$NETWALK_DATA`, else `$XDG_DATA_HOME/net-walk`, else `~/.local/share/net-walk`: `scores.json`
(`{"version":1,"last":"normal","best":{"easy":{"moves":12,"seconds":31}, ...}}`, 0 = no score yet). A game in progress is not saved.

## Multitasking

`X-Background=freeze`: hidden, the app is stopped; the board and the clock are kept in memory and the clock does not count the time
it was frozen (a gap of more than a second between two clock ticks is dropped).

## Requirements

A screen of at least 480x320 (portrait 480x640 in PDA mode). Touch or keyboard. No network, no sound.

## Known limits

- Right and middle mouse clicks are not bound (above). A mouse works as a finger.
- No sound, no hint button yet (the solved orientation of every tile is kept for one).
- A game in progress is not saved; a new game (button, N) starts at once without asking.
- The tile size is what the body allows: at 480x320 a 9 x 9 tile is 28 px (28 px).

## Credits and licences

<!-- branding-allow-begin: credits -->
The game and the screen are this app's own code (MIT, 2026 OSRdesign), written from scratch from the public idea of a
network-wiring puzzle: no code, art, names or text of any other program of its kind; the icon is drawn by `tools/make_icon.py`;
no font file or image is bundled (the bold bitmap glyphs are the app's own, in `src/core/pixfont.cpp`). The generator uses the
SplitMix64 mixing function (Sebastiano Vigna, public domain algorithm), implemented from its published description. The binary links
the cp0_lvgl runtime of the CardputerZero launcher by M5Stack (MIT) and LVGL (MIT): see
`root/usr/share/doc/net-walk/copyright`. An independent hobby project, not affiliated with M5Stack.
<!-- branding-allow-end -->

## Build and test

```
wsl -e sh <repo>/apps/net-walk/build.sh host                 # unit tests and the scenes at nine sizes
wsl -e sh <repo>/apps/net-walk/build.sh cross --sync         # the aarch64 binary into root/
wsl -e sh <repo>/apps/net-walk/build.sh cross --arch amd64   # the x86-64 binary into root.amd64/
```

Build from a WSL-native copy with LF line ends and a fresh build folder (`CDK_BUILD`, `LVBUILD`), as described in
[`apps/_common/README.md`](../_common/README.md). The unit tests (`tests/test_*.cpp`, one binary) run, for each of the four sizes,
2500 seeds: the solved orientation wins; undoing the recorded scramble wins; a scrambled board never starts as a win; every tile is
used and the layout is a spanning tree; types never change; the server is a T or a cross and nobody else is a cross; wrap boards use
their edges and flat boards never point off the board; a hand-made ring (a loop, not the generator's tree) wins; power cases (chains,
gaps, islands, wrap on both axes, tees and crosses); the game's moves, locks and clock; the scores file; the layout at eleven bodies and the device sprites;
the font table. The scenes (`tests/scenes/`) go through touch, keys, a win, Size, Scores and the header (Esc button, title, status strip) at nine sizes
(480x320 to 800x1280) with invisible probe labels (`NETWALK_TEST_PROBES`), see `tests/BOARD-TESTS.md` for the board protocol.

## Changes

- 0.1.0: first version (shared page header, pixel-art monitors and server, magenta icon). `app.json` carries `"draft": true` (tools/make_registry.py skips the app) until it is published.
