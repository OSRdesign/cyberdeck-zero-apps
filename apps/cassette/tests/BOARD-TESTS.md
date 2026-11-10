# Cassette: board tests (the main path first, about 90 minutes in all)

For the owner (controller-led testing: the controller installs the package and reads the logs, you do the physical
steps). The core (ring, WAV/BWF/RF64 header, writer, journal, repair, trim and split, playback and scan), the binding
and every page are tested on the PC (`build.sh host`, the scenes at 480x320, 320x480 and 640x480 on a scripted sound
system). What the PC cannot tell: **the real ALSA backend on a real USB card** (it has only run against a fake), the
SD card under a long take, a real power pull, the cost of the turning hubs on a Zero 2 W, and how the keys feel.
Keep it short where you can: B0 to B3 are the main path; the rest can wait for a second session.

Anything marked **(to confirm)** is a point the controller could not settle from the code: report what you see.

## 0. Before you start

**Boards.**

| Board | Screen | Used for |
|---|---|---|
| zero8 (Zero 2 W class) | 480x320 panel, now run upright (320x480) | main target: B0-B4, B6, B8, B9 (touch) |
| deck (Zero 2 W) | 640x480 | main target: B0-B4, B8, B9 (keyboard) |
| HackBerry CM5 | 720x720, desktop with PipeWire | B5 (sound server) |
| Pi 4B (landscape), Pi 3A+ (portrait) | 800x480, 480x800 | optional: a quick B1 and B9 if time allows |

The zero8 and the deck are the lowest-powered boards and the ones that matter. For a landscape 480x320 run of B1/B4 the
controller can switch the zero8 back to landscape, or use the deck.

**Install.** The controller installs the package from `pkg-out-cassette1\` (arm64; `pkg-out-cassette1-amd64\` for a
desktop board) and copies the helper `tests/board/cassette-check.sh` to the board's home folder; you only test. The
generic steps, for the record: copy `cassette_<version>_arm64.deb` to the board, `sudo dpkg -i cassette_<version>_arm64.deb`,
then the Cassette tile shows on the home grid (whether the launcher needs a restart for the tile to show: **to confirm**,
the controller checks).

**Start.** Open Cassette from the home grid. Log: `journalctl --user -u <launcher service> | grep cassette` shows lines
`[cassette] ...`, among them `recordings in /home/<user>/Recordings; sound input: <card>` and
`ui <W>x<H>, <n> px/mm, touch target <n> px, ... animation on at <n> fps`. Report the `ui` line once per board.

**Demo mode (a first look without a card).** Started by hand: `CASSETTE_DEMO=1 /usr/share/APPLaunch/bin/cassette`.
A scripted card, nothing is played, the takes go to a demo folder (never `~/Recordings`). Good for walking through the
five screens before the card is at hand. Not a test of the sound path.

**The probe.** `cassette-probe` lists the capture cards, their formats and mixer controls, then opens each card for
2 seconds and counts the frames. It only listens: no file is written, no mixer control is changed. Where it is
installed: `/usr/share/APPLaunch/bin/cassette-probe` **(to confirm: whether the package carries it; if not, the
controller copies it to the board)**. Options: `--card NAME`, `--seconds N`, `--no-open`. What its lines mean:

| Line | Meaning |
|---|---|
| `ALSA library: loaded` | libasound was found (the app loads it at run time) |
| `card "<label>"` / `index N (this boot only)` / `usb VID:PID port ... by-id ...` | the card, how the app tells it apart (by name and by-id path, never by index) |
| `offers: ...` | rates, channels, sample formats the card can capture |
| `ctl <name> int range a..b (x..y dB) = v` | a mixer control and its value now; `(no dB from the driver)` if the kernel hides the dB scale |
| `capabilities: has_level=1 has_agc=1 has_mute=0 level ... dB` | which card rows the app will show (Input level, Auto gain, Mic mute); `INFERRED` = dB guessed |
| `open: device hw:... -> OK direct: 48000 Hz, 1 ch, 16-bit, period ..., buffer ... (... ms)` | the card opened directly |
| `... OK VIA SERVER` + `note: Recording through the sound server: the card is in use` | the card was busy, the sound server was used |
| `captured N frames in 2.0 s (expected about 96000), overruns 0, lost (est.) 0, error: none` | the capture worked; overruns or lost frames above 0 are worth reporting |

**Have ready.** One cheap USB sound card (the PCM2902 one) with a microphone, the USB-C headset "UC02", a USB or
Bluetooth keyboard (for the deck), a phone and the PC (to play files back), a phone timer. Your recordings are private:
nothing is uploaded anywhere, and nothing in these tests asks you to send audio; send the helper's text output instead.

**The helper** (read-only, no sudo): `sh ~/cassette-check.sh cards | take [FILE] | journal | watch [S] [N] | usb [MIN]`.
`take` prints the newest take's size, header, length and its journal entry, including the **DROPPED** count (frames the
app's own buffer had to drop) and the **LOST** figure (audio the card or the disk could not keep up with, about; the same
number the screen shows during the take as `lost 0.3 s` in red at the end of the info strip, and the journal keeps it). `watch` prints the app's CPU
%, memory, temperature and throttle flags every 5 s.

## B0. Probe (every board, 2 min)

Goal: the first run of the real ALSA backend on real cards.

1. Plug the USB card in. On the board: `cassette-probe` (full path above if needed), then `sh ~/cassette-check.sh cards`.
2. The same with the headset instead of the card.

Expected: each card listed with `open: ... OK direct`, `captured` close to the expected count, overruns 0. PCM2902:
`has_level=1 has_agc=1`, level range 0..23.81 dB. UC02: mono 48 kHz 16-bit, level 0..127 with `(no dB from the driver)`
or `INFERRED`.

Report: both outputs in full, per board.

## B1. A 10-minute take (deck or zero8, 20 min)

Goal: a long take records, lists, plays and opens everywhere; the board copes.

1. Open Cassette, card plugged in. The window shows the take name (e.g. `TAKE 001`), the format (`48k/16 WAV`), the time left; the meter
   moves when you speak. Settings (S or the gear) > Channels shows Mono or Stereo (what the card records: the PCM2902 and
   the headset should say Mono) and Format `WAV 48 kHz 16-bit`.
2. In a second ssh session: `sh ~/cassette-check.sh watch 30 25` (a line every 30 s for 12.5 min).
3. REC (R or the red key). Talk or leave a radio on for **10 minutes**; touch nothing. Watch: the hubs turn, the counter
   runs, the meter moves.
4. STOP (X or the key). The recordings list (L or the list button): the new take on top, the row says `today HH:MM Â·
   10 min Â· 58 MB` (about 5.8 MB a minute at 48 kHz mono 16-bit).
5. Select it and PLAY (Enter or PLAY): you hear the start; STOP.
6. On the board: `sh ~/cassette-check.sh take`. Expected: `ok header and file size agree`, length about 600 s, journal
   state `closed`, DROPPED 0, LOST 0.
7. On the PC: `scp <user>@<board>:Recordings/<file>.wav .` (files are named `take_001_YYYYMMDD-HHMM.wav`, or
   `take_001_nodate.wav` when the board's clock was not set from the network) then `ffprobe -hide_banner <file>.wav` (if ffmpeg is
   installed). Expected: `Duration: 00:10:0x`, `pcm_s16le, 48000 Hz, mono`. Play it in a PC player (VLC or the Windows
   player) and copy it to the phone and play it there; skip through: no gaps, no clicks.

Report: the `watch` lines (CPU %, temperature, throttled; the first and last few are enough), the DROPPED count, the
ffprobe line, whether it plays on the PC and the phone, any gap or click you heard and roughly when.

## B2. Unplug during a take (deck or zero8, 5 min)

Goal: the take keeps its audio, the app says what happened, the card comes back by itself.

1. Settings: set Input level to something other than the default (e.g. +12 dB) and Auto gain (card) Off; note them.
2. REC, talk for about 1 minute, then pull the card's USB plug.
3. Expected at once: the take ends, a notice `The sound card was unplugged: the take was closed and kept` (or `The
   sound card was unplugged`), the window shows NO INPUT and the strip says why.
4. `sh ~/cassette-check.sh take`: header ok, length about the minute you talked (at most a fraction of a second
   short), journal state `closed` or `error` with the reason. `sh ~/cassette-check.sh usb 5`: the disconnect and the
   reconnect lines.
5. Plug the card back: within a few seconds the notice `The sound card is back`, the window shows the take name again.
   Settings: Input level and Auto gain are what you set in step 1 (re-applied to the card).
6. REC again: a new take starts and records; STOP. Both takes play from the list.

Report: the notice texts, the length of the unplugged take against your timer, how long the card took to come back, the
level and auto gain after replug.

## B3. Power pull during a take (zero8 or deck, 10 min)

**Warning: this is a real power pull.** If the SD card holds anything you cannot recreate, back it up first. The P0 test
on the zero8 lost about 6 s with a 10 s sync and the file system came back clean.

Goal: after boot the app finds the take, repairs it, says so; at most about the last sync interval is missing.

1. Settings > Sync every: 10 s (the default). REC, start a phone timer at the same moment, talk for about 2 minutes,
   counting aloud every 10 s ("ten", "twenty", ...).
2. Note the timer, then pull the power cable at once.
3. Power on, wait for the launcher, then **before opening Cassette**: `sh ~/cassette-check.sh take`. Expected: the
   `WARNING the header declares more audio ...` or `note the file holds more audio than declared` line, journal state
   `open`.
4. Open Cassette. Expected: a notice `Recovered <take> after an interrupted recording (2 min kept, about N s at the
   end lost)`; the list row says `repaired`.
5. `sh ~/cassette-check.sh take` again: `ok header and file size agree`, journal state `repaired`. Play the take in the
   app and on the PC: the last number you hear against the timer.

Report: the timer at the pull, the length kept, the last number heard, the notice text. Expected loss: 10 s or less
(about the sync interval plus a buffer of under a second).

## B4. The cost of the animation (zero8 and deck, 10 min)

Goal: whether the turning hubs cost too much on a Zero 2 W.

1. In ssh, close the launcher's Cassette first (Esc), then start it by hand with the timing line:
   `CASSETTE_TIMING=1 /usr/share/APPLaunch/bin/cassette` (the controller can start it for you). A line every 5 s:
   `timing: N refreshes in 5.0 s, render avg x ms, max y ms, hub frames n`.
2. Second session: `sh ~/cassette-check.sh watch 5 36`. REC for 3 minutes with the hubs turning. Note CPU % and
   temperature at the end, and a few timing lines. STOP, Esc.
3. The same without the animation: `CASSETTE_ANIM=0 CASSETTE_TIMING=1 /usr/share/APPLaunch/bin/cassette`, REC for
   3 minutes, note the same figures. (Also possible: `CASSETTE_ANIM_FPS=10` for a slower turn.)
4. Both runs: `sh ~/cassette-check.sh take` (DROPPED and LOST should be 0).

Report: per board, CPU % and temperature with and without the animation, the render avg / max, DROPPED, and whether the
UI feels smooth (the hubs, the meter, the keys answering at once).

## B5. Sound server (HackBerry, 10 min)

Goal: the card held by the sound server: the app falls back to the server and says so; the other way round, other apps
get silence while Cassette holds the card.

1. Headset or card plugged in. On the desktop start a recording elsewhere first: `pw-record /tmp/other.wav` in a
   terminal (or a browser page that records).
2. `cassette-probe`: expected `OK VIA SERVER` and the `note:` line.
3. Open Cassette. Before any take the info strip leads with the card name and `(server)`. REC 30 s, STOP, play it: it
   holds your voice. Expected on screen: when REC starts, a 4 s notice "Recording through the sound server: the card is
   in use"; during the take the strip reads like `TAKE 007 · 48k/16 WAV (server) · 12 h 40 left` (on the narrow screen
   the small box says `(server)` before the time left). Report exactly what the screen shows.
4. Stop `pw-record` (Ctrl+C). Close and open Cassette (it now opens the card directly). REC, and while it records start
   `pw-record /tmp/other2.wav` for 10 s. Expected from P0: `other2.wav` holds silence. Expected on screen: when REC
   starts, a 4 s notice "Other apps cannot use this card while Cassette records" (shown only because a sound server
   runs; on a board without one there is nothing). Report what you see.
5. `rm /tmp/other.wav /tmp/other2.wav`.

Report: the probe output, the screen texts in steps 3 and 4, whether both takes play, and `sh ~/cassette-check.sh take`
(LOST should be 0 ms unless the strip showed `lost`).

## B6. zero8 upright (320x480, 10 min)

Goal: all five screens usable at 320x480.

1. Start once with the layout check: `CASSETTE_LAYOUT_CHECK=1 /usr/share/APPLaunch/bin/cassette`. A failed check shows
   `LAYOUT FAIL` at the top left and a log line.
2. Walk through the recorder (idle, recording, playing, the countdown of B9), the Recordings list, Settings (scroll to
   the last row: Auto-split), the Trim page, the save box, the confirm box (DELETE on a test take), the rename box with
   the on-screen keyboard (no keyboard plugged in).
3. Look for: text cut or overlapping, keys too small to hit (the toolkit's target is 44 px or more; only the two small
   header-side buttons are 40 x 44), notices that do not fit their box.
4. Rename a take to a 24 character name with spaces (for example `Field recording of birds`) and open the list: the
   name wraps over two or three lines, the row grows, the details line sits under the name (nothing over it, no
   ellipsis), the chevron and sticker stay centred in the row, and the rows below move down. Select it, step up and down
   with the arrow keys and scroll to the last row: the selected row is always fully in view and the scroll bar follows.

Report: `LAYOUT FAIL` seen or not (and the log line), anything cut or hard to hit, per screen.

## B7. A card without a level control (if one is at hand, 5 min)

Goal: the card rows go away cleanly.

1. `cassette-probe` with that card: `has_level=0`.
2. Open Cassette: the CARD stepper is not on the recorder; the SOFT stepper alone, full width **(to confirm: the exact
   look)**. Settings: no Input level and no Auto gain (card) rows.
3. SOFT + / - change the level you see on the meter; REC 20 s, STOP, play it.

Report: the probe's capabilities line, what the recorder shows, the Settings rows.

## B8. Trim and split (deck with keyboard, zero8 by touch, 20 min)

Goal: trim and split work, originals are kept or replaced as chosen, a power pull during a save leaves no half file.

1. Record a take of about 1 minute, counting aloud every 10 s. List: select it, TRIM (T). The Trim page: the waveform,
   the marker, IN / OUT / LEN.
2. Zoom in and out (Up / Down or ZOOM - / +): the waveform scrolls with the marker when zoomed in. Move the marker
   (Left / Right 0.1 s, Shift 1 s, POSITION < >, or a tap on the waveform). PLAY (Space) plays from the marker.
3. **Trim:** marker at "ten", SET IN (I); marker at "fifty", SET OUT (O); LEN about 40 s. SAVE (S): the save box lists
   the result file and its length. SAVE AS NEW FILES. Expected: a new file with `-1` added to the name; the original is
   still in the list. Play both.
4. **Split:** TRIM the original again. Marker at "twenty", SPLIT (Enter); at "forty", SPLIT. Tab selects a flag (the
   marker jumps to it). Add a wrong third split, Tab to it, REMOVE (Enter while a flag is selected): it goes. SAVE:
   three files listed, SAVE AS NEW FILES. Expected: three files named `-1`, `-2`, `-3` (or the next unused numbers if
   step 3 used `-1`: numbers are never reused for a take (to confirm on the board)); play each: they join where you
   split.
5. **Replace:** TRIM one of the results, trim a few seconds, SAVE, REPLACE ORIGINAL. The confirm box `Delete the
   original take?` / `It is deleted only after every new file is written and synced.`: DELETE. Expected: the notice
   `Saved, original replaced`, back on the list, the original gone, the result there. Esc from the Trim page without
   saving changes nothing.
6. **Power pull during a save (the warning of B3 applies):** TRIM the 10-minute take of B1, split it in two, SAVE AS NEW
   FILES and pull the power while the save runs (a few seconds on the SD card; what the screen shows while saving:
   **to confirm**). After boot, before opening Cassette: `sh ~/cassette-check.sh journal` (leftover `.tmp-*` files are
   expected). Open Cassette, then the same command again. Expected: the original is whole, and either both results or
   none of them exist, never a half file; the `.tmp-*` files are gone.

Report: the file names, the lengths against your counting, whether the original was kept or replaced as chosen, the
journal output before and after the start in step 6.

## B9. Keys and touch (deck by keyboard, zero8 by touch, 10 min)

Goal: the map in `docs/KEYS.md` works, and the same by touch.

1. Recorder: R REC, Space PLAY / PAUSE, X STOP, L list, S settings, Esc ends the app. **PLAY / PAUSE while recording**
   pauses (the counter stops) and resumes the **same** take (one file in the list). Esc during a take ends the app and
   keeps the take.
2. Scan: play a take, hold `.` (FWD): it plays forward at 5x with the sound audible; release: normal play. Hold `,`
   (REW): backwards at 5x. The key stays drawn pressed while held. Settings > REW / FWD: `Jump 5 s`: each tap jumps 5 s,
   a held key repeats. REW and FWD are dimmed while recording.
3. Record timer: Settings > Record timer 3 s, REC: `RECORDING IN 3`, 2, 1, then it records. Try 10 s and 20 s once. Cancel
   the countdown three ways: a tap on the cassette window, STOP, Esc (Esc cancels the countdown only, the app stays).
   Back to Off.
4. Recordings list: Up / Down, Enter plays, N rename (only the prefix is edited, take number, date and time stay), T trim,
   Delete asks (B11), S settings, Esc back.
5. Trim keys as in B8; save and confirm box: Left / Right or Tab move the white outline, Enter chooses, Esc cancels.
6. Settings: Up / Down, Enter or Right opens or flips, Left / Right on Input level, Esc closes a choice list first, L the
   list.
7. zero8 by touch: the same paths with the on-screen keys and buttons.

Report: any key that does nothing or the wrong thing, anything hard to hit by touch.

## B10. Settings kept across a reboot (deck or zero8, 5 min)

1. Change: Input level (e.g. +6 dB), Auto gain (card) Off, SOFT to +12 dB on the recorder, Record timer 10 s, REW / FWD
   Jump 5 s.
2. Reboot the board (a normal reboot). The card mixer resets itself at boot (PCM2902: level 0, auto gain on); check with
   `sh ~/cassette-check.sh cards` before opening Cassette.
3. Open Cassette: all five values as set; `sh ~/cassette-check.sh cards` again: the card's level and auto gain are back to
   your values (applied at start).
4. Put the values back (timer Off, REW / FWD Fast scan 5x).

Report: what came back and what did not.

## B11. Notices and errors (any board, 15 min)

1. **Disk full.** The app stops a take cleanly when the free space falls under 200 MB. To test without filling the card,
   take the space with a filler file and give it back after:
   `avail=$(df --output=avail -BM ~ | tail -n 1 | tr -d M); fallocate -l $((avail - 230))M ~/cassette-filler`
   (leaves about 230 MB: about 5 minutes of mono). REC and wait. Expected: the notice `Recording stopped: free space is
   low: the take was closed. The take was kept.`; the take plays; REC again is refused with a notice (`... not enough
   free space`). Then **`rm ~/cassette-filler`** and check with `df -h ~`.
2. **Rename: names and refusals.** List > RENAME: the box shows `4/24` under the entry (a take called `take`). Type
   `my_take-2`: `9/24` in grey; accepted, the list shows `My_take-2 007`, the number, date and time stay. Rename
   again to `my band 2` (spaces inside): accepted; the cassette label shows the spaces. Type 20 or more characters: the
   counter turns amber (20 to 23), red at `24/24`, and further keys add nothing. Refusals: `a b/c` gives `Not renamed:
   use letters, digits, space, - and _`; an empty name gives `Not renamed: the name is empty`; spaces only are trimmed
   to empty. Settings > File prefix with the same texts: `Prefix not changed: ...` with the same causes. A name with
   spaces around (` walk `) is accepted as `walk`. A foreign file in the folder (copy any
   `interview.wav` in: the list shows it): RENAME on it says `Not renamed: this file is not a Cassette take and cannot
   be renamed`.
3. **Delete.** List > DELETE: `Delete this take?` with DELETE and CANCEL; Enter without moving = CANCEL (nothing
   deleted); move to DELETE, Enter: gone from the list and from `~/Recordings`.
4. **No card.** Unplug every card, open Cassette: `No sound input found. Plug in a USB sound card or headset.`; plug one:
   found by itself.

Report: the notice texts as shown, whether the filler was removed.

## B12. A speech-only headset at its own rate (any board with a headset that offers 16000 Hz only, 10 min)

Goal: a card that records at neither 48000 nor 44100 Hz (a USB headset in its call profile, for example the Jabra Evolve
65: `mono S16 at 16000 Hz`) opens and plays back at the right speed.

1. `cassette-probe --card <the headset>`. Expected: `open: ... OK direct: 16000 Hz, 1 ch, 16-bit` (no `Unsupported card`).
2. Open Cassette with only that headset plugged in. Expected: the strip says `16k/16 WAV`, Settings > Sample rate reads
   `16 kHz` (Codec `WAV`, no Bit depth row: the headset records 16-bit only), the meter moves when you speak.
3. Say the numbers 1 to 30 aloud, one a second, and press REC with a watch in view; record 30 s, STOP. Expected: the
   counter at STOP reads about 0:30 (not 0:10 and not 1:30); the list shows the take at about 0:30.
4. PLAY it through the headset (its output may play 16000 Hz only) and through another output (Settings > Output, one that
   plays 48000 only). Expected on both: normal speed and voice pitch, about 30 s long, counting in step with your watch.
   REW / FWD scan at 5x and the 5 s jump move by the right amount of time.
5. TRIM: split at about 15 s and save as new files. Expected: two takes of about 15 s each that play at normal speed.
6. `file ~/Recordings/take_001_*.wav`.
   Expected: `... WAVE audio, Microsoft PCM, 16 bit, mono 16000 Hz`.

Report: the probe line, the counter and list length of the 30 s take, whether both playbacks were at the right speed, the
`file` line.

## B13. Live monitor (a card without sidetone, any board, 10 min)

Needs a USB card whose earphones you can wear and that has no hardware sidetone (the UC02 headset or the Apple dongle with
earbuds), and the Jabra Evolve 65 for step (d). Wear the headphones: speakers cause echo.

(a) Open Cassette. Settings: **Monitor** is Off and **Monitor level** is dimmed at 50 %, after Limiter. Nothing is heard
in the earphones on the recorder screen.

(b) With the UC02 or the dongle selected, switch Monitor On. Expected: you hear yourself with a small delay (a few tens of
ms), the voice is clear. Drag or step Monitor level (Right / Left on the keyboard, 5 % per step): the loudness follows at
once and without a click; 0 % is silence, 100 % is loud (turn the card's own volume down first). Press REC and record
about 30 s while talking, STOP. Expected: the take is 30 s long (same as the counter), no "lost" text appears, and it
sounds the same with Monitor on and off (the monitor level is not in the file). Do it once more with Monitor Off and
compare the lengths.

(c) With Monitor On, press PLAY on a take: the monitor stops at once (no echo of the take in the microphone path, no
howl). Press STOP: it resumes. Also try REW / FWD held, and the list's preview (double tap): off while it plays, back
after. With the record timer on (3 s): the monitor runs during the countdown.

(c2) Watch the small **MON** lamp at the bottom left of the meter panel (under IN): the MON lamp is lit while the monitor
runs, dark during PLAY and with the switch off. (A card without a playback side shows no MON lamp at all.)

(d) With the Jabra Evolve 65 selected and Monitor Off: you still hear yourself, that is the headset's own hardware
sidetone; Cassette cannot change it. (With Monitor On you hear a second, later copy: expected, switch it off.)

(e) Monitor On, pull the USB card out while it runs: no crash, the notice for the lost card appears, no noise, no frozen
screen. Plug it back: the app finds the card again and the monitor comes back by itself.

Report: pass or fail per step, how the delay felt (none / small / annoying), anything cut off in the Settings rows.

## B14. Blank cassette (any board, 5 min)

Goal: with no take loaded the recorder shows a blank cassette in the deck, never an empty window.

1. Start with an empty recordings folder (move `~/Recordings` aside) and open Cassette. Expected: from the first frame the
   cassette is in the window (shell, window, still hubs, SIDE A, the format such as `WAV 48k`), the label is blank paper
   with no handwriting, no OPEN word on the window, the strip names the take REC would write next.
2. Press REC (after the countdown, if one is set). Expected: the hubs start to turn; the cassette does not drop in or
   appear again.
3. STOP, then open Recordings, LOAD the new take. Expected: the same 0.9 s swap as between two named cassettes (the old
   one drops out, the new one falls in, the name is written).
4. Delete that take (Recordings, DELETE, confirm), Esc. Expected: the blank cassette is back at once, no animation, no
   name, PLAY dimmed.
5. With a blank cassette in the deck, record a take and LOAD it from the list again. Expected: the blank one drops out first.
6. Check 320x480 and 640x480 boards if you have them: nothing cut or overlapping.

Report: pass or fail per step, a photo of step 1 and step 4.

## B15. Take number (any board, 10 min)

Goal: Settings > Take number sets where the next take counts from; nothing existing changes.

1. Note the takes in the list. Settings > Take number: the row reads `Next: NNN` (one more than the newest take, or the
   number you last set). Tap it: the page shows the number large, -1 / +1, -10 / +10, -100 / +100, Reset to 1 and Set.
2. Press Reset to 1, then +10 five times (51) and -1 once: the number reads 050. Tap Set.
   Expected: back on Settings, the row reads `Next: 050`; the recorder label shows TAKE 050.
3. Record a few seconds, STOP. Expected: the list has `take_050_...` (name `Take 050`); the other takes are untouched
   (same names); the row now reads `Next: 051`.
4. With takes 001, 002 and 003 in the folder (copy or record them first if needed): Take number > Reset to 1 > Set.
   Expected: the row reads `Next: 004`, not `Next: 001` (a number the prefix already uses is skipped). Record, STOP: the
   new take is `take_004_...`; no earlier take was overwritten or renamed.
5. Open the page, press +1 a few times, then Esc. Expected: nothing changed (the row still reads the same).
6. Set 7, tap Set, then reboot the board and open Settings. Expected: the row still reads `Next: 007` (or the first free
   number above it). The row is dimmed while a take records.

Report: what the row read at each step, the names of the takes recorded in 3 and 4, and whether the restart kept it.

## B16. Square screen (HackBerry 720x720, 10 min)

1. Open Cassette on the HackBerry (or any square screen). Expected: the cassette fills its window (no OPEN word), the
   counter, the Recordings and Settings buttons and the take box sit in one row under it, the meter panel is thin, CARD and
   SOFT stand side by side, the five keys run across the full width. The left and right margins are equal all the way
   down; nothing is cut or overlapping.
2. Go through the states and look at each: idle with a take; recording (the MON lamp lit and dark if the card has a
   playback side: the lamp sits under IN); playing (the meter reads L R); paused; the record timer's countdown over the
   cassette window; LOAD from the list (the old cassette drops out, the new one falls in, the name is written); a blank
   cassette (an empty recordings folder); a card without a level control (no CARD, SOFT full width, if one is at hand);
   a stereo card (the meter shows two bar rows, L over R; the panel keeps its height and nothing under it moves between mono, stereo, recording and playing; the keys stay inside the body).
3. Open Settings, the list, trim and the save box once: nothing cut.
4. A photo of idle, recording and the stereo state if possible, and of the countdown. Expected: the touch targets feel
   right (the steppers' - and + buttons and the keys are 7 mm or more).

## B17. FLAC and Opus takes (any board with the libraries; zero8 or deck for the power pull, 30 min)

Goal: takes record as FLAC and as Ogg Opus, play on the PC, and survive a power pull and an unplug like WAV takes.
The pickers (B19) can choose them; here the format is set in the settings file, which also tests the stored key.
Playing, previewing and trimming them in the app is B18 (phase C); here the takes are also checked on the PC.

1. The libraries (the controller can do this): `ldconfig -p | grep -E 'libFLAC|libopusenc'`. Expected: `libFLAC.so.14`
   (or `.so.12`) and `libopusenc.so.0`. If missing: `sudo apt install libflac14 libopusenc0` (Debian trixie;
   `libflac12t64 libopusenc0` on Ubuntu 24.04). Report which were there before.
2. **FLAC.** Close Cassette (Esc on the recorder) and check it is not running (`pgrep -a cassette` prints
   nothing): a running Cassette writes its own settings back over the edit. Then:
   `sed -i 's/"format": *"[a-z]*"/"format": "flac"/' ~/.config/cassette/settings.json` and check it with
   `grep format ~/.config/cassette/settings.json` (expected: `"format": "flac"`).
3. Open Cassette. Expected: the format on the window reads `48k/16 FLAC` (Codec `FLAC`, Sample rate `48 kHz` in Settings; Bit depth `16-bit` only on a card that also has 24-bit);
   the time left is about three times what it was with WAV. Do not pick a value in Settings > Codec, Sample rate or Bit depth during B17
   (that is B19): it would overwrite the setting under test.
4. REC, talk for 1 minute, STOP. The list: the new take `take_NNN_...flac` on top, its row ends in `FLAC` (the WAV
   takes end in `WAV`). Its take number follows the WAV takes (never a number already used). Press LOAD and PLAY once:
   it plays (B18 goes further). Rename it (a new prefix) and check the name still ends in `.flac`.
5. On the board: `cat ~/Recordings/.journal/<take name without .flac>.json`. Expected: `"state": "closed"` and a line
   `"codec": {"format": "flac", ...}`. (The helper's `take` command reads WAV files only.)
6. On the PC: `scp <user>@<board>:Recordings/<file>.flac .`, then `flac -t <file>.flac` (expected: `ok`) and
   `ffprobe -hide_banner <file>.flac` (expected: `Duration: 00:01:0x`, `flac, 48000 Hz, mono, s16`). Play it in VLC
   (or `ffplay <file>.flac`) and on the phone: no gaps, no clicks. Compare its size with a 1-minute WAV take (expected:
   about a third to a half).
7. **Power pull with FLAC** (as B3, read its warning). Settings > Sync every: 10 s. REC, start a phone timer, count
   aloud every 10 s for about 2 minutes, pull the power. Boot; before opening Cassette:
   `cat ~/Recordings/.journal/<take>.json` (expected: `"state": "open"`) and `ls -l ~/Recordings/<take>.flac`.
   Open Cassette. Expected: the notice `Recovered <take> after an interrupted recording (... kept ...)`, the row says
   `repaired`. The journal now says `"state": "repaired"`. On the PC: `flac -t` says `ok` (no error, no `lost sync`),
   ffprobe's duration matches what was kept; the last number you hear against the timer. Expected loss: about the sync
   interval (10 s) or less.
8. **Opus.** Close Cassette, then
   `sed -i 's/"format": *"[a-z]*"/"format": "opus", "format_preset": "music128"/' ~/.config/cassette/settings.json`.
   Open Cassette. Expected: `48k/16 OPUS` (with a 44.1 kHz-only card the app records WAV instead: Opus needs 8, 12, 16,
   24 or 48 kHz; report it if that happens). REC 1 minute, STOP; the row ends in `OPUS`; the journal has
   `"codec": {"format": "opus", "preset": "music128", "bitrate": 128000, ...}`.
9. On the PC: `ffprobe -hide_banner <file>.opus` (expected: `Duration: 00:01:0x`, `opus, 48000 Hz`), and if opus-tools
   are installed `opusinfo <file>.opus` (no errors, about 128 kbit/s) or `opusdec <file>.opus out.wav`. Play it in VLC.
10. **Power pull with Opus**: as step 7 with the Opus setting. Expected after the repair: the notice and `repaired`;
    ffprobe and VLC play it to the end; expected loss: the sync interval plus up to about 2 s (the Opus encoder holds
    2 s of look-ahead that is not in the file yet).
11. **Unplug** (as B2) during a FLAC take: the take is closed and kept, `flac -t` says `ok` on the PC.
12. **CPU** (zero8 or deck): `sh ~/cassette-check.sh watch 10 12` during a 2-minute FLAC take and a 2-minute Opus
    128 kbps take (with the stereo card if one is at hand, else the mono one). Report the CPU % of each.
13. Put it back: close Cassette, then
    `sed -i -e 's/"format": *"[a-z]*"/"format": "wav"/' -e '/"format_preset"/d' ~/.config/cassette/settings.json`
    (the second expression removes the preset line; if `format_preset` was written on the same line as another key,
    edit the file by hand with `nano`).

Report: which libraries were present, the window's format text, the list rows (FLAC/OPUS tags), the `flac -t`,
ffprobe and opusinfo lines, whether both play on the PC and the phone, the power-pull results (timer, kept length,
last number heard, notice text) for FLAC and Opus, the unplug result, the CPU % of step 12.

## B18. Play, scan, trim and split FLAC and Opus takes (any board with the libraries; zero8 or deck for the CPU, 30 min)

Goal: a FLAC and an Opus take play, scan and trim in the app exactly like a WAV take, the results keep their format
and their lengths are right, and the 5x scan of an Opus take does not starve a Pi Zero 2 W. Needs B17's settings
recipe or the pickers (B19) to choose the format. Phase C decodes FLAC with libFLAC and Opus with libopus (no
libopusfile).

1. The libraries (the controller can do this): `ldconfig -p | grep -E 'libFLAC|libopus'`. Expected: `libFLAC.so.14`
   (or `.so.12`), `libopus.so.0`, `libopusenc.so.0`. Report which were there.
2. Record a FLAC take of about 1 minute (B17 steps 2-4: count aloud "one, two, three ..." once a second, so the
   position can be heard), then an Opus `music128` take of about 1 minute the same way (B17 step 8). For each take, in
   the list: note the length the row shows.
3. **Play.** LOAD the FLAC take, PLAY: the counter runs from 0:00, the numbers are heard in time, the hubs turn, the
   meter reads the playback. PAUSE in the middle, wait, PLAY: it goes on from the same word (nothing lost, nothing
   repeated). Let it play to the end: the deck stops by itself at the length the row shows.
4. **Scan.** PLAY again, hold FWD (Settings > REW / FWD: Fast scan 5x): fast, high-pitched but audible speech, the
   counter runs 5x; release: normal play from there. Hold REW: the same backwards; keep holding until the start: the
   deck stops at 0:00 by itself (REW stops at the start), the hubs stop. Hold FWD until the end: the take ends.
5. **Jump.** Settings > REW / FWD: Jump 5 s. While playing, tap FWD: +5 s (the number heard jumps by 5); tap REW
   three times: -15 s, never before 0:00. Set it back to Fast scan.
6. **Preview** from the list (double tap the row, or Enter): it plays from the start without loading; double tap again
   stops it.
7. **Trim and split.** TRIM on the FLAC take: the waveform appears after a short progress (it is decoded once, then
   the `.flac.peaks` file is reused: close and open TRIM again, it is instant). Zoom in to the closest level and back.
   Set IN at about 0:10 (on a counted word), OUT at about 0:50, a split at about 0:30; preview the kept part. Save as
   new files. Expected: two new rows `take_NNN_...-1.flac` and `-2.flac`, about 20 s each (row lengths), rows end in
   `FLAC`. Play each: the first starts at the word where IN was, the second at the split's word; nothing missing at the
   joins. Then trim the Opus take the same way: `-1.opus`, `-2.opus`, rows end in `OPUS`.
8. **Replace original.** TRIM a part, set IN at 0:02, save with REPLACE ORIGINAL and confirm: the row keeps its name,
   its length is 2 s shorter, it plays from the new start.
9. **On the PC** (the controller can do this): copy the four parts and check them: `flac -t <part>.flac` says `ok`;
   `ffprobe -hide_banner <part>` shows the durations the rows showed; `opusinfo <part>.opus` (if opus-tools are
   installed) shows no errors. Play them in VLC: they start and end on the words chosen, no clicks at the cuts.
   Expected: each part's length equals OUT - IN (or the split points) to the frame (`ffprobe -show_entries
   stream=duration_ts`).
10. **CPU** (zero8 or deck): `sh ~/cassette-check.sh watch 10 12` while holding FWD on the Opus take for 10 s, then
    while holding REW for 10 s, then during normal play. Report the CPU % of each (expected: well under one core; the
    sound must not stutter while scanning). Also the time the FLAC trim of step 7 took (from SAVE to the new rows).
11. **Without the library** (optional, a board where it can be removed and put back): `sudo apt remove libflac14`
    (or move the take to a board without it). LOAD on a FLAC take: the notice `This take cannot be played: FLAC not
    installed (libflac14)`, nothing hangs, WAV takes still play. Reinstall afterwards.
12. Put the settings back to WAV (B17 step 13).

Report: the libraries present, the row lengths, what was heard in steps 3-6 (the words at pause/resume, where REW
stopped), the part names and row lengths, the `flac -t` / ffprobe / opusinfo lines, the CPU % of step 10 and the trim
time, anything that stuttered or clicked.

## B19. The Codec, Sample rate and Quality pickers (any board with the libraries, 30 min)

Goal: Settings > Codec, Sample rate and Bit depth / Bit rate list every choice with sizes that match what a recording really takes, and each choice reaches the file.

1. Open Settings and read the rows. Expected: `Codec` (`WAV`), `Sample rate` (`48 kHz` on a card that has it) and,
   only on a card that records both 16 and 24-bit, `Bit depth` (`16-bit`); a 16-bit only card has no Bit depth row. Tap Codec (or Right / Enter). Expected: the page is titled `Codec`; the heading `Codecs`
   and WAV, FLAC, Opus, then the heading `Quick presets` and six two-line rows (WAV, FLAC, Opus speech 24 kbps, Opus
   speech 32 kbps, Opus music 64 kbps, Opus music 128 kbps); each row shows the size per minute on the right (`5.8
   MB/min`, `about ... MB/min` for FLAC, `... kB/min` or `... MB/min` for Opus) and a grey line under the name (the
   one-line description and `... h free`); the codec in use (in the first group) has a dot and is darker; the footer
   under the list reads `Sizes for <rate>, <bits>-bit, this card`. The headings cannot be selected (Up / Down skip them).
   Scroll to the last row (drag, or Down keys).
2. A card or library that cannot do it: with the PC's `ldconfig -p | grep -E 'libFLAC|libopusenc'` as the reference, a
   missing library's rows must be dimmed with `FLAC not installed (libflac14)` or `Opus not installed (libopusenc0)`
   in place of the description. On a card that records only 44.1 kHz (or any rate Opus does not take) the four Opus
   preset rows and the Opus codec are dimmed with `Opus needs 8, 12, 16, 24 or 48 kHz; this card has 44.1 kHz`. Tap a dimmed row. Expected: nothing is chosen, the notice
   line says `Not available: <the same reason>`; Esc leaves the page.
3. Choose FLAC (the codec row). Expected: the page closes, the Codec row reads `FLAC`, the window's format text reads
   `48k/16 FLAC`, the time left grows to about three times the WAV figure. Close Cassette and open it again: still
   `FLAC` (the choice is stored). Repeat with the preset `Opus speech 32 kbps` (Codec `Opus`, the third row now reads
   `Bit rate` and `32 kbps`) and back to WAV.
4. **Sizes against files.** For each of WAV, FLAC, Opus 24, 32, 64 and 128 kbps (the ones the card allows; the presets
   or Codec plus Bit rate): choose it,
   REC for exactly 1 minute by the counter, STOP, and read the file size in the list row (or `ls -l ~/Recordings`).
   Write down the picker's figure and the file's size. Expected: WAV within 1 % of the figure; Opus within 10 % of
   the figure (a take with pauses is smaller, never much larger); FLAC within the range of the take's content (speech
   about 30 %, music about 55 % of WAV) and the picker's `about` figure of the same order.
5. A stored choice that stops working: choose Opus on a card that allows it, close Cassette, start it with a card that
   records 44.1 kHz only (or with the library removed). Expected: the Codec row reads `WAV (Opus unavailable)`, a
   take records as WAV, the picker marks WAV as in use and shows the Opus rows dimmed.
6. Without a sound input (unplug the card, Cassette open): the Codec picker lists all entries with no sizes and the
   footer `No sound input: sizes unknown`; the Sample rate row is dimmed. During a take the Codec, Sample rate and
   Bit depth / Bit rate rows are dimmed and do not open.
7. By touch on a narrow screen (zero8 upright, 320x480): every row readable, nothing cut or overlapping, the footer
   under the list (a notice, when shown, takes its place).
8. **Every codec, rate and quality reaches the file** (a card with more than one rate; a USB card with 44.1 and 48 kHz is
   enough, 24-bit and 96 kHz if the card has them). For each of the following, choose it in Settings, REC for 10 s by the
   counter, STOP, and read the take's row in the list and `file` / `soxi` / `ffprobe` on the file (rate, channels, bits,
   codec):
   - Codec WAV, Sample rate 44.1 kHz, Bit depth 16-bit: the row ends `44.1k/16 WAV`; the file says 44100 Hz, 16 bit.
   - the same at 48 kHz, and at 24-bit if the card offers it (`48k/24 WAV`; a card without 24-bit has no Bit depth row
     at all for WAV and FLAC, and with Opus the row reads Bit rate).
   - Codec FLAC at 44.1 or 48 kHz, 16-bit (and 24-bit if offered): `.flac`, the rate and depth chosen.
   - Codec Opus at 48 kHz with Bit rate 16, 24, 32, 48, 64, 96 and 128 kbps in turn (seven takes of 10 s): `.opus`;
     `ffprobe` reports the bit rate near the one chosen (within about 15 % for 10 s), 48 kHz.
   - Opus at 16 kHz if the card has it (or another of 8, 12, 24 kHz): `.opus` at that rate. On the Sample rate page with
     Opus chosen, 44.1 kHz is dimmed (`Opus needs 8, 12, 16, 24 or 48 kHz`).
   - Set 44.1 kHz with Codec WAV, then choose Codec Opus. Expected: the Sample rate row now reads `48 kHz` (or 16 kHz) and a
     line says `Sample rate set to 48 kHz: Opus cannot record at 44.1 kHz`; the take is at that rate.
   - A quick preset (`Opus speech 24 kbps`) puts the Sample rate back to the card's default.
   Close Cassette and open it: the last choice of the three rows is back. `~/.config/cassette/settings.json` holds `format`,
   `rate` and `quality` (and `format_preset` after a preset).

Report: the row texts (value and every line) as read, the picker figure against the file size per format (a small
table), the notice texts, the window's format text after choosing FLAC and Opus, what the page looked like when a row
was dimmed, any cut or overlapping text, and for step 8 a table: choice, rate / bits / codec read from the file.

## Results

| Test | Board | Pass / fail | Notes |
|---|---|---|---|
| B0 probe |  |  |  |
| B1 10-minute take |  |  |  |
| B2 unplug |  |  |  |
| B3 power pull |  |  |  |
| B4 animation cost |  |  |  |
| B5 sound server |  |  |  |
| B6 upright 320x480 |  |  |  |
| B7 no level control |  |  |  |
| B8 trim and split |  |  |  |
| B9 keys and touch |  |  |  |
| B10 settings after reboot |  |  |  |
| B11 notices and errors |  |  |  |
| B12 speech-only headset |  |  |  |
| B13 live monitor |  |  |  |
| B14 blank cassette |  |  |  |
| B15 take number |  |  |  |
| B16 square screen |  |  |  |
| B17 FLAC and Opus |  |  |  |
| B18 FLAC and Opus playback, trim |  |  |  |
| B19 Codec, rate and quality pickers |  |  |  |

**What to send back:** the table; the probe output per board (B0); the `watch` and `take` outputs of B1, B3, B4; the
`ui` log line per board; the notice texts as you read them; screenshots or photos of anything cut, overlapping or odd.
No audio files.

**Known limits:** WAV is the default format (FLAC and Opus are optional: they need libflac / libopusenc0 and libopus0, and Opus is not offered at 44.1 kHz; the boards' CPU for them is measured in B17 and B18); no background recording (leaving the app ends the take,
which is kept); the card is found again after a replug but the take that was running is closed, not continued; the
headset's own level is weak (peaks around -17 to -7 dBFS at full level in P0): use SOFT on top of it.

**Cleanup:** `sudo dpkg -r cassette`; the takes stay in `~/Recordings` (with `.journal/`, `.edits/`, `.take-counter`),
the settings in `~/.config/cassette/`; delete them to reset. `rm ~/cassette-check.sh`.
