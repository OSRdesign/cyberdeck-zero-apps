# Cassette (next version): release notes (draft)

Draft for the next release; the release step renames this file and sets the version. This version adds **Bluetooth**:
recording from a Bluetooth headset, playback to Bluetooth headsets and speakers, and the live monitor over Bluetooth, on
boards with a sound server. Everything else is as in [0.2.0](RELEASE-0.2.0.md); USB cards work exactly as before.

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
  headset...* while it switches (about 0.7 s) and *Switching to music mode...* before playing to it; REC while connecting
  starts the take at the first real audio.
- **A take is never continued from another microphone.** If the headset disconnects, the take ends and is kept; when the
  headset comes back it is used again without restarting the app.
- **Silent headsets are caught.** A headset that connects but sends pure silence gets the notice *The headset is connected
  but sends no sound.* after 3 s.
- **`cassette-probe --server`** lists what the sound server sees (sources, sinks, Bluetooth cards, their modes);
  `--card bt:<MAC>` for one device.
- The echo line under Monitor is drawn in a darker amber so it can be read on the cream rows.

## Requirements

- A sound server: PipeWire with pipewire-pulse (the desktop boards, such as the HackBerry) or PulseAudio. The package now
  depends on `libpulse0` (the client library, opened at run time); it does not install a sound server.
- The Lite boards (zero8, Pi 3A+, the deck) have no sound server today: Cassette says *Bluetooth audio is not set up on this
  board* and records from USB as before.

## Tested devices (HackBerry, PipeWire 1.4.2)

- Jabra Evolve 65: recording OK at 16 kHz mSBC; music mode OK when no other music device is connected.
- Beats Fit Pro: connects but sends no microphone audio; Cassette reports it.
- XWF-M18-M28-M38 speaker: playback OK.

## Known limits

- Bluetooth recording is speech quality only (16 or 8 kHz mono).
- Some headsets send no microphone audio at all (see above).
- Only one music (A2DP) device at a time on the HackBerry: the second one gets call quality only.
- LE Audio is not supported (our boards' radios are Bluetooth 5.0 or older).
- The monitor delays over Bluetooth (estimated 60-150 ms on the same headset, 150-350 ms to a speaker) are not yet measured
  on a board. The limits of 0.2.0 otherwise stand.

More: [`USER-GUIDE.md`](USER-GUIDE.md#bluetooth), [`README.md`](../README.md), [`FORMATS.md`](FORMATS.md),
[`PULSE.md`](PULSE.md).
