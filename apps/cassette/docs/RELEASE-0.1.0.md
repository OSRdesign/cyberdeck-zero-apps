# Cassette 0.1.0: release notes

Cassette is a sound recorder with the look of a tape deck, for the CyberDeck boards. Plug in a USB sound card or a USB
headset, press REC, and the sound goes into a file in `~/Recordings`. Nothing is sent anywhere: the app has no network
code.

## What you get

- **Three formats.** WAV (the default, Broadcast Wave, one RF64 file or `_p2` parts past 4 GiB), FLAC (lossless, about a
  third to half the size) and Ogg Opus (small, made for voice). Three pickers (Codec, Sample rate, Bit depth or Bit rate)
  show the size per minute and the hours that fit; six quick presets set them at once.
- **A real level meter.** 15 steps from -40 to 0 dBFS with a TARGET zone, peak hold and a PEAK lamp; the sound card's own
  level (CARD) and software gain (SOFT, up to +24 dB); optional low cut and limiter, shown by the LIM and LOW CUT lamps.
- **Crash-safe.** A take is synced to the card every 10 s (5 to 30 s); after a power cut the next start repairs it and
  marks it `repaired`. This holds for WAV, FLAC and Opus.
- **Listen while you record.** Settings > Monitor plays the microphone into the earphones of the same card (headphones
  only; off by default). The MON lamp shows whether it runs.
- **Play, scan, preview.** Hold REW or FWD to scan at 5x, or set Jump 5 s. LOAD puts a take into the deck with the
  cassette animation; a double tap previews a take from the list.
- **Edit page: trim and split.** A scrolling waveform (TRUE or FIT scale), IN and OUT, any number of split points, and a
  save box: save as new files (the original stays) or replace the original. WAV is copied, Opus packets are copied, FLAC is
  re-encoded without loss.
- **Names and numbers.** A prefix you choose, a take number that never repeats (and can be moved: Settings > Take number),
  rename in the list, a record timer (3, 10 or 20 s).
- **Any screen from 480x320 up.** Landscape (480x320, 640x480, 800x480), portrait (320x480, 480x800, PDA mode) and square
  (720x720, 480x480) layouts. Touch or keyboard (R, Space, X, comma, period, L, S; see `docs/KEYS.md`).

## Needs

A USB sound card or headset and a launcher with full-screen app support. The Store installs the codec libraries with
the app (`libopus0`, `libopusenc0`, `libflac14` or `libflac12t64` or `libflac12`); they are opened at run time, so if one
is removed the app still records and plays WAV and says which package is missing. Your user must be in the `audio` group.
Packages for arm64 (the Raspberry Pi boards) and amd64 (x86-64 PCs).

## Seen on

The test boards: the deck and the zero8 (Pi Zero 2 W class, 640x480 and 320x480), a Pi 3A+ (480x800 portrait), a HackBerry
CM5 (720x720 square, with a sound server) and the desktop build for x86-64. Cards and headsets tried: a PCM2902 USB sound
card, the UC02 USB-C headset, a Jabra Evolve 65 (16 kHz only in its call profile), a Link 370 headset, an Apple USB-C dongle.

## Known limits

- No recording in the background: leaving the app stops the take (and keeps it).
- Opus is not offered on a card that records at 44.1 kHz only (Cassette does not resample); use FLAC or WAV there.
- No merge of takes, and no Bluetooth audio.
- A speech Opus setting (24 or 32 kbps) codes one channel; the music settings keep stereo.
- The CPU cost of FLAC and Opus on the slowest boards, and how every card behaves, are only partly measured.

More: [`README.md`](../README.md), [`USER-GUIDE.md`](USER-GUIDE.md), [`FORMATS.md`](FORMATS.md).
