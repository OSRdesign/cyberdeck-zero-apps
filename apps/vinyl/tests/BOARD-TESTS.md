# Vinyl 0.1.0: board tests

Controller-led: the Controller installs the package and copies the test music; the owner runs the steps and says what
was seen. First board: the HackBerry (CM5, 720x720 square, PipeWire, the XWF case speaker and the Evolve 65 over
Bluetooth); a Lite board (zero8 or the deck, a USB sound card) for B11. Numbers follow report 059 section 9 (B1-B12),
trimmed to what 0.1.0 has; B13-B16 are the design-QA round (the transport press of the owner ruling of 2026-11-12, the
volume group), B17-B19 the retests of 2026-11-12, B20-B24 the round of 2026-11-13 (Clear all, the seek tap, the
Raspberry Pi headphone jack, the approved 640x480 and 480x320 layouts).

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
| B9 | The volume keys while playing on the default output | the level changes; the "Vol" chip shows the playing output's level and follows within a second or two | partial pass, on evolve 65 the vol chip makes more than a second to reflect.
| B10 | Esc at the browser (quit), open Vinyl again | the same track at the same time, paused; Space plays on | pass
| B11 | Lite board (zero8 or the deck) with the USB card: Output > the card, play | it plays; no Bluetooth row; the strip "Bluetooth audio is not set up on this board." | pass
| B12 | Portrait (zero8 in PDA mode or the Pi 3A+) and square (HackBerry): every page | no app title in the band at 320x480 (the title shows at 480x640 and 480x800); nothing off the screen; the drafts' layout | pass
| B13 | Now playing, a keyboard in front: Left within the first 2 s of a track; Left again after 5 s of it; Right | Left within 2 s: the previous track; Left after 2 s: the same track from 0:00; Right: the next track. Each press acts once, on its release | pass
| B14 | Hold Right (about 2 s), let go; hold Left (about 2 s), let go; once playing, once paused | after half a second the time runs ahead (back) at about 3x with no sound, no scratch; on release the sound comes back from there; paused stays paused; holding Left stops at 0:00 (never the previous track); holding Right past the end goes on into the next track. No extra skip on release | pass
| B15 | The same short and held presses with the headset's / speaker's next and previous buttons, in front and hidden, and on the touch keys beside play (and the mini player's next key) | the same results as B13 and B14 (if a headset sends no release while held, say what happened: the log has `media key: next` and `(released)` lines) | pass
| B16 | Every layout (HackBerry square, PDA mode portrait, 800x480, the deck's 640x480, a 480x320 board): tap "+" twice and "-" once in the volume group, then keep pressing "-" until it shows Muted; press "-" once more; press "+"; then tap the output button (the output chip on 800x480) | the level between "-" and "+" goes up 10, down 5, the playing output's own level (pactl / amixer agree); at the card's lowest level the next "-" mutes: the group shows the speaker-off mark and "Muted" (never a number); "-" while muted changes nothing; "+" brings the sound back at the last level (one step above the lowest when that is where it was). Muting from elsewhere (the launcher's mute key, alsamixer) shows Muted too. The output button or chip opens Output | pass (the strip, before this round: see the note)

B16 note (2026-11-13): rewritten for the owner's design changes (the volume group on every layout, no Vol chip, no
strip, and the muted step of the owner's deck report); the result recorded beside it is the owner's for the strip as it
was on every layout: the group needs a run.

B8 note (2026-11-12): only the Evolve 65's volume buttons could be tried (it has no play/pause or next button); the
play/pause and next keys of a headset or speaker remain untested.

Retests after the B7 / B9 fixes (2026-11-12):

| # | Do | Expected |
|---|---|---|
| B17 | B9 again: the volume keys, then the Evolve 65's own volume buttons, while playing on the default output | the "Vol" chip follows each press within about half a second; the journal has `[vinyl] volume: system NN % from the sink` |
| B18 | B7 again with Output > Evolve 65 chosen: set a level (the strip or the keys), switch the headset off, on again, play | the chip shows the Evolve's own level after the reconnect: the level set before, unless the headset or the system changed it (then the chip shows that value, and nothing plays louder than the chip says); while the headset is gone or its level is not read yet the chip shows "Vol --" (never a negative number, in the chip or the journal); the journal's `[vinyl] volume: sink ...` lines give it |
| B19 | Output > the USB DAC (not the default output): read the level, then `pactl list sinks` (or `wpctl status`); tap "+" twice, "-" once | the group shows the DAC's own level, the same number as pactl / wpctl; "-" / "+" change the DAC (never the default sink, whose level stays); on the deck zero7 (no sound server) the same with the card's mixer: the level follows `amixer -c <card>` on the sound server's curve (50 % = -18 dB, 40 % = -24 dB, 25 % = -36 dB: the same loudness as the same percent on the HackBerry), "+" never past 0 dB |

Round of 2026-11-13:

| # | Do | Expected |
|---|---|---|
| B20 | Queue: tap Clear all; tap Cancel. Clear all again, Esc. Clear all again, tap outside the question. Clear all again, tap Clear. With a keyboard: C, Right, Enter | each time "Clear the queue?" with Cancel (highlighted, the default) and Clear over the queue buttons; Cancel, Esc and a tap outside leave the queue as it was; Clear (or Enter on it) leaves only the playing track, which plays on. An empty queue shows the four buttons dimmed |
| B21 | Now playing: tap the seek bar's line, then just above and just below it (half a finger off), then the top edge of a transport key under it and the bottom edge of the output button or the volume group above it | the taps on and near the line seek there; the key's edge plays that key, the button's edge opens Output, the group's edge changes the volume: a tap is never taken by a neighbour |
| B22 | Pi 4B and Pi 3A+, the headphone jack (no sound server): Output > Headphones; set 5 %, 50 % and 100 % with the group; after each, `amixer -c 0 sget PCM`; listen at 5 % and 50 %, and compare 50 % with 50 % on the HackBerry | amixer shows about -78.06 dB at 5 %, -18.06 dB at 50 %, 0.00 dB at 100 % (never above 0 dB, never raw 391 / +3.91 dB again); 5 % is quiet, 50 % sounds about as loud as 50 % on the HackBerry; the journal has `volume: set mixer hw:CARD=Headphones 'PCM Playback Volume' 5 % raw=-7806 (target -78.06 dB) read back raw=-7806`. `vinyl-mixer-probe hw:CARD=Headphones --set 5` gives the same numbers |
| B23 | The deck (640x480): now playing, playing; tap the group's "+" and "-", the output button, Browse, Queue | as the approved NP-640x480: the cover with the record sliding out, title on up to two lines, artist, album, the format and Gapless chips (flat, no action), the output button (icon, name, chevron), the seek row, five transport keys, Browse, Queue and the volume group in the bottom row; no Up next, no Vol chip, no Menu key. Each control does its job; the touch rows are 70 px on the deck (under the 7 mm, R-4): say whether they are easy to hit |
| B24 | A 480x320 board (zero8 in landscape, or the Pi 3A+'s panel in landscape): now playing; tap the output button, the group's "-" and "+", the queue key | as the approved NP-480x320: title, artist, album, one plain line "WAV 44.1 / 16" (", gapless" at its end when the next track continues), the output button, the volume group under it (34 px high), the seek row, six transport keys; no Vol chip, no strip. The record's label shows about a third out of the sleeve, its cream dot turning while it plays. Say whether the group and the seek row (43 px touch rows) are easy to hit |

## Cleanup

`sudo dpkg -r vinyl`; `rm -r ~/.config/vinyl`; remove the test music the Controller copied.
