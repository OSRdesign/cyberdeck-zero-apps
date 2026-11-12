# Cassette 0.3.0: release notes

Cassette 0.3.0 adds **Bluetooth audio**: recording from a Bluetooth headset, playback to Bluetooth headsets and speakers, and
the live monitor over Bluetooth, on boards with a sound server. Everything else is as in [0.2.0](RELEASE-0.2.0.md); USB
cards work exactly as before.

## What is new

- **Record from a Bluetooth headset.** Pair and connect it in the launcher (Settings > Bluetooth), then choose it in
  Cassette's Settings > Input, where it is listed after the USB cards with its rate. Bluetooth headsets record at speech
  quality: 16 kHz (mSBC) or 8 kHz (CVSD), mono. WAV, FLAC and the Opus speech presets record as usual; the Opus music
  presets are dimmed for a headset.
- **Play to Bluetooth headsets and speakers.** Settings > Output lists them with *Music quality* or *Call quality (16 kHz)*.
- **Live monitor over Bluetooth.** Monitor (off by default) can play to the headset itself or, with the new **Monitor to**
  row, to a Bluetooth speaker or another output. The delay is shown under the Monitor row; to a speaker Cassette warns
  *you will hear an echo*.
- **The headset's two modes are handled for you.** The headset is in call mode while the recorder or Settings is open, and
  back in music mode (about 2 s later) on the list, the Edit page and during playback. Cassette shows *Connecting the
  headset...* while it switches and *Switching to music mode...* before playing to it.
- **A take is never continued from another microphone.** If the headset disconnects, the take ends and is kept; when the
  headset comes back it is used again without restarting the app, as input and as output. While it is away the cassette
  window says *The Bluetooth headset is not connected.*
- **Silent headsets are caught.** A headset that connects but sends pure silence gets the notice *The headset is connected
  but sends no sound.* after 3 s.
- **Paired devices that are not connected are listed** (launcher 0.10.1 or later): dimmed in Settings > Input (headsets) and
  Settings > Output (headsets, headphones, speakers), *Not connected: connect it in Settings > Bluetooth*. A speaker or
  headphones are never offered as an input.
- **The Input footer says *The sound server is not running*** when the launcher reports PipeWire or PulseAudio as the board's
  sound stack but none answers; *Bluetooth audio is not set up on this board* stays for a board with no sound stack.
- **`cassette-probe --server`** lists what the sound server sees (sources, sinks, Bluetooth cards, their modes);
  `--card bt:<MAC>` for one device.

## What it needs

- A sound server: PipeWire with pipewire-pulse (the desktop boards, such as the HackBerry) or PulseAudio, and the client
  library `libpulse0`. The package depends on `libpulse0` (the Store installs it); it does not install a sound server.
- **Launcher 0.10.1 or later only for the dimmed paired-device rows.** Everything else works without it; with an older
  launcher those rows simply do not appear.

## Tested devices (HackBerry, PipeWire 1.4.2)

- Jabra Evolve 65: recording OK at 16 kHz mSBC; music mode OK when no other music device is connected.
- Beats Fit Pro: connects but sends no microphone audio; Cassette reports it.
- XWF-M18-M28-M38 speaker: playback OK.

## Known limits

- Bluetooth recording is speech quality only (16 or 8 kHz mono).
- Some headsets send no microphone audio at all (see above).
- Only one music (A2DP) device at a time on the HackBerry: a second one gets call quality only.
- The Lite boards (zero8, Pi 3A+, the deck) have no sound server today: Cassette says *Bluetooth audio is not set up on this
  board* and records from USB as before.
- LE Audio is not supported (our boards' radios are Bluetooth 5.0 or older).
- The delay under Monitor is Cassette's own figure and excludes the headset's own link: in call mode that link adds more
  (about 0.25 s felt with the Evolve 65 against 0.1 s shown). The estimates for a speaker (150-350 ms) are not measured.
- Board tests B32 and B34 have not been run on hardware.

More: [`USER-GUIDE.md`](USER-GUIDE.md#bluetooth), [`README.md`](../README.md), [`FORMATS.md`](FORMATS.md),
[`PULSE.md`](PULSE.md).
