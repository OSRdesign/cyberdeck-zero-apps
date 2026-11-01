# Net Walk: board tests (the main path, 5 minutes)

For the owner (controller-led testing: Claude deploys and reads logs, you do the physical steps). The game rules, the
generator and the layout arithmetic are tested on the PC (`build.sh host`: about 2.5 million checks and nine scenes at nine
sizes). What the PC cannot tell is **how it feels on the glass and on a Pi**: the touch rule (tap, press, hold), whether the
tiles are easy to hit, the crispness of the picture, the colours. Keep it short: the main path must work, the rest is a bonus.
Expected pictures: the contact sheets in `~/nw-build/host-app/host/out/sheets/` of the build that made the bundle, or the
`screenshots/` folder.

Boards: the deck (about 480x320 to 640x480, keyboard and touch) and the FZ-M1 tablet (1280x800, touch and keyboard; portrait
800x1280 in PDA mode).

## T0. The look (every board, 1 min)

Open Net Walk. The launcher's page header on top, as in Mine Field: the Esc button, the title "Net Walk" and the status strip
(clock, battery, Bluetooth, Wi-Fi). Under it a grey window, the board in a pale blue-grey frame, the panel (level name, Moves,
Time, Best) and three buttons: New game, Size, Scores. In the middle of the board a dark rack-tower server (status LEDs, a red
power button, drive bays) with its cables; the computers are beige CRT monitors, screen blue when off and lime when the power
reaches them (the cable runs behind each device, its stub shows which sides are wired). Everything is made of sharp squares: no
blur at any size. On the deck the 9 x 9 board is 28 px per tile at 480x320 (scale 2, the header takes 40 px). Portrait (tablet in
PDA mode): the panel is under the board.

## T1. Touch: turn, turn back, lock (2 min, any touch board)

1. Tap a tile: it turns a quarter clockwise, Moves counts 1, the clock starts. Cables that reach the server turn green, the
   computers on them light up (blue to lime).
2. Press a tile for about half a second and let go: it turns counter-clockwise (Moves counts again).
3. Hold a tile for a full second: a small padlock appears and the tile darkens; it no longer turns when tapped and Moves does not
   count it. Hold it again: the padlock goes.
4. Drag a finger over the board: nothing turns. Note whether a fingertip hits the right tile (a 7 x 7 tile is 36 px at 480x320 and the 9 x 9 one is 28 px: can you still hit the monitor you aim at?)
   and whether the 0.35 s / 0.6 s thresholds feel right.
5. Turn tiles until every computer is lit and no cable end points at nothing: the cables flash, then "Network up!" with the moves
   and time and an OK button. OK starts the next board of the same size.

## T2. Keys (1 min, any board with a keyboard)

Arrows move a ring (navy and white) over the board; Enter or Space turns clockwise, C counter-clockwise, L the padlock, N a new
game. Esc closes a window; Esc on the board ends the app.

## T3. Size, Scores, header Esc (1 min)

Size: four rows (Easy 5x5, Normal 7x7, Hard 9x9, Expert 9x9 wrap), a check on the current one, a dot at the right of each row;
tap a row (or arrows and Enter) and a new board of that size starts. Expert's frame is dashed: a cable off the right edge
comes in at the left. Scores: after a win the best moves and the best time of that size are listed (grey values). The header's
Esc button: with Size or Scores open it closes the window; on the board it ends the app (the same as the Esc key). The
status strip shows the clock (and the battery where there is one).

## T4. What to tell me

- Is a tile big enough for a finger at 9 x 9 on the deck (28 px, was 30 before the header)? (If not, the next step is a smaller default or a bigger scale rule.)
- Do the monitors read as computers and the server as a server at 9 x 9 (small devices) and at 5 x 5 (full ones)? Is lit versus unlit clear in sunlight?
- Is the 0.35 s press (counter-clockwise) distinguishable from a tap and from the 0.6 s hold?
- Anything cut, overlapping, blurred or the wrong colour; the time the first screen takes on the Zero 2 W.
- On the tablet with a mouse: right and middle click are NOT bound (the toolkit passes only the left button); say whether you miss them.
