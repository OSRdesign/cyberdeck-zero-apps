# Markdown Notes 0.2.0 (slice N1): board tests

About 10 minutes on the **deck** (Pi Zero 2 W, 640x480, Bluetooth keyboard), then one touch-only pass on the **Pi 3A+**
(800x480 touch, no keyboard: the on-screen keyboard). Controller-led: the Controller deploys and reads the logs, the
owner does the physical steps and ticks the boxes. Commands run from Windows: `deck.py` is
`C:\CLAUDE\zero7\launcher\docs\dev\deck\deck.py` (password in `PIPW`; `DECK_HOST=<ip>` points it at another board). The
app's process name is `markdown-notes` (14 characters: `pgrep -x markdown-notes`; never `pkill -f`).

Expected pictures: the flow shots (`build.sh ui`, or `C:\CLAUDE\zero7\apps-notes-shots`), made with the fixture vault and a
clock fixed at 2026-10-24 14:03 UTC: the notes, dates and ages differ on a board, the layout does not.

## 0. Deploy (each board)

```
set DECK_HOST=<board ip>        (deck: leave it unset)
python deck.py put C:/CLAUDE/zero7/pkg-out-notes/markdown-notes_0.2.0_arm64.deb /tmp/markdown-notes.deb
python deck.py sudo 'dpkg -i /tmp/markdown-notes.deb'
python deck.py run '/usr/share/APPLaunch/bin/markdown-notes --version'
python deck.py put C:/CLAUDE/zero7/apps-notes/apps/markdown-notes/tests/board/big-note.sh /tmp/big-note.sh
```

`--version`: `Markdown Notes (markdown-notes) 0.2.0`, launcher `d0cb456...` with `cp0_lvgl pinned`. Logs while testing:
`python deck.py run 'journalctl --user -n 60 --no-pager | grep markdown-notes'`; the first line of a start names the
vault (`~/Notes (created)` the first time) and its note count. Back up an existing `~/Notes` first if there is one.

## D. The deck (about 10 minutes)

1. Open Markdown Notes from the launcher. The header says **Notes** (whole, gold) with Esc and the status strip identical
   to the home grid's; under it `Vault: ~/Notes · 0 notes`; six rows (`screens/01-home.png`); the key hint at the
   bottom. [ ] pass
2. Ctrl+N, type `Deck test`, Enter: the editor opens with the title `Deck test`. Type three lines, one of them
   `- [ ] buy screws`; the title shows a dot while typing, the dot goes about 1.5 s after the last key. Esc: Home says
   `1 note`. `python deck.py run 'cat ~/Notes/"Deck test.md"'` prints the three lines. [ ] pass
3. Home > All notes > Deck test: the preview (headings, the box). Tab rings the box, Enter ticks it; a tap on it
   unticks it; the file says `- [ ]` again (`cat` as above). [ ] pass
4. **Lag test (decides the 128 KB cap or the custom editor E2)**: the textarea lays the whole note out again at each
   update, so the cost grows with the note (on the PC: about 4 ms per update at 10 KB, 12 at 30 KB, 40 at 100 KB; keys
   that come during an update are inserted together at the next one, so the text comes in bursts rather than falling
   behind). `python deck.py run 'sh /tmp/big-note.sh 30; sh /tmp/big-note.sh 100'`, then for each note: Ctrl+P,
   `big note 30 kb` (then `big note 100 kb`: the whole name, else the first row would create a new note), Enter (the
   preview), Ctrl+E (the editor), Ctrl+End, type a sentence at normal speed for 20 s,
   then Ctrl+Home, type in the middle for 10 s. The journal has the times: `preview Big note 100 KB.md: ... blocks (cut)
   in N ms` and `editor ...: N bytes in N ms`. Judge each: **smooth / slow but usable / unusable** (a visible lag of more
   than about 0.3 s): 30 KB ______ , 100 KB ______ . Preview ms ____ , editor ms ____ . [ ] noted
5. Memory, with the big note still open in the editor:
   `python deck.py run 'p=$(pgrep -x markdown-notes); grep -E "VmRSS|VmHWM" /proc/$p/status; top -b -n 2 -d 3 -p $p | tail -1'`.
   Expected VmRSS under 25 MB (25600 kB; the estimate is 16-20 MB), idle %CPU a few percent at most. VmRSS ______ kB,
   VmHWM ______ kB, %CPU ____ . [ ] pass
6. **Changed on disk**: open `Deck test` in the editor (Ctrl+E) without typing; from the PC run
   `python deck.py run 'echo "from the PC" >> ~/Notes/"Deck test.md"'`; type one letter on the deck. About 1.5 s later the
   box **Changed on disk** asks Keep theirs / Keep mine (`conflict/01-conflict.png`). Keep mine: the note keeps your
   letter, and `python deck.py run 'ls ~/Notes'` shows `Deck test (deck <date> <time>).md` holding "from the PC". [ ] pass
7. Ctrl+O (the options box: every action with its Ctrl key in grey) > Delete note > Yes: back in the list, the note is
   gone; `ls -a ~/Notes/.trash` shows it. [ ] pass
8. Today's note (Home, or Ctrl+D): `<today>.md` opens in the editor; Ctrl+T inserts the date and time (check it is the
   right day: the clock). [ ] pass
9. Multitasking: in the editor type a word, wait 2 s, Alt+Tab to the launcher, come back from the switcher: the word is
   there and the note saved. Then hold Esc 3 s: the launcher ends the app, the home grid is intact,
   `pgrep -x markdown-notes` prints nothing. [ ] pass

| Board | Lag 30 KB (4) | Lag 100 KB (4) | Preview ms | VmRSS kB | VmHWM kB | %CPU | Pass | Fail |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| deck | | | | | | | [ ] | [ ] |

## K. The on-screen keyboard on the deck (0.1.1, about 3 minutes)

The shared rule (the launcher's, every app the same): no on-screen keyboard while a keyboard is connected, and for 10
minutes after a **Bluetooth** keyboard went away (it may only be asleep); 2 s after another keyboard went away.

1. With the Bluetooth keyboard connected, open a note in the editor (Ctrl+E). Settings (Esc back to Home first) >
   On-screen keyboard reads `allowed: no` and `a keyboard is connected  ·  keyboards=1 vkb=auto` (scroll down, or End
   twice). [ ] pass
2. Switch the Bluetooth keyboard off. Settings > On-screen keyboard reads `allowed: no` and
   `Bluetooth keyboard asleep N min more  ·  keyboards=0 vkb=auto`, counting down (it updates every second). Compare with
   `python deck.py run 'cat /run/user/$(id -u)/applaunch/input.state; cat /proc/uptime'`: bt_gone + 600 is the uptime at
   which it turns to `allowed: yes`. [ ] pass
3. Open a note in the editor and tap in the text, then wait: when the line said `allowed: yes` (10 minutes after the
   keyboard went off) the on-screen keyboard shows by itself, the text above it. Open or create and New note
   the same (`osklate/01`-`03`). [ ] pass
4. Switch the Bluetooth keyboard on again: the on-screen keyboard goes within about 1 s; the line under the title
   says "Ctrl+O: options" again (without a keyboard: "Hold 3 s: options"). [ ] pass

## P. Pi 3A+, touch only (about 5 minutes)

No keyboard connected: the on-screen keyboard is the only way to type.

1. Home: tap New note twice (select, then act): the name page with the on-screen keyboard; type a name on it, its Enter
   key creates; the editor shows with the keyboard up, the text above it, "Hold 3 s: options" under the title
   (`osk/03-editor-osk.png`). About 7 lines show above the keyboard at 800x480. [ ] pass
2. Type a line; the keyboard's Esc puts it away (the text takes the page); a tap in the text brings the keyboard back with the
   cursor where tapped. [ ] pass
3. Hold a finger 3 s on the text (the "Keep holding" bar fills, then the options box; the cursor does not move): Preview.
   The note drawn; tap a task box: it ticks; tap a `[[link]]` (write `[[Second]]` first): Second is created
   and opens in the editor; the header's Esc saves and goes back. [ ] pass
4. All notes: hold a note's row 3 s (the "Keep holding" bar, then Options): Delete note, Yes. The header's Esc at Home
   quits. [ ] pass

## Cleanup

`python deck.py sudo 'dpkg -r markdown-notes'`; the test notes: `python deck.py run 'rm -rf ~/Notes ~/.config/markdown-notes'`
(only if `~/Notes` held nothing of yours before step 0).

## T. The tile (deck, 0.1.1)

The home grid's tile says **Notes** on one line under the badge, and the label never touches the badge (0.1.0 said
"Markdown Notes" on two lines over its lower part). Expected: `C:\CLAUDE\zero7\apps-notes-shots\tile\home-tiles-640x480.png`
(the launcher's render harness: the 0.1.0 badge on the left column, 0.1.1's on the right and lower left). [ ] pass

## H. Markdown help (deck, 0.1.2, about 1 minute)

In a note's editor type a word, then at once Ctrl+H: the page "Markdown help" (`help/01-help-top.png`), the text size of
Settings; scroll to the end (the keys). Esc: the editor exactly as it was (the cursor, the scroll, the word, the dot of
the title), saved 1.5 s later. In the preview, hold the note 3 s > Markdown help, Esc: the same place. A tap on an example
box or link in the help does nothing but say so. [ ] pass

## P. Portrait (PDA mode, 0.2.0, about 10 minutes)

Boards: the **Pi 4 B** (DSI 800x480 panel, portrait 480x800, arm64 package) and, if there, the **FZ-M1 tablet** (800x1280,
amd64 package). Launcher v0.8.11 or later, Settings > Screen > PDA mode On; the first three steps with a Bluetooth keyboard,
the rest touch only. Expected pictures: `portrait.png`, `portrait-keyboard.png`, `portrait-band.png` of the sheets
(`build.sh host`: `host/out/sheets/` of the build dir). Back up an existing `~/Notes` first.

`--version` says `needs a screen of at least 640x480, in portrait 480x640`.

1. **Upright start.** The home grid shows the **Notes** tile; a tap starts it upright (not turned): Home in one column,
   the header *Notes*, Esc and the status strip not overlapping, the key hint at the bottom not cut. [ ] pass
2. **Touch on the lists.** All notes, a folder, Recent: a tap highlights the row under the finger (not another one), a
   second opens it; a drag scrolls; the header Esc goes back. A long note name ends with dots. Note any wrong row. [ ] pass
3. **Preview and help.** Open a note: it wraps at the screen width, no sideways scroll; tick a task box by touch; the 3 s
   hold opens the options, Markdown help reads without sideways scroll. [ ] pass
4. **On-screen keyboard** (no physical keyboard; Settings > On-screen keyboard = Auto): Open or create: the field, the
   matches under it, the keyboard below; type part of a name, Enter opens the first match at once. New note: field, Create.
   A note's editor: tap the text, the keyboard comes, the text and the cursor stay above it (about 13 lines); type a
   sentence, it appears where the cursor is and scrolls; the keyboard's Ctrl then o opens the options box (readable, not
   cut); Esc of the keyboard puts it away. [ ] pass
5. **Band** (Settings > On-screen keyboard = Handwriting + keyboard): the same steps: the band instead of the keyboard
   (Keyb, Tab, Ctrl, Alt, arrows, Enter over the writing boxes); write a few letters into the editor and a name into
   Open or create, strip Enter makes a new line in the editor / opens the match; Ctrl on the strip then a written `o`
   opens the options box; Keyb brings the keyboard and its cross the band back; the header Esc leaves the editor with the
   text saved (reopen it). [ ] pass
6. **A keyboard connects** (Bluetooth or USB) while the editor is open: the band or keyboard goes, keys type as before. [ ] pass
7. Settings > Screen > PDA mode Off: the landscape app of 0.1.2, unchanged. [ ] pass
