# Cassette: recording formats (FLAC and Ogg Opus next to WAV)

Design note for the codec layer (phase A, 2026-11-10). Decisions it follows: report 058 D3 (WAV/BWF stays the
default), D4 (FLAC and Opus as options), D7 (libraries loaded at run time with `dlopen`, no link-time dependency), D11
(the card's native rate, no resampling in 0.1), D16 (Vorbis comments for metadata).

**Status (2026-11-10).** Phase A (the encoders, the library loading, the format list with its size estimates, the
stored key), phase B (recording: the writer, names, the list, the journal, the repair) and phase C (playback, preview,
the 5x scan and the 5 s jump, the peak cache, trim and split of FLAC and Opus takes) are done. The recorder writes
FLAC and Ogg Opus takes when the stored setting asks for it and `effective_choice()` allows it. The Settings
Format row (the owner's design, 2026-11-10, branch `cassette-fmtui`) listed the six entries below; since 2026-11-10
(branch `cassette-r7`) the format is **three settings**, Codec, Sample rate and Quality, each with a picker of its own
(the six entries are now the Codec picker's quick presets). Entries the card or the libraries cannot record stay in
the lists, dimmed, with the model's reason; Opus is dimmed (never resampled) on a card with no rate it can record. See
"The Codec, Sample rate and Quality pickers" below. A
FLAC / Opus take whose decoder library is missing is refused with a notice. Code: `src/core/codec/`,
`src/core/writer.*`, `src/core/repair.*`, `src/core/takes/`, `src/core/playback/player.*`, `src/core/edit/`; tests:
`tests/codec/`, `tests/test_stream_scan.cpp`, `tests/test_codec_writer.cpp`, `tests/test_codec_crash.cpp`,
`tests/takes/test_formats.cpp`, `tests/app/test_formats.cpp`, `tests/codec/test_writer_real.cpp`, and for phase C
`tests/codec/test_decoders.cpp`, `tests/codec/test_decoders_real.cpp`, `tests/playback/test_codec_play.cpp`,
`tests/edit/test_codec_edit.cpp`, `tests/fake_decoders.hpp`. Board tests: B17 (recording), B18 (playback and
editing) and B19 (the pickers) in `tests/BOARD-TESTS.md`. Merging takes of one format (2026-11-11): see
[Merging takes](#merging-takes) at the end, board test B23.

## What is offered

The Codec picker lists every entry below in this order (as its quick presets, after the three codecs), each with a name, a one-line detail (the wording
here is a placeholder for the owner), whether it works with the selected card (and a short reason when not) and an
estimated size per minute at the card's current capture format.

| id | settings.json | Name | Detail | Encoder |
|---|---|---|---|---|
| `wav` | `"format": "wav"` | WAV | uncompressed, plays everywhere | BWF header + PCM (as today) |
| `flac` | `"format": "flac"` | FLAC | lossless, a third to half the size | libFLAC level 5, streamable subset |
| `opus-speech24` | `"format": "opus", "format_preset": "speech24"` | Opus 24 kbps | speech, smallest | VOIP mode, voice signal, one coded channel |
| `opus-speech32` | `"format": "opus", "format_preset": "speech32"` | Opus 32 kbps | speech, clear | VOIP mode, voice signal, one coded channel |
| `opus-music64` | `"format": "opus", "format_preset": "music64"` | Opus 64 kbps | music and voice | AUDIO mode, music signal |
| `opus-music128` | `"format": "opus", "format_preset": "music128"` | Opus 128 kbps | music, best | AUDIO mode, music signal |

The six entries are **presets** of the three-parameter model (next section): each is one combination of codec and quality
at the card's default rate and depth.

`format_preset` is a new, optional key. Absent or unknown means the format's default (Opus: `speech32`; FLAC and WAV
have one entry), so every settings file written before keeps its meaning, and older app versions keep the key on
their next write (unknown keys are preserved). An empty preset is not written.

### The three parameters

The model (`src/core/codec/formats.*`) separates what the user chooses into a codec, a sample rate and a quality:

| Parameter | settings.json | Values | Default when absent or 0 |
|---|---|---|---|
| codec | `"format"` | `wav`, `flac`, `opus` | `wav` |
| sample rate | `"rate"` (Hz) | any rate the card offers and the codec can record | WAV and FLAC: the card's own (`choose_rate`: 48 kHz, then 44.1, then the highest below 48, else the lowest); Opus: 48 kHz, then 16, then the highest of 24, 12, 8 |
| quality | `"quality"` | WAV and FLAC: bit depth 16 or 24; Opus: 16, 24, 32, 48, 64, 96 or 128 kbps | 16-bit (24 when the card has no 16); Opus: the preset's bit rate, else 32 kbps |

- **Old files load unchanged.** `"format"` and `"format_preset"` keep their meaning: a file with only those records at the card's
  default rate and depth and the preset's bit rate, exactly as before. `"rate"` and `"quality"` are written only when
  non-zero; a value out of range or of the wrong type is read as 0. `quality` holds a bit depth or a bit rate depending on the
  codec (one key, as the owner asked), so a change between Opus and WAV / FLAC puts it back to 0; between WAV and FLAC the
  bit depth stays.
- **Presets.** `set_format_choice(format, preset)` stores the codec and the preset and sets rate and quality to 0: the
  combination of the preset on whatever card is there. A plain bit rate (the Bit rate row) clears the preset and sets `quality`;
  the effective bit rate is `quality`, else the preset's, else 32 kbps.
- **Opus modes by bit rate.** Up to 32 kbps: speech (VOIP mode, one coded channel on a stereo card); 48 kbps and up: music
  (AUDIO mode, stereo kept). This reproduces the presets (24 and 32: speech, 64 and 128: music); 16 is speech, 48 and 96 music.
  The journal's `preset` is the preset whose bit rate it is ("" for 16, 48 and 96); `bitrate` is the bit rate in bit/s and
  reaches libopusenc (`OPUS_SET_BITRATE`) for every value of the list.
- **Legal rates.** Opus: 8, 12, 16, 24, 48 kHz and at most 2 channels. WAV and FLAC: any rate the card offers (FLAC's own
  limits aside). A card whose offer has no Opus-legal rate has Opus **dimmed** with the reason `Opus needs 8, 12, 16, 24 or 48
  kHz; this card has 44.1 kHz`; nothing is resampled. The stored choice is kept and `effective_for()` records WAV at the card's
  default rate instead (the safety net: it is never an illegal state on the screen, but a settings file or a card swap can reach it).
- **A change of codec re-validates the other two** (`change_codec`): if the current rate is not legal for the new codec the
  nearest legal one is stored (Opus: 48 kHz, then 16 kHz, then the nearest of the rest) and the page says so in one line
  (`Sample rate set to 48 kHz: Opus cannot record at 44.1 kHz`). The bit depth is not applicable to Opus: it stays in the
  settings only as far as the one `quality` key can hold it (see above) and the Opus row shows the bit rate instead.
- **The capture follows the choice.** `OpenOptions::want_rate` / `want_bits` (filled by a callback once the card's offer is known)
  make `choose_params` open the card at the chosen rate (when offered) and depth (24: packed `S24_3LE`, else `S32_LE` converted;
  16: `S16_LE`). Choosing a rate or a depth in Settings reopens the capture at once (never while a take or the countdown runs); a
  card that refuses what it offered is opened with its own defaults and a notice says so. A card busy and recorded through the
  sound server stays at 48 kHz 16-bit: the model then knows only that rate.

### Why these four Opus presets

The four bitrates sit on the sweet spots of the Opus specification (RFC 6716, section 2.1.1: "16-20 kbit/s for WB
speech", "28-40 kbit/s for FB speech", "48-64 kbit/s for FB mono music", "64-128 kbit/s for FB stereo music") and cover
the uses of a pocket recorder:

- **24 kbps speech**: long voice notes and meetings at the smallest size, 11 MB per hour (8 GB holds about 720 hours).
  Above the wideband speech range, so voices stay natural. P0-e: 10x real time on the zero8, 10 % of a core.
- **32 kbps speech**: in the fullband speech range; the default when Opus is chosen (interviews, lectures). 15 MB/h.
- **64 kbps music**: fullband mono music and the bottom of stereo music; also a "safe" choice for mixed material
  (voice with music, field recordings). 29 MB/h.
- **128 kbps music**: the top of the stereo music range, near transparent; rehearsals and concerts. 58 MB/h.

Speech presets set `OPUS_SET_FORCE_CHANNELS(1)`: on a stereo card the encoder codes one channel (no downmix code in
Cassette; the file still says two channels), which is where low bitrates belong. Music presets keep stereo. Not
offered: a 16 kbps preset (narrower than 24 for a small saving), 96 kbps (between two presets that already bracket
it), CBR (VBR gives better quality at the same average).

## Which entries a card allows

Input: the capture format the recorder will use (`open_policy`'s `choose_params()` on the card's offer: rate 48000,
else 44100, else the highest at or below 48000; mono or stereo; S16, else 24-bit packed) and which libraries load.

| Entry | Needs | Reason shown when not |
|---|---|---|
| WAV | any format the open policy accepts | (the card's own reason) |
| FLAC | 16 or 24 bits, 1 or 2 channels; libFLAC | "more than 2 channels", "FLAC not installed (libflac14)" |
| Opus | rate 8, 12, 16, 24 or 48 kHz; 1 or 2 channels; libopusenc | "Opus cannot record at 44.1 kHz", "Opus records at most 2 channels", "Opus not installed (libopusenc0)" |

The card's reason comes first (installing a library would not help). A card the open policy cannot record from (an
8-channel-only interface) makes every entry incompatible with the policy's sentence ("the card offers no mono or stereo
capture"). Without a card the list shows compatibility by library only and no sizes. The recorder uses the stored
choice when it is compatible, else WAV (`effective_choice()`).

Examples (tests/codec/test_formats.cpp): a mono 48 kHz headset allows all six; a stereo 44.1 kHz-only card allows WAV
and FLAC; a 16 kHz speech headset allows all six (16 kHz is an Opus rate); a 24-bit card allows all six; an 8-channel
card offering 1..8 channels records stereo and allows all six.

### A Bluetooth headset as the input (8 and 16 kHz)

A Bluetooth headset records through its call link only, at the rate of its codec: **16000 Hz** (mSBC, wideband) or
**8000 Hz** (CVSD, narrowband), always **mono, 16-bit** (the controller asks the sound server for S16 mono at that rate:
docs/PULSE.md, "What T2 uses"). The Sample rate row has that one rate. What that means for the formats:

| Format | With a Bluetooth headset |
|---|---|
| WAV | as usual, at 8 or 16 kHz mono 16-bit (16 kHz: 1.92 MB per minute, exact) |
| FLAC | as usual, lossless at 8 or 16 kHz |
| Opus speech 24 / 32 kbps (and 16 kbps) | offered, listed first: 8 and 16 kHz are Opus rates |
| Opus music 64 / 128 kbps (and 48, 96 kbps) | **dimmed** with "Bluetooth call audio is speech only: Opus up to 32 kbps" (`FormatInputs::speech_only`): a music bit rate would only spend bytes on a 16 kHz mono call signal |

A stored music preset falls back to WAV like any choice the input cannot record (the stored choice is kept and comes back
with a USB card). No resampling: a Bluetooth take is 8 or 16 kHz in the file. Playback of any take to a Bluetooth output
goes through the sound server, which resamples to the device (the take's own rate is sent).

Note on D11: libopusenc always codes at 48 kHz and upsamples any other input rate inside the library (speex resampler,
quality 5; `opusenc.c`). So 8-24 kHz cards already go through a resampler, inside libopusenc. Allowing 44.1 kHz cards
the same way would be a one-line change (`opus_rate_ok`) if the owner wants it; the D11 rule is kept as decided.

## Size model

`SizeEstimate`: bytes per minute of stream after the file's one-off header, a low..high range, the header bytes, and
a kind: **Exact** (WAV), **Target** (Opus), **About** (FLAC), Unknown (no card).

- **WAV**: rate x channels x bytes x 60, the same `bytes_per_hour` the recorder's time left uses; header 5120 bytes
  per file (per part with auto-split). 48 kHz mono 16-bit: 5,760,000 B/min.
- **Opus**: bitrate / 8 x 60 plus the Ogg overhead: 20 ms packets need one lacing byte per started 255 bytes, and a
  page (27-byte header) closes at least every second. 24 kbps: 184,620 B/min; 32: 244,620; 64: 484,620; 128: 967,620.
  Header about 700 bytes (OpusHead page, OpusTags page with 512 bytes of padding). Measured with libopus 1.5.2 /
  libopusenc 0.2.1 (cassette-codec-measure): the music presets land at 1.00-1.01 of the figure on every signal; the
  speech presets (VBR, voice mode) at 0.73-0.95 on real recordings with pauses and at 0.90-1.01 on continuous
  synthetic signals. So the figure is a reliable upper bound.
- **FLAC**: PCM x ratio, shown as "about". Ratio for 16-bit: 0.30 typical (speech), range up to 0.55 (music). 24-bit:
  the low 8 bits of a real card are noise and FLAC stores them nearly verbatim, so ratio24 = (ratio16 x 16 + 8) / 24
  (0.53 for speech, 0.70 for music). An observed ratio (the running take's, phase B) replaces the typical value and
  widens the range when outside it. Header about 4400 bytes (STREAMINFO, comments, 4096 bytes of padding).
- **Hours for the free space**: free bytes / (bytes per minute x 60), header ignored, as `hours_left()` does; for WAV it
  is the same number.

### FLAC ratios measured

libFLAC 1.5.0 (and 1.4.3: same results), level 5, through `FlacEncoder`. Ratio = FLAC stream after the header / PCM.

Real recordings from P0 on the zero8 (48 kHz mono 16-bit):

| Recording | Content | Level 0 | Level 5 | Level 8 |
|---|---|---|---|---|
| powercut/B.wav (UC02 headset, level 70) | speech, 60.3 s | 28.2 % | **27.0 %** | 27.0 % |
| powercut/U.wav (UC02 headset, level 127) | speech, 22.8 s | 33.3 % | 30.5 % | 29.9 % |
| powercut/A.wav (PCM2902 card) | 30.4 s | 35.9 % | 35.3 % | 35.3 % |
| wav/n48-v0-agcoff.wav (PCM2902) | quiet room, noise floor -70.6 dBFS | 33.8 % | 33.4 % | 33.4 % |
| wav/n48-v16-agcon.wav (PCM2902, +23.8 dB) | quiet room, -65.4 dBFS | 38.7 % | 37.7 % | 37.7 % |
| wav/n44-v12-agcon.wav (44.1 kHz) | quiet room, -64.2 dBFS | 41.2 % | 40.8 % | 40.8 % |
| wav/long48-p1024.wav (PCM2902) | 300 s | 36.3 % | 35.8 % | 35.8 % |

B.wav is the P0-e input: 27.0 % here equals the 27 % the `flac` 1.5.0 tool gave on the board, which also confirms the
encoder settings.

Synthetic signals (tests/codec/siggen.hpp: formant speech with pauses, a chord progression with bass and hi-hat), level
5, by noise floor:

| Capture format | Content | -80 dBFS | -70 dBFS | -60 dBFS |
|---|---|---|---|---|
| 48 kHz mono 16-bit | speech | 33.3 % | 40.4 % | 48.5 % |
| 48 kHz mono 16-bit | music | 49.7 % | 52.2 % | 55.9 % |
| 48 kHz stereo 16-bit | speech / music | 30.2 / 46.6 % | 38.2 / 49.6 % | 47.2 / 54.3 % |
| 44.1 kHz stereo 16-bit | speech / music | 30.6 / 47.1 % | 38.5 / 50.1 % | 47.4 / 54.6 % |
| 16 kHz mono 16-bit | speech / music | 41.2 / 59.7 % | 45.7 / 60.2 % | 51.9 / 62.3 % |
| 48 kHz mono 24-bit | speech / music | 55.5 / 66.5 % | 60.3 / 68.1 % | 65.7 / 70.6 % |
| 48 kHz stereo 24-bit | speech / music | 53.4 / 64.4 % | 58.8 / 66.5 % | 64.8 / 69.6 % |

Reading: the noise floor of the card decides most of it (each 10 dB of noise costs about 1.6 bits per sample);
level 0 vs 8 differs by 1-2 points; the 24-bit rule predicts the 24-bit rows exactly (speech 40.4 % -> 60.3 %, music
52.2 % -> 68.1 %). Spread for 16-bit at 44.1/48 kHz: about 27-41 % on real speech and room sound, 47-56 % on synthetic
music (real music with cymbals and room noise will reach 60-70 %). At 16 kHz the ratio is 5-10 points higher (less
redundancy between samples); the model ignores that, the running ratio corrects it.

## The encoders

`Encoder` (encoder.hpp): `start(sink, tags)`, `write(pcm, frames)` with any block size in the capture format,
`flush()`, `finish()`, `stats()` (frames in, frames in the sink, bytes, pages or frames written, the sticky first error,
`pending_frames()` = what a power cut would lose). Output goes to a `ByteSink`; `FileByteSink` writes with `pwrite`
through `FileOps` on the writer's own descriptor, so the writer keeps calling `fdatasync` on its timer as for WAV, and
the fault-injecting test FileOps work unchanged. Errors: the sink's `-errno` pass through (ENOSPC, EIO), plus
`kCodecMissing`, `kCodecRefused`, `kCodecFailed`, `kCodecState`.

| | FLAC (`FlacEncoder`) | Ogg Opus (`OpusEncoder`) |
|---|---|---|
| Library calls | stream encoder with write, seek, tell callbacks | libopusenc **pull** API: pages are taken with `ope_encoder_get_page` |
| Settings | level 5, streamable subset, verify off, 16/24-bit, 1-2 channels | bitrate, VBR, complexity 10, 20 ms frames, VOIP/AUDIO + voice/music signal, force mono for speech |
| Header at `start()` | `fLaC`, STREAMINFO (totals 0 = unknown), Vorbis comments, 4096 bytes of padding | OpusHead and OpusTags pages (512 bytes of comment padding) |
| During `write()` | a frame is written whenever a block (4096 samples) is complete | complete pages are written (a page closes after at most 1 s of audio) |
| `flush()` | nothing more can leave: the partial block stays until `finish()` | closes the page being filled, so every encoded packet is in the sink |
| Not in the sink after `flush()` | under 4096 frames (85 ms at 48 kHz) | libopusenc's decision delay: 2 s by default (`decision_delay_ms`), measured 2.03 s; 0 ms gives under 30 ms |
| `finish()` | last frame; seeks back and rewrites STREAMINFO (samples, MD5, frame sizes) | drains the 2 s, writes the end-of-stream page (granule = exact length + pre-skip) |
| Metadata | Vorbis comments, padding for in-place edits | OpusTags (Vorbis comments) |

The libopusenc defaults are kept (decision delay 2 s, muxing delay 1 s): a power cut loses the sync interval plus up
to 2 s with Opus. `decision_delay_ms` can be lowered in phase B after a listening check (in libopusenc 0.2.1's source the
delay serves the decisions at stream ends and chaining, which Cassette does not use; to confirm by listening). Stopping costs the encoding of up to 2 s of audio (P0-e:
10-13x real time on the zero8, so about 200 ms) within the 3 s the launcher allows.

Why the pull API: the callback API cannot close a page on demand (pages leave only by the muxing delay), so a sync
would leave up to 1 s more audio behind. The pull API is the same library, same pages.

## Libraries and packages

| Library | File loaded | Debian / Ubuntu package | Needed for |
|---|---|---|---|
| libFLAC | `libFLAC.so.14`, else `libFLAC.so.12` | `libflac14` (trixie, Ubuntu 26.04 likely), `libflac12t64` (Ubuntu 24.04), `libflac12` (bookworm) | FLAC recording, playback, peaks, trim and split |
| libopusenc | `libopusenc.so.0` | `libopusenc0` (0.2.1 in trixie) | Opus recording |
| libopus | `libopus.so.0` (phase C loads it for playback; libopusenc needs it for recording) | `libopus0` | Opus recording (through libopusenc), playback, preview, peaks |
| libogg | (needed by libFLAC: Debian links libFLAC with Ogg support) | `libogg0` | pulled in by the libflac package; Cassette itself does not use it |
| libopusfile | not used | (`libopusfile0`) | phase A planned it for playback; phase C reads the Ogg pages itself (below) |

The encoder calls are the same in FLAC 1.4 and 1.5 (the soname moved for decoder changes); both were tested. The
decoder calls Cassette uses are the same too: FLAC 1.5 only appended enum values (chained Ogg FLAC) that native FLAC
never produces; checked against both header sets (`check_codec_abi.cpp`).

**What each format needs at run time (phase C):**

| | Record | Play, preview, scan, peaks | Trim and split | Merge |
|---|---|---|---|---|
| WAV | nothing | nothing | nothing | nothing |
| FLAC | libFLAC | libFLAC | libFLAC (decode and encode) | libFLAC (decode and encode) |
| Ogg Opus | libopusenc + libopus | libopus | nothing (a copy of packets) | nothing (a copy of packets) |

**In the package (owner, 2026-11-11): the codec libraries are Depends**, so the Store (and apt) installs them with
Cassette:

```
"depends": "libstdc++6, libgcc-s1, libfreetype6, libinput10, libxkbcommon0, libasound2t64 | libasound2, libopus0, libopusenc0, libflac14 | libflac12t64 | libflac12, libpulse0"
```

`libpulse0` (the Bluetooth release, 2026-11-11) is the sound server's client library, for Bluetooth devices through
PipeWire (pipewire-pulse) or PulseAudio. It is opened at run time like the codecs (`src/core/pulse/pulse_lib.cpp`, never
linked) and only when a sound server runs; without it, or without a server, Cassette records from USB cards as before and
says "Bluetooth audio is not set up on this board". The name is `libpulse0` in Debian bookworm (16.1), trixie (17.0) and
Ubuntu 24.04 (1:16.1) (checked 2026-11-11 on packages.debian.org and with `apt-cache policy` on Ubuntu 24.04). It pulls
in a few small libraries (libsndfile1, libasyncns0, ...), not a sound server: Cassette does not depend on PipeWire.

The libFLAC alternatives cover Debian trixie (`libflac14`), Ubuntu 24.04 (`libflac12t64`) and Debian bookworm
(`libflac12`); `libopus0` and `libopusenc0` have these names in all three (checked 2026-11-11: trixie on the zero8 and
the HackBerry with `apt-cache policy`, noble and bookworm on packages.ubuntu.com / packages.debian.org). Installed
sizes in trixie: libflac14 479 KB, libopusenc0 70 KB, libopus0 3.5 MB (libopus 1.5 carries its neural models), libogg0
55 KB. With missing Depends the launcher's Store goes through `apt-get install` (`appstore_native.cpp`), which needs the
network.

The binary still does not link them: it opens them at run time (`dlopen`), so the app keeps working if someone removes
them: WAV records and plays, the FLAC and Opus rows are dimmed with the reason (the package to install), and a FLAC /
Opus take whose library is missing is refused by LOAD, PLAY, the preview and EDIT with one notice (`This take cannot be
played: FLAC not installed (libflac14)`, `... Opus not installed (libopus0)`). Phase C added nothing to that list
(phase A expected `libopusfile0`; it is not needed: `libopus0` plays Opus, and `libopusenc0` already depends on it).

Which boards already carry the libraries: with the package's Depends the Store installs them with Cassette (read-only
probe to check a board: `ldconfig -p | grep -E 'libFLAC|libopus|libogg'` and `dpkg-query -W 'libflac*' 'libopus*' 'libogg*'`).

## ABI and tests

- `codec_abi.hpp` declares the C ABI by hand; `check_codec_abi.cpp` compares it with real headers when present
  (constants, the structures used, and the shape of every function and callback) and does not compile on a
  mismatch. Checked against FLAC 1.4.3 + libopus 1.4 + libopusenc 0.2.1 (Ubuntu 24.04 packages) and FLAC 1.5.0 +
  libopus 1.5.2 + libopusenc 0.3 (Debian packages). arm64 headers were not checked (same C ABI rules; the layouts are
  plain `uint32_t` and pointers). Phase C added libFLAC's stream decoder (its states and statuses, the `FrameHeader`
  layout field by field, the eight callbacks, the ten functions) and libopus' decoder (its constants and five functions).
- Fakes (`tests/codec/fake_codecs.*`) behave like the libraries where the encoders depend on it: block and page
  boundaries, the decision delay, the STREAMINFO rewrite, end of stream, errors; the encoders' conversion, settings,
  statistics and error paths are tested on them.
- The real-library group runs the same encoders on libFLAC and libopusenc when they load and parses the output
  (STREAMINFO, comments, Ogg CRCs, OpusHead/OpusTags, granule positions), including a copy taken after `flush()` (what
  a power cut leaves). Skipped, with a line, when the libraries are missing.
- No install is needed on the build box: a private root works. For example
  `apt-get download libflac-dev libflac12t64 libopus-dev libopus0 libopusenc-dev libopusenc0 libogg0` and
  `dpkg-deb -x <each>.deb <root>`, then
  `CODEC_INCLUDES=<root>/usr/include CODEC_LIBPATHS=<root>/usr/lib/x86_64-linux-gnu wsl -e sh tests/codec/run.sh`.
  `CODEC_MEASURE=1` adds the synthetic measurement table; `cassette-codec-measure FILE.wav ...` (built by the same
  Makefile) measures real files.

## Phase B: recording (done)

**Which format a take gets.** At REC the controller builds the format list for the capture format the card was opened
with and the libraries found (`codec_availability_here()`, loaded only when the stored format is not WAV) and takes
`effective_choice()` of the stored key: the stored choice when compatible, else WAV. The recorder's format text
(`48k/16 FLAC`, `48k/16 stereo OPUS`; the info box beside the counter shows it as `FLAC 48k`, and for Opus as
`OPUS 48k · 32 kbps` with the choice's bit rate, `TakeSnap::opus_kbps`) and the hours left (from the choice's size estimate) follow the same choice,
recomputed with the free space every 2 s. Encoders: `FlacEncoder` level 5, or `OpusEncoder` with the preset's bitrate,
application and, for the speech presets on a stereo card, one coded channel (`OPUS_SET_FORCE_CHANNELS(1)`). Tags
(Vorbis comments): `TITLE` (the part's file name without extension), `DATE` (only with a set clock, D15), `ENCODER=Cassette`.

**Writer** (`WriterConfig::make_encoder`, null = WAV exactly as before). Each part gets a fresh encoder writing through a
`FileByteSink` on the writer's descriptor. The ring, the block size, the flush time and the sync cadence are the WAV
ones; at each sync the writer calls `Encoder::flush()` and then `fdatasync`, and counts as synced the encoder's
`frames_out` (whole FLAC frames; Opus: the granule of the last page, so libopusenc's 2 s decision delay is never
counted as safe). Stop: `finish()` (FLAC rewrites STREAMINFO with the totals and the MD5; Opus drains and writes the
end-of-stream page), then `fdatasync`. No header patches, no RF64. Auto-split (same setting, same `split_bytes`): a part
ends before its file could pass `split_bytes` (the check before each block counts the file, the PCM size of what the
encoder holds and of the block, and `split_slack`, 256 KB, for frame and page headers); the next part `_p2`, `_p3`
... is a new stream with its own encoder and headers. Disk full, EIO, a failing `fdatasync`: the take stops as for WAV
(the same reasons and notices); the encoder is dropped without `finish()` and the file is mended in place at once
(the cut frame or page removed, STREAMINFO / end of stream set), so even an "error" take is a valid file. The encoder
runs on the writer thread; the capture thread only writes into the ring (a 20 ms-per-call encoder never made a ring
write wait: 2-4 us worst).

**Journal.** A compressed take's journal has `"codec": {"format": "flac"|"opus", "preset", "bitrate", "force_mono",
"encoder"}` next to the capture format; a WAV journal is unchanged (no `codec` = WAV).

**Names and the list.** `<prefix>_<take>_<when>[_pN][-N].wav|flac|opus` (`TakeName::format`, WAV by default). Take
numbers are shared: the counter and the guards read every take file and journal whatever the extension, so a number
is never used twice for a prefix. Rename keeps the extension; delete takes the peak cache and the journal along; edit
results keep the source's format. The list reads FLAC's STREAMINFO (a take being recorded says 0 = unknown: 0 s) and
Opus's OpusHead plus the granule of the last page (the last 128 KB of the file); each row has `format_tag`
(WAV/FLAC/OPUS), shown at the end of the row's detail line (`today 14:32 · 42 s · 4 MB · FLAC`).

**Repair** (`codec/stream_scan.*`, study 058 table 4.3), for open journals and, without a journal, for `.flac`/`.opus`
files that look unfinished (FLAC total 0, Opus without end of stream; a finished file with a foreign tail such as an
ID3 tag is left alone):
- FLAC: frames are walked from the first one; a frame counts when its header passes CRC-8 and its end passes CRC-16.
  The end of a middle frame is where the CRC-16 matches and the next frame's header (the next number) starts; the last
  frame's exact length comes from walking its subframes (CONSTANT, VERBATIM, FIXED, LPC, Rice residuals), because with
  this CRC a frame whose CRC ends in 0x00 also "matches" one byte early, and in a zero-filled tail at every later byte.
  A damaged frame in the middle is skipped (resync), never cut. The file is cut after the last valid frame (one
  ftruncate, first), then STREAMINFO's total samples are rewritten (from the last frame's number); a set MD5 that no
  longer fits is zeroed first.
- Ogg Opus: pages are walked checking each CRC; OpusHead gives the pre-skip and the input rate; the file is cut after
  the last valid page with a granule, the end-of-stream flag is set on it (flag and CRC in one 21-byte pwrite).
- Idempotent and safe to interrupt; the take gets the same notice and the same "repaired" mark as a WAV take.

**The phase B gate** (`playback_refusal()` in takes/recordings and its five calls in the controller: LOAD, PLAY on a
loaded take, PLAY from the list, the preview, EDIT) is gone since phase C. In its place the controller refuses only a
take whose decoder library is missing, with the notice above.

**Tests.** The test encoders `tests/fixture_codecs.hpp` (a real FLAC stream with verbatim subframes, an Ogg Opus stream
with real pages, no library) make every writer, list and repair path testable byte for byte: the cadence and what each
sync makes durable, a power cut emulated at every step of a take (and cut back into the unsynced part), SIGKILL crash
runs, `fdatasync` failing at each sync, a full card and EIO, short writes, auto-split, pause, the free-space stop, a
missing codec, the repair at every byte offset of small files (recording and finished state), mid-file damage, zero
tails, mixed folders, the app end to end. With the real libraries (`tests/codec/run.sh` with the private roots, libFLAC
1.4.3 and 1.5.0, libopusenc 0.2.1, libopus 1.4 and 1.5.2): FLAC takes through the writer decode sample-exact with
libFLAC (MD5 checked), Opus takes have exactly the recorded length in libopusfile and decode to the end, power cuts
after each sync are repaired and decode, the repair at every byte offset of a short real FLAC (17 248 cuts) and Opus
stream (9 571 cuts) holds, and libFLAC / libopusfile accept the test encoders' streams.

**CPU measured** (this PC, WSL, one x86 core; 20 s of synthetic 48 kHz stereo 16-bit music): FLAC level 5: 0.2 % of a
core (about 640x real time), the same through the writer; Opus 128 kbps: 0.7 % (about 140x real time), 0.7-0.9 % through
the writer. A Pi Zero 2 W core is very roughly 15-25x slower, so about 3-5 % (FLAC) and 10-20 % (Opus 128 stereo): to be
measured on the board (B17 step 12; P0-e measured mono speech Opus at 7-10 %).

**Left for later (not phase B).**
- A notice when the stored choice falls back to WAV at the start of a take (the Codec row already says so, for
  example `WAV (FLAC unavailable)`; the pickers are done, see "The Codec, Sample rate and Quality pickers").
- The observed FLAC ratio fed back into the estimate during a take (`FormatInputs::flac_ratio`).
- `decision_delay_ms` for Opus stays libopusenc's 2 s (a power cut loses the sync interval plus up to 2 s); lowering
  it needs a listening check.
- The take's notes as COMMENT and SCENE/TAKE tags.
- The list shows 0 s for a FLAC take whose STREAMINFO says "unknown" (only a take being recorded, or one whose repair
  failed).
- The packaging: done, the libraries are Depends (owner, 2026-11-11, see above).

## The Codec, Sample rate and Quality pickers (done)

Code: `src/bind/view_map.cpp` (`codec_picker()`, `rate_picker()`, `quality_picker()`, `selectable_param()`,
`size_per_minute_text()`), `src/core/codec/formats.cpp` (the model: `resolve_params()`, `choice_for()`, `effective_for()`,
`change_codec()` ...), `src/ui/page_settings.cpp` (`build_format_rows()`, with the group headings),
`Controller::set_rec_params()` and `set_format_choice()`. Tests: `tests/codec/test_formats.cpp` (every legal combination,
the dimming, the 44.1 kHz-only card, the re-validation, old and new settings keys, sizes), `tests/bind/test_map.cpp`,
`tests/bind/test_binder.cpp`, `tests/alsa/test_open.cpp` (rate and depth choice), `tests/app/test_formats.cpp`
(`t_rec_params`, `t_rec_params_old_and_44k`); scenes `tests/bind/scenes/fmt*.scene`, `sq480-w-*`, `sq720-w-*`. Board test: B19.

- The snapshot carries `Snapshot::format_inputs`: the capture format the recorder uses (state `Known` while the card delivers,
  else `NoCard`), the libraries found, and what the card offers (`rates`, `has16`, `has24`). Three pickers are built from it:
  - **Codec**: the group heading `Codecs`, WAV, FLAC and Opus, then the heading `Quick presets` and the six presets (WAV and
    FLAC named with the rate and depth they get on this card, `WAV, 48 kHz, 16-bit`; Opus as `Opus speech 24 kbps` ...). The three
    plain codecs show the size they would give after the re-validation. The current marker is on the plain codec row. Footer:
    `Sizes for 48 kHz, 16-bit, this card`.
  - **Sample rate**: every rate of the card, ascending, with a hint (`CD`, `video and most devices`, `high resolution`), sizes at
    the current codec and quality; a rate the codec cannot record is dimmed with its reason. Footer: `Sizes for FLAC, 16-bit, this card`
    (Opus: `Opus, 32 kbps`).
  - **Quality**: WAV and FLAC: the depths the card can record (`card_bits`: `16-bit`, `24-bit` or both); a depth the card lacks
    is left out, not dimmed (owner decision; the Sample rate rows and Opus at 44.1 kHz stay dimmed with the reason). With one
    depth there is nothing to choose, so the Settings list has no Quality row for WAV and FLAC (the take gets the card's depth;
    the row ids, not their positions, tie the page to the rows). Opus: the seven bit rates with the speech and music hint, the
    row always shown. The row's label is `Bit depth` or `Bit rate`. Footer: `Sizes for FLAC, 48 kHz, this card`.
  Each entry: name and size on line 1; on line 2 the hint and the hours that fit in the free space (`hours_for_space`), or, when the
  entry is dimmed, the reason. The page is the one the Format picker had; the group headings are the one addition
  (`FormatEntry::header`: small bold text, not tappable, skipped by Up / Down).
- Size text: 1 MB = 10^6 bytes; from 1 MB `5.8 MB/min` (one decimal under 10, none from 10); under 1 MB `190 kB/min`
  rounded to 5 kB; `about ` in front of an estimate (FLAC); a bitrate target (Opus) and an exact size (WAV) have none;
  no card: blank.
- The rows' values are the effective choice: `FLAC`, `48 kHz`, `16-bit` or `32 kbps`; `WAV (Opus unavailable)` on the Codec row when the
  stored codec cannot be used (library missing, no rate or channel count the codec takes), and then Sample rate and Quality show
  WAV's. The Sample rate row is dimmed with no sound input.
- Choosing: the binder maps the entry to the model's change. A preset: `set_format_choice(format, preset)`; anything else:
  `set_rec_params(codec, preset, rate, quality)`, which validates the values (rate 8000..768000, depth 16 or 24, a bit rate of
  the list, a preset of Opus only), stores them, reopens the capture when the rate or depth changed and refreshes the recorder's
  format text and the hours left. A codec change that moved the rate shows its one-line message. Refused while a take or the
  countdown runs (the rows are dimmed then too). The controller does not check the card: the pickers do not offer an incompatible entry, and a
  stored one that stops being compatible falls back to WAV when a take starts.
- Demo and scenes: `CASSETTE_DEMO_CARD=stereo` (44.1 kHz stereo only), `speech` (16 kHz), `rich` (mono, 16 / 44.1 / 48 / 96 kHz,
  16 and 24-bit) and `CASSETTE_DEMO_CODECS=none|flac|opus|both` make the cases reproducible without hardware.
- Not done: a compression level setting for FLAC (fixed at 5), resampling, a separate memory of the bit depth while Opus is chosen.

## Phase C: playback and editing (done)

FLAC and Ogg Opus takes load, play, pause and resume, seek, scan at 5x both ways (REW stops at the start, FWD ends the
take at its end), jump 5 s, preview from the list, open on the Edit page with their peaks, preview there, and are
trimmed and split into results of their own format, exactly as WAV takes. WAV keeps its own code paths (WavReader,
its peak builder, its byte-copy trim): nothing changed for WAV.

**Decision: Opus is decoded with libopus and Cassette's own Ogg reader, not libopusfile.** The brief allowed
libopusfile with a fallback to libopus plus an own page reader when libopusfile is missing; the smallest set of
dependencies is the fallback alone, so it is the only path:
- `libopus0` is needed anyway (libopusenc depends on it for recording); libopusfile would add `libopusfile0` and
  `libogg0` for playback, and a second, fallback code path to test.
- Trim needs the packets and their exact granule positions in Cassette's hands anyway (the remux below); the decoder
  reuses that reader (`codec/ogg_opus.*`, built on the page format the phase B repair already parses).
- libopusfile always delivers 48 kHz; libopus decodes at any Opus rate, so a 16 kHz take plays at 16 kHz on a 16 kHz
  headset without resampling, and the length the player shows equals the list's.
- Checked against libopusfile (loaded by the tests only): the linear decode is within 1 LSB of libopusfile's on the
  48 kHz streams (libopusfile decodes at 48 kHz only, so the 16 kHz stream is compared by its length), and libopusfile
  reads every trimmed file, at 48 and 16 kHz, at exactly the requested length and decodes it to the end cleanly.
Without libopus Opus takes are refused with a notice; WAV and FLAC are not affected (the package depends on each library,
but each is opened at run time, so a removed one only disables its format).

**Decoders** (`codec/decoder.hpp`; `make_decoder()` picks by the file's magic):
- `FlacDecoder`: libFLAC's stream decoder over FileOps callbacks, MD5 checking off. Output: the recorded format (16 or
  24 bits packed, as WAV). Seeks: `FLAC__stream_decoder_seek_absolute`, sample exact, libFLAC's own bisection over the
  frames (a few small reads; 0.07-0.09 ms per seek on this PC). No index is built at open: a frame scan would read the
  whole take (100-200 MB per hour of FLAC on an SD card), which is not cheap; libFLAC's seek is. A frame lost to damage
  becomes silence of its length, so every later sample keeps its place. A STREAMINFO that says 0 samples (a take still
  recording, a failed repair) gets its length from the frame scan of `stream_scan`.
- `OpusFileDecoder`: libopus over `oggopus::OggOpusFile`. Output: 16 bits at the take's input rate when that is an Opus
  rate (8, 12, 16, 24, 48 kHz), else 48 kHz (a foreign 44.1 kHz stream). The mapping (`oggopus::Timeline`): output
  frame o is decoded sample o + pre-skip/k (k = 48000 / output rate); packet boundaries fall on whole output samples at
  every Opus rate; a pre-skip that is not a multiple of k is rounded down (less than one output sample). Seeks: a
  bisection over the pages by granule position (no index, no read of the whole file: 7.8 probes and 33 KB read per
  seek on a 20-minute take, with a small map of the pages already seen as hints), the decoder reset, 80 ms of pre-roll
  decoded and dropped (RFC 7845 section 4.6), then decode-and-drop to the exact sample. The end trimming of the last
  page is honoured (positions on the end-of-stream page are counted forward from the page before). A damaged page
  becomes silence of its packets' length; a packet libopus refuses is concealed (PLC) with its own duration; a stream
  without its end-of-stream page (before its repair) plays to its last page and is marked damaged.
- Accuracy with the real libraries: FLAC decodes, seeks (107 positions per stream) and reverse-scans sample exact at
  16 and 24 bits, mono and stereo; an Opus seek lands on the sample (the best match against the linear decode is at lag
  0 for 60 positions per stream) and agrees with the linear decode to 36-41 dB (the decoder's convergence after 80 ms;
  libopusfile's seek has the same property).

**The Player** (`playback/player.*`) reads a FLAC / Opus take through `DecodedSource` (`codec/decoded_source.hpp`), the
same random-access frame interface as WavReader, in its playback thread: reading on from where the decoder stands
(normal play, the forward 5x scan) decodes just what each period needs; a read just before the window (the reverse
scan reads its blocks backwards) decodes a whole window of half a second ending where the read ends, plus the frames
the decoder had to decode anyway to get there (an Opus seek starts at a page and decodes the pre-roll: those frames are
kept), so the reverse scan decodes 1.05-1.17 frames per frame it plays; a jump repositions the decoder. The 500 ms
device buffer absorbs the decoding time of a window. Proven by twin files (`tests/playback/test_codec_play.cpp`): a WAV
holding exactly the PCM a FLAC / Opus take decodes to, the same scripts on both Players (play to the end, pause and
resume, scan forward to the end, reverse to the start, REW at the start and FWD at the end, jumps, a change of scan
direction, seeks while paused, stop and play), at two device latencies: state, position, events and every sample the
device received are identical.

**CPU** (this PC, one x86 core, 60 s of 48 kHz stereo music, real libraries; share of one core):

| Take | Normal play | 5x forward scan | 5x reverse scan | x real time |
|---|---|---|---|---|
| FLAC 16-bit | 0.07-0.08 % | 0.33-0.41 % | 0.38-0.48 % | about 1200-1500 |
| Opus 128 kbps | 0.22 % | 1.1 % | 1.3 % | about 450 |
| Opus 32 kbps (one coded channel) | 0.17-0.23 % | 0.9-1.1 % | 1.0-1.7 % | about 450-580 |

A Pi Zero 2 W core is very roughly 15-25x slower (the phase B estimate), so the 5x scan of a 128 kbps Opus take needs
about 17-28 % of one core forward and 20-40 % in reverse; FLAC under 12 %. That stays audible without starving the
board, so the seek-skip scan (short slices every N ms) the brief offered as a fallback was not needed; B18 measures it
on the board. The Player's own work per period with the fakes: 23 us (FLAC) and 56-58 us (Opus) against 11-19 us for
the WAV twins, for a period of 21.3 ms.

**Peaks** (`edit/peaks.*`): the same pyramid and cache file (`<take>.flac.peaks`, `<take>.opus.peaks`), keyed by the
take's size and mtime and recording the decoder's output format, built from the decoded audio in one streaming pass of
65536-frame steps (a take of hours needs no more memory than a short one) on the controller's worker thread with the
progress the Edit page already shows; the UI never waits. Exact bars (fewer than 256 frames per bar) decode the few
frames they need, under a lock (the UI may ask from several threads). A missing decoder: `cannot decode the take: FLAC
not installed (libflac14)`.

**Trim and split** (`edit/trim_split.*`): the results keep the source's format, names (`-N.flac`, `-N.opus`), the
temporaries, the order of renames and the checks of the WAV engine, so every guarantee holds (the original untouched
until every result is synced; replace removes it only after; a kill leaves the original or complete results).
- FLAC: the part is decoded and encoded again (libFLAC level 5, streamable subset, 4096 bytes of padding, as a
  recording); lossless and sample exact at both cuts; STREAMINFO gets the part's length and MD5 (libFLAC verifies it in
  the tests). Keeping the whole frames between re-encoded edge frames was not done: a fixed-block-size stream needs
  every frame but the last of the same size, so the frames can be kept only when IN falls on a frame boundary; otherwise
  the copied frames' headers must be rewritten as a variable-block-size stream (sample numbers, CRC-8 and CRC-16
  again), which libFLAC cannot write and some players handle badly. A full re-encode is simpler and just as lossless.
  Cost: decode and encode run at about 400x real time on this PC (stereo music), so a one-hour stereo take takes
  roughly 2-4 minutes on a Pi Zero 2 W (estimate; B18 measures a short one); the save box shows the progress.
- Opus: no re-encoding. The packets of the part plus 320 ms before it are copied into new pages
  (`oggopus::remux_opus_range`); the new pre-skip drops that pre-roll and the part of the first packet before IN, the
  last page's granule trims the end at OUT, granules restart at 0, the comments are copied. Sample exact (the fakes
  check every sample; libopusfile reads exactly the requested length). Why 320 ms: with the minimum 80 ms the decoder
  had not settled at IN (CELT predicts band energies from the frame before) and the first 100 ms of a part differed from
  the take (13-24 dB SNR in the first 20 ms); with 320 ms the parts match the take's own decode to 96 dB or better for
  music at 48 kHz and 48 dB or better for 16 kHz speech, many bit for bit. No codec library is needed for the cut.
- Both: a TITLE comment that named the source file names the result; the rest is copied unchanged.
- Crash safety, as for WAV (`tests/edit/test_codec_edit.cpp`): every file operation of eight FLAC and Opus saves (trim,
  split, replace with one part and with several, mono and stereo, 16 and 24 bits, 48 and 16 kHz) made to fail with EIO
  and with ENOSPC, and to be the point of a kill: afterwards the original is byte-identical or every result is complete
  and decodes to exactly its slice, never a half file under a final name, and `cleanup_temporaries()` leaves nothing.
  The same sweep for a peak build of a FLAC and an Opus take.

**Tests.** Fakes everywhere (`tests/fake_decoders.hpp`: a libFLAC decoder for the test encoder's VERBATIM frames with
libFLAC's calling conventions, seek_absolute included; a stateless libopus decoder whose samples identify their packet,
so positions are checked to the sample): the codec suite (framing, decoders, seeks at four rates, bisection on a long
take, damage, the window cache, remux of 60 ranges), the playback suite (the twin scripts), the edit suite (results,
errors, crash sweeps, peaks), the app suite (a FLAC and an Opus take recorded, loaded, played to the end with every
sample checked, previewed, trimmed and split from the Edit page; the notices without the libraries). The real-library
group (`tests/codec/test_decoders_real.cpp`) runs with both private roots: FLAC 1.4.3 / libopus 1.4 and FLAC 1.5.0 /
libopus 1.5.2, libopusfile 0.12 as the reference.

**Left for later (not phase C).**
- Notes editing in place: the FLAC padding and the OpusTags padding allow it without rewriting the file.
- An Opus take with more than two channels (mapping family 1) is refused (Cassette records mono or stereo).
- A seek in an Opus take uses the 80 ms pre-roll of RFC 7845 (fast, 36-41 dB against the linear decode at the landing
  point); a longer one would cost a little more decoding per seek and reverse-scan window.
- FLAC MD5 is not checked during playback (libFLAC turns it off after any seek anyway).

## Merging takes

MERGE (the recordings list's fifth key, branch `cassette-merge`, added in 0.2.0, 2026-11-11) joins two or more
takes, in the order the user chose them, into one new take of the same format. Code: `src/core/edit/merge.*` (the engine: the compatibility rule,
`plan_merge()`, `merge_takes()`), `Controller::plan_merge()`, `start_merge()`, `cancel_merge()`, `src/bind/view_map.cpp`
(`merge_row_reason()`, the merge strip) and `binder.cpp`, `src/ui/page_list.cpp` (merge mode), `src/ui/boxes.cpp`
(`MergeBox`, `ProgressBox`). Tests: `tests/edit/test_merge.cpp`, `tests/codec/test_merge_real.cpp`,
`tests/app/test_merge.cpp`, `tests/bind/test_binder.cpp` (`test_merge_flow`), `tests/ui/test_format.cpp`; scenes
`mg1-merge-*`, `mg2-merged-*`, `mg3-merge-one-take`, `tests/bind/scenes/x9-bound-merge`. Board test: B23.

**What can be joined.** The first take chosen sets the group; a take joins it only when nothing has to be converted:

| | Must be equal | Not compared |
|---|---|---|
| WAV | codec, sample rate, channels, bit depth | |
| FLAC | codec, sample rate, channels, bit depth | the compression (one level, 5) |
| Ogg Opus | codec, input rate (OpusHead), channels (and the output gain) | the bit rate (not stored with a take; packets of any bit rate decode in one stream) |

The reasons the list shows, in this order: `Not a Cassette take` (the name does not follow the take grammar),
`Damaged take`, `Different format: FLAC 44.1 kHz` (another codec: the other take's codec and rate), `Different rate`,
`Different channels`, `Different bit depth`; and from the controller's state `Playing: stop it first`, `Open on the Edit
page`, `Being recorded`. The rule is one pure function (`merge_join_reason`) used by the list (from the take rows) and by
the engine (from the files).

**The join, per codec.** Memory is bounded for takes of any length (WAV: blocks of 256 KB; FLAC: decode and encode steps
of 256 KB of PCM; Opus: one packet at a time).

- **WAV**: the PCM of each take is copied as it is behind a fresh header from `wav.hpp` (`build_header`: the sizes of the
  whole, RF64 with `ds64` when the data passes 4 GiB; a `bext` with the date, the time, the time reference and the
  description `Cassette <name>` exactly as a new recording gets them; the coding history and iXML generated as for a
  recording). Sample exact. The RF64 boundary is tested with two generated 2.5 GiB takes.
- **FLAC**: every take is decoded (libFLAC, `FlacDecoder`) and the samples go through **one** `FlacEncoder` at the takes'
  rate, channels and depth (level 5, streamable subset, 4096 bytes of padding, the Vorbis comments of a new take: `TITLE`,
  `DATE` when the clock is set, `ENCODER=Cassette`). Lossless and sample exact; STREAMINFO says the sum of the lengths and
  its MD5 is the whole's (libFLAC 1.4.3 and 1.5.0 accept it). Keeping the frames as they are was not possible: each take's
  frame numbers start at 0 and the last frame of a take is short, which a fixed-block-size stream does not allow in the
  middle (the trim engine met the same, see phase C). CPU: the trim engine's figure applies, about 400x real time on this
  PC for stereo music; an estimate of 2 to 4 minutes per hour of stereo on a Pi Zero 2 W (not measured on a board yet).
- **Ogg Opus**: no re-encoding. The packets of every take are copied into **one** logical stream: a new serial number,
  the page sequence from 0, new CRCs, granule positions counted on from take to take (pages of whole packets, at most
  about one second and 255 lacing values each, as `remux_opus_range` writes them). OpusHead is the first take's (its
  pre-skip, channels, input rate, gain), OpusTags carries the new take's comments with the first take's vendor string. The
  first take keeps its pre-skip, the last take keeps its end trimming (the end-of-stream page's granule). Packets that lie
  wholly inside a later take's pre-skip are left out (an edited take keeps 320 ms of pre-roll there, see phase C: it would
  otherwise be heard twice). A take whose packets do not add up to its page granules (a damaged page in the middle) stops
  the merge: never a take with a hole.

**Opus: why one stream, and what a join does.** An Ogg Opus stream can drop decoded samples only at its start (pre-skip)
and at its end (end trimming, RFC 7845 sections 4.2 and 4.4); nothing in the middle. Each take hides samples at both ends:
the encoder's look-ahead at the start (312 samples, 6.5 ms, for libopusenc at 48 kHz; for an edited take the part of its
first packet before IN) and the padding of its last 20 ms frame at the end (0 to 20 ms; for an edited take the part of its
last packet after OUT). In one stream those samples of the inner takes are played. So:

- each join adds the earlier take's tail plus the later take's lead: 7 to 27 ms between takes straight from the recorder,
  always under 40 ms. `MergePlan::seam_frames` says exactly how much, and the new take's length (verified after writing) is
  the sum of the takes' lengths plus the seams;
- the decoder runs on across the join with the state of the earlier take, while the later take's first packets were coded
  from a fresh encoder state (CELT predicts each band's energy from the frame before; SILK carries pitch and LPC history),
  so the first packets of the later take decode differently from the take alone until the state has converged.

Measured with the real libraries (`tests/codec/test_merge_real.cpp`: three consecutive pieces of one synthetic signal,
each coded as its own take by libopusenc, merged, decoded by libopus; libopusfile 0.12 reads the merged file to the end at
exactly the plan's length; identical with libopus 1.4 and 1.5.2):

| Takes | Seams (tail + lead) | Seam level vs the audio before | Later take vs its own decode: first 20 ms / 20-100 ms / rest |
|---|---|---|---|
| 48 kHz stereo music, 96 kbps (CELT) | 17.4 ms (10.9 + 6.5), 20.0 ms (13.5 + 6.5) | -31 dBFS vs -22, -39 vs -18 | -3.9 / 8.5 / 46.3 dB and -2.2 / 5.3 / 45.5 dB |
| 16 kHz mono speech, 24 kbps (SILK) | 12.3 ms, 20.0 ms | -44 vs -38, -35 vs -21 | 47.4 / 51.7 / 60.4 dB and 53.3 / 46.3 / 49.9 dB |
| 48 kHz mono speech, 32 kbps | 17.4 ms, 20.0 ms | -66 vs -42, -45 vs -28 | 41.9 / 50.6 / 53.5 dB and 48.7 / 46.0 / 54.7 dB |

Against the source signal itself, the music take decodes alone at 19-22 dB in its first 100 ms and in the merge at
-3 / 5-9 dB, and both at 20.6-20.8 dB after that; the speech takes are the same alone and merged (within 0.1 dB). The
first take always decodes exactly as alone. Reading: with the speech settings a join is a quiet gap of 12-20 ms and nothing
else; with the music settings the first 20 ms of the later take are wrong (an error as large as the signal) and the next
80 ms clearly off, settling after about 100 ms: a short blip, to be confirmed by listening (B23 step 6).

Alternatives considered (none implemented):
- **A chained Ogg stream** (each take a link with its own OpusHead, pre-skip and end trimming, RFC 7845 section 3) would be
  gap-free and sample exact in a reader that handles chains (libopusfile resets the decoder per link). Cassette's own
  reader does not: `OggOpusFile` follows one serial number, finds the length from the last page's granule and seeks by
  granule over one stream; the list (`read_stream_info`), the repair (`scan_ogg_opus` / `fix_ogg_opus`), the trim remux,
  the peaks and the player would all need chain support (a link table, per-link timelines, seeks across links). Not done.
- **Re-encoding** the seams or the takes would be a second round of lossy coding: not done.
- A Cassette-only marker (a tag listing the join granules, our decoder resetting there) would fix playback in Cassette
  only and change the timeline the list, the peaks and other players see. Not done.

**Safety** (the trim engine's rules): the new take is written to `<dir>/.tmp-merge-<random>`, `fdatasync`ed, closed and
**read back** (WAV: the header's format and sizes; FLAC: STREAMINFO's total; Opus: the end-of-stream page and the length
by the timeline) before anything else happens; the takes must still have the size and time they had (else
`a take changed while it was being merged`); then the temporary is renamed to the final name and the folder is fsynced
(a failing fsync removes the new take again). Only then the take counter moves on to the new take's number
(`note_take_number_used`: never back; a merge that fails or is cancelled uses no number) and, with DELETE ORIGINALS, the
originals are deleted one by one through `delete_take` (the take, its peak cache, its journal), the folder fsynced again;
a delete that fails is a warning (`1 original could not be deleted`), the merge stands. A cancel, an error or a kill
before the rename leaves the takes untouched and no take under the new name; `cleanup_temporaries()` removes the
temporary at the next start. Free space: the takes' total size + 5 % (refused before anything is written). The new take's
name is a new recording's: `peek_take_number` with the first take's prefix, the date and time now (or `nodate`), the
takes' extension; recording waits while a merge runs (the merge holds that number).

Crash tests (`tests/edit/test_merge.cpp`): every file operation of six merges (WAV mono and stereo 24-bit, FLAC mono and
stereo 24-bit, Opus 48 kHz mono and 16 kHz stereo with an edited take, keeping and deleting the originals) made to fail
with EIO and with ENOSPC and to be the point of a kill (834 runs): afterwards every original is as it was, or deleted only
when the new take is complete, never a broken take under the new name, and the cleanup leaves no temporary.
