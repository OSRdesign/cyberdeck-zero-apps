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
2. Open Cassette with only that headset plugged in. Expected: the strip says `16k/16 WAV`, Settings > Format reads
   `WAV 16 kHz 16-bit`, the meter moves when you speak.
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

**What to send back:** the table; the probe output per board (B0); the `watch` and `take` outputs of B1, B3, B4; the
`ui` log line per board; the notice texts as you read them; screenshots or photos of anything cut, overlapping or odd.
No audio files.

**Known limits of 0.1:** WAV only (FLAC and Opus come in 0.2); no background recording (leaving the app ends the take,
which is kept); the card is found again after a replug but the take that was running is closed, not continued; the
headset's own level is weak (peaks around -17 to -7 dBFS at full level in P0): use SOFT on top of it.

**Cleanup:** `sudo dpkg -r cassette`; the takes stay in `~/Recordings` (with `.journal/`, `.edits/`, `.take-counter`),
the settings in `~/.config/cassette/`; delete them to reset. `rm ~/cassette-check.sh`.
