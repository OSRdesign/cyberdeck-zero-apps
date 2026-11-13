# Vinyl 0.1.0: board tests

Controller-led: the Controller installs the package and copies the test music; the owner runs the steps and says what
was seen. First board: the HackBerry (CM5, 720x720 square, PipeWire, the XWF case speaker and the Evolve 65 over
Bluetooth); a Lite board (zero8 or the deck, a USB sound card) for B11. Numbers follow report 059 section 9 (B1-B12),
trimmed to what 0.1.0 has; B13-B16 are the design-QA round (the transport press of the owner ruling of 2026-11-12, the
volume group), B17-B19 the retests of 2026-11-12, B20-B24 the round of 2026-11-13 (Clear all, the seek tap, the
Raspberry Pi headphone jack, the approved 640x480 and 480x320 layouts), B25-B28 the levels fitted to each output (the
owner's decision of 2026-11-13, one test per kind of output), B29 the seek bar's drag.

## Preparation (Controller)

1. `sudo apt install ./vinyl_0.1.0_arm64.deb` (apt adds what is missing, FFmpeg's libraries included: about 104 MB on a
   Lite board with `--no-install-recommends`, about 462 MB through the launcher's installer today).
2. Copy test music to `~/Music`: the study's set (`059-testset`: MP3 CBR and VBR, FLAC 16/44 and 24/48, Vorbis, Opus, AAC
   and ALAC M4A, WAV, the CJK MP3) in one folder, and a gapless album (an MP3 LAME album or two FLAC halves of one track) in
   another.
3. Logs: the launcher's journal (`journalctl --user -u <launcher service> -f`); Vinyl's lines start with `[vinyl]`.

## Steps (owner)

| # | Do | Expected |
|---|---|---|
| B1 | Open Vinyl, open the test folder, tap an MP3 | it plays on the default output; title, artist, cover (or the drawn sleeve), the record turns | pass
| B2 | Play the FLAC 24/48, the Opus and the Vorbis files | they play; the time counts to the file's length; the chip says FLAC 48 / 24 | pass
| B3 | Play the M4A (AAC, ALAC), on every board after installing the package (no FFmpeg step) | they play everywhere: FFmpeg comes with the package (decision D20, 2026-11-13); the journal has `FFmpeg loaded for a tier-2 file` at the first one, not at start. The notice "The FFmpeg libraries could not be loaded. Reinstall Vinyl." only if the libraries are broken | pass (HackBerry, before D20: retest on a Lite board)
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
| B16 | Every layout (HackBerry square, PDA mode portrait, 800x480, the deck's 640x480, a 480x320 board): tap "+" twice and "-" once in the volume group, then keep pressing "-" until it shows muted; press "-" once more; press "+"; then tap the output button (the output chip on 800x480) | the level between "-" and "+" goes up two of the output's levels, down one (B25-B28), the playing output's own level (pactl / amixer agree); at the first level the next "-" mutes: the level box turns dark with the white speaker-off mark, and "Mute" beside it on 800x480 and 480x320 (the mark alone, large, on the deck's 640x480, the square and PDA portrait screens), never "Off" and never a number; "-" while muted changes nothing; "+" brings the sound back at the last level. Muting from elsewhere (the launcher's mute key, alsamixer, `pactl set-sink-mute @DEFAULT_SINK@ 1`) shows the same. The output button or chip opens Output | pass (the strip, before this round: see the note)

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
| B19 | Output > the USB DAC (not the default output): read the level, then `pactl list sinks` (or `wpctl status`); tap "+" twice, "-" once | the group shows the DAC's own level, the same number as pactl / wpctl; "-" / "+" change the DAC (never the default sink, whose level stays); on the deck zero7 (no sound server) the same with the card's mixer: the level follows `amixer -c <card>` as the nearest of the card's own levels (B25-B27), "+" never past the card's top |

Round of 2026-11-13:

| # | Do | Expected |
|---|---|---|
| B20 | Queue: tap Clear all; tap Cancel. Clear all again, Esc. Clear all again, the header's Esc. Clear all again, tap outside the dialog. Clear all again, tap Clear. With a keyboard: C, Enter; then C, Y. Then, with an empty queue (Remove the last track), try shuffle, repeat and the Queue key on now playing | each time the toolkit's dialog "Clear the queue?" with Cancel (the toolkit's primary colour) and Clear, and the text "Y = Clear, Enter or Esc = Cancel"; Cancel, Enter, Esc and the header's Esc close it and the Queue page stays; a tap outside does nothing; Left / Right do nothing; Clear (or C, Y) leaves only the playing track, which plays on; then Move up, Move down and Clear all are dimmed and do nothing. With an empty queue the transport keys are dimmed evenly (icons too) and do nothing, shuffle and repeat included; the Queue key is not dimmed and opens the empty queue |
| B21 | Now playing: tap the seek bar's line, then just above and just below it (half a finger off), then the top edge of a transport key under it and the bottom edge of the output button or the volume group above it | the taps on and near the line seek there; the key's edge plays that key, the button's edge opens Output, the group's edge changes the volume: a tap is never taken by a neighbour |
| B22 | Pi 4B and Pi 3A+, the headphone jack (no sound server): Output > Headphones; see B26 | (replaced by B26: the levels of the owner's decision of 2026-11-13) |
| B23 | The deck (640x480): now playing, playing; tap the group's "+" and "-", the output button, Browse, Queue | as the approved NP-640x480: the cover with the record sliding out, title on up to two lines, artist, album, the format and Gapless chips (flat, no action), the output button (icon, name, chevron), the seek row, five transport keys, Browse, Queue and the volume group in the bottom row; no Up next, no Vol chip, no Menu key. Each control does its job; the touch rows are 70 px on the deck (under the 7 mm, R-4): say whether they are easy to hit |
| B24 | A 480x320 board (zero8 in landscape, or the Pi 3A+'s panel in landscape): now playing; tap the output button, the group's "-" and "+", the queue key | as the approved NP-480x320: title, artist, album, one plain line "WAV 44.1 / 16" (", gapless" at its end when the next track continues), the output button, the volume group under it (34 px high), the seek row, six transport keys; no Vol chip, no strip. The record's label shows about a third out of the sleeve, its cream dot turning while it plays. Say whether the group and the seek row (43 px touch rows) are easy to hit |
| B25 | The Sharkoon DAC, on the HackBerry (through PipeWire) and on zero8 (no server): press "-" until level 1, then "+" one press at a time to 100 %, listening; at 52 % read `amixer -c <card> sget PCM` | 23 levels: each press a little louder (about 2 dB), from the first press on (no dead stretch at the bottom, no jump at a quarter); level 1 is quiet but heard; 52 % is about -22 to -23 dB and 100 % 0 dB on both boards (the same table: the journal's `volume: sink ... -45.00..0.00 dB, 23 levels of 2.05 dB` on the HackBerry, `volume: hw:CARD=... 'PCM Playback Volume' ... 23 levels of 2.05 dB` on zero8) |
| B26 | Pi 4B and Pi 3A+, the headphone jack (no server): from Muted press "+" through every level; at 3 %, 50 % and 100 %, `amixer -c 0 sget PCM` | 30 levels of about 2 dB: 3 % (level 1) is about -56 dB, quiet but heard; 50 % about -27 dB; 100 % +4.00 dB (the jack's own top, raw 400); never raw 391 at a low level again; each press logs `volume: set hw:CARD=Headphones 'PCM Playback Volume' step i/30 P % target X dB raw=M read back raw=M`. `vinyl-mixer-probe hw:CARD=Headphones` lists the same 30 levels |
| B27 | The deck zero7 with the Apple USB-C adapter (no server): the same walk; `amixer -c <card> sget Headset` at 50 % and 100 % | 30 levels of about 2 dB over -60..0 dB; 50 % about -31 dB; 100 % 0 dB |
| B28 | A Bluetooth headset or speaker (HackBerry or a Pi 5 board): the same walk, then the headset's own volume buttons | 30 levels over the server's -60..0 dB; the headset's buttons show as the nearest level within about half a second |
| B30 | The owner's mute report (2026-11-13): the deck zero7 (640x480, USB card with its switch) and RPI52 (800x480): mute with "-" at the first level, then with `amixer -c <card> sset PCM mute` (deck) or `pactl set-sink-mute @DEFAULT_SINK@ 1` (RPI52) | the deck: the dark level box with a large white speaker-off mark (about two thirds of the box high), no word; RPI52: the dark box with the mark and "Mute"; both read as muted at arm's length; "+" brings the level back |
| B31 | zero8 on HDMI only (no playback mixer: Vinyl's software volume): press "-" to the first level, once more, then "-" again; quit Vinyl from the launcher and open it again; press "+" | the same muted box as B30 (480x320: the mark and "Mute"), no sound; "-" muted changes nothing; still muted after the restart; "+" plays again at the first level (log: `volume: software step ... (muted, level 1 kept)`, then `step 1/30`) |
| B29 | Now playing, playing: press on the seek bar's knob and drag it slowly to the right and back, let go near the middle; then the same while paused; then press on the bar and drag off it (up onto the keys, sideways past its ends) before letting go; then a plain tap on the bar | while the finger is down the knob and the fill follow it and the elapsed time shows where it will land (the remaining time with it); the music plays on (or stays paused) without jumping; on release it continues from there, once; past the bar's ends the knob stops at the end; sliding onto a key neither presses that key nor stops the drag; the tap seeks where it lands as before |

## Cleanup

`sudo dpkg -r vinyl`; `rm -r ~/.config/vinyl`; remove the test music the Controller copied.
