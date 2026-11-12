# Vinyl 0.1.0: board tests

Controller-led: the Controller installs the package and copies the test music; the owner runs the steps and says what
was seen. First board: the HackBerry (CM5, 720x720 square, PipeWire, the XWF case speaker and the Evolve 65 over
Bluetooth); a Lite board (zero8 or the deck, a USB sound card) for B11. Numbers follow report 059 section 9 (B1-B12),
trimmed to what 0.1.0 has.

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
| B1 | Open Vinyl, open the test folder, tap an MP3 | it plays on the default output; title, artist, cover (or the drawn sleeve), the record turns |
| B2 | Play the FLAC 24/48, the Opus and the Vorbis files | they play; the time counts to the file's length; the chip says FLAC 48 / 24 |
| B3 | Play the M4A (AAC, ALAC) | they play (FFmpeg is on the HackBerry). On a board without FFmpeg: the notice "This format needs the FFmpeg libraries..." and no crash |
| B4 | Play the gapless album across a track change | no click, no gap; the title changes when the next track is heard |
| B5 | Tap the seek bar at about 3/4 of a VBR MP3 | it continues within about a second of that point |
| B6 | Super (hide) while playing; open two other apps; come back with Alt+Tab | the music went on; the right track and time show |
| B7 | Output > Evolve 65 while playing; switch the headset off | playback pauses; nothing comes out of the case speaker; the red notice "Evolve 65 disconnected. Paused." |
| B8 | The speaker's and the headset's play/pause and next keys, Vinyl in front and hidden; the keyboard's media keys in front | each press acts once (one toggle, one skip) |
| B9 | The volume keys while playing on the default output | the level changes; the "Vol" chip follows within a second or two |
| B10 | Esc at the browser (quit), open Vinyl again | the same track at the same time, paused; Space plays on |
| B11 | Lite board (zero8 or the deck) with the USB card: Output > the card, play | it plays; no Bluetooth row; the strip "Bluetooth audio is not set up on this board." |
| B12 | Portrait (zero8 in PDA mode or the Pi 3A+) and square (HackBerry): every page | no app title in the portrait band; nothing off the screen; the drafts' layout |

## Cleanup

`sudo dpkg -r vinyl`; `rm -r ~/.config/vinyl`; remove the test music the Controller copied.
