# Vinyl 0.1.0: board tests

Controller-led: the Controller installs the package and copies the test music; the owner runs the steps and says what
was seen. First board: the HackBerry (CM5, 720x720 square, PipeWire, the XWF case speaker and the Evolve 65 over
Bluetooth); a Lite board (zero8 or the deck, a USB sound card) for B11. Numbers follow report 059 section 9 (B1-B12),
trimmed to what 0.1.0 has; B13-B16 are the design-QA round (the transport press of the owner ruling of 2026-11-12, the
volume strip).

## Preparation (Controller)

1. `sudo apt install ./vinyl_0.1.0_arm64.deb` (apt adds libmpg123-0t64 and libvorbisfile3 if missing; FFmpeg is already on
   the HackBerry).
2. Copy test music to `~/Music`: the study's set (`059-testset`: MP3 CBR and VBR, FLAC 16/44 and 24/48, Vorbis, Opus, AAC
   and ALAC M4A, WAV, the CJK MP3) in one folder, and a gapless album (an MP3 LAME album or two FLAC halves of one track) in
   another.
3. Logs: the launcher's journal (`journalctl --user -u <launcher service> -f`); Vinyl's lines start with `[vinyl]`.

## Steps (owner)

| # | Do | Expected |
|---|---|---|
| B1 | Open Vinyl, open the test folder, tap an MP3 | it plays on the default output; title, artist, cover (or the drawn sleeve), the record turns | pass
| B2 | Play the FLAC 24/48, the Opus and the Vorbis files | they play; the time counts to the file's length; the chip says FLAC 48 / 24 | pass
| B3 | Play the M4A (AAC, ALAC) | they play (FFmpeg is on the HackBerry). On a board without FFmpeg: the notice "This format needs the FFmpeg libraries..." and no crash | pass
| B4 | Play the gapless album across a track change | no click, no gap; the title changes when the next track is heard | pass
| B5 | Tap the seek bar at about 3/4 of a VBR MP3 | it continues within about a second of that point | pass
| B6 | Super (hide) while playing; open two other apps; come back with Alt+Tab | the music went on; the right track and time show | pass
| B7 | Output > Evolve 65 while playing; switch the headset off | playback pauses; nothing comes out of the case speaker; the red notice "Evolve 65 disconnected. Paused." | partial pass: when evolve reconnects the volume is not the same as set before
| B8 | The speaker's and the headset's play/pause and next keys, Vinyl in front and hidden; the keyboard's media keys in front | each press acts once (one toggle, one skip) | could not fully test, evolve 65 only have volume buttons and it worked.
| B9 | The volume keys while playing on the default output | the level changes; the "Vol" chip follows within a second or two | partial pass, on evolve 65 the vol chip makes more than a second to reflect.
| B10 | Esc at the browser (quit), open Vinyl again | the same track at the same time, paused; Space plays on | pass
| B11 | Lite board (zero8 or the deck) with the USB card: Output > the card, play | it plays; no Bluetooth row; the strip "Bluetooth audio is not set up on this board." | pass
| B12 | Portrait (zero8 in PDA mode or the Pi 3A+) and square (HackBerry): every page | no app title in the band at 320x480 (the title shows at 480x640 and 480x800); nothing off the screen; the drafts' layout | pass
| B13 | Now playing, a keyboard in front: Left within the first 2 s of a track; Left again after 5 s of it; Right | Left within 2 s: the previous track; Left after 2 s: the same track from 0:00; Right: the next track. Each press acts once, on its release | pass
| B14 | Hold Right (about 2 s), let go; hold Left (about 2 s), let go; once playing, once paused | after half a second the time runs ahead (back) at about 3x with no sound, no scratch; on release the sound comes back from there; paused stays paused; holding Left stops at 0:00 (never the previous track); holding Right past the end goes on into the next track. No extra skip on release | pass
| B15 | The same short and held presses with the headset's / speaker's next and previous buttons, in front and hidden, and on the touch keys beside play (and the mini player's next key) | the same results as B13 and B14 (if a headset sends no release while held, say what happened: the log has `media key: next` and `(released)` lines) | pass
| B16 | Tap the "Vol" chip; tap "+" twice, "-" once; wait; tap the chip twice | the strip "-", the level, its bar, "+" replaces the seek row; the level goes up 10, down 5 (the launcher's volume on the default output: the volume keys show the same level); it closes 4 s after the last tap, and a second tap on the chip closes it at once; a tap at the bottom edge of the chip closes the strip (never "+"), a tap at the top edge of a transport key plays that key (never "-" / "+") | pass

B8 note (2026-11-12): only the Evolve 65's volume buttons could be tried (it has no play/pause or next button); the
play/pause and next keys of a headset or speaker remain untested.

Retests after the B7 / B9 fixes (2026-11-12):

| # | Do | Expected |
|---|---|---|
| B17 | B9 again: the volume keys, then the Evolve 65's own volume buttons, while playing on the default output | the "Vol" chip follows each press within about half a second; the journal has `[vinyl] volume: system NN % from the sink` |
| B18 | B7 again with Output > Evolve 65 chosen: set a level (the strip or the keys), switch the headset off, on again, play | the level heard is the one set before; the chip shows it; the journal's `[vinyl] volume:` lines say which volume is in force (Vinyl's own for an output that is not the default, else the system's) |

## Cleanup

`sudo dpkg -r vinyl`; `rm -r ~/.config/vinyl`; remove the test music the Controller copied.
