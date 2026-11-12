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
  non-default sink plays; software volume only without a mixer.
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
- Browser rows act on the first tap (open a folder, play a track) at the compact list height.
- Controls drawn smaller than 7 mm keep their drawn look and get a larger invisible touch area (R-4 applies).
- The volume strip's "-" and "+" are drawn lower than the touch target where the band between the chips and the
  transport keys is lower (480x480, 800x480, 480x640, 720x720, 320x480); their touch areas take the band (accepted as a
  WARN by the coordinator, 2026-11-12; the sizes are on the `volume_strip_buttons` line of the QA hand-off).
- The icon: four colours (cream sleeve, teal ring and label, black record, grey grooves) and thin groove rings, an
  exception to the palette's rule 3; the coordinator's design call (2026-11-12).

## Open

- While the volume strip is open no touch area overlaps another one (design QA run 2). At 480x480 and 480x640 the gap
  between the volume chip and the transport keys is 79 and 61 px, under the target (80 and 63): the "+" area is the
  whole gap there and the harness's size check reports it (R-4: the owner's call on usability).
- At 480x480 the six transport keys cannot all be the target wide (80 px) without overlapping: their touch areas overlap
  their neighbours' as before (the strip open or not).
