# Cassette: decisions recorded with the code

The owner's decisions and the worker's calls that the code follows, newest first. The study and the earlier decisions
(D1 to D19, the recorder screen, the CARD and SOFT rule) are in the launcher repository's report 058.

## Card gain on the FZ-M1's microphone: the Mic boost, and a gain that read "stuck" (owner report, 2026-11-21)

The report (Panasonic FZ-M1, amd64, `card 0: PCH [HDA Intel PCH] ALC256 Analog`, the integrated microphone): the CARD gain
"is not working, stuck at -17.3 dB". The mixer facts read on the tablet: `Capture` (Capture Volume + Capture Switch) 0..63,
-17.25 .. +30 dB, 0.75 dB a step, stereo, at 0 (-17.25 dB); the input source (Internal Mic / Headset Mic); `Internal Mic Boost`
and `Headset Mic Boost` 0..3 = 0 / 10 / 20 / 30 dB; amixer writes to both worked.

- **What the code does with that card** (traced end to end, and run in `tests/alsa/test_mixer.cpp`
  `alc256_level_moves_and_reads_back` and `tests/app/test_settings.cpp` `t_alc256_gain` on `fake_alc256`, which has exactly those
  controls): `Mixer::discover` takes `Capture Volume` (never a boost, never a playback control), the range -17.25 .. +30 dB
  comes from the driver's dB data, the driver's step of 0 counts as 1, `set_level_db` writes the raw value to all channels
  and `read()` returns it. Every position of the Settings slider (47, one dB each) and every CARD press lands on a distinct
  raw step and reads back. So the gain cannot be stuck in Cassette's own mapping, in the read-back or in the saved state;
  what the fake cannot do is a card or a sound server that changes the value again after the write (the tablet runs
  PipeWire, and Cassette records there through the server: `via_server`).
- **What 0.4.1 adds to settle it on the tablet**: the controller logs one journal line for every change of the card's capture
  controls (`[cassette] card mixer level set: hw:CARD=PCH 'Capture Volume' raw 30 (5.25 dB)`, or `level NOT set ... error -N`
  when the write fails), and it reads the controls again every 1.5 s: a value changed outside Cassette (amixer, PipeWire
  restoring its own volume) now shows on the screen (and is logged as `changed outside`) instead of the last value Cassette
  wrote. If the tablet still "sticks", the journal says whether the write failed, was taken and reverted, or never came.
- **Why the card sounded dead at "0 dB"**: with the boost at Off the whole range is -17.25 .. +30 dB and the microphone is
  weak; the boost's +30 dB is the part that was missing.
- **Mic boost** (the second gain stage; the owner asked for it):
  - A card has one when it lists `<X> Mic Boost Volume` (writable, 2 or more steps): the HDA names `Internal Mic Boost
    Volume` and `Headset Mic Boost Volume`, or a plain `Mic Boost Volume`. Cards without one (the USB microphones) are
    unchanged: no row, no stepper change, no new write.
  - Which boost: the one of the input in use. The input is the selected item of the card's `Input Source` (or `Capture
    Source`) enumeration; without one, the `<X> Capture Switch` that is on; with neither, the one named `Internal ...`,
    else the first. It is looked at again at every read, so the jack switching to the headset moves the row to the headset's
    boost. (`real_alsa` reads enumerated controls and their item names through optional libasound symbols; without them the
    rule falls to the last two cases.)
  - The steps are the control's own: Settings gets a **Mic boost** picker row right after Input level, entries `Off`, `+10 dB`,
    `+20 dB`, `+30 dB` (labels from the control's dB, `Step n` when the driver gives none).
  - Remembered per card as `boost_db` in the card's `settings.json` entry (a gain, not a raw value, so it follows the active
    input); written again at start with the level.
  - Total gain: the Input level row stays the level alone (it is a slider of the capture level) and carries a muted second line
    `Total +10 dB with the Mic boost` while the boost is on; the recorder's CARD stepper value shows the total (level +
    boost). With the boost off, or none, both are exactly as before.
  - Not offered for a Bluetooth headset (no card controls there).
- **Worker's calls** (to be confirmed by the owner): the detail line's wording; the stepper showing the total rather than the
  level alone; the poll interval of 1.5 s (a few `snd_hctl_open` calls per read); one boost value per card rather than one per
  input.

## Playback VOLUME (owner, 2026-11-16)

The owner's request: "While playing a take there's nothing to adjust playback volume. Swap in the cassette deck, while
playing, the input soft gain buttons and value to become volume at playback."

Owner decisions (final, relayed by the coordinator):

1. The VOLUME control steers the **output's own volume**, the same model as Vinyl (`apps/vinyl/docs/decisions.md`, "D11
   changed" and "Volume levels fitted to each output"): the playing output's mixer, steps fitted to the output, a software
   volume only where the output has no control, Bluetooth and sound-server outputs through the libpulse layer, the
   launcher's volume keys staying in step, and the first use of a silent output starting near 50 %. Vinyl's gain boost and
   its fine "Volume steps" (Normal / 1 / 0.5 / 0.1 dB) are **not** taken: only the standard step of about 2 dB.
2. On the Recorder page, while a take plays, scans (REW / FWD), is paused in playback or previews from the Recordings
   list, the SOFT stepper's slot becomes a **VOLUME** stepper (label VOLUME, the value in percent, the same widget and
   buttons, at the full width as SOFT is on a card without a level control) and the CARD stepper is hidden. On STOP, REC
   or the end of playback CARD and SOFT come back exactly as before. Recording and the idle meter are unchanged; VOLUME
   never touches the input gain and playback never changes the SOFT or CARD values. The change of state must not make the
   layout jump on any layout class.

How the code follows them:

- **The model** is Vinyl's, copied into `src/core/playback/output_volume.*` (same author and licence) without the fine
  steps: per output, the window is the top 60 dB of its real range, N = ceil(window / 2 dB) levels (at least 2, a step
  never under 1 dB nor under the control's own step), level 0 mute, level 1 the window's bottom, linear in dB, the percent
  shown round(100 i / N). Where the range comes from: a card's playback control (Master, PCM, Speaker, Headphone,
  Headset, else another "... Playback Volume"), a sound-server sink's own volume (the window of the ALSA card behind it
  moved under the sink's 0 dB; -60..0 dB without one, Bluetooth), or Cassette's software volume over -60..0 dB where the
  output has no control (the Player's gain; at 100 % the samples go out bit for bit).
- **Which output** (`volume_target_for`): the output in use (Settings > Output, automatic by default). A card Cassette
  opens itself (`hw:CARD=...`) uses its own control, also on a board with a sound server; ALSA's `default` uses the
  server's default sink when a server answers (asked with the server's info while a take plays), else the default card's
  control; a Bluetooth output uses its sink. Settings key: `bt:<MAC>`, `pulse:<sink>`, `alsa:hw:CARD=<id>`,
  `alsa:default`.
- **Added to Cassette's layers** (only what was missing; the rest of the ALSA and libpulse layers is Cassette's own): the
  Raspberry Pi jack's mute-flag fix (`CtlInfo::db_next_*`, `alsa_fix_mute_db`, applied only to the playback control so
  the capture controls are read exactly as before), the sinks' volume fields, `set_sink_volume_raw` / `read_sink_volume`
  and the two optional libpulse symbols, `PulseWorker::request_sink_volume`, the server's info in a scan on request, the
  take's playback stream opened at 100 % unmuted with its own restore key (`own_volume`; the live monitor's stream is
  unchanged).
- **Worker's calls** (the brief left them open; to be confirmed by the owner):
  - Keys: SOFT and CARD have no keys (their buttons are on the screen), so VOLUME takes Vinyl's keys, `-` and `=`
    (keypad - and +), on every page while a take plays (the recorder, the list's preview, the Edit page's PLAY); held, the
    keyboard's own repeat steps it. They do nothing while no take plays. The launcher's volume keys are untouched: they act
    on the default output, and the stepper follows them (a card's control read every 0.3 s while a take plays, a sink's
    level with the server's change events).
  - Hold to repeat on the touch buttons: none, as SOFT has none (one level a tap).
  - The value: `52 %`; muted `Mute` (Vinyl's word); not known `--` with both buttons dimmed. An output without a volume
    control shows its software level in the same way (no extra marking).
  - A microphone's loop-back into the output ("Mic Playback Volume" of the PCM2902, "Line", "Sidetone", "Loopback",
    "Monitor" ... Playback Volume) is never taken as the output's level (Vinyl's fallback would take any playback volume).
  - The first-use rule (Vinyl has no such rule; the owner's decision names it): the first time a take plays on an output
    with no `playback_volume` entry and the output is silent (muted, level 0, or level 1, the bottom of its window), its
    level is set to the level nearest 50 %; the entry is written then, so the rule runs once per output. Not for the
    software volume (100 % until changed). No notice is shown for it (Vinyl has none); the stepper shows the new level and
    the journal logs `(first use of a silent output)`.
  - The remembered value per output (`settings.json` `playback_volume`): `volume` (the level last set from Cassette, the
    level itself for the software volume) and `muted` (Cassette's own mute); written within a second of the last change and
    when the app ends; at most 32 outputs.
- **The layout**: `ui_format.hpp stepper_slots` picks the places from the transport (Playing, PlayingPaused, ScanForward,
  ScanReverse: VOLUME; the list's preview runs the deck too), the same places as a card without a level control
  (CassetteNoCard), so nothing else on the page moves; checked in the scenes `v1`-`v4`, `sq480-w-volume`,
  `sq720-w-volume` and the bound `x10-bound-volume` at 480x320, 320x480, 640x480, 800x480, 480x800, 480x480 and 720x720.
