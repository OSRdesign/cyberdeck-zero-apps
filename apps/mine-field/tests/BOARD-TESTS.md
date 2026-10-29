# Mine Field: board tests (the main path, short)

For the owner (controller-led testing: Claude deploys and reads logs, you do the physical steps). The UI is tested on the PC
(`build.sh host`: unit tests and the scenes at eight sizes); what the PC cannot tell is **how it feels on the glass and on a
Pi**, and how long a fair board takes to prepare. Keep it short: the main path must work, the rest is a bonus. Expected pictures:
the contact sheets in `~/cdk-build/mine-field/host/out/sheets/` (or the build dir you gave `CDK_BUILD`).

Boards: the deck (640x480, keyboard and touch), the Pi 3A+ with the 4.3" DSI (800x480, and portrait 480x800 in PDA mode), the
Pi 4B with the 5" DSI (800x480), the FZ-M1 tablet (800x1280 portrait), the HackBerry (720x720).

## T0. The whole board is on the screen (every board, 1 min)

No board ever scrolls, pans or zooms. On each board check, for Beginner, Intermediate and Expert (Menu > Difficulty): the whole
grid is visible, nothing under the header or the dock, cells are squares, each harder level has more columns, rows and mines and
smaller cells. Read the sizes the Difficulty page shows and compare with the README table (deck: 5 x 6, 6 x 6, 8 x 7).
Landscape: the dock column at the right holds Mines, Time and the four buttons (New game, Dig / Flag, Hint, Menu), all visible,
none cut. Portrait: the strip over the board and the three buttons under it.

## T1. Touch: dig, flag, chord (3 min, any touch board)

1. Open Mine Field: a Beginner board, the dock (landscape: readouts Mines and Time over the New game, Dig, Hint and Menu buttons).
   Note whether a cell is easy to hit with a fingertip (5 to 9 mm).
2. Tap a cell: it digs and an area opens, the clock starts.
3. Tap the Dig button: it says Flag and fills. Tap a cell: a flag; tap it again: gone; "Mines" counts down and up.
4. Back to Dig. **Long-press** a cell (about half a second, keep the finger still): it flags (a short ring shows). In Flag mode a
   long press digs. Tap a flag in Dig mode: nothing happens. Drag a finger over the board: nothing digs or flags.
5. Flag the mines around an opened number and tap the number: its neighbours open. Tap a number with too few flags: the
   neighbours flash amber and "Flags do not match" shows for about two seconds.
6. Hint: a ring on a cell and "Safe: dig here" or "Mine here: flag it" with "+10 s" over the edge of the board (never over the
   hinted cell); Time jumps by 10.
7. The New game button (refresh glyph) asks first once a cell was dug.

Report: the board, whether the long press feels right (too slow / too fast), mis-taps, whether the smallest cells (Expert) are
usable.

## T2. Keys only (2 min, a board with a keyboard)

Arrows move a blue ring; Enter or Space dig, F flag, C chord (or Enter on a number), H hint, T the tap mode, N new game (asks once a
cell was dug), M or Ctrl+O the menu, Esc leaves: it closes a dialog, then a page, then a short Esc in the game ends the app. Every
action must be reachable without touching the screen.

## T3. Fair boards: preparation time (the slowest board, 2 min)

Settings > Fair boards > On. New game, first dig on Expert (the worst case) and on Intermediate. "Preparing board..." shows and
the board ignores taps meanwhile. Note the **seconds until the board opens** on each board (the PC does it in well under a
second; the budget is 200 boards or 4 million solver steps, then the last board is played, unproven). The app must not freeze.
Leave the app while it prepares: it must end at once.

## T4. Win and loss (3 min)

Beginner: win by playing (or by hints). The remaining mines get flags, a light band sweeps over the board, then "You won!" with the
time and "New best!" (a first win), New game / Same board / Close ("Replay" on a narrow portrait panel). Same board starts the very
board again, first dig included. Lose on purpose: the mines show nearest-first, wrong flags are crossed, the dug mine is red; "Mine
hit": New game / Try again / Close. A win with a hint says "Hints used: no best time". Menu > Best times: the time is listed with
the board size, a short seed and "fair" when the board was; Menu > Statistics: played, won, win rate, streaks.
Settings > Animations Off: the same, at once.

## T5. Resume and the old save (2 min)

Dig a few cells, flag one, press Esc until the app ends. Open it again: "Game resumed", the same board, flags and clock. Hide it with
Alt+Tab (multitasking boards) and come back: the clock did not run while hidden.
After updating from the first build of the app: the old saved game (a fixed 9x9, 16x16 or 30x16 board) is dropped with
"Saved game dropped: screen changed" (no loss counted) unless the screen happens to give that very board; the old statistics are
gone (set aside as `stats.json.bad`); the settings are kept.

## T6. Portrait (2 min, a portrait run)

PDA mode (Settings > Screen): the strip over the board, three buttons along the bottom, the header narrow; all three levels fit.
Turn PDA mode off and on with a game in progress: the board is another shape, so the saved game is dropped with the one-line note
(no loss counted) and a fresh board shows.
