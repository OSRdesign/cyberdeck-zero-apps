# Vinyl: decisions in force

The owner's rulings and the accepted points that shape Vinyl, newest last within each section. Where the owner spoke,
the owner's words are quoted exactly; everything else says who decided. The study behind the app is the launcher repo's
`docs/dev/reports/059-music-player-study.md`; the design QA rulings R-1 to R-6 are in the launcher repo's
`docs/dev/decisions.md` (2026-11-12) and `docs/dev/checklists/app-design.md`.

## Name

- **2026-11-12: the name and id `vinyl` ("Vinyl").** Report 059 D1 ("Name and id: owner's choice"). The owner chose
  Vinyl; no wording of the owner is recorded here.

## Report 059 decisions D2-D18 (accepted 2026-11-12)

- **D2 Decode:** small libraries for the common formats (mpg123, vorbisfile, libFLAC, libopus, own WAV/AIFF) and FFmpeg
  as the wide net for the rest.
- **D3 FFmpeg packaging:** `Recommends` (61 or 62), loaded at run time only when a file needs it; Vinyl works without it.
  **Replaced by D20 (owner, 2026-11-13): FFmpeg is a Depends; see "D20" below.**
- **D4 MVP scope:** folder first; the library database is P2.
- **D5 Output:** libpulse when a sound server runs, ALSA `default` otherwise; the codec is WirePlumber's choice; no
  direct `hw:` in the MVP.
- **D6 Code sharing:** Cassette's modules copied now; an opt-in shared `apps/_audio` in P3.
- **D7 Background:** `X-Background=run`; below 480 px on the short side the app owns the panel (no daemon).
- **D8 Device events:** pause when the playing output disconnects; never move to another sink; Automatic follows the
  default sink at play or at the next track; keep playing at call quality with a hint.
- **D9 Media keys:** Vinyl reads the media-key devices itself in the MVP; launcher forwarding later.
- **D10 AVRCP metadata / MPRIS** (BlueZ `RegisterPlayer`): P2.
- **D11 Volume:** the system volume (the launcher's keys) on the default sink; an in-app stream volume when an explicit
  non-default sink plays; software volume only without a mixer. **Changed by the owner on 2026-11-12: see "D11 changed"
  below.**
- **D12 Library store:** SQLite through dlopen, `~/Music` and chosen mounts, an incremental idle scan (P2).
- **D13 Hi-res:** 24-bit kept (S32/F32 streams), the server resamples, no bit-perfect mode.
- **D14 Out of scope:** MIDI, native DSD/DoP (DSF as PCM through FFmpeg only), SACD images, tag editing, video.
- **D15 Later tier:** internet radio, podcasts, scrobbling, speed, smart playlists (P3 and later).
- **D16 Screen while playing in front:** `dim allow` on battery boards (sent where board.state's `requests` lists
  `dim`: design QA D-72); a generic launcher verb later.
- **D17 Screen sizes:** Cassette's classes (480x320, 320x480, square, large), no 320x170 on main; the coordinator's
  design call.
- **D18 Lite boards:** wired outputs only until report 058's D2 (a sound server on Lite boards) is decided.

## The top band is the launcher's (2026-11-12)

- No app chips, counters or levels in the band: the band is the toolkit's header (`cp0::ui::Page`, its title, its Esc
  and the shared status strip), as in Cassette; the volume chip lives in the body. The owner: "the top band is coming
  from the launcher and must respect its rules so you can't put what you are showing here".
- No app name in the band at 320x480 (the toolkit's narrow portrait shows only the Esc and the status strip). The owner,
  on a draft that still had it: "on portrait 320x480 you still write the app name in the launcher band". At 480x640 and
  480x800 the title shows as the toolkit places it (design QA D-10, the coordinator).

## Transport keys (owner ruling, 2026-11-12)

The owner: "single press right goes to next track, single press left restart current track, if player is within the
first 2 seconds it goes to the previous track on list. long right press is a fast forward (muted) x3 , long left press is
a fast rewind (muted) x3. usually those are industry standard music players behavior."

- The same behaviour for the arrows on now playing, the comma and dot keys, the on-screen previous and next keys and the
  media keys, in one module (`src/core/transport`); the 2 s threshold is one constant (`kRestartAfterMs`).
- An exception to design QA D-29 (Left = back, Right = Enter) on the now playing page only; Queue and Output keep
  Left = back and Right = Enter.
- Shift+Left / Shift+Right seek 10 s: the coordinator's addition (the old Left / Right seek moved there).

## Touch size (owner ruling R-4, 2026-11-12)

The owner: "7mm is a minimum recommendation not a requirement, if for some reason UI do not allow it, it's down to me to
decide usability".

## Accepted for P1

Each accepted for P1 by the coordinator, owner informed (no owner wording recorded):

- The sleep-timer and Equalizer keys of the square and large drafts are drawn dimmed with no action (P2 features).
  **Superseded by P2a (0.2.0, below):** the square layout's sixth key is the Menu key, the large layout's sleep-timer
  key opens the Sleep timer page; nothing is drawn dimmed any more.
- Browser rows act on the first tap (open a folder, play a track) at the compact list height.
- Controls drawn smaller than 7 mm keep their drawn look and get a larger invisible touch area (R-4 applies).
- The volume group's "-" and "+" are drawn as the drafts give them (lower than the touch target on most layouts); their
  touch areas are the target high where the screen has the room (`volume_group_buttons` line of the QA hand-off). The
  volume strip of 2026-11-12 is gone (see "640x480 and 480x320" below).
- The icon: four colours (cream sleeve, teal ring and label, black record, grey grooves) and thin groove rings, an
  exception to the palette's rule 3; the coordinator's design call (2026-11-12).
- At 480x480 the six transport keys cannot all be the target wide (80 px: 6 x 80 is more than the row). Accepted as a
  WARN by the coordinator under R-4 (2026-11-12), first as overlapping touch areas; since 2026-11-13 the areas meet in
  the middle of the gaps (70 px wide each) and nothing overlaps.

## D11 changed: the volume of the playing output (owner, 2026-11-12)

The owner accepted a change to report 059's D11, relayed by the coordinator; the owner's words, recorded on
2026-11-13: "accept the D11 change, send it to the worker". The reason, from the boards: on the HackBerry Vinyl played to
an explicitly chosen USB DAC; the chip, the strip and the in-app volume only moved Vinyl's own level (already 100 %, so
nothing could get louder), while the playing sink stayed at its own 40 % (-23.9 dB) and the launcher's keys moved the
default sink, another device: Vinyl's "100 %" was about -24 dB. On the deck zero7 (no sound server) the same adapter's
mixer sat at -20 dB (this line first said zero8: the coordinator's correction of 2026-11-13, the board was the deck
zero7). The owner's board results that led there: B7 "partial pass: when evolve reconnects the volume is not the same
as set before"; B9 "partial pass, on evolve 65 the vol chip makes more than a second to reflect."

- The Vol chip, the volume strip and the `-` / `=` keys set the volume of the output Vinyl actually plays on, and the
  chip shows that real level.
- With a sound server: the sink's own volume (set with pa_context_set_sink_volume_by_name, read from the sink list), in
  the server's own percent (the number pactl / wpctl show), default sink or not, Bluetooth included: a headset keeps its
  level across a reconnect, and if the device changes it the chip shows the real value. Vinyl's stream stays at 100 %.
- Without a sound server: the card's playback mixer control, never above 0 dB; the control's switch mutes at 0 %. Only a
  card with no playback mixer gets Vinyl's software volume (kept per output in state.json).
- Follow-up, the coordinator (UI design, 2026-11-13): one curve everywhere. The deck zero7's probe showed the ALSA mapping
  linear in dB (50 % = -30 dB) while the sound server's percent is cubic (50 % = -18.06 dB): the same number was 12 dB
  apart between boards, which is why the owner heard them as inconsistent. Without a server Vinyl now uses the server's
  curve, dB = 60 log10(p / 100) (40 % = -23.88 dB, 25 % = -36.1 dB, 100 % = 0 dB), clamped to the control's range (a
  range that starts higher clamps at its minimum, and the chip shows what is set: p = 100 * 10^(dB / 60), rounded), never
  above 0 dB or the control's max; a step down that cannot move the control any more mutes. The software volume uses the
  same curve (amplitude (p / 100)^3). A control without dB data stays linear in steps. Same percent, same loudness, on
  every board. The owner's listening test of the curve (2026-11-13): "yes volume at 50% is perceived the same, it's a
  pass": the same output (the Sharkoon DAC) on two boards (the HackBerry and zero8), not a rule for every output. The
  owner's correction, 2026-11-13: "50% sounds the same was for the same output DAC used on different boards not a
  generic rule independent of the hardware used." **This curve is replaced by the levels of "Volume levels fitted to
  each output" below.**
- The volume controls the output Vinyl plays on; the launcher's volume keys still act on the default sink; when the
  chosen output is the default sink, Vinyl's level follows the launcher's keys (the README's rule, design QA run 3 D-71).
- The separate in-app volume is gone from the UI; an older state.json's `app_volume` is read and kept, never applied.
- The chip refreshes on the server's change events (pa_context_subscribe) within a tick and a list round trip; the mixer
  is read every 300 ms. When the playing output is the default sink, the launcher's volume keys show through the same
  events (the keys themselves still act only on the default sink: a launcher matter).
- Every playback stream opens unmuted at 100 % with its own restore key (`module-stream-restore.id`
  `sink-input-by-application-id:vinyl`) and WirePlumber's `state.restore-props` / `state.restore-target` false, so no
  server re-applies another music app's stream level; the stream's identity says vinyl (it said cassette, a leftover of
  the copied code). Board check (HackBerry, 2026-11-13): pw-dump shows `sink-input-by-application-id:vinyl` and
  `state.restore-props` / `state.restore-target` false on Vinyl's node, while `pactl list sink-inputs` shows
  `sink-input-by-media-role:music`: pipewire-pulse displays its own computed key there. Harmless as long as restore is
  off for the node (the stream also opens at its own 100 %).
- A level that is not known (the sink gone, not read yet, or reported without a volume) shows as "Vol --" and is
  logged as "--", never as a negative number (board test B18, 2026-11-13).

## Volume group and output button on the square and portrait layouts (owner, 2026-11-13)

The owner (drafts NowPlaying-480x480 and NowPlaying-320x480 updated by the coordinator): "that label for the output is
redundant with the output button, output is not something you change that often. I would replace for PDA mode and
square the output button for volume controls with the volume value between - and + buttons (keeping the green
background), remove the volume label and expand the output one that can be selected to change the output device."

- Square (480x480, 720x720) and portrait (320x480, 480x640, 480x800, the other portrait canvases): the bottom row's
  Output key is a volume group, "-", the level (the number on teal, white bold), "+"; 480x480: Browse 120, Queue 120, the
  group 180 (52, 68, 52, 4 px gaps); 320x480: Queue 100, the group 164 (48, 60, 48). It sets and shows the volume of the
  playing output (D11 as changed); the level itself is no control. No Vol chip and no volume strip on these layouts.
- The output chip is a button there: the Bluetooth or speaker icon, the device's name only (call quality stays a
  notice), a chevron; the full width of the info column (portrait: right of the format chip); a name too long for it
  scrolls, never ends with dots; a tap opens the Output page (the only way there by touch on these layouts).
- 480x320 and 800x480 stayed as they were at first (the Vol chip and its strip); both have the group since (the large
  layout below, 480x320 in "640x480 and 480x320").
- The coordinator, same day: the time labels as wide as their format needs, h:mm:ss for a track of an hour or more on
  both sides of the seek bar (a one-hour mixtape showed "-3..."); a sink's name without the server's profile words
  ("USB Audio Device Analog Stereo" -> "USB Audio Device").

## The large layout and the Menu key (owner, 2026-11-13)

The owner's answers on the full-UX canvas: the Menu key is accepted as the entry point for tools and settings
(equalizer, sleep timer, playback, library, output, about; the Menu pages are P2, not built now); the large layout:
"on large the volume label "Vol 60" is useless with the volume controls at the bottom of the screen"; landscape
480x320 kept its Vol chip and strip that day, until the owner approved NP-480x320 (below).

- Large (800x480 and the other two-pane canvases): the bottom row is Browse | Output | the volume group ("-" 40, the
  level 52 on teal, "+" 40, 4 px gaps; the keys 140 wide), as NP-800x480 of the canvas draws it with Output where its
  Menu key will be; the dimmed Equalizer key is dropped. No Vol chip and no volume strip on large; the output chip stays
  a chip that opens Output, on a row of its own, scrolling when a name is too long.
- Not built now: the Menu key and its pages (P2). The layout data leaves room: a Menu key can take the dimmed sleep-timer
  key's place on square and be a seventh key on landscape.

## Muted shows as muted (owner, 2026-11-13)

The owner's report from the deck: "zero7 is showing muted but volume bar shows 18" (the deck zero7, not zero8: the
coordinator's correction, 2026-11-13).

- Muted, the volume group shows the speaker-off mark and "Muted" (the mark alone where the word does not fit), never the
  level it will come back at; the same for a mute from elsewhere (the launcher's mute key, alsamixer, a headset).
- "+" while muted brings the sound back at the last level; "-" while muted does nothing; "-" at the first level mutes
  (the card's switch, the sink's mute, or the control's own mute value; a card with none of them stays at its first
  level). With the levels fitted to each output (below) the first level is the bottom of the output's window and sounds
  (the Sharkoon's -45 dB is its level 1, 4 %), so the earlier "one step above the floor" is gone.
- The sound server's placeholder output at start (`auto_null`) never shows a level ("--").

## Muted on every layout and every path (owner, 2026-11-13)

The owner's report on the final build: "zero 8 and pi52 are not showing mute", then "arf not zero8 , I meant zero7": on
RPI52 (800x480) the group showed a speaker-off mark of about 14 px in its 52 px level box, on the deck (640x480) the
mark and "Off"; neither read as mute. Then "no need for the word muted if it doesn't fit the speaker off glyph is
enough" and "Mute is better than Muted". This replaces the "Muted, Mute, Off, the mark alone" chain (design QA run 4,
D-53):

- Muted, the level box takes the muted look, defined once and the same on every layout: a dark fill #222222 with the
  speaker-off mark and the word in white (the coordinator's pick, 2026-11-13: no extra slash, the mark already has its
  cross). The mark and "Mute" side by side where both fit with the word at the toolkit's CAPTION size or larger (BODY
  first), the mark then at least 16 px; else the mark alone, large: its drawn height about 60 % of the box, 24 px or
  more (core/layout `mute_fit`, tested at every QA size; the scenes stop on a box that breaks it). Never "Off", never
  "Muted", never the level it comes back at. What each QA size draws: qa-info.txt `muted`.
- 800x480 (the coordinator, 2026-11-13): Browse and Output 118 wide, the volume group 184 ("-" 40, the level 96, "+"
  40, 4 apart), so "Mute" fits beside the mark; the other layouts keep their widths (the mark alone where the word does
  not fit).
- The same muted state on every path: the card's switch; the control's own mute value (the Pi jack without a switch);
  the sink's mute (`pactl set-sink-mute`, the launcher's key: shown muted at the level it keeps, "+" un-mutes there);
  and Vinyl's own mute for a card without a playback mixer (Vinyl's software volume, HDMI on zero8) and for a control
  with neither a switch nor a mute value (before, "-" stopped at level 1 there and the software volume showed "0"):
  Vinyl's stream silent, the output's level kept, saved per output in state.json (`app_muted`). Everywhere "-" muted
  does nothing and "+" brings the level back. Tested: tests/test_volume.cpp (test_software_mute, test_vinyl_mute,
  test_mute_steps), the scene qa-soft-muted at every QA size.

## The record (owner, 2026-11-13)

The owner, on a deck screenshot: "the disc going out of the song cover picture seems to be too low, it appears under the
cover".

- The record is 94 % of the sleeve's height and vertically centred on it, on every layout.
- The coordinator, with the approved 640x480 and 480x320 layouts: the label is about 40 % of the record and about a
  third of it shows out of the sleeve (the coordinator said the centre sits about a tenth of a label diameter inside the
  sleeve's edge; the approved drafts measure 12 of 84 and 9.5 of 57, so Vinyl uses 0.15 of the label: it matches both
  drafts within 2 px); the label carries spin marks, a cream dot near its rim and a cream ring round the dark spindle
  hole, so the turning shows even with most of the label hidden. The record never reaches into the info column beside
  the sleeve (2 px clear): on 800x480 its label then shows less (about a fifth), on 480x480 a third less 1 px.

## Design QA run 3 (the coordinator, 2026-11-13)

- Notices on now playing cover nothing that matters: over the seek row when the band between the output button (or the
  chips, or the 480x320 volume group) and the transport keys holds the notice; else over the up-next box (square); else
  in that band at its own height when it is at least the danger strip's 30 px (480x320: 38 px, two lines of text fit);
  else over the bottom row.
- The mini player's times are always whole ("01:14 / 4:04", h:mm:ss for an hour or more); the artist before them takes
  the dots.
- No two touch areas overlap anywhere (a unit test checks every one of now playing, the queue page and the mini player
  at every QA size). Clear all asks first: "Clear the queue?", Cancel (the default) or Clear; Esc, Cancel and a tap
  outside cancel (run 4: the toolkit's dialog, a tap outside does nothing; see "Design QA run 4").
- The drafts' "A-Z" pill is not built (sorting is no control in P1); the format and Gapless chips are information,
  not keys (plain text, flat chips at 640x480); with an empty queue the transport keys and the queue buttons are dimmed.
- The scene `nocontract` runs at 6.08 px/mm (`ppmm 608`): at the harness's default 11.3 px/mm for 480x320 (ROW 80) the
  480x320 drawing has no room for its rows without overlaps; the other 480x320 scenes already ran at that density.

## The Raspberry Pi headphone jack (Pi 4B, Pi 3A+, 2026-11-13)

The owner, on the Pi 4B: "i'm at 5% and it's like 80 to 100% perceived". The coordinator's probes: 'PCM Playback Volume'
of the bcm2835 Headphones card, raw -10239..400 in hundredths of a dB (dBscale-min=-102.39dB step=0.01dB mute=1), joined
mono, on the Pi 4B and the Pi 3A+ alike; amixer read 391 (+3.91 dB) while Vinyl showed 5 %.

- The cause: `snd_ctl_get_dB_range` reports the lowest value of a dB scale with the mute flag as
  SND_CTL_TLV_DB_GAIN_MUTE (-9999999), not as -102.39 dB. Vinyl took it as the range's real bottom, so each raw step
  looked like 9.4 dB: 5 % became raw 391 (+3.91 dB) and read back as 5 %; each "-" or "+" moved the control by one raw
  step (the journal's 5 %, 9 %, 13 % were raw 391, 393, 394; amixer's 393 was Vinyl's own last write). The writes did
  reach the control.
- The fix: Vinyl reads the dB of the step above the lowest (`snd_ctl_convert_to_dB`, loaded as an optional symbol) and
  puts the lowest step's real dB back (a dB scale is linear in steps); the lowest step counts as mute (shown Muted, 0 %;
  a level above 0 % never sets it). Without that symbol the control is taken as having no dB data (linear in steps).
- Never above 0 dB (raw 0 here, not 400); a level above 0 dB set by something else reads 100 %. Each set is logged with
  a fresh read of the control: `volume: set mixer <card> '<control>' P % raw=N (target X dB) read back raw=M`. Replaced
  the same day by the owner's levels fitted to each output (above): the jack's real top, +4.00 dB, is its 100 % now, and
  the log line is the new one.
- A board-probe tool, `tools/mixer-probe` (`vinyl-mixer-probe`, arm64 and amd64, not packaged), lists a card's controls
  as Vinyl and as alsa-lib's simple mixer see them, sets a level with Vinyl's code and reads it back.

## Volume levels fitted to each output (owner, 2026-11-13)

The owner: "I think we're wrong trying to get all sound output to have the same dB at 50%, 50% should be the real mid
output of the soundcard/DAC so if the pi's internal jack tops at +4dB as this forces us to have a specific curve. let's
read the output characteristics and use steps adapted for each output so not necessarily 5% increments as described
here: https://www.dr-lex.be/info-stuff/implement-a-volume-control.html where each step should be 1-2db Increment across
the full range of the DAC/output". And, on the earlier listening pass: "50% sounds the same was for the same output DAC
used on different boards not a generic rule independent of the hardware used."

The evidence behind it (the coordinator): the Sharkoon DAC spans only -45..0 dB, so the common curve (5 % = -78 dB,
20 % = -42 dB) left 5 to 18 % on the DAC's floor and then jumped (the owner: the sound barely changed from 5 to 20 %,
then four to five times louder at 25 %); the Pi 3A+ jack at the same -18 dB is quieter than a USB DAC (the hardware's
own sensitivity), and the Pi's control goes to +4 dB.

Dr. Lex's article (read 2026-11-13): volume must be exponential in amplitude, which is linear in dB ("If your control
already works in dB, don't apply a non-linear transformation. Just choose the range and step size."); "A sensible
assumption for consumer equipment is that it will have a usable range of 60 dB"; for discrete controls "2 dB is pretty
much ideal as step size", 1 to 3 dB acceptable (under 1 dB is not heard, 4 dB is the limit); "A volume control should
not be based on percentages", unless the percentage maps onto a dB range; true silence only at the lowest position.

- The rule "the same percent is the same dB on every output" is dropped. What holds instead: the same output (the same
  DAC or card model, the same dB range) has the same levels at the same dB on every board, through a sound server or not
  (a unit test: the Sharkoon range on two boards gives identical tables).
- One model per output (`OutputVolumeModel`, `src/core/volume/output_volume.*`): the window is the output's real range,
  its upper 60 dB at most, [max(lo, hi - 60), hi], hi the real top (the Pi jack's +4.00 dB included: 100 % is the
  output's top, never above it); N = ceil(window / 2 dB) levels (the coordinator's rule; at least 2, a level never under
  1 dB nor under the control's own step); level 0 is mute (the switch, the sink's mute, or the control's mute value),
  level 1 the window's bottom, level N its top, dB(i) = lo + (i - 1) (hi - lo) / (N - 1); the percent shown is
  round(100 i / N); a press moves one level. With N - 1 intervals a 60 dB window has levels of 60 / 29 = 2.07 dB.
- Where the range comes from: an ALSA control's TLV (the mute-flag fix included), set by raw value with the control's
  own dB scale and read back fresh; a sound-server sink's volume set as the server's dB (pa_sw_volume_from_dB), its window
  the range of the ALSA card behind it (the sink's alsa.card / api.alsa.card / api.alsa.path) placed under the sink's 0 dB
  (the server puts the hardware's top at the sink's 100 %), so a DAC with a -45 dB floor never gets a level below it; a
  sink with no known range (Bluetooth A2DP absolute volume, software sinks) -60..0 dB. The sink's base volume, volume
  steps and flags are logged once per sink for the probes. A level changed elsewhere reads as the nearest level.
- The tables (unit tests, `tests/test_volume.cpp`): the Sharkoon -45..0 dB, 23 levels of 2.05 dB, the middle level 12 =
  52 % = -22.5 dB (no level shows exactly 50 %: 23 is odd); the Pi jack -56..+4 dB, 30 levels of 2.07 dB, 50 % = level
  15 = -27.03 dB (the window's middle, -26 dB, lies between levels 15 and 16); the Apple adapter -60..0 dB, 30 levels,
  50 % = -31.03 dB; an unknown range -60..0 dB, the same.
- The log line of a set: `volume: set <output> step i/N P % target X dB raw=M read back raw=K` (a sink's raw is its
  pa_volume_t, its read-back a fresh ask of the sink). The software volume of a card without a mixer: -60..0 dB, the
  amplitude 10^(dB / 20), kept as its percent in state.json.
- The step size (owner, 2026-11-13): "for the step size around 2 db is fine". N = ceil(window / 2 dB) stays: the 60 dB
  windows have levels of 2.07 dB.
- The model takes the step aimed at and the group's unit as parameters (`volume_model(..., step_db)`, `VolumeUnit`,
  `volume_level_text`), so the planned settings below need no new model; the controller formats the group's text.

## D20: FFmpeg in Depends, the common codecs out of the box (owner, 2026-11-13)

The owner: "in that case for D20 let's make it universal and add FFmpeg as a dependency the route to optional, suggested
is complicating the thing, let's make it work with all most common codecs out of the box."

- The package Depends on `libavformat61 | libavformat62, libavcodec61 | libavcodec62, libavutil59 | libavutil60,
  libswresample5 | libswresample6` (the Debian 13 / Raspberry Pi OS names first, Ubuntu 26.04's as the alternatives;
  checked against packages.debian.org trixie, FFmpeg 7.1.5, and packages.ubuntu.com resolute, FFmpeg 8.0.1); the
  Recommends line is gone.
- Why: the launcher's installer installs a package's Recommends (it does not pass `--no-install-recommends`), so the
  Recommends were never optional in practice; a Recommends only made the formats depend on how Vinyl was installed.
- Sizes (the coordinator's measurements): on a Lite board the FFmpeg libraries and what they need are about 104 MB with
  `--no-install-recommends`, about 462 MB through the launcher's installer today (their own Recommends come too); a
  launcher change to pass `--no-install-recommends` is proposed separately.
- Unchanged at run time: the libraries are loaded (dlopen) only when a tier-2 file is opened, never at start-up (about
  22.5 MB of memory then). The notice for a tier-2 file is now a rare failure: "The FFmpeg libraries could not be
  loaded. Reinstall Vinyl." (the old "Install them from Settings, Apps" pointed to nothing).

## Design QA run 4 (2026-11-13)

- **"Clear the queue?" is the toolkit's dialog** (`cp0::ui::Dialog`, D-61): title "Clear the queue?", its Cancel button
  and its destructive Clear (the toolkit's red label). **Cancel is the default.** Asked by the coordinator whether a
  destructive confirmation defaults to Cancel, the owner answered (2026-11-13): "yes default cancel". Its buttons are
  their own touch areas (no invisible area of Vinyl's: the 12 px above the old box at 800x480 are gone). Run 5 changed
  how its keys work (below).
- **A modal takes Esc and Back** (D-30): while the dialog shows, the Esc key and the header's Esc only cancel it
  (`core/nav` back_step, tested); the page stays.
- **One dimmed look** (D-81): a control that cannot act is drawn at 40 % (alpha 102 of 255), its fill, its word and its
  icon alike (the key fill #B9B19B reads #CDC5AE on the cream panel, the ink #222222 #928D7F, the teal play key #91B09E):
  the transport keys with an empty queue, the dimmed sleep-timer key, the queue buttons that cannot act. With only the
  playing track in the queue, Move up, Move down and Clear all are dimmed and do nothing (`queue_buttons_enabled`).
- **Muted stays legible** (D-53; the word chain below replaced by "Muted on every layout and every path"): the word is drawn at the level number's own size, shrinking at most to the toolkit's
  caption size (`text_px[CP0_TEXT_CAPTION]`); where "Muted" does not fit the box at that size the shorter "Mute", then
  "Off", follow the speaker-off mark; where none fits, the mark alone.
- **Notices touch what they cover, nothing more** (D-33): on now playing a notice sits inside the seek row's touch area
  and the free band above the keys, and while it shows its touch area is the seek row's (the seek row's own is hidden);
  over a list page's strip it stays under the list's end and its touch area holds the strip's touch areas it covers
  (hidden while it shows). The touch-area test checks both, with the dialog's buttons left to the toolkit.
- **The deck's own density** (D-01): the scenes `qa-deck`, `qa-deck-empty`, `qa-deck-muted`, `qa-deck-scrub` and
  `qa-deck-novolume` render 640x480 at 11.3 px/mm (the harness's default for the deck; tools/boards/boards.json names no
  density). The harness's new scene option `allow-small` reports a size check under ROW / TARGET as a WARN line
  (the owner's R-4) instead of failing; every other scene keeps the strict check. The 640x480 scenes at 9.01 px/mm stay.

## Design QA run 5 (2026-11-13)

- **The toolkit's dialog unrestyled** (D-56; the coordinator's decision, to avoid restyling a toolkit widget): run 4's
  teal outline on the dialog's buttons is gone, and with it the Left / Right / Tab highlight. The dialog's own
  semantics carry the owner's default: Cancel is its Primary button, so Enter cancels (the owner's "yes default
  cancel", unchanged); Esc and the header's Esc cancel too (Vinyl routes them: a modal takes them until it closes); the
  destructive Clear is pressed by a tap or, on a keyboard, by the letter Y and nothing else. The dialog's text says so:
  "Y = Clear, Enter or Esc = Cancel". A tap outside does nothing (`core/nav` modal_answer, tested).
- **Dimmed means inert, inert means dimmed** (D-81): `layout::key_active` decides both, tested for every layout: with an
  empty queue the transport keys are dimmed and do nothing (shuffle and repeat, which still acted, included; the S and R
  keys too); the Queue key (480x320) and the bottom row's Browse, Queue and Output are never dimmed (they open their
  pages, which show their empty states); the sleep-timer key is always dimmed and does nothing. The dimmed icon is drawn
  opaque in the 40 % colour, so its strokes no longer show darker joints (run 4 drew them at 40 % alpha).
- **The toolkit's text sizes** (D-53): the level in the volume group at the toolkit's BODY size (shrinking at most to
  CAPTION in a narrow box), the time labels at CAPTION; the muted word at the level's size, down to CAPTION, then
  "Mute", "Off" or the mark alone. Which sizes stay the drafts' and why: qa-info.txt `text_sizes`; what each size draws
  muted: qa-info.txt `muted` (read from the muted scenes, which the harness checks).

## Design QA run 6 (2026-11-13)

- The queue page's four buttons keep their icon ids, so a redraw (the dimmed look) draws them again (their icons had
  vanished since run 4); the scenes stop on any button redrawn without its icon, so every queue shot at every QA size
  checks it.
- The seek bar keeps the knob P(4) clear of the time labels at both ends ("--:--" touched it at 480x640).
- **Accepted (the coordinator's decision, owner informed, 2026-11-13):** the Output page's paired device that is not
  connected keeps the drafts' look, a grey dot and grey words, and a tap on it shows "... is not connected: connect it
  in Settings, Bluetooth." It acts (it tells how to connect), so it is not the dimmed look of a control that does
  nothing (D-81); the grey is the drafts' state colour for "not connected".

## Dragging a slider, the seek bar (owner, 2026-11-13)

The owner: "for D30 equalizer should also work by dragging the bars and btw so should the track timeline to seek a
specific point".

- The ruling (relayed by the coordinator): dragging a slider-style control is allowed and wanted; this refines the "no
  dragging" rule (design QA R-1 / D-34): a drag is fine for a slider when a key and a tap alternative exist.
- Built in P1, every layout, touch and pointer (`src/core/seek`): a press on the seek bar (or its knob) starts a scrub;
  the knob and the fill follow the finger, only its x counts, clamped to the bar's ends; the elapsed time shows the
  target time and the remaining time follows; the seek is applied once, on the release (never while moving: the
  decoders and the SD card); a tap is a press and release where it started (the same seek); playback runs on, or stays
  paused; the bar's touch area is press-locked, so a finger that slides off keeps the scrub and starts nothing else;
  after the release the target stays shown until the engine's position is there (at most 0.8 s), so the knob never
  jumps back. Shift+Left / Shift+Right (10 s) stay. The touch area keeps its row of its own (the row height where there
  is room, no overlap: the earlier rules).
- The Equalizer's drag (D30) is P2, with the Equalizer.

## Later (not built)

Recorded on 2026-11-13; none of these is offered in 0.1.0.

- **Software gain boost** (owner: "add on the roadmap a setting for a software gain boost and limit to 4dB with 1dB
  increment"): in the Playback settings (P2), 0 to +4 dB in 1 dB steps, with a limiter against clipping. The reason: the
  Pi 3A+ and Pi 4B headphone jack at its real maximum is much quieter than a USB DAC at its maximum.
- **Precise volume in dB** (owner: "I'll add a setting for later to allow user to control the volume more precisely
  based on dB with a choice of 0.1 0.5 and 1dB increments, so the volume tag is not showing the % of maximum but the real
  dB output. like on some high end hi-fi systems."): P2/P3, in the Playback settings, "Volume steps": Normal (about 2 dB,
  the percent shown) / 1 dB / 0.5 dB / 0.1 dB; in the dB modes the volume group shows the output's real level in dB (for
  example "-22.5 dB") instead of the percent. The model already takes the step (never finer than the control's own step)
  and formats a level in dB (`volume_level_text`, tested); the setting and its page are not built.
- The Menu key and its pages: built in P2a (0.2.0, below) with the rows of the features built so far (owner decision
  D32); the Equalizer and Library rows come with P2d and P2b.
- The gain boost and "Volume steps" are not in 0.2.0: the P2a brief's Playback page has Gapless, ReplayGain and Remember
  position in long files (report 060 had put both in P2a; the coordinator's brief of 2026-11-13 left them out).
- The Equalizer's bars by drag (owner, 2026-11-13, "for D30 equalizer should also work by dragging the bars"): P2, with
  the Equalizer.

## 640x480 and 480x320 (owner, 2026-11-13)

The owner, on the coordinator's proposals of the full-UX canvas (NP-640x480 and NP-480x320): "try without the up next"
(640x480), then "that works, would it fit on 480x320?" and "ok for the 480x320 layout".

- 640x480 (the medium class: aspect 1.2 to 1.6 where the width is under 800 and the height 480 or more; the deck): one
  pane as on large without the queue list: cover 224 with the record, title (22 px, two lines), artist, album, the
  format and Gapless as flat chips ("AAC 44.1 kHz / 161k"), the output as a full-width button (icon, name, chevron), the
  seek row, five transport keys 56 high (shuffle, previous, play, next, repeat), the bottom row 52 high: Browse | Queue
  | the volume group. No Up next. P1 has no Menu key: Browse and Queue share its width (196 each); the draft's widths
  with the Menu key are kept in the layout's comments for P2. The list pages keep the 480x320 drawing scaled 4/3.
- 480x320 (the small class): cover 148, the info column at 228: title, artist, album, one plain line for the format and
  gapless ("AAC 44.1 / 161k, gapless"), the output as a full-width button, the volume group under it (34 high; its "-"
  56 wide and its "+" 80, as the draft draws them), the seek row, six transport keys (shuffle, previous, play, next,
  repeat, queue; the Menu key is the seventh in P2).
- The coordinator, same day: 800x600 and 1024x768 must not get the 480x320 layout: aspects 1.2 to 1.6 take the two-pane
  large layout from 800 wide and 480 high, the 4:3 single pane from 640 to 799 wide (and 480 high), the 480x320 layout
  below.
- No Vol chip and no volume strip on any layout any more: square, portrait, large, 640x480 and 480x320 share the volume
  group (the strip's code is gone).
- Touch: the drafts give the deck's 640x480 rows (output button, seek, keys, bottom row) less than 7 mm at its 11.3
  px/mm: the touch rows meet at boundaries in their gaps, 70 px each (6.2 mm); 480x320 gives the volume group and the
  seek row 43 px (the transport keys 2 px lower, the rule of 2026-11-12), 6.6 mm on a 3.5-inch panel. Accepted
  warnings under the owner's rule R-4, listed in qa-info.txt. The design-QA scenes of 640x480 run at 9.01 px/mm (a
  3.5-inch 640x480 panel, the harness's own 480x640), where the rows are the 7 mm (63 px) and the header is the deck's.

## Now playing touch areas (the coordinator, 2026-11-12; rows since 2026-11-13)

No touch area of now playing overlaps another one (design QA runs 2 and 3, `layout::place_now_hits`): the areas whose
drawn controls share a height form a row, neighbours in a row meet in the middle of their gap, the rows meet at
boundaries in the gaps between them, the target high where the screen has the room, else the largest height every row
can have, each area covering what it draws. Where the rows miss the target by a few px the transport keys (and their
repeat / shuffle marks) sit up to 4 design px lower, P(4) clear of what is under them (the coordinator's rule of
2026-11-12: solved by geometry, not by an R-4 exception): 2 px at 480x640 and at 480x320.

## P2a: the Menu, the sleep timer, Playback, Keys, About (0.2.0, 2026-11-13)

The owner approved the P2 plan (the launcher repo's report 060, its decisions in section 15) and said "start the P2a
build" (2026-11-13). The owner's answers that shape 0.2.0, as section 15 of report 060 records them: D25 (ReplayGain from
tags only, Off / Track / Album, default Track) "OK"; D32 (Menu and Playback show only built features: no dimmed rows, the
"Speed (later)" row dropped) "OK"; D22 (five steps P2a-P2e) "OK". Everything else below is the coordinator's P2a brief
(2026-11-13) or, where it says so, the builder's reading of it, for the owner to confirm.

- **The Menu key** where the full-UX canvas draws it: 480x320 the seventh transport key after Queue (the sliders icon;
  the seven keys 46 wide, the play key 80, as NP-480x320 draws them, where 0.1.0 had six wider keys); square
  (480x480, 720x720) the sixth transport key, in the dimmed sleep-timer key's place; portrait and PDA (320x480, 480x640,
  480x800) an icon key at the left of the bottom row, then the Queue icon key (it was a key with its word), then the volume
  group; large (800x480 and the two-pane canvases) the bottom row Browse | Menu | the volume group, Output now a Menu row
  (the output chip stays the quick way to the Output page); 640x480 Browse | Queue | Menu | the volume group, each 128
  wide (the width 0.1.0 reserved). The large layout's sleep-timer key acts now: it opens the Sleep timer page (the
  canvas draws it undimmed; the builder's reading). M opens the Menu and T the Sleep timer page on every layout (their
  physical key codes, design QA D-32).
- **The Menu page** in the Browser's look (the dark panel at the class's scale; the medium class the 480x320 drawing
  scaled, portrait the Menu-320x480 drawing with 44 px rows): TOOLS: Sleep timer (Off, or the time left; "This track",
  "This album" for the track modes); SETTINGS: Playback, Output (the output's name), Keys, About (the canvas has one row
  "About and keys": two rows, the coordinator's decision, owner informed, below). No Equalizer and no Library row until their phases (D32). Rows act on the
  first tap, as the Browser's and the Output page's; Up / Down move (the headings skipped), Enter or Right act, Left
  goes back (design QA D-29). Esc goes back to the page the Menu was opened from; its pages go back to the Menu.
- **The Sleep timer page:** STOP PLAYING AFTER: Off, 15 minutes, 30 minutes, 60 minutes, This track, This album or
  queue (the canvas says "This album or playlist": the coordinator's decision, owner informed, below); the chosen one has
  the blue dot, and a choice closes the page (the owner-approved behaviour, below). While the timer runs, the status line at the bottom: "Stops in 29:12. The music fades out for the
  last 10 seconds." (the canvas's words), "Stops at the end of this track. ..." and "Stops at the end of this album or
  queue. ..." for the track modes (the builder's words). The minute modes count on the monotonic clock whether the music
  plays or not, and fade over their last 10 s, linear in dB to -60 dB; the track modes name no next track where playback
  stops, and the engine fades that track's last 10 s; "This album or queue" stops before the next track of another album
  (the album tag when both tracks have one, else the folder) or at the end of the queue, a playlist's queue only at its
  end. "This track" and "This album" leave the queue on the next track, paused. The timer keeps running when the pages
  change and while Vinyl is hidden (X-Background=run); it is not saved, so a restart forgets it. When it ends it pauses
  the music and nothing else, and says so: "Sleep timer ended. Paused." (the builder's words).
- **The Playback page:** Gapless (On by default; Off: every track change drains the stream and opens it again), ReplayGain
  (Track by default, D25: its pick list Off / Track / Album, in the Sleep timer page's style; a choice closes it),
  Remember position in long files (On by default); Enter or a tap toggles Gapless and Remember; no Speed and no Crossfade
  row (D32). The canvas's strip keeps its first sentence, "ReplayGain evens out the loudness between tracks." ("Crossfade
  needs a faster board" goes with the Crossfade row, and report 060 3c contradicts it). The pick list's strip: "Track:
  every track as loud as the others. Album: an album keeps its own loudness steps. Files without ReplayGain tags play as
  they are." (the builder's words). The settings live in `~/.config/vinyl/settings.json`, written only when one changes.
- **ReplayGain** (report 060 3d, D25): read from the tags only (ID3v2 TXXX, Vorbis comments, Opus R128_TRACK_GAIN /
  R128_ALBUM_GAIN as value / 256 + 5 dB, MP4 freeform items); the engine reads a track's tags when it opens it and applies
  the gain to that track's own samples (a gapless change switches it at the boundary), before the output's volume, which
  never moves: the output's levels, percent and dB stay the volume model's. Track takes the track gain (the album gain
  without one), Album the album gain (the track gain without one), an untagged file 0 dB. The gain never passes
  -20 log10(peak) when the peak tag exists, so the loudest sample stays at full scale. A pre-amp is later (0 dB now, report
  060 7.2 #5). Square and large show the playing track's gain after Gapless, as NP-480x480 and NP-800x480 draw it
  ("Gapless, RG -3.2 dB"); 480x320 and 640x480 show no figure (report 060 left them to the coordinator: an open question).
- **Positions in long files:** a file longer than 20 minutes starts again where it was left (the browser, the queue, the
  transport keys), kept in `~/.config/vinyl/positions.json` (the 500 most recent), written on pause, on a track change, at
  exit and at most once a minute while a long file plays, never inside state.json; the first 5 s and the last 15 s of a file
  keep no position.
- **The state file and the SD card** (report 060 finding 3): state.json is written on events (a track change, a pause, a
  stop, a queue or folder change, a setting, the exit) and at most once a minute while playing (0.1.0: every 5 s, the
  whole queue each time). The MP3 tag reader walks the ID3 tag frame by frame in the file and skips a picture by its size
  unless the cover is asked for (a 2 MB cover: about 1.5 kB read for the tags instead of the whole 2 MB tag); the cover
  stays on demand, with the 1200 px decode cap.
- **The Keys page:** two columns, the key on a key chip and what it does, the canvas's rows first (Space, Left, Right,
  Shift+Left / Right, S, R, Q / B / O / M, - and =), then the rest of the real key map (T, comma and dot, the list keys,
  the queue's U / D / X / C then Y, Esc, the media keys); one table in core/menu that the key handler dispatches from, so
  the page cannot drift from the keys (tests/test_menu.cpp). It scrolls (a drag, Up / Down). The 320x480 drawing joins
  "S, R" in one row; Vinyl keeps the 480x320 rows (S and R apart) at every size.
- **The About page** as About-480x320 draws it: the name and version, "A music player for CyberdeckZero Launcher boards.
  MIT licence." (the canvas's "CyberDeck Zero boards" in the branding's spelling), FORMATS (FFmpeg without its version:
  Vinyl does not load it to learn it), THIS OUTPUT (the playing output's volume model: "Sharkoon Mobile DAC: -45.0 to 0.0
  dB, 23 levels of 2.05 dB."), LIBRARIES (the canvas's list with libasound and stb_image added: Vinyl uses both), the
  licences' place.
- **Touch sizes** (R-4): the Menu rows keep the canvas's heights (34 design px, Playback 40, portrait 44), never under the
  toolkit's LIST_ROW; they act on the first tap (as the Browser's rows, accepted for P1). The new 480x320 and 320x480
  scenes run at the 3.5-inch panel's 6.3 px/mm (ppmm 630, design QA's rule for new scenes) with allow-small: the approved
  480x320 layout's volume group and seek row are 43 px there, ROW 44 (the WARN lines of their report.txt).

## Design QA of 0.2.0, run 1 (2026-11-13)

The QA's three FAILs (D-01, D-32, D-55) and the coordinator's rulings on its WARNs and questions, in one rebuild:

- **The keypad's echo (D-32):** after every key event the toolkit's keypad hands LVGL a key too: a navigation key's own,
  an F / X / Z / C alias's arrow, and for any other key its raw code unchanged (cp0_keyboard_lvgl_input.c at the pin).
  The raw codes of W, E, R, T (17-20) are LVGL's Up, Down, Right, Left, and those of the digits 1, 2, 7, 8, 9, 0 are
  Home, End, Backspace, Next, Enter, Prev: T opened the Sleep timer page and its echo went straight back, R toggled repeat
  and acted as Right on the selected row of every list page (since 0.1.0 on Queue and Output), 9 acted as Enter. Fixed as
  the launcher's decision of 2026-10-05 says ("a real W/E/R/T press is recorded and the matching native key within 300 ms
  is ignored"), generalised (`core/nav/key_echo`, tested with a fake keypad): the echo of any key that is not a
  navigation key, when it is an LVGL key, is dropped; an alias's arrow only when Vinyl used the letter (X and C on the
  queue page), so F / X / Z / C stay a small keyboard's arrows elsewhere. The scenes press M, T, R, 9 and Esc on the Menu
  pages.
- **Ctrl+O** (D-42, the pointer's held click, AC 10b): the options key opens the Menu; plain O stays Output. The Menu key
  is drawn on every now-playing layout (tests/test_menu.cpp checks one per class), so a pointer reaches the Menu there
  by a click too.
- **Long and CJK output names (D-01, D-55):** the Menu's Output value scrolls in its own room when it is wider than what
  the row's label leaves (the label keeps its width, up to 60 % of the row), as now playing's output button scrolls a long
  name; About's THIS OUTPUT line breaks after ": " and ", " and wraps a long name. The scenes qa-names-cjk (a headset named
  "寝室のワイヤレスヘッドホン", CJK tags) and qa-names-long ("Living room speaker, Bose SoundLink Revolve+ II") show now
  playing, the Menu, About and the Output page at every QA size (the demonstration world's VINYL_DEMO_BT_NAME).
- **Pick lists close after a choice** (the owner-approved behaviour, relayed by the coordinator, 2026-11-13): a choice on
  the Sleep timer page or the ReplayGain pick list closes it and the page it was opened from shows the new value (the
  Menu's time left, Playback's ReplayGain value); Esc and Left still go back without a change.
- **The coordinator's decisions, owner informed (2026-11-13):** the canvas's Menu row "About and keys" is two rows, Keys
  and About; "This album or queue" replaces the canvas's "This album or playlist". **The ReplayGain pick list** has no
  board on the canvas: it is the coordinator's design, in the Sleep timer page's style (the heading "REPLAYGAIN", the
  three choices with their dots, a help strip).
- **The Sleep timer page's strip** (the deck's Sleep timer page hid "This album or queue" under it while a timer ran): it
  stays at the panel's bottom where the rows fit above it, and otherwise follows the last row in the scrolling list, so
  no row is ever under it (the strip comes into view by scrolling).
- **The Keys page** (D-37, D-81): the key names are drawn flat (no key look: they do nothing), on their action's line
  (landscape; they sat 7-14 px above it where LIST_ROW raised the row) or above it (portrait); a row grows to an action
  of two lines. The rows of now playing say so ("On Now playing: ..."), Enter on now playing, Backspace in the browser,
  the queue pane and Ctrl+O are listed.
- **Texts for the owner to confirm** (the builder's words, kept): "Sleep timer ended. Paused."; the status lines "Stops
  at the end of this track. The music fades out for its last 10 seconds." and "Stops at the end of this album or queue.
  The music fades out for its last 10 seconds."; the Menu values "This track" and "This album"; the ReplayGain pick
  list's strip "Track: every track as loud as the others. Album: an album keeps its own loudness steps. Files without
  ReplayGain tags play as they are."; the Keys page's actions after the canvas's rows. Listed in the README too.
