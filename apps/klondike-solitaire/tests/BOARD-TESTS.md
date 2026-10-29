# Klondike Solitaire: board tests (the main path, short)

For the owner (controller-led testing: Claude deploys and reads logs, you do the physical steps). The UI is tested on
the PC (`build.sh host`: unit tests and 170 scene runs at seven sizes); what the PC cannot tell is **how it feels on the
glass and on a Pi**. Keep it short: the main path must work, the rest is a bonus. Expected pictures: the contact sheets in
`~/cdk-build/klondike-solitaire/host/out/sheets/` (`a-fresh.png` ... `o-number-band.png`).

Deploy as for any app (package from `tools/make_registry.py --only klondike-solitaire` once the release task has built
it; published as 0.1.0, installable from the Store). Boards worth trying: the deck (640x480, keyboard
and touch), the Pi 3A+ with the 4.3" DSI panel (800x480, touch only), a portrait run (PDA mode, 480x800), the FZ-M1 tablet
if the amd64 package is built.

## T1. Touch: the table and a drag (3 min, any touch board)

1. Open Klondike Solitaire. A deal shows: the seven columns, the stock top left, four empty foundations (A), the bar of
   Undo / Redo / Hint / Menu at the side (landscape) or the bottom (portrait). Undo and Redo are grey.
2. Tap the stock: a card goes to the waste, "Moves 1". Tap again a few times. When the stock is empty its place shows a
   turn-again arrow; tap it: the waste goes back.
3. **Drag** a face-up card (or a run) to a column it fits: it follows the finger, lifted with a shadow, and snaps in. Drag
   it onto nothing: it flies back. Note whether the drag looks smooth (about 20 frames a second or better is the goal on a
   Pi 3A+ and a Zero 2 W) and whether a card is easy to pick up in a long column.
4. Tap a card (blue frame), tap its destination: it moves. Double-tap a card that fits a foundation: it goes up.
   **Stacking check:** after a few moves of a run (K Q J ...) onto an empty column and back with Undo / Redo, every column shows each card OVER the one before it (a later card never hidden behind an earlier one), and the dock labels (Undo, Redo, Hint, Menu) and the top line (Score / Time / Moves / Deal) are whole, not clipped.
5. Undo, Redo (they work while grey = nothing to do). Hint: an amber frame on the card and where it goes.

Report: the board, the drag smoothness (good / sluggish / stuttering), any mis-taps on the fanned columns, the size of the
cards on that panel (readable index? pips?).

## T2. Keyboard play (2 min, a board with a keyboard)

1. Arrows move a cyan frame. Down from the top row goes into a column; Up leaves it again. Enter picks up (blue frame),
   Enter on another pile puts it down. Enter twice on a card that fits sends it to its foundation.
2. U undo, R redo, H hint, N new game (asks first once a move was made), M or Ctrl+O the menu, Esc drops a pick and then
   goes back; a short Esc at the table quits.

## T3. Portrait (2 min, a portrait run)

PDA mode on a portrait launcher (Settings > Screen): the game must be **upright** (the card columns fill the width, the
four buttons in a row at the bottom, the header narrow). Turn PDA mode off and on again with a game in progress: the same
game comes back. Menu > Settings > Left-handed: the top row and (in landscape) the button column swap sides at once.

## T4. Resume (1 min)

Play a few moves, leave with Esc (or Alt+Tab to another app and back: the clock must not run while away). Open the app
again: "Game resumed", the same cards, the same Moves, Undo still works. Kill the app (hold Esc 3 s) and reopen: the game
is back (it is saved after every move).

## T5. Settings and statistics (2 min)

1. Menu > Settings: Draw 3 cards, Scoring Vegas (values turn amber: they apply from the next game). Back: with a game in
   progress a box asks "New rules ... Later / New game". Later keeps the old rules for this game; Menu > New game (asks:
   "counts as a lost game") then plays draw 3 / Vegas (HUD shows Score and Bank).
2. Menu > Statistics: pick the variation (Draw / Passes / Scoring rows): the abandoned game shows as played 1, won 0 for its
   own variation only. Reset this variation (asks first).
3. Look: Felt, Card back, Four-colour, Animation Off (cards jump).

## T6. Winnable deal (1 min, a Pi Zero 2 W or Pi 3A+)

Menu > Winnable deals On, then Menu > New game. A notice "Looking for a winnable deal..." shows and the old table stays
usable; the new game appears in well under a few seconds with draw 1 (draw 3 with 1 pass can take longer). Note the time
and whether the board stayed responsive (the search is a helper process: `pgrep -af klondike-worker` while it runs).

## T7. Dead end (optional)

A game that cannot be won any more shows "No more useful moves. Undo, or start a new game." within a second of the last
move.

## T8. The win cascade (3 min, a Pi 3A+ or Zero 2 W first, then the deck)

Easiest to get to: Menu > Settings > Look > Animation On, then play a game to the end, or take a game that is one move
from winning (menu > New game by deal number: pick a deal you have solved, or play Menu > Winnable deals and let Auto
finish it). The box "You won!" must **not** appear at once.

1. **What you should see (Animation On, about 15 s).** The four kings leave the foundations first, then the queens, one
   pile after the other; each card is thrown to the side, falls, bounces on the bottom of the board a few times with a
   smaller bounce each time and runs off the side, and **leaves a picture of itself at every step**, so the board fills
   with arcs of cards, like the classic cascade. Up to 8 cards fly at once. The Score / Moves line and the buttons stay
   clean on top; the cards pass under them. Then the result box (Close, Statistics, New game) comes up over the last
   picture; **Close** puts the table back (four kings on the foundations).
2. **Frame rate (the thing the PC cannot tell).** Does it look smooth (about 20 frames a second or more is the goal), or
   does it stutter or run slow? Does a card ever stop for a moment (a hitch when one is launched)? If the board cannot
   keep up, the cascade plays slower, never in a burst: note "smooth", "a bit choppy" or "slow motion". Note the board.
3. **Skip.** Win again and tap the table during the cascade: it ends at once, the trail is gone, the box comes up and
   the table is back behind it. Do it again with a key (any key), with Esc (it must **not** quit the app) and with a
   button of the bar (Undo must **not** act): each only ends the animation.
4. **Reduced** (Settings > Look > Animation Reduced): the same cascade, about 9 s, with fewer cards on the screen (6 at
   most), a coarser trail (every other step: cards are visibly separated) and about 15 pictures a second. Note whether it
   is noticeably lighter on this board than On. **Off**: no cascade, the box at once.
5. **Hidden.** Start the cascade (On), switch to another app (or Alt+Tab, or the launcher's home) and come back after
   10 s: the cascade goes on from where it was (it did not run on while away) and ends normally.
6. **Quit mid-way.** During the cascade hold Esc (3 s) or close the app from the launcher, start it again: no crash, the
   game is not in progress (it was won), a new deal can be started.
7. **Portrait** (PDA mode): the cascade fills the board above the bar; the cards bounce on the bar's top edge, not under
   it; left-handed (Settings): the cards start at the left.

Report: the board, "smooth / choppy / slow", the hitch at launches (yes / no), skip by tap / key / Esc / button
(each ok?), Reduced lighter (yes / no).

Expected pictures on the PC: the contact sheets `q-cascade-early.png`, `q-cascade-mid.png`, `q-cascade-late.png`,
`q-cascade-end.png` (the cascade at steps 45, 180, 380 and 560 of a run, four sizes), `q-cascade-left.png`,
`q-cascade-reduced.png`, `q-cascade-natural.png` (a whole reduced run to the result box, then Close),
`q-cascade-skip-tap.png`, `q-cascade-skip-esc.png`, `q-cascade-hide.png`.

Not tested here: the amd64 package on the tablet, the PDA handwriting band for the deal number on a real panel (the PC
scene `o-number-band` covers it with the shipped strokes).
