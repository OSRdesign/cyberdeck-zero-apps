# Mine Field (`mine-field`)

A mine-clearing puzzle for the cyberdeck, played by touch or keyboard: dig the safe cells, flag the mines, clear the
board. Native fullscreen app on the launcher's `cp0_lvgl` (like Klondike Solitaire), MIT, no network, no hardware.
Written from scratch: no code, art or text of other programs of its kind.

## Screenshots

`screenshots/` (rendered by the PC harness, `build.sh host`): `01-deck-640x480`, `02-pi4b-dug-800x480`,
`03-portrait-board-480x800`, `04-expert-portrait-480x800`, `05-hint-640x480`, `06-loss-640x480`, `07-win-640x480`,
`08-settings-640x480`.

## What it does

- **Boards that fit the screen.** There is no panning, zooming or scrolling: the board always fits the area under the page header
  (left of the dock in landscape, between the strip and the dock in portrait). The three difficulties are derived from that area:
  Expert is the largest grid of 5 mm cells that fits (at most 30 x 30, at least 6 x 6), Intermediate has about 78 % of Expert's
  columns and rows, Beginner about 55 %, and the mines are 12 % (Beginner), 16 % (Intermediate) and 20.5 % (Expert) of the cells.
  The grid is not forced square; the cells are, and an easier level gets bigger cells (up to 12 mm). The Difficulty page shows
  each level's size on this screen. On a very small screen where rounding would leave two levels the same size, the lower one
  loses a row or column (down to 5) so that Beginner < Intermediate < Expert always holds.
- **First dig.** The mines are laid on the first dig, never on that cell or its eight neighbours, so it always opens
  an area. The layout is a pure function of the seed and that first cell.
- **Fair boards** (Settings). Only boards the built-in solver can finish by logic, never a guess, are laid. The search can
  take a moment on a slow board (the status line says "Preparing board..." and the board waits); if no such board is
  found within its budget the last one is played anyway (still a board, only not proven).
- **Hint.** Rings a cell that is safe for sure ("Safe: dig here") or a mine for sure ("Mine here: flag it"), else says
  "Nothing certain here" (no cost). A hint costs 10 s ("+10 s") and the game can no longer make a best time
  ("Hints used: no best time"). Not before the first dig.
- **Winning** flags the remaining mines, sweeps a light band over the board, then shows the result: time, "New best!" or
  the rank in the best five of that size ("Not in the best 5"), and New game / Same board (Replay on a canvas under
  560 px wide) / Close. **Losing** shows the mines nearest the dug one first, the wrong flags crossed, the dug mine red,
  then New game / Try again / Close. Same board and Try again play the very board again, first dig included. Animations
  Reduced or Off show everything at once.
- **Scoring.** The time in seconds (clock plus 10 s per hint), shown to 999. Best five times per size (Beginner,
  Intermediate, Expert) with its board size, a short seed and a "fair" tag; a game with hints never makes a best time. Statistics per size: played, won, win rate, streaks, total time. A game left after the first dig (a new game,
  another size) counts as lost; one never dug costs nothing.
- **The game in progress** is saved after every action and when the app is left; the next start brings it back
  ("Game resumed").

### Touch and keys

| | Touch | Keys |
|---|---|---|
| Dig / flag | tap a hidden cell: it digs, or flags when the Dig / Flag button says Flag | cursor + Enter or Space dig, F flag |
| The other one | long press (0.4 s, held still) on a hidden cell: flags in Dig mode, digs in Flag mode | |
| Chord | tap an opened number: opens its neighbours when the flags around it equal it; else the neighbours flash and "Flags do not match" shows | C, or Enter on a number |
| Flag, Dig mode | a tap on a flagged cell does nothing (no accidental dig) | Enter on a flag says so |
| Mode | the Dig / Flag button (filled when Flag) | T |
| Hint | the Hint button | H |
| New game | the refresh button (asks first when a cell was dug) | N |
| Menu | the Menu button | M or Ctrl+O |
| Leave | the header Esc | Esc (a dialog and the pages go back first; a short Esc in the game ends the app) |

A finger that moves more than 10 px is neither a tap nor a long press, so a slip never digs or flags.

### Cells are 5 to 12 mm, by design

Cells are squares of about 5 mm (the hardest level on a small screen) to 12 mm (an easy level on a large one); the 9 mm button
target does not apply to cells. A long press and the chord exist so that a mis-tap costs little (a tap on a flag never digs).
Buttons are 7 mm or more (11 mm where the screen allows).

### Layout, landscape and portrait

- **Landscape** (640x480, 800x480, 720x720, 1024x600, 1280x720): no strip. A dock column at the right edge, 14 mm wide (a little
  more where the labels would not fit, 14.4 mm at the widest): the two readouts stacked on top (mines left, which can go negative;
  the clock in seconds up to 999, hidden by Settings > Show timer), then four buttons: New game (the refresh glyph), Dig / Flag,
  Hint, Menu, each 7 to 11 mm tall as the height allows (the deck gets 7.3 mm, a 4.3 inch DSI 9.8 mm, a Pi 4B 10.9 mm). The board
  takes the whole area left of the dock. The hint and notice line is drawn over the board's top edge for about 2.5 s (over the
  bottom edge when it would cover the hinted cell) and takes no space of its own.
- **Portrait** (`X-Portrait=480x640`; 480x800, 480x640, 800x1280): a strip over the board (mines left, the new-game button, the
  clock) and three buttons (Dig / Flag, Hint, Menu) along the bottom; the notice line is drawn over the bottom of the board.
- **What the screens give** (columns x rows, mines, cell size):

| Screen | Beginner | Intermediate | Expert |
|---|---|---|---|
| deck 640x480, 11.3 px/mm | 5 x 6, 4 mines, 6.1 mm | 6 x 6, 6, 6.1 mm | 8 x 7, 11, 5.1 mm |
| Pi 4B 5 inch 800x480, 7.41 px/mm | 10 x 6, 7, 9.2 mm | 14 x 9, 20, 6.2 mm | 18 x 11, 41, 5.0 mm |
| Pi 4B 5 inch portrait 480x800 | 7 x 8, 7, 9.1 mm | 9 x 12, 17, 6.5 mm | 12 x 15, 37, 5.3 mm |
| 4.3 inch DSI 800x480, 8.5 px/mm | 8 x 6, 6, 8.1 mm | 12 x 7, 13, 6.4 mm | 15 x 9, 28, 5.2 mm |
| 4.3 inch DSI portrait 480x800 | 6 x 7, 5, 9.1 mm | 9 x 10, 14, 6.1 mm | 11 x 13, 29, 4.9 mm |
| tablet 800x1280, 8.6 px/mm | 10 x 13, 16, 9.2 mm | 14 x 19, 43, 6.3 mm | 18 x 24, 89, 5.0 mm |

### Settings

Fair boards (with its one-line explanation: "Never needs a guess; may take a moment to prepare"), Animations (On, Reduced,
Off: the sweep, the mine sequence and the hint pulse), Show timer, Flag mode on start (at the app's start), Reset
statistics (asks first). A board setting changed in a game in progress starts with the next game.

### A saved game and the screen

A saved game keeps its own board. If the screen now gives its difficulty another size (the screen changed shape, for example PDA
mode on or off), it is dropped without a loss record and "Saved game dropped: screen changed" shows once. Files written by the
first build of the app: its saved game (a fixed 9x9, 16x16 or 30x16 board) is read and, unless the screen happens to give that
board, dropped the same way; its settings are read (a Custom difficulty becomes Beginner); its statistics file is set aside as
`stats.json.bad` and the app starts with empty statistics (its best times had no board size and measured other boards).

## The engine (`src/core/`)

`board` (cells, flood fill with an explicit stack), `generator` (mine layouts, the fair search), `solver` (the logic
behind hints and fair boards), `game` (lifecycle, chord, loss and win, the clock, hints, the saved game, `prepare()` to lay
the mines on another thread), `stats`, `settings`, `store` (atomic files), `session` (what counts as a played game),
`view_layout` (the boards that fit a screen, the cell size, the hit test). No LVGL: `tests/` (85 000 checks).

## The UI (`src/ui/`)

| File | What |
|---|---|
| `minefield_app.{hpp,cpp}` | the game screen, input, clock, first-dig worker, win and loss sequences, result panel, test fixtures |
| `minefield_pages.cpp` | menu, difficulty, best times, statistics, settings |
| `board_view.{hpp,cpp}` | the board widget |
| `palette.hpp` | every colour, from the toolkit's tokens where there is one |
| `app_spec.cpp` | the `AppSpec` of the runners |

**Drawing.** One LVGL object draws the whole board in its draw event, only the cells in the area LVGL asks to redraw, with
rectangles, lines, triangles and a label for the digit; no object per cell. A 30 x 30 board as 900 objects would mean 900 styles,
layout passes and draw tasks on a Pi Zero 2 W for nothing. A change invalidates only the cells that changed (a one-byte code per
cell is compared with the last drawn); a win sweep redraws the board once per step. Hidden cells are raised tiles (lit top and left
edge), opened cells flat; digits 1 to 8 use a colour-blind-safe set (Okabe-Ito, lightened) and are always also the digit; a flag
is a triangle on a pole, a mine a disc with spikes, a wrong flag a flag with a cross, the dug mine's cell red.

## Files the app keeps

`$MINEFIELD_DATA` or `~/.local/share/mine-field/`: `stats.json`, `game.json` (the game in progress, removed when it ends);
`~/.config/mine-field/settings.json`. Written atomically; a file that cannot be read is set aside as `.bad` and the app
starts that part afresh.

## Multitasking

`X-Background=freeze`: stopped while hidden; the clock does not count the time it was frozen; the game is saved when hidden.

## Requirements

A screen of at least 640x480 landscape or 480x640 portrait (PDA mode); a launcher with full-screen app support. arm64 and
amd64. No special hardware, no network.

## Known limits

- No undo (a mine is final), no question-mark marks.
- On a small screen the hardest level is small (the deck: 8 x 7): the boards are what fits at 5 mm cells.
- On a very slow board a fair board can take seconds to prepare (the board shows "Preparing board..."); no ARM timing was
  measured in the PC tests.

## Credits and licences

<!-- branding-allow-begin: credits -->
The engine and the screen are this app's own code (MIT, 2026 OSRdesign), written from scratch from the public idea of a
mine-clearing puzzle: no code, art or text of any other program of its kind; the icon is drawn by `tools/make_icon.py`, no
third-party assets, fonts or images are bundled. The mine layout generator uses the SplitMix64 mixing function (Sebastiano
Vigna, public domain algorithm), implemented from its published description. The binary links the cp0_lvgl runtime of the
CardputerZero launcher by M5Stack (MIT) and LVGL (MIT): see `root/usr/share/doc/mine-field/copyright`. An independent hobby
project, not affiliated with M5Stack.
<!-- branding-allow-end -->

## Build and test

```sh
wsl -e sh /mnt/c/CLAUDE/zero7/apps-minefield/apps/mine-field/build.sh host --strict   # unit tests, every scene at every size
wsl -e sh .../build.sh cross                      # arm64 into root/
wsl -e sh .../build.sh cross --arch amd64         # x86-64 into root.amd64/
python3 tools/make_registry.py --only mine-field --out <dir>   # the .deb packages
python3 apps/mine-field/tools/make_icon.py        # the icon
```

Scenes (`tests/scenes/`): the three levels at eight sizes and the dock at 7.41 and 8.5 px/mm (every scene checks that the board
lies inside its area: no `@OVERFLOW` label), first dig, modes, long press, chord (matching and not), hints (safe, mine, none, early,
Reduced), wins (sweep, static, rank, hinted, same board, new game), losses (animated, Reduced, close, try again), keys only, every
page, settings, new-game confirm, the preparing board, resume, the dropped saved game (another screen density, an old save), the
deck, the Pi 4B, the 4.3 inch DSI and the tablet (`ppmm`). Fixtures are environment variables read by the app only to build a
scene (`MINEFIELD_TEST_*`: data folder, seed, settings, fixed board sizes instead of the screen's, a setup script, a statistics
file, an old-version save, probes that a scene taps by name `@c3,4`, `@safe`, `@mine`, `@num`, `@flag`, `@new`). The harness's `hold TEXT MS`
presses and holds (long press). The board tests for the owner: [`tests/BOARD-TESTS.md`](tests/BOARD-TESTS.md).

## Changes

0.1.0: first version.
