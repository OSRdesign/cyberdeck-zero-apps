# Cassette: decisions recorded with the code

The owner's decisions and the worker's calls that the code follows, newest first. The study and the earlier decisions
(D1 to D19, the recorder screen, the CARD and SOFT rule) are in the launcher repository's report 058.

## 0.4.1 published (2026-11-21)

The owner tested draft.4 on the Panasonic FZ-M1 and it works well: the MIC BOOST stepper on the recorder steps the boost and the
meter follows (board test B45). Published as 0.4.1 with the draft.4 binaries. Still open: the card's own level (`Capture Volume`)
did not respond on the tablet through its PulseAudio (the pactl path of draft.2 and the draft.3 check); the cause is not found
(board test B46). That is why the recorder shows the boost in CARD's slot for a card that has one; the pactl path, the direct
fallback and the journal lines ship unchanged, so a later look on the tablet has the logs it needs.

## Draft.4: the Mic boost takes CARD's slot on the recorder (owner, 2026-11-21)

After draft.3 on the tablet the card's level still did not move (cause unknown) but the Mic boost worked. Owner decision: on the
recorder screen, for a card that has a Mic boost control, the CARD stepper is replaced by a **MIC BOOST** stepper in the same slot
and layout (landscape 480x320 and 640x480, portrait 320x480, square 480x480 and 720x720); the card's level stays reachable in
Settings > Input level (and Settings > Mic boost is the same boost as a picker).

- **What shows where**: caption `MIC BOOST` (the stepper's own caption fitting, the same face and size rule as CARD and SOFT), value
  `Off` / `+10 dB` / `+20 dB` / `+30 dB` (the control's own steps; `Step n` where the driver gives no dB), minus and plus one step a
  press, minus dimmed at Off, plus at the top step. SOFT stays on its side; while a take plays the slot is hidden and SOFT shows
  VOLUME, as before. Which boost: the input in use (Internal Mic, or the Headset Mic when the jack switches), as in draft.1.
- **When**: only when the card has a boost and its current value is one of its steps (`CardSnap::boost_in_card_slot()`); a card
  without a boost, or with a boost value that is none of the steps, keeps the CARD stepper with its level exactly as before (the
  level in dB, 1 dB a press). A boost without a level control still shows the stepper.
- **Saved per card** as before (`boost_db`, written again at start); the stepper writes through the same `set_mic_boost` the
  Settings picker uses. The level is not touched by the stepper.
- **Draft.3's pulse path and log lines stay** (the level in Settings still goes through the sound server's source, the journal
  still shows `card gain asked by the UI` and `asked V / source now V`).
- Changed from draft.3: the stepper no longer shows the card total (level + boost); the Settings row still says
  `Total ... with the Mic boost`. Scenes: `bo2-recorder-boost` (boost in the slot, - steps it, three layouts), `bo3-recorder-noboost`
  (CARD as before), `sq480-bo2-recorder-boost`, `sq720-bo2-recorder-boost`; unit tests in `test_map.cpp` (`test_mic_boost`) and
  `test_binder.cpp` (the stepper's presses).

## Draft.3: "still stuck" after draft.2 (owner's tablet journal, 2026-11-21)

The tablet journal showed every `card mixer level set` line at raw 0 (-17.25 dB), also with draft.2's `via sound server source`.
What that proves and what it does not: the `level set` line prints the card's value READ BACK after the write (`mstate`); the value
that was ASKED appears only when the write failed (`asked ... error`). So raw 0 in every line means "the card is at 0 afterwards", not
"the app asked for 0". The path from the UI to the mixer was traced and driven in tests with the tablet's controls
(`t_alc256_gain`, `t_alc256_through_the_server`, `test_binder` card_gain / setting_slider): slider positions 0..47 and the CARD
stepper (each press tries 1, 2, 3 ... dB until the card moves, so a press that does not move logs up to 12 `level set` lines: that is
the "dozens" in the journal) all reach `Mixer::set_level_db` with the right value. Nothing writes the level except an explicit
UI action, the saved value at a card open (`reapply`) and nothing re-asserts on a tick; the 1.5 s poll only READS (a changed
value is shown and logged `changed outside`, never written back); the saved `level_db` is the read-back at the last change, so a
card that stays at 0 saves -17.25. No gain row is disabled by the boost, auto gain or mute state.

- **A real defect found and fixed**: draft.2 counted a server call as "set" when the card landed within a step and a half of the wanted
  step, so a server that rounds every request one step down left the card a step short and the stepper looked dead. Now only the
  wanted step counts (the exact request, then a quarter and 0.6 step towards it); anything else gives the server up for the card
  and writes ALSA directly, as before draft.2. Regression: `tests/alsa/test_server_gain.cpp` `a_server_one_step_short` (fails
  with the old tolerance), `tests/app/test_settings.cpp` `t_alc256_through_the_server` (Controller, three server behaviours).
- **Not found in the app**: why the tablet card stays at 0 after a server call that reports success is not settled by the code. Two
  candidates for the board check: PulseAudio maps the source volume to the card in a way other than the formula (for example it
  merges `Capture` and a boost into one range, or applies the volume only when the source runs), or the recording stream's own
  volume (flat volumes) pulls the source back. Draft.3 therefore logs what is needed to tell: `[cassette] card gain asked by the
  UI: <card was> dB -> <asked> dB (card now <x> dB)` for every UI change, and on the server path `[asked V <n>, source now V <m>
  (<g> dB from its base)]` (the volume the server has after the call, read with `pactl list sources`), next to the boost raw.
  If `asked V` is not what `source now V` shows, the server rewrote it; if it is the same and the card is still at 0, the server
  accepted the volume and did not apply it to this control.

## Card gain through the sound server (draft.2, 2026-11-21)

Found on the tablet after draft.1: the sound server is real PulseAudio (`pactl info` says `Server Name: pulseaudio`; no PipeWire).
PulseAudio owns the ALSA `Capture` element and had the source `alsa_input.pci-0000_00_1f.3.analog-stereo` saved at 5 % (= ALSA
Capture 0 = -17.25 dB; base volume 10 % = -60 dB), so every direct ALSA write was reset. `pactl set-source-volume ... 100%` moved
ALSA Capture to 63 (+30 dB) at once. So the level is set where the owner of the control listens.

- **When**: at the card's open Cassette asks `pactl info` and `pactl list sources` (LC_ALL=C) and takes the source of the card
  (`alsa.card`, else `api.alsa.card`, else the N of `api.alsa.path` equals the card's index of this boot; the default source when
  it is that card's, else the card's first non-monitor source). The runner starts pactl only when a server socket exists
  (`PULSE_SERVER`, or `$XDG_RUNTIME_DIR/pulse/native`): pactl would otherwise autospawn a PulseAudio daemon next to PipeWire or on
  a console board. No socket, no pactl, no source for the card, a pactl that fails or takes over 2 s: the ALSA path, exactly as
  before (Pi, zero8 and the other console boards).
- **Mapping**: the source volume is a `pa_volume_t` V (0x10000 = 100 % = 0 dB, dB = 60 log10(V / 0x10000), what pactl prints as
  `3277 / 5% / -78.00 dB`). `Base Volume` B is the V at which the hardware gain is 0 dB, so hardware gain G = dB(V) - dB(B) and
  for a wanted card gain G: **V = B * 10^(G / 60)**, written as a plain integer (`pactl set-source-volume <source> 3380`: no sign, no
  locale, no percent rounding). On the tablet B = 6554: the card's -17.25 .. +30 dB is V = 3380 .. 20726 (5.2 % .. 31.6 %) and the
  whole Settings slider and CARD stepper work. A source without a base volume uses 0x10000. (Checked against the tablet's own
  figures: 5 % = -78 dB, -78 + 60 = -18 dB, below the card's -17.25 = Capture 0.)
- **Read-back**: the value shown is read from the ALSA control after the pactl call (what the server really wrote to the
  hardware), never the requested one. If a step is not exactly reached (a server rounding down), one more call a quarter step
  towards the wanted one; within 1.5 steps it counts as set. If it is not (a server that acknowledges but does not manage the
  control), the server is not used again for this card and the card is written directly, as before; the journal line then says
  so.
- **Journal**: `[cassette] card mixer level set: hw:CARD=PCH 'Capture Volume' raw N (x dB) via sound server source '<name>'`;
  on a fallback `(the sound server did not set the card to X dB (the card has Y dB); the card is written directly)` or `(the
  sound server did not take the level (pactl failed) ...)`; `level NOT set` only when the direct write fails too; `changed
  outside` as before (the 1.5 s poll reads the ALSA control, no pactl).
- **Not moved to the server**: Mic boost, capture switch and auto gain stay on ALSA (not verified on the tablet whether PulseAudio
  keeps its own saved state for `Internal Mic Boost`; the 1.5 s poll and the journal show it if it does).
- **Why pactl and not libpulse**: the libpulse layer here is owned by the Bluetooth worker (its connection, callbacks and timing
  rules); a second client for one set per click is a few lines through pactl, with the NOAUTOSPAWN guard replaced by the socket
  check. The cost is one process per level change (tens of ms).
- Untested on a real PipeWire board: the formula follows the PulseAudio base volume; if pipewire-pulse maps differently the
  read-back check falls back to ALSA, as it does today.

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
  PulseAudio, not PipeWire, as found afterwards: see the next section).
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
