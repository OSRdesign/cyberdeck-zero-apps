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

## P2b core: the library without its pages (0.3.0 work, 2026-11-14)

The core of the Library (report 060 2.3 and 3a; owner decisions D27 and D28 as section 15 changed them; D31 is the UI's):
SQLite, the scanner, the views, search, the queue's track actions. No page draws it yet. The plan left the points below
open; each is the builder's choice, the simplest one consistent with report 060, for the coordinator to confirm.

- **SQLite** is loaded at run time (`core/db/sqlite_lib`, the cp0_map_sqlite.c pattern) and joins Depends
  (`libsqlite3-0`, no version change yet). It needs 3.45 or later (the trigram tokenizer's `remove_diacritics`) and uses
  FTS5's `contentless_delete=1` (3.43) so a track's FTS row can be deleted or replaced by rowid, instead of FTS5's
  'delete' command, which needs the old values. Every board has 3.46.1, the build box 3.45.1. Without the library, or
  with an older one, the Library is unavailable with a reason (`LibraryWorker::open_error()`); playback and the folder
  browser do not use it.
- **The file** is `$XDG_DATA_HOME/vinyl/library.db` (`~/.local/share/vinyl/library.db`). The schema version is
  `PRAGMA user_version` (1). A file SQLite calls corrupt or not a database is moved to `library.db.bad` and made again (the
  tags come back with a scan; the play counts are lost). A file of a newer schema is left alone and the Library is
  unavailable, so a downgrade never destroys play counts.
- **Schema v1** is the plan's DDL plus `tracks.root_id` (an offline drive's rows are hidden with `root_id not in (...)`,
  without a join), `roots.mount` (the mount point a drive root was found at), and unique `artists.sort` /
  `genres.sort`: one artist per sort key, so "Beyoncé", "BEYONCE" and "Beyonce" are one artist (the first spelling
  seen is shown), while "The Beatles" and "Beatles" stay two (the moved article differs). The P2c playlist tables exist
  already, so P2c needs no migration.
- **Albums** are keyed by (album sort key, album artist), the album artist being the album-artist tag, else the track
  artist: CD1 / CD2 folders join. A compilation without an album-artist tag splits by track artist (known limit, no
  heuristic). A missing album tag is an album titled "" for each artist, a missing artist an artist "" (sorted last; the
  UI names them). An album's year is the first non-zero year among its tracks.
- **Sort keys:** only a leading "The " moves (the plan's words; not "A" / "An"). Numbers are padded to 8 digits; a longer
  run of digits stays as written. Leading ASCII punctuation is skipped. Not folded: half-width katakana and Vietnamese
  letters (Latin Extended Additional). Unknown names get U+FFFF and sort last.
- **Views:** Artists lists album artists only (the plan); an artist's page also shows the albums the artist appears on
  as a track artist (so an artist found by search always has a page). An album's tracks come in disc, folder, track order
  (two untagged CD folders do not interleave). Songs page by LIMIT / OFFSET; the keyset cursor for 100k tracks is left to
  P2e's tuning.
- **Search:** the whole query is one FTS5 phrase (its words in that order, anywhere in the field). Songs match titles,
  Artists the artist or album-artist fields, Albums the album, Genres the genre; 50 results each. Under three Unicode
  characters, the start of the sort keys is matched instead (an index range): "ab" finds ABBA and "Abbey Road", "艺术"
  titles that start with it; a digit query matches whole numbers ("2" finds "2 Become 1", not "25 Minutes"). The 250 ms
  debounce is the UI's; `LibraryWorker::post(..., key)` replaces a waiting search and interrupts a running one.
- **Roots:** ~/Music, the drives P1's browser lists (mounted under /media/<user>, /run/media/<user>, /mnt, /media), and
  the folders of a new setting, `library.folders` in settings.json (written only once a folder was added). A folder that
  overlaps another root is left out. A drive root keeps its mount point and file-system UUID; it is online only while that
  mount point is in /proc/self/mounts with that UUID, so the empty folder a pulled stick leaves behind is offline, not an
  empty drive. A drive mounted again under another name is matched by its UUID and its path updated. Offline roots keep
  their rows, hidden, with no limit in time (P2e may prune them); removing a folder in the settings deletes its rows
  (`remove_root`).
- **Commits:** D27 says "database writes are batched per folder", report 060 "every 500 files or 2 s". Both hold: a
  batch is committed at a folder boundary once 500 rows were written (1,000 on boards with more than 512 MB, report 060
  section 4) or 2 s passed since it began; inside one big folder only at the row count. FTS rows and the pruning of empty
  albums, artists and genres are in the same transaction. An unchanged rescan reads no tag and writes only each root's
  last-scan time.
- **Deletes:** a row whose file is not listed is deleted when `lstat` says ENOENT and the root is still online (its mount
  read again); a folder that is gone goes the same way after a full walk of its root. A file whose tags cannot be read
  gets no row (an existing row stays as it was). `scan_folder` (one folder) never removes other folders.
- **Resume:** each commit of a full scan stores the last folder done (`meta.scan_resume`: the root id and the folder, in
  the walk's fixed order). The next `start_all()` skips the roots and folders before it, then clears it. A scan cancelled
  before any write keeps the older point (the walk repeats, nothing is read again). Files deleted in folders a resumed run
  skips are found by the next full scan.
- **Heat:** `/sys/class/thermal/thermal_zone0/temp`, read every 2 s at the next folder; at 75 °C the open batch is
  committed and the scan waits, reading every second, until 70 °C. The progress says "paused" with the temperature.
- **Threads:** the `ScanThread` lowers only itself (nice 19 on its thread id, `ioprio_set` IDLE); the `LibraryWorker`
  that answers the pages runs at normal priority, each with its own connection (WAL lets the pages read while the scanner
  writes; busy timeout 2 s). The UI posts jobs and runs their results with `drain()` on its own thread.
- **D27 wiring is the UI's:** the core offers `start_all()`, `start_folder()` and `cancel()` and never starts by itself.
  Whether the launch scan also stops when the user is not in the Library is for the UI task to settle with the
  coordinator (D27's two sentences can be read either way).
- **Play counts:** `record_play()` when a track played to its end, `record_skip()`; the engine wiring decides when. A
  re-tagged file keeps its row (and count); a moved or renamed file is a new track. A deleted track's counts go with it
  (row ids can be reused).
- **Tags in scan mode** (`read_tags_scan`): FLAC and Ogg comments are walked in the file, so a picture comment costs its
  first 4 kB; WAV / AIFF id3 chunks now go through the frame-by-frame ID3 walker in both modes; the Ogg length looks at
  the last 16 kB before the last 64 kB (both modes). An MP3 tag unsynchronised as a whole still reads its first 256 kB
  (rare). APEv2 is read for APE, WavPack, Musepack and TTA, and between an MP3's ID3v2 and ID3v1 (mp3gain's ReplayGain);
  those formats get no duration from Vinyl's readers (unknown until played). ID3v1 numeric genres ("(17)") are not mapped.
- **The queue's track actions:** `insert_next()` (after the current track, first when none) and `append()`; with shuffle
  on they go into the play order at that place in their own order, and into the list order after the current track, so
  turning shuffle off keeps them there. The queue's source stays.
- **Memory (report 060 section 4):** the scan mode reads under 64 kB a file, the page cache is 2 MB on the 415 MB boards,
  the scanner keeps one folder's rows and a (dev, ino) set. TagJobs' cache of 20,000 entries stays until the pages read
  the database (the budget's 500 entries is a UI-task change: the folder browser still needs it).
- **Measured on the build box** (x86, in memory): 1,000 tracks written in 7 ms, 25 Songs pages in 3.6 ms, a search in
  0.2 ms; the scan mode reads 1.5 kB of an MP3 with a 2 MB cover, 4.3 kB of a FLAC with two, 33 kB of an Ogg Vorbis file
  with one (mostly its page headers and the length's 16 kB). The board figures are B37-B45's.

## P2b: the Library pages (0.3.0 work, 2026-11-14)

The pages of report 060 2.3 on the core above, drawn from the approved canvas boards (the P2b set: Artists, Artist, Albums,
Album, Songs, Genres, TrackInfo, EmptyNoMusic, EmptyNoResults, DriveOffline, Scanning, ScanPaused, MusicFolders,
Search-320x480, TrackActions-320x480, AlbumActions, NoticesP2b; and the UX set's Library, Search, TrackActions,
LibrarySettings; the boards are not in the repository yet). The version stays 0.2.0 until publish. Each point is the builder's choice, for the coordinator to
confirm; the deviations from the boards are listed so that nothing is silent.

**Where the code is.** `core/library/lib_model` (rows, headings, the pager, the scan strip and drive note, Track info, the
sheets, the nav stack: pure, `tests/test_lib_model.cpp`), `core/library/library_ctl` (the worker and scanner wiring, the
places, paging, search, sheets, Music folders, D27: `tests/test_lib_ctl.cpp`), `ui/library_pages` (the pages),
`ui/row_list.hpp` (the Browser's row list, now shared: heading rows, the hold), the hub in `ui/vinyl_view.cpp` (the
Browser's list page, so the mini player stays), the controller in `ui/vinyl_app.cpp`.

**Browse is the hub.** With the library's database open (it opens at start, a few ms) Browse (B, the Browse key, Esc on Now
playing) opens the Library hub, the app's first page when no queue is resumed, and Esc at the hub ends the app as Esc at the
Browser did. Folders opens the Browser (Esc goes back to the hub). Without the Library (no SQLite, a newer schema) Browse is
the folder list as in 0.2.0 and the Menu has no Library row. The scenes run the old Browser unless `VINYL_DEMO_LIBRARY` is set.
The Menu's Library row (below Playback, as the Menu board draws it) opens the Library settings; Esc there goes back to the Menu.

**D27 as wired.** The owner's words (2026-11-13): "scan at launch and when opening a folder from the library but not out of
the library". The scan at launch runs once (`start_launch_scan`, after one worker job read the track count for "about N"); a
folder opened in the Browser from the hub is scanned (`scan_folder`, not recursive); "Rescan now" and a folder just added in
Music folders start a full scan; the user going into the Library and then leaving it for any page that is not a Library page or
the Browser opened from Folders (Now playing after Play, the Queue, the Menu) cancels the scan, which resumes at the next
start. A launch scan nobody went into the Library to look at is left to finish. No timer, no background scan. When the mounts
change while the Library is open the roots are synced again (a pulled stick's tracks leave the views, hidden not deleted;
plugged in again they come back); a stick never seen before needs a scan (Rescan now, or a folder opened from Folders). The
boards' "Esc leaves the page; the scan goes on in the background" note and the settings board's "Scan USB sticks when plugged
in" row are not drawn: they contradict D27. The settings strip says "...stops when you leave the page or the board gets hot",
as drawn.

**Pages.** Rows are the Browser's (chevron opens, dot plays, 15 px names, values at the right, the toolkit's compact list
rows). Hub: Folders, Playlists, Artists n, Albums n, Songs n, Genres n, the Search chip, the mini player. Artists, Albums, Songs
and Genres load 40 rows and the next 40 when the highlight is within 8 rows of the end (once; a reload by a scan commit keeps
what was loaded). An artist's page lists albums by year with the albums it appears on; an album lists its tracks in disc order
with a "DISC 2" row; a track tap plays the list from there (Songs: the loaded rows; album: the album; search: the songs found),
"Play album" plays from the first. Genres show their track counts and lead to the albums. Esc steps back one place, Left too.
Search is the toolkit's `TextField` (unrestyled; its label hidden), results after 250 ms on the worker, Songs, Albums, Artists
then Genres each under a heading row; Enter hands the keys to the results (Up from the first row edits again); Esc leaves the
field and the page at once (the toolkit's two-step Esc is replaced by the field's `escape` event). While the field edits, no
shortcut acts and no letter goes anywhere else (D-32). The on-screen keyboard and the PDA band are the toolkit's and
`_common/osk_policy`'s; the results list shrinks by the keyboard's inset and is hidden when not even one row fits above it
(480x320 with a keyboard up: only the field). `/` and Ctrl+F open Search on any Library page.

**Actions sheets (D31).** A 1.5 s hold (see the ruling below) on a song, an album or an artist row, or Ctrl+O on the selected one, opens the cream sheet
(Ctrl+O keeps opening the Menu where the selection has no sheet). Track: Play (Enter), Play next (N), Add to queue (A), Add to
playlist (P), Track info (I). Album: Play album, Play next, Add to queue, Add to playlist. Artist: Play all (its albums in year
order), the same others. Add to playlist is drawn **dimmed and inert** in 0.3.0 (the boards draw it; playlists are P2c); P
does nothing. The hint line is on the sheet. A tap on a sheet row acts, a tap outside or Esc or Left closes it. The Playlists
row of the hub is drawn as the board draws it (normal) and shows the notice "Playlists arrive in a later version." The queue
changes are `Queue::insert_next` / `append` (the playing track stays); the notices say "Will play next." / "Added to the queue."

**Music folders.** Add a folder opens the Browser in **choose mode**: the mini player's strip becomes "Use folder" (the key
U too), tracks and playlists do nothing, Esc goes back to the Music folders page. No board draws this; it is the cheapest use of
the Browser's own strip and an open design question for the owner (a dedicated page would be a design task). Remove selected
folder removes only the folders the user added (`library.folders`); Music and the USB drives are always indexed (a removal would
come back at the next start), so the button says so in a notice. Removing deletes the folder's rows and nothing else.

**Play counts, drives.** A track that played to its end (the engine's Advanced and QueueEnd events) adds a play on the worker.
While a track plays from a drive, the mount point is checked every 2 s; when it has left /proc/self/mounts playback pauses and
the red notice "Drive removed. Paused." shows (no engine change: `pause()`).

**Deviations from the boards (the toolkit's widgets and the Browser's rows win over pixels).**
- Hub rows are the Browser's 36 px (the board's 34); the list scrolls above the mini player. The board draws the mini player and
  the scan strip over the last row; here the strip sits between the list and the mini player and the list gives up the height.
- The heading band is as high as the touch target where a chip shares it (Search, Play album): 44 px at 6.3 px/mm against the
  board's 30. At 320x480 "Play album" has its own line under the heading (it does not fit beside it).
- Section headings ("DISC 2", "SONGS", "ALBUMS") take a full row, not the board's 18 px.
- Search: the field is the toolkit's (its colours and cursor, not the board's cream and teal); the dim behind a sheet is 70 %.
- The sheet sits at the bottom of the panel (the 320x480 board draws it lower than 120 px), its rows at least the touch target.
- Track info: "Track, disc" says "1, disc 1" (the number of tracks is not in the row), the file is shown with "~"; long values wrap.
- Music folders: the two buttons sit at the panel's bottom (one over the other on a narrow panel), not under the rows.
- Not drawn: the Artist board's hint line (an annotation), "3 files could not be read" (the scanner keeps no count of unreadable
  files: a core change), the Library settings' USB-stick row (D27). "Not enough space for the library" shows when a scan fails
  with a full disk. Track info has no "of 14".
- Pause on a pulled drive is polled (2 s), not an event.

**TagJobs** keeps 2,000 entries (was 20,000), not the budget's 500: its cache still serves the Browser and the Queue, which ask
for every file of a folder or queue, and it empties itself when full, so 500 would drop the first rows of a 600-track queue.
A least-recently-used cache would allow 500.

**Tests.** `test_lib_model` (rows, headings, the pager, notes, Track info, sheets, nav, the two new queries), `test_lib_ctl` (the
hub, paging of 101 artists, an album, the sheet keys, search with its debounce, play counts, Music folders, D27 with a slow tag
reader). Scenes: `qa-library` (800x480 480x480 720x720 480x800 480x640), `-640`, `-wide`, `-small` (480x320, 320x480);
`qa-library-scan`, `-paused`, `-offline` (each with a `-small`); `qa-empty-library`, `qa-cjk-library` (each with a `-small`);
`keyboard`, `keyboard-small`, `portrait-band` (480x800, 480x640).

## Owner rulings on the P2b open questions (2026-11-14)

The Controller put seven questions to the owner after design QA run 2 (BLOCKED only on D-55) and the owner answered each
as recommended. The owner's choices, by the options' own words:

- **CJK in the search field (D-55):** "Waive for 0.3.0". Typed CJK shows boxes in the toolkit TextField (the results and
  every text the app sets draw correctly); the field is fixed later in a separate launcher/toolkit change. D-55 is waived
  for 0.3.0 for the search field only.
- **Playlists row and Add to playlist (D-81):** "Keep both". The hub's Playlists row opens the notice "Playlists arrive in
  a later version." and Add to playlist is dimmed and inert until P2c.
- **Launch scan nobody visited (D-68):** "Accept as built". The launch scan finishes at nice 19 (pausing above 75 C) even if
  the Library is never opened; it stops when the Library is left after being looked at (D27).
- **Hub at 480x320 while scanning:** "Accept as is". The scan strip sits above the mini player; the list scrolls.
- **Unreadable files:** "Ship without it". No "N files could not be read" notice in 0.3.0; a file the scanner cannot read
  gets no row.
- **Folder-choose mode:** "Keep the strip button". The mini player's strip becomes the "Use folder" key (U also works).
- **Search order and long album headings:** "Fine as is". Songs, Albums, Artists, Genres; a long heading is cut with dots at
  narrow widths.


## Owner ruling: a source that is gone is shown nowhere (2026-11-14, board test B45)

The owner, after pulling a USB stick: "once unmounted the USB stick folder remain in the library list and so do the track, after
using it I don't think that's correct, if en external source is gone so is gone all its content we should no longer show it".
- **Nothing of a gone root is shown**: no tracks, albums, artists, genres or search hits (the views already left out an offline
  root's rows; the counts too), no folder row in the Folders browser, no row in Music folders ("not connected" is gone), no
  "<LABEL> is not connected. Its N tracks are hidden ..." note on the hub (the DriveOffline board's note and the dimmed row are
  dropped; the Music folders count and the hub's empty state count only the roots that are there). Track info and play counts are
  reached from the views, so a gone drive's are never shown.
- **The rows are kept in the database, hidden** (not purged): a drive plugged in again comes back without a rescan, and its play
  counts with it. The owner's concern is what is shown, not storage; a purge would also cost every play count of a stick that is
  only borrowed. (`Remove selected folder` still deletes a folder's rows, as before.)
- **Mounted but unreadable is gone too.** The board's stick was mounted by hand and pulled: the kernel kept the mount listed in
  /proc/self/mounts (a stale mount: reads fail with EIO or ENODEV) and the root still counted as online. `root_online` now probes
  the folder (`root_readable`: a stat, then opening it and reading an entry; EIO, ENODEV, ENXIO, ESTALE and ENOTCONN mean gone),
  the scanner's online checks use it, the Browser's list of roots leaves out a drive that cannot be read, and the Library looks
  at its drive roots every 2 s while it is open (the probe's result is part of the mounts signature that triggers the sync).
  `tests/test_scanner.cpp` tests it with a fake probe (a stale mount, then readable again); `set_root_probe` is the seam.

## Owner rulings of 2026-11-14 (B49 follow-ups): a gone drive leaves the queue; Mount in Music folders

**Media that goes away.** The owner, on the Pi 4B: "if it disappear from the library all track are still in the queue list and the
player screen tries to load them 'The file is gone...' message is displayed. that should not happen, if media was removed vinyl
shoudl get back to default music folder and on reopen go to the library direclty for user to select from there what to load in the
player."
- When a drive goes away (pulled, a stale mount that cannot be read, Eject, unmounted at exit) every queue entry that lies on it is
  removed at once (`Queue::remove_under`, `core/library/media_guard`: the drive directory a path lives on, /media/<user>/<label>,
  /run/media/<user>/<label>, /mnt/<label>; gone = not on a listed, readable mount; a folder of the system disk under /mnt whose
  files are there is not a drive). The app looks once a second and at once on Eject and on an open that failed.
- **The playing track among them: playback stops cleanly** (no "file is gone", no retry, no skipping through the dead entries),
  the next entry that is left is loaded **paused, never started by itself** (decision: whether it was playing or not, the sound
  ended with the drive and a different song starting on its own would be a surprise; the queue is as the owner left it, one press
  of play away), and the queue is empty when none is left. The red notice says it once: "Drive removed. Paused." (the playing
  track was affected) or "Drive removed. Its tracks left the queue." (others only; the playing one goes on).
- **An empty queue leaves Now playing (or the Queue) for the Library at the default music folder** (~/Music when it is a root, else
  the first root; Esc goes to the hub). The mini player is hidden with an empty queue.
- **At every start** the saved queue loses its entries on a drive that is not there before the first screen is drawn
  (`drop_gone_media(true)`): with nothing left to resume Vinyl opens on the Library hub (without a Library: the Browser), the
  dropped entries never reach the engine, so "The file is gone..." stays for one file deleted from a drive that is there.
- A drive that returns later does not bring the entries back: they are gone from the queue; its library rows return as before.
- Tests: `tests/test_media_guard.cpp` (the drive of a path, the gone drives, `remove_under` with the playing track on the gone
  drive, some entries, all, a shuffled queue, a saved queue at start), scenes `qa-media-pulled` (a drive pulled while playing from
  it: the Library at the default folder, no "file is gone") and `qa-media-gone-at-start` (the hub at start), each with `-small` and
  `-deck`; the scenes' pull is `VINYL_DEMO_PULL_AFTER`, the saved queues `VINYL_DEMO_DRIVE_QUEUE`.

**Mount from Music folders.** The owner: "unmounted drive from setting music folder shouls offer ability to mount drive, now we
only have eject but no other option to mount which recquire to go back several screens to library and select folder -> mount".
(Board MusicFoldersDriveMount.) A drive that is plugged in and not mounted is a row "<LABEL> (USB drive)  not mounted" with the
amber action dot, no longer dimmed; with it selected the drive button is "Mount <LABEL>" in the brown look (the primary look moves
from Add a folder to it), and it mounts exactly as a tap on the row in Folders does (`request_mount`: the same worker, "Mounting
<name>...", the denial and error texts); the row then says "mounted by Vinyl" and the button "Eject <LABEL>". The button is Eject on
a drive Vinyl mounted, Mount on one not mounted, Eject dimmed and inert for a drive the system mounted (and with no drive
selected). After Eject the drive stays listed, not mounted, with Mount again; a drive that is gone is shown nowhere. Remove
selected folder is as before (it explains for a drive). Tests: `test_lib_ctl` (the button for each state, Mount from the page, the
state after an Eject), scene `qa-drives-mount` and the Mount-again steps of `qa-drives-eject`.

## Owner decision: mount on request in Vinyl (2026-11-14, board test B45; the mounting half of D28)

The owner: "Mount on request in Vinyl". On the HackBerry a plugged stick (sda1, vfat) is never mounted: the desktop's file manager
does it and does not run while the launcher owns the screen, and Vinyl only read /proc/self/mounts. Boards:
DrivesFolders, MusicFoldersDrives, DriveNotices (the coordinator's).
- **Listing** (`core/drives`): the removable block devices that carry a file system, from sysfs (`/sys/block/<dev>/removable`,
  partitions) and the udev database (`/run/udev/data/b<maj>:<min>`: ID_FS_TYPE, ID_FS_LABEL, ID_FS_UUID, ID_FS_USAGE), mounted or
  not (/proc/self/mounts): no root, no library. A disk with no file system (a partition table, a RAID member) is not listed.
- **Mounting through `udisksctl mount -b <dev> --no-user-interaction`** (exec with a 20 s limit, `LC_ALL=C`), not D-Bus through
  sd-bus: the same polkit rules (the active session is allowed, an ssh login is refused with NotAuthorizedCanObtain), no library
  to load or version, the answers are text that is parsed and tested (`parse_mount_reply`), and an unknown one is shown
  shortened (its last sentence, at most 60 characters). udisks2 is a run-time need, not a Depends: `udisksctl` is looked for in
  PATH; without it the drives are listed and a tap says "Drives need the udisks2 package.". The mount runs on a thread of its own
  (`DriveManager`), never the UI's; the UI takes the answer at its next tick.
- **The pages**: Folders (the Browser's root list; with a drive plugged in the hub's Folders row shows the root list, and ".." at
  the top of the single root leads to it) gets a "USB DRIVES" heading and, per drive not mounted, a row with the amber action dot
  and "not mounted. Tap to mount" ("Mounting <LABEL>..." notice, then the drive opens and joins the Library: its root is synced
  and the folder is scanned, the D27 rule for a folder opened from the Library); a mounted drive is an ordinary root row with
  "<size>, mounted". Music folders lists "<LABEL> (USB drive)" with "mounted by Vinyl" or its tracks, the drives not mounted
  ("not mounted", the amber dot: Mount acts on it, see the ruling above), and the buttons Add a folder, "Eject <LABEL>" and Remove selected folder (they flow onto lines
  by their words: one line at 640 wide, two at 480x320, three on a narrow panel). The notices are the board's: "Could not
  mount it: <reason>.", "<LABEL> ejected. You can unplug it.", "Drives need the udisks2 package."; reasons: "this session is not
  allowed to mount drives", "the disk has no readable filesystem", "it is in use", "it took too long", else the udisks message
  shortened.
- **What Vinyl mounted is Vinyl's to undo, nothing else**: `DriveManager` records the devices it mounted (an AlreadyMounted
  answer is not a mount of Vinyl's: it is recorded only when the mount worked). Eject (`udisksctl unmount`) and the unmount of
  what is still mounted when Vinyl quits (`shutdown`, the journal says each) apply to those only; a drive the system mounted is
  never unmounted and its Eject is dimmed (the manager refuses it too). A drive that was pulled is forgotten (a later mount by the
  system is the system's). A drive pulled while Vinyl had mounted it is the gone-root path above.
- **Tests**: `tests/test_drives.cpp` (the list on a fake sysfs and udev tree, udisks's answers, the manager on `FakeDrives`: mount
  ok, denied, the exit unmount of Vinyl's only, Eject, a system-mounted drive left alone, a pulled drive, no udisks),
  `test_lib_ctl` (Music folders with drives, Eject enabled for Vinyl's only, a gone drive's row and note, the rows back when it
  is back), scenes `qa-drives-folders`, `qa-drives-eject`, `qa-drives-denied`, `qa-drives-nofs`, `qa-drives-nodisks` (each
  with `-small` and `-deck`), `qa-library-offline` rewritten (nothing shown).
- **Not verified without a board**: that polkit lets the launcher's session mount without a password (the owner's HackBerry is
  the test, B49); udisksctl's exact English messages (`LC_ALL=C` is set; the parser keys on the udisks error names); a drive
  with a label that has escapes; a hung stick (the 20 s limit kills the command and says "it took too long").

## Owner ruling: the hold is 1.5 s with a cue; a place is given back as it was left (2026-11-14, board test B44)

The owner, on the HackBerry: "on touch, 3sec is long get it down to 1.5 sec and as we did in others apps (i.e markdown notes) show
a progression bar and a message 'keep holding for options'", and "when exiting track info song list previous position is not
maintained". Both are done.
- **The hold** (Track, Album and Artist actions) is **1.5 s**, a departure from the launcher checklist's 3 s hold **for Vinyl
  only**. From 0.5 s the cue shows over the bottom of the list, in Markdown Notes' look (a box with an accent border, a bar that
  fills over the 1.5 s, the message "Keep holding for options"); a tap acts as before; a hold given up after the cue showed (the
  finger lifted, or it moved more than 16 px: a scroll) opens nothing and is not a tap. `core/nav/hold_tracker` is Markdown Notes'
  `HoldTracker` with these times (tested); the cue is `RowList::enable_hold_cue`. Ctrl+O is unchanged. The sheet's hint says
  "Hold a row for 1.5 s, or press Ctrl+O, to open this sheet."
- **The position**: a Library place keeps its selected row as it moves and where it is scrolled to (in pixels) when another is opened from it
  (Track info, an album, an artist, a genre's albums, a search result); Esc gives both back once the rows are in (a long list
  loads the 40-row page the highlight is on at once; the placeholder before the rows arrived used to overwrite the saved row, so
  the list came back at the top). `tests/test_lib_ctl.cpp restore_test`, scene `qa-library` (Songs, Track info, Esc).
- The scene harness has two new commands, `press TEXT` and `release` (a finger held down while the app runs and a shot is made),
  for the cue's shot.

## P2c: playlists (0.4.0 work, 2026-11-14)

Report 060 2.4 and decision D26 ("Playlists are M3U8 files in `~/Music/Playlists`: yes"), built on the approved boards
(PlaylistsEmpty, AddToPlaylist, RenamePlaylist, DeletePlaylist, PlaylistEditMissing, ImportM3U, NoticesP2c from the P2b canvas set;
Playlists, PlaylistEdit, SavePlaylist from the UX set). **Owner ruling:** on the evening of 2026-11-14 (the owner's date: 2026-11-15)
the owner said "go with your recommendations" on the P2c design. The version is 0.4.0; the README says "not published yet"; no
registry, package or root README row was touched. Where the spec left a choice open, the builder's choice is below, for the
coordinator to confirm; the deviations from the boards are listed so that nothing is silent.

**Where the code is.** `core/playlist/m3u` (reader and writer), `core/playlist/playlists` (the folder of files: names, atomic writes,
edits, import, export), `core/playlist/playlist_index` (the database index, the edit page's rows), `core/library/lib_model`
(rows, headings, sheets), `core/library/library_ctl` (the places Playlists and Playlist, pick mode, the dialogs' answers),
`ui/library_pages` (the two pages, the chips and buttons, the scrolling sheet), `ui/vinyl_view` (the name and question dialogs, the
Queue's chip, the Browser's hold), `ui/vinyl_app` (wiring), `core/demo/demo_world` (the scenes' playlists). Tests: `test_m3u`,
`test_playlists`, `test_playlist_model`, `test_playlist_ctl`; scenes `qa-playlists`, `-empty`, `-missing`, `-export`, `keyboard-playlists`,
`portrait-band-playlists`.

**The files are the truth.** `<name>.m3u8` in `~/Music/Playlists` (made on the first write); only `.m3u8` is listed there (a `.m3u`
put in by hand is not; import it from Folders). A write is a hidden temporary file in the same folder, flushed, renamed over the
old file, the folder flushed: an interrupted write leaves the old file whole and a leftover `.name.m3u8.tmp`, which the next
listing deletes (`tests/test_playlists.cpp`: a hook stops the write after the temporary file, as a power cut). The tables
`playlists` and `playlist_items` of schema v1 are a cache (no migration): `playlist_sync` runs when the Playlists page opens and when
a scan ends, finds files added, changed (their mtime in nanoseconds) or gone, rewrites only those rows and writes nothing when
nothing changed (SD wear: a test counts `total_changes()`); a playlist Vinyl edits is re-indexed at once; the edit page and playing
read the FILE, never the cache; a rebuild from empty tables gives the same list (tested). Rows of an entry the library knows keep
its track id; the counts and lengths come from the library's durations, else the `#EXTINF` seconds.

**Names.** Letters, digits, space, - and _ , 24 characters at most, as Cassette's prefix rules; the builder's reading of "letters":
every letter of every alphabet counts (a byte sequence from U+00A1 up that is valid UTF-8), so "Été" and an import named in CJK
work (a name of the Cassette type would have refused an accent), controls, `/ . \ : " * ? < > |` and a bad sequence are refused. A
name is trimmed; taken is compared without case (a rename that only changes the case of its own file is allowed). A typed name
is checked: a bad one gets a notice in the box ("Letters, digits, space, - and _ only.", "Use 24 characters or fewer.", "Type a name
first.") and the box opens again with the text; a taken one the notice "That name is taken. Pick another." (red) the same way. A
name made from a file name or a default is *cleaned*, not refused (bad characters become one space, cut to 24, "Playlist" when
nothing is left).

**The notice in the name box.** The board draws the red pill. The name box is the toolkit's Dialog with its TextField (not
restyled; Cassette's pattern), and the page behind it is hidden while it is up (as Cassette hides its page: the toolkit's keyboard
lies over the bottom of the screen), so a pill drawn on the page would not be seen: while the box is up the notice is written in the
box's own message line (red for the red ones). The controller still raises the notice (tests check its words); the view routes it.

**M3U reader** (`core/playlist/m3u`). UTF-8 with or without a BOM; UTF-16 with a BOM; otherwise a text that is not valid UTF-8 is
read as Windows-1252 (Latin-1 plus the printable 0x80-0x9F), and a Latin-1 file name is looked up as converted (UTF-8 on the disk)
first and as the raw bytes second; CR LF, LF and a lone CR end lines; backslashes are separators; `file://` (percent escapes
decoded, `file:///D:/x`) is its path; every other URL is skipped (and its `#EXTINF` is not carried to the next line); a relative
path is joined to the playlist's folder and made lexical (`P1's parse_m3u` kept `../` in the path; the wrapper's test now expects the
lexical one); a `D:\...` or `\\server\share\...` path drops its leading folders one by one until the file is found under the
playlist's folder, then under each music folder; if nothing is found it is kept *as read* with `resolved` false (a missing entry
that is shown dimmed and written back unchanged); a Linux path from another machine is kept as it is. Known limits: a Windows path is
matched with its case as written; a name with a literal backslash is read as folders. `#EXTINF` with attributes
(`#EXTINF:123 key="v",Title`) is read. At most 5,000 entries (the Browser's cap).

**M3U writer.** UTF-8, LF, `#EXTM3U`, `#EXTINF:seconds,Artist - Title` for an entry that has a text or a length (`-1` and an empty
text are not invented), paths relative to the file's folder when the entry and the file are on the same drive (the drive of
`core/library/media_guard`: the system disk is one drive; /media/<user>/<label>, /run/media/<user>/<label>, /mnt/<label> are one
each), else absolute. An entry from the library is written with `Artist - Title` and its length in whole seconds.

**Pages.** The Playlists page follows PlaylistsEmpty and Playlists: "PLAYLISTS, N", the "+ New playlist" row first, the names A-Z
(natural order) with "N tracks" and a chevron; with none, the board's message sits under the "+ New playlist" row (the row stays).
The edit page follows PlaylistEdit (the tracks as a dot, "Artist, Title" and the length; the heading "18 TRACKS, 1 H 23 MIN" with ". 2
NOT FOUND"; Rename and Export M3U at the heading's right, the first chip rightmost as in the board; Move up, Move down, Remove and
Add tracks (teal) under the rows, flowing onto lines on a narrow panel by their words as the Music folders buttons do). A move
keeps the selection on the moved row; a removal keeps it at that place; Move up is dimmed on the first row, Move down on the last,
Remove with nothing selected (dimmed means inert). Other sizes follow the Library pages' scaling (`list_geom(Browser)`, the heading
band as high as the touch target where a chip shares it, the chips on a line of their own when the heading does not fit beside
them, as "Play album" at 320x480).

**Choices where the spec left them open.**
- **Play all** is the *Play* row of the playlist's sheet (a hold or Ctrl+O on a playlist row in the list, or on any track row of its
  page); a heading is never selectable, so Enter on the heading was not an option. A tap or Enter on a track plays the playlist
  from there (the queue is replaced, the source is the playlist file, so the sleep timer's playlist rule applies; shuffle and
  repeat keep their state, as the Library's lists do).
- **The sheet of an open playlist** has Play, Add tracks (T), Rename (R), Export M3U (E), Delete (D); the list's sheet has Play,
  Rename, Delete, Export M3U as the brief says. Add tracks is there so that a keyboard-only user can reach it; U, D and X or Delete
  move and remove on the page (the Queue's letters). Play and Export are dimmed on an empty playlist.
- **"+ New playlist"** is a row with the action dot and the title after it (owner rule: a dot on every row that acts, the title
  starts after the dot), not the amber text with a chevron of PlaylistsEmpty / NoticesP2c (the Playlists board itself draws a dot).
- **New playlist** asks for a name ("An empty playlist. Name:", suggested "Playlist", "Playlist 2" ...) and stays on the list;
  there is no notice for it, nor for Rename; Delete says "Playlist deleted." after the page it came from is shown (showing a page
  takes a notice away).
- **Add tracks (pick mode).** The hub without Folders and Playlists (nothing to pick there), its heading "ADD TRACKS TO <NAME>"; a
  tapped song is appended and "Added to <name>." shown, the album page's chip reads "Add album" and appends the album; the
  actions sheets are off in pick mode and nothing plays; Search works; Esc steps back as always and, from the hub, returns to the
  playlist's page (reloaded). Songs, Search and Albums are all there, as the brief asks; Artists and Genres come free.
- **Add to playlist** builds the picker on the worker (the playlists' list is synced first) and shows it as a sheet: its title
  "ADD <TITLE> TO", the playlists' counts in the key column, "N" for "+ New playlist"; more rows than the panel holds scroll inside
  the sheet (touch and keys); an album or artist adds its tracks in album order (disc, folder, track), duplicates allowed. A
  sheet may now reach over the panel's bottom edge up to the screen (the 5-row track sheet at 480x320 did already).
- **Save the queue.** A chip "Save as playlist" at the right of the Queue's heading (shown with the Library and a queue; the heading
  band grows to the touch target while it shows, the list below gives that height up), and P on the Queue page (Keys page row).
  The queue is saved in its play order. The suggested name is "Queue" or the next free one ("Queue 2"). Files the library does not know are
  kept as plain entries.
- **Import.** The tap on a `.m3u` row keeps its P1 meaning (play). The hold (1.5 s with the cue, only on these rows) or Ctrl+O opens
  the ImportM3U sheet; "Import as a playlist" copies it under its file name cleaned (a free name: "Imported as <name>."; a taken
  one: the name box opens with the next free name and the notice "That name is taken. Pick another."). "Play" and "Add to the
  queue" read the file with the same reader and skip what is not there ("This playlist has no track Vinyl can find." when nothing
  is).
- **Export.** The folder chooser is the Browser's choose mode ("EXPORT TO: <path>", the strip's "Use folder", U); Esc goes back to
  the playlist. The file is `<name>.m3u8` in that folder, replacing one that is there. "Exported to <where>: <name>.m3u8." where
  <where> is the drive's name for a folder on a drive (SANDISK), else the folder's own name; a folder that cannot be written
  shows "Could not export here: the folder cannot be written. Choose another." and the Browser stays.
- **Missing entries** (the rule that wins over "a gone drive's tracks are shown nowhere"): kept in the file through every edit, listed
  dimmed with "file not found" (the title from the library when it knows the file, else the `#EXTINF` text, else the file name),
  inert to a tap, skipped when playing and never queued, selectable so that Remove can take them out; the heading counts them; a
  playlist with nothing playable is still listed.

**Deviations from the boards** (the toolkit's widgets win over pixels).
- The name box and the question are the toolkit's Dialog (Cassette's look), not the cream boxes; the board's hint line under the field
  is the dialog's own ("Letters, digits, space, - and _ only. Up to 24 characters."; the keyboard sentence of the board is not
  drawn: the keyboard's presence is the toolkit's policy). With the on-screen keyboard at 480x320 the dialog's title goes to the page
  header, as the toolkit does.
- The delete question has a second line, "Y = Delete, Enter or Esc = Cancel" (the owner's "Clear the queue?" pattern: the letter that
  confirms is named).
- Move up, Move down, Remove and Add tracks have no icon (the board draws one); the buttons are 42 design px high; the chips 24.
- Notices use the Library pages' place (over the last rows) and the owner's six / eight second timing, not the board's centred pills.
- Playlist rows in the Browser hold no sheet except the .m3u / .m3u8 ones (the cue shows only on them).
- The edit page shows no row number (the PlaylistEdit board's; PlaylistEditMissing draws numbers) and a missing Windows path shows
  its file name, not the whole path.

**Tests and scenes.** `test_m3u` (72 checks: Windows backslashes and drive letters, Latin-1, BOM, UTF-16, CRLF, relative, absolute,
missing kept, URL lines skipped, #EXTINF, the writer, round trips), `test_playlists` (names, sanitising, create / rename / delete /
move / remove / append, collisions without case, an interrupted write, import, export, the index = the files, rebuild, nothing
written when nothing changed), `test_playlist_model` (rows, headings, sheets, keys), `test_playlist_ctl` (every page and key:
New playlist, Add to playlist from a song, an album and an artist, the edit page's buttons and keys, pick mode, play from an entry,
missing entries, rename, export, delete after a question, Save the queue, a file added outside Vinyl). Scenes (each with `-small` and
`-deck`): `qa-playlists`, `qa-playlists-empty`, `qa-playlists-missing`, `qa-playlists-export`, `keyboard-playlists` (osk on and off,
the box for Save and Rename), and `portrait-band-playlists` (480x800, 480x640, the writing band); the Library walks of P2b lost
their "Playlists arrive in a later version." step for the page.

**Not verified without a board.** That the toolkit's on-screen keyboard and the PDA band behave in the name box on the HackBerry
and zero8 as in the scenes; the Browser's hold and Ctrl+O on a `.m3u` row on a pointer (the hold is the P2b one); that vfat
refuses nothing in the export (a read-only stick shows the notice); a playlist with 5,000 entries on the Pi Zero 2 W (the edit page
reads the file and the library per entry on the worker: about 0.1 ms an entry on the build box); Windows players on the exported
file (B54).

## P2c, round 2: marks in Add tracks, the Play button (owner, 2026-11-15, board test on the Pi 4B)

**Owner, on the marks:** "adding songs to a playlist there's nothing indicating that I already added a song in a playlist. we need an
indicator in lists that songs are already part of the playlist being created/edited". Answers to the coordinator's two questions: a tap on
a song already in the playlist ADDS IT AGAIN and the number grows ("Add it again, the number grows"); the Add to playlist picker SHOWS
which playlists already contain the song ("Yes, a check mark on those playlists"). Boards: AddTracksSongs-480x320 and
AddToPlaylistMarked-480x320.
- **The rule.** Marks are on SONG rows only: Songs, Search's songs, an album's tracks (the pages a track can be picked from). An album,
  artist or genre row is never marked: a mark there would need every track of every row looked up in the playlist (a library walk a row),
  and "all its tracks are in" is not what the owner asked for; the album's page shows the songs. A track is its path; a missing entry
  counts for its path. The count is a hash map (`PickMarks`, `core/library/lib_model`) built once from the playlist's entries when Add
  tracks starts and kept by the controller as tracks are added; a row is one lookup (5,000 entries: well under 500 ms with the rows, in
  the test; in practice a few ms). The edit page is not reachable while picking, so removals cannot happen in between; the next Add
  tracks counts again from the file.
- **A tap** appends the song again at once (the mark and the heading's N change before the file is written, with no list reload, so the
  scroll stays; a failed write takes it back and says so). The notice: "Added again." when the song was in already, else "Added to
  <name>.". "Add album" is unchanged.
- **The header** of every page that has a heading of its own while picking (the hub, Artists, an artist, Albums, a genre's albums, Songs,
  Genres) reads "ADDING TO <NAME>, N TRACKS" with the brown **Done** chip at its right (the heading's band grows to the touch target while
  it shows, as the Queue's chip does); an album's page keeps its own heading and its "Add album" chip; Search keeps its field; the hub
  keeps its Search chip. Done and Esc both go back to the playlist.
- **Colours** are the board's: the check #3FBF6A, the chip #1D3A28 with #7FE0A0; the toolkit's widgets are not restyled (the check and the
  chip are drawn by the app's list rows, like the dots and chevrons).
- **The picker** says "in it, 18 tracks" / "in it x2, 31 tracks" in a playlist's right text for a song (counts from the index, synced
  just before the sheet); choosing it adds the song again; an album or an artist has no marks (the same rule); the right column grows to its
  text.

**Owner, on the playlist page (second redesign, the Play button of the first answer is replaced):** "still on the playlist screen : if add
tracks is a secondary that shoud be the same to remove a track those 2 have similar impact. on a touchscreen only moving up/down tracks is
not possible as when you click a track it starts to play. on that playlist view a click should not start to play. so overall there's
something no working on the playlist management we should have by default on this view in a playlist 2 options, play and edit, Play should
start the playlist from the first track and edit chnage the buttons for move up/down, remove and add track. rename and export M3U should
be part of the option menu after long press or ctrl+O". Boards: PlaylistView, PlaylistEditMode (480x320 and 320x480), PlaylistOptions.
- **Two modes.** The VIEW (the default when a playlist opens): numbered tracks (the boards number them and draw no dot: the rows inform), a
  tap or click ONLY selects a row; Play (primary) and Edit (secondary) under the rows; no Rename or Export chips. Play replaces the queue
  with the playlist's playable entries, from the first one (shuffle and repeat as set); dimmed with "Nothing in it can be played." (a tap, or
  Space, says it) when every entry is missing. The EDIT mode: "EDITING <NAME>, N TRACKS", a Done chip at the heading's right, Move up, Move
  down, Remove and Add tracks all in the same secondary look (no primary button), two rows of two where the four words do not fit in one
  row (the portrait boards); taps only select. Done or Esc returns to the view; a second Esc leaves the page. Add tracks (the pick mode)
  returns to the edit mode.
- **The options sheet** (hold a row 1.5 s, or Ctrl+O, in BOTH modes): Play from here (Enter), Rename (N), Export M3U (E), Delete playlist
  (the Del key; it asks first, Cancel the default). **The Controller's choice, flagged for the owner's veto:** Enter on the keyboard in the
  view does "Play from here" (a tap never plays); in the edit mode Enter does nothing. Space plays from the first track in the view (in the
  edit mode it is the player's play/pause as everywhere).
- **Keys.** Up and Down move the selection in both modes. In the edit mode Shift+Up and Shift+Down move the selected track (the keypad's
  own arrow that follows is dropped; NOT verified on a board, so U and D, the Queue's letters, do the same and the buttons are there), X
  and Delete remove it. Delete outside the edit mode does nothing (a track is not removed by accident).
- Tests: `test_playlist_model` (the numbered rows without a mark, the options sheet, the editing heading), `test_playlist_ctl` (a tap never
  plays in the view or the edit mode, Play from the first track and its missing-first-entry and all-missing cases, Enter and Play from
  here, Done and Esc in order, the four edit buttons, Shift+arrows, the sheet in both modes with Rename, Export and Delete by their keys);
  scenes `qa-playlists` (the view, the selection, the options, the edit mode, Done), `qa-playlists-missing`, `-export`, `keyboard-playlists`.

## P2c, round 3: the queue (owner, 2026-11-15, board test on the Pi 4B)

**Owner:** "when starting to play a playlist the queue is not populated with that playlist. the more a I try to use the queue the less I
find it useful clear all makes me think I will delete the files not just clear the queue, I would change that clear all for a add that
send to the library where user can select multiple track (using the check for playlist) and finish with a done button instead of the mini
player." Answers to the coordinator's questions: emptying the queue gets NO replacement button ("Nowhere: remove rows one by one"); a tap on
a song in the queue's pick mode "Adds it at the END of the queue, marks show".
- **The queue after Play.** Every entry point already replaces the queue with the list that was played, from the chosen index (`Queue::set`,
  then the engine loads that index): the Playlists page's Play and Play from here, the playlist and the import sheets, an album or an
  artist, a song list of the Library, a folder's tracks and a `.m3u` row of the Browser. I could not reproduce an unpopulated queue on the
  build box by reading the code or in the scenes, so this round adds a regression check for each entry point: the unit tests assert what
  the controller hands to the player (the whole playable list in order, the start index, the playlist file as the source), and the scene
  `qa-queue-add` plays a playlist, an album and a folder's track in turn and opens the Queue after each, expecting exactly that list
  (and none of the previous one). If the owner sees it again, the journal of the board (`[vinyl] media gone ...`, which empties the queue
  of an entry of a drive that is not mounted) is the first thing to read.
- **No Clear all.** The button, its "Clear the queue?" dialog, the C key, the Keys row and the dialog scenes (`qa-dialog`,
  `qa-dialog-small`, `qa-640-dialog`) are gone; the toolkit's question dialog stays for Delete playlist. The fourth Queue button is **Add**
  (the secondary look of the other three, a plus; the A key), enabled with an empty queue too; Move up and Move down are dimmed with one
  entry.
- **Add to the queue** is the playlist pick mode aimed at the queue (`LibraryCtl::begin_queue_pick`): the marks are the queue's own entries
  (a hash map by path, "x2" when twice); the heading "ADDING TO THE QUEUE, N TRACKS"; a tap on a song appends it at the end ("Added to the
  queue."); the hold or Ctrl+O gives Play next and Add to the queue on a song, an album or an artist (an album or an artist whole, in album
  order); "Add album" on an album page. The same marks rule as the playlists' (song rows only).
- **The Done button** is a full-width brown button at the bottom of every pick page where the mini player strip is (the hub hides the strip;
  on the other pages the list stops above it; it hides while the on-screen keyboard is up in Search). It REPLACES the Done chip of the
  AddTracksSongs board for the playlists' Add tracks too (the chip stays only in a playlist's edit mode, where it leaves the mode). Done and
  Esc at the hub both finish; the queue pick returns to the Queue page, the playlist pick to the playlist's edit mode.


## Gain boost and Volume steps (0.5.0 work, 2026-11-15)

Built from report 060 3e and 3f with the owner's words of 2026-11-13 (see "Later (not built)" above) and decisions D23 and D24. Not
published yet; the Controller bumps the version with the Equalizer and the crossfade.
- **Rows.** Playback is Gapless, ReplayGain, **Gain boost**, **Volume steps**, Remember position in long files, each an Edit row (value, then a
  chevron) in the style of the others. Gain boost opens a pick list (Off, +1 dB ... +4 dB) and Volume steps one (Normal, 1 dB, 0.5 dB, 0.1 dB);
  a choice closes the list (the owner's rule for pick lists) and the dot marks the current one. Boards: scenes `qa-gain`, `-small`, `-640` (the
  page and both lists) and `qa-dbchip`, `-small`, `-640` (the chip in dB). No new component: the lists are `ListPage`s like ReplayGain's.
- **Boost per output** (D23): `settings.json` `boost_db` {output key: 0..4}, keyed as the volume is (`output_volume_key`), applied at every volume
  level; the row shows the playing output's value; an out of range value is clamped, a non number ignored. It is part of the engine's gain
  chain (volume x ReplayGain x boost x the sleep fade); the limiter comes in when that net gain is above 0 dB.
- **Limiter** (`core/dsp/limiter`): linked stereo, 5 ms look-ahead, release 150 ms, ceiling -1.0 dBFS; the gain is the boxcar average (the
  look-ahead long) of the release-limited sliding minimum of the required gains, so it never exceeds what a frame needs and moves in ramps;
  one frame out per frame in (the delay line starts with zeros, `flush` plays the tail). The engine latches it on for an open stream the first
  time the net gain is above 0 dB (a ReplayGain above 0 dB counts too) and drops it with the stream; with a net gain of 0 dB or less and no
  limiter latched nothing changes: today's integer `convert_pcm` path, bit-exact. Where the report was silent: no dither (rounded), the 5 ms is
  not taken off the heard position (at most 5 ms), the first 5 ms of a boosted stream is silence.
- **Volume steps** (D24): one global setting, `settings.json` `volume_steps` ("normal", "1", "0.5", "0.1"). Normal is 0.4.0 unchanged (about 2 dB,
  the percent shown). The fine modes use `volume_model_steps`: levels exactly on the grid hi, hi - s, hi - 2s ..., s the step but never finer than
  the output's own step (the Sharkoon's 1 dB, the Apple adapter's 0.5 dB; the Pi jack's 0.01 dB lets all three through); the chip shows the
  output's real level in dB with one decimal ("-22.5 dB", "+4.0 dB"); where the text does not fit the box even at the caption size the " dB" goes
  and the number stays. A sound server's sink is continuous, so its card's step does not limit it (the report's "0.1 dB works even on a 1 dB
  card"). Changing the step re-reads the output and lands on the nearest dB. The boost is not added to the chip (the chip is the output's level:
  the jack's top stays "+4.0 dB"). A card without a mixer (Vinyl's own software volume, a percent in state.json) keeps its Normal levels and
  shows their dB.
- **Hold to repeat** (`core/volume/hold_repeat`): the group's "-" and "+" act on the press and repeat while held: nothing for 0.5 s, then 10 a
  second, from 2 s of holding 30 a second (the 100 ms tick; at most 8 steps in one tick after a stall); release or sliding off stops it. The -
  and = keys repeat through the keyboard's own auto-repeat as before.
- **Tests.** `tests/test_gain_steps.cpp` (limiter, bypass, gain product, step arithmetic, hold with a fake clock), the engine's boost tests in
  `test_replaygain.cpp`, the settings round trip in `test_settings_state.cpp`, the pages in `test_menu.cpp`. Board tests B64-B71.
- **What a boost really gives (measured 2026-11-15, owner's board tests on the Pi 4B and the HackBerry).** The limiter is a sample peak limiter with
  a -1 dBFS ceiling, so a boost can only be as loud as the headroom of the track allows. Measured as RMS loudness gain of a +4 dB boost
  (`test_dense_loudness`, a deterministic master of kick, bass and noise): a normal level (peaks -12 dBFS) **+4.00 dB**; peaks at -3 dBFS
  **+3.6 dB**; a dense master (peaks -0.5 dBFS, crest 9.2 dB, the limiter reduces 0.8 dB on average, 4.5 dB at most, 49 % of the frames)
  **+2.6 dB**; a hard clipped master (crest 3.9 dB) **+0.1 dB**. So "a dense master keeps about 2.6 dB of the 4": the owner measured about
  2 dB on his dense dance masters on the HackBerry and **decided that is the physical maximum: the limiter stays as it is** (no redesign, no
  soft clip; a crest reducing clipper would give more at the price of distortion and is not planned). The help text of the Gain boost list says
  "a dense master gains less than the set value"; B64 expects the same.
- **The gain journal line** (engine thread): once after the first 2 s of a track, at its end, and when it is left after half a second, while a
  boost or a positive net gain is in force:
  `[vinyl] gain: net +4.0 dB (boost +4 dB, replaygain +0.0 dB, app volume +0.0 dB) limiter on: reduction avg 0.8 dB, max 4.5 dB, 48 % of frames
  limited, peak in -4.1 dBFS, after gain -0.1 dBFS, out -1.0 dBFS (first 2 s of song.m4a)` (a net gain under 0 dB: `limiter off (net gain under 0 dB)`).
  "App volume" is Vinyl's own software volume and the sleep fade, not the output's mixer. It covers every decoder: the engine applies the gain
  after the decoder, so the native ones and FFmpeg (tier 2) go through the same code; `test_engine_boost_tier2` plays an AAC .m4a through the
  real FFmpeg at 0 and +4 dB and checks +4.00 dB at the output.
- **Gain paths checked** (engine.cpp): the boost is one factor of `sample_gain()`, which every block of every stream uses, so it reaches the output on
  a gapless next track, after a seek or a pause (the limiter is rebuilt and latches again), under the sleep fade, and when set while playing
  (an atomic read at the next block, about 20 ms). No path bypasses it.

## A list never jumps while it is browsed (owner, 2026-11-15, HackBerry, a long queue)

**Owner:** "while scrolling on a long queue, when I stop the list reverts to the selected line, so I don't have time to read all the song
names if I want to select another one."
- **Cause.** `RowList::set_rows` (ui/row_list.hpp, the one list the Browser, Queue, Output, the Now playing pane and every Library page share)
  put the highlighted row back in view on every refresh that rebuilt the rows (a track change, a rescan: the rows' version changed), so a list the user
  had scrolled away from the selection was scrolled back as soon as anything changed. The periodic snapshot itself did not: the rows are only
  set when their version or the selection changes.
- **Rule** (`core/nav/scroll_follow.hpp`, a pure policy with timestamps, tested with a fake clock in `tests/test_scroll_follow.cpp`): the user is
  browsing from his last scroll (a drag and its momentum; the app's own scrolls are fenced off) until his next explicit action (a key that moves the
  highlight, a tap on a row) or the page being opened again. The list scrolls to the selection when the page opens and on an explicit action; on a
  refresh only while the user is not browsing. While he browses, a track change may move the highlight and the list stays exactly where it is, with no
  time limit. Keys keep working as before: the first key after a scroll brings the selection into view.
- **Where.** In the shared RowList only (the Menu's short lists have their own code and never scroll on a refresh). The Now playing queue pane counts
  its cursor as an action only when the user moved it (`queue_selected_by_user`), not when the cursor follows the track.
- **Scenes.** `qa-queue-scroll`, `-small`, `-640`: a 200-row queue (the demo variant `VINYL_DEMO_BIGQUEUE`, the 101st playing), eight drags away,
  the next track starts by itself (`VINYL_DEMO_NEXT_AFTER`), `expect notscrolled`; then a key and `expect scrolled`.


## Vinyl 0.5.0 published (2026-11-16)

Published as 0.5.0: Gain boost, Volume steps (with the dB chip and hold to repeat) and the list scroll fix; the Equalizer and the
crossfade are not in it and stay P2d. The owner's words: "publish gain boost and volume steps as 0.5.0."
- **Owner board results before the publish.** Volume steps works (B67, B68 and B69 pass on the Pi 4B; the 1 dB steps verified on a HackBerry
  USB DAC through the sound server). The gain boost is audible; on dense masters about 2 dB of the 4, which the owner took as physics: **the
  limiter stays as it is** (measured: normal level +4.00 dB, peaks at -3 dBFS +3.64 dB, dense master +2.62 dB, hard-clipped +0.10 dB). The
  boost per output was verified (a Bluetooth speaker Off, a USB DAC with its own value).
- **Not run on hardware at the publish.** B70 second half (0.1 dB on a sound-server output and on a Bluetooth speaker), B71 (the layout on the
  deck, zero8, HackBerry, pi52 and pi3a: the draft was installed on them and the owner was checking), B59 (accent typing), and the retest of
  the queue scroll on the board. The unit tests (8452, normal and with the address and undefined sanitizers) and the headless scenes passed.
- **A tip written into the README**: the built-in jacks of the Pi 3 and Pi 4 are basic quality; a USB DAC is cleaner and louder, and the boost
  helps most on the built-in jack.
- **Packages**: vinyl_0.5.0 arm64 and amd64, built against the shared pin 28817a5 (0.1.0 to 0.4.0 kept, as in 0.4.0).

## Knock-knock: the screen off while the music plays (0.6.0 work, 2026-11-16)

**Owner** (request and decisions of 2026-11-16, the launcher roadmap row "Vinyl: knock-knock"): a Vinyl setting, Off by default; three quick
taps on the cover square switch the backlight fully off and Vinyl ignores every touch but another three quick taps, anywhere, which bring it
back. Decided: (1) a new `launcher.sock` request switches the backlight off and on without saving it, the launcher restores it by itself when
the app hides, quits or crashes, on levels and on / off backlights; the setting is hidden where board.state's `requests` lacks the verb; (2)
while off only the media and volume keys work (headset and keyboard play, pause, next, previous, volume); every other key and touch is
ignored; holding Esc 3 s still closes the app; (3) taps: three within 1.2 s, each under 0.4 s, gaps under 0.5 s, little finger movement; arm on
the cover square only, wake anywhere; (4) ships as Vinyl 0.6.0 with a launcher release carrying the request (0.10.4 or later). The launcher half
is built (launcher branch `backlight-request`, commit 827d6cb7: `screen off|on`, `cp0_app_backlight()`, `requests=...,screen`). Below, the
builder's readings where the decisions were silent, for the owner to confirm.
- **The row** (moved by the owner after the first board look, 2026-11-16): the Menu page, TOOLS, directly under the Sleep timer:
  "Knock-knock" with On / Off, an Edit row (the value, then a chevron) like the Sleep timer's; Enter or a tap toggles it on the first tap, as
  every Menu row acts (the coordinator's brief said "two-tap rows like the other settings"; Vinyl's Menu rows act on the first tap since P2a).
  Listed only where `knock_available` (Vinyl has the request, board.state's `requests` lists `screen`, screen.state's `touch_slots` is not
  0); a value left On where it is not listed does nothing. The Playback page lists it no more and its strip is its ReplayGain sentence
  again. The Menu page has no help strip (Menu-480x320 draws none), so the sentence written for the Playback strip ("Knock-knock: three quick
  taps on the cover switch the screen off while the music plays on; three quick taps anywhere bring it back.") went with the Playback row:
  the explanation is in this README and the board tests only. `settings.json` `knock_knock` (bool).
- **The taps** (`core/screen/knock`, `TapSequence`, tested with a fake clock): a press shorter than 400 ms (399 counts, 400 does not), a gap from
  a release to the next press shorter than 500 ms, the three within 1200 ms from the first press to the last release (1200 counts); the finger
  within the toolkit's tap slop of its press (a sixth of TARGET, at least 6 px: the map widget's rule; 13 px on the deck), checked on its moves
  and its release. A press elsewhere (arming), a long press, a drag or a lost press breaks the run; the window slides, so after a slow start
  the last three taps still count and a timeout simply restarts the count. The three taps need not land on the same spot.
- **Where a tap lands.** The cover square is a transparent box exactly over the sleeve, made right after it, so every control drawn later (a
  neighbour's touch area grown over the sleeve's edge) keeps its own taps; it acts on nothing. The controller sees every press and release of
  the touch screen through the pointer's own event list (LVGL 9.5 sends PRESSED and RELEASED there before the object gets them), so a tap
  elsewhere between two cover taps breaks the run.
- **While off** (`ScreenGate`: Disabled, Ready, going off, Off): an opaque black box over the whole display on its system layer (above the
  header, the notices, the dialogs and the on-screen keyboard) takes every touch from the moment the third tap is released until the third
  wake tap is: nothing under it is pressed, the first wake tap included. Keys: the media keys (KEY_PLAYPAUSE, PLAYCD, PAUSECD, NEXTSONG,
  PREVIOUSSONG, STOPCD, from the evdev reader and the keyboard path) act; the volume and mute keys are the launcher's and Vinyl never acted on
  them; `-` and `=` (Vinyl's own volume keys) act, one step a press (owner decision, below); every other key is dropped, a short Esc
  included. The launcher's 3 s
  Esc reads the keyboards itself and sends SIGTERM, which ends Vinyl cleanly. Nothing is drawn while off (the page refresh and the record's turn
  stop, and the drawing is frozen under the black cover, below; the music, the queue, the sleep timer, the saves go on); a held previous /
  next scan and the volume repeat are cancelled when it goes off.
- **Black, not only dark** (owner board finding, 2026-11-16, Pi 3A+ Waveshare DSI panel, `/sys/class/backlight/10-0045`, max 255):
  backlight 0 and even `bl_power=4` only dim that panel, the picture stays faintly visible. As the launcher's own lock screen does,
  the shield is fully opaque black (no border, no radius, 100 % of the display on the system layer, so every rotation and portrait
  canvas) from the third tap to the third wake tap: only black pixels remain whatever the backlight can do. Once that black frame is on
  the panel (the next tick: the pending areas drawn with `lv_refr_now`) the display's invalidation is switched off, so no animation,
  clock or scrolling label paints under it; on the way back the invalidation comes back and the whole display is invalidated, so the
  first frame after waking is complete (`core/screen` `BlankCover` decides when, tested: never frozen in the step that showed it nor
  while hidden, every freeze thawed exactly once, as LVGL counts the pairs and the lifecycle helper nests its own while hidden). Every
  restore path takes the black away (exit, hide, the setting Off, refused); a crash leaves the last black frame on the framebuffer
  until the launcher repaints as it takes the panel back (its normal path after an app ends; the crash guard only restores the light).
- **The request** goes through `cp0_app_backlight()` (given to the app by the device main: the headless harness has no client) on a worker
  thread (`BacklightWorker`: in order, a waiting request replaced by a newer one, answers matched to their `off` by a tag); input is ignored
  from the third tap on, without waiting for the answer. Refused or no service: the screen stays on, the input comes back at once, the notice
  "Screen off is not available." (the builder's words; the brief suggested "Screen off not available"), and an `on` for safety.
- **Every way out restores** (the launcher does it too, about a second later, but Vinyl does not rely on it): the exit (Esc at the root, the
  launcher's close, SIGTERM / SIGINT / SIGHUP: the worker stopped, then `screen on` at once on the UI thread whenever an `off` was ever asked),
  hidden (Super, Alt+Tab, the three-finger hold), the setting turned Off, the launcher no longer listing `screen`, and a crash Vinyl can catch
  (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT: `core/screen/crash_guard` sends `screen on` with async-signal-safe calls, then the signal's previous
  action runs). `kill -9` is the launcher's (its process watch).
- **Scenes** (the scenes' fake backlight `VINYL_DEMO_SCREEN=1|fail`, `VINYL_DEMO_KNOCK=1`, `VINYL_DEMO_NOTOUCH=1`): `qa-knock`, `-small`,
  `-640` (the Menu with the row under the Sleep timer, toggled; the Playback page without it), `qa-knock-old` (no `screen`: no row, a setting left On does nothing), `qa-knock-notouch` (no touch
  screen: the same), `knock-taps`, `-small`, `-640` (three taps on the cover,
  then a tap on the output button and the keys Q, Esc, M do nothing, three wake taps on the output button press nothing, then a tap opens
  it; slow taps on the cover, and a tap on the title between cover taps, do nothing), `knock-refused` (the notice, the input stays),
  `knock-hide` (hidden while off, shown: the first tap acts, a full repaint). The screen cannot be seen to go dark in the harness: the scenes check what the
  input does. Board tests B72-B83.
- **Owner decisions on the open questions** (2026-11-16, through the coordinator): (1) while the screen is off `-` and `=` work (Vinyl's
  volume down and up, as the other volume keys; every other key stays ignored); (2) the row is also hidden on a board without a touch screen:
  screen.state's `touch_slots=0` (`core/screen` `screen_touch_slots`, `knock_available`; read once at start; a launcher that writes no
  `touch_slots`, an older one or multitasking off, means unknown and the row shows); (3) the strip after the rows, reached by scrolling where it
  does not fit (480x320, 640x480), is accepted as it is (since the move the Playback strip is its one sentence again); (4) the row is a tool:
  Menu > TOOLS, under the Sleep timer (see The row). Still to confirm: the notice "Screen off is not available." and whether the Menu should
  say how Knock-knock works anywhere (it has no strip).
- **Built against** the launcher branch `backlight-request` at 827d6cb7 (an LF copy, not the shared pin): the release pins the launcher
  release that carries `cp0_app_backlight()`; there is no fallback for a toolkit without it.
