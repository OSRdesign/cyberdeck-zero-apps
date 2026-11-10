# Cassette: recording formats (FLAC and Ogg Opus next to WAV)

Design note for the codec layer (phase A, 2026-11-10). Decisions it follows: report 058 D3 (WAV/BWF stays the
default), D4 (FLAC and Opus as options), D7 (libraries loaded at run time with `dlopen`, no link-time dependency), D11
(the card's native rate, no resampling in 0.1), D16 (Vorbis comments for metadata).

**Status (2026-11-10).** Phase A (the encoders, the library loading, the format list with its size estimates, the
stored key) and phase B (recording: the writer, names, the list, the journal, the repair) are done. The recorder writes
FLAC and Ogg Opus takes when the stored setting asks for it and `effective_choice()` allows it. The Settings Format row
(the owner's design) is done too (2026-11-10, branch `cassette-fmtui`): it opens a picker page listing every format of
the model with its size per minute, the model's detail line and the hours that fit in the free space; entries the card
or the libraries cannot record stay in the list, dimmed, with the model's reason. See "The Format picker" below. Playing, previewing,
trimming and the peak cache of FLAC/Opus takes are phase C: until then those refuse with a notice. Code:
`src/core/codec/`, `src/core/writer.*`, `src/core/repair.*`, `src/core/takes/`; tests: `tests/codec/`,
`tests/test_stream_scan.cpp`, `tests/test_codec_writer.cpp`, `tests/test_codec_crash.cpp`, `tests/takes/test_formats.cpp`,
`tests/app/test_formats.cpp`, `tests/codec/test_writer_real.cpp`. Board test: B17 in `tests/BOARD-TESTS.md`.

## What is offered

The Settings Format row will list every entry below in this order, each with a name, a one-line detail (the wording
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

`format_preset` is a new, optional key. Absent or unknown means the format's default (Opus: `speech32`; FLAC and WAV
have one entry), so every settings file written before keeps its meaning, and older app versions keep the key on
their next write (unknown keys are preserved). An empty preset is not written.

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
| libFLAC | `libFLAC.so.14`, else `libFLAC.so.12` | `libflac14` (trixie, Ubuntu 26.04 likely), `libflac12t64` (Ubuntu 24.04), `libflac12` (bookworm) | FLAC recording (and FLAC playback in phase C) |
| libopusenc | `libopusenc.so.0` | `libopusenc0` (0.2.1 in trixie) | Opus recording |
| libopus | (needed by libopusenc) | `libopus0` | Opus |
| libogg | (needed by libFLAC and libopusfile) | `libogg0` | not used by libopusenc (it has its own page writer) |
| libopusfile | `libopusfile.so.0` (phase C) | `libopusfile0` | Opus playback |

The encoder calls are the same in FLAC 1.4 and 1.5 (the soname moved for decoder changes); both were tested.

**Proposed for the package (not applied; packaging is the owner's step):**

```
"depends": "libfreetype6, libinput10, libxkbcommon0, libasound2t64 | libasound2",
"recommends": "libflac14 | libflac12t64 | libflac12, libopusenc0, libopus0"
```

and in phase C add `libopusfile0` (it pulls `libogg0`). The app runs with none of them (WAV only; FLAC and Opus rows
greyed with the package to install). Caveat: the launcher's Store installs a package with plain `dpkg --install` when
its Depends are already met (`appstore_native.cpp`), and dpkg never installs Recommends; only `apt install` does. So
with the current Store, Recommends only help users who install by apt. If the owner wants FLAC and Opus to work after
a Store install on every board, the alternatives are moving the two libraries to Depends (installed sizes in trixie:
libflac14 479 KB, libopusenc0 70 KB, libopus0 3.5 MB (libopus 1.5 carries its neural models), libogg0 55 KB; the Store
then goes through `apt-get install`, which needs the network) or a Store change.

Which boards already carry the libraries is not known yet: the six boards were offline during phase A (read-only
probe to repeat: `ldconfig -p | grep -E 'libFLAC|libopus|libogg'` and `dpkg-query -W 'libflac*' 'libopus*' 'libogg*'`).

## ABI and tests

- `codec_abi.hpp` declares the C ABI by hand; `check_codec_abi.cpp` compares it with real headers when present
  (constants, the two structures used, and the shape of every function and callback) and does not compile on a
  mismatch. Checked against FLAC 1.4.3 + libopus 1.4 + libopusenc 0.2.1 (Ubuntu 24.04 packages) and FLAC 1.5.0 +
  libopus 1.5.2 + libopusenc 0.3 (Debian packages). arm64 headers were not checked (same C ABI rules; the layouts are
  plain `uint32_t` and pointers).
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
(`48k/16 FLAC`, `48k/16 stereo OPUS`) and the hours left (from the choice's size estimate) follow the same choice,
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

**Phase C gate.** `playback_refusal()` (takes/recordings) is the one place: LOAD, PLAY / PLAY-PAUSE on a loaded
FLAC/Opus take, the list's preview and TRIM (so the peak cache too) answer `FLAC playback is not available yet` /
`Opus playback is not available yet` and nothing opens the file.

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
- The Format row (the owner's design) and `set_format()` accepting the compatible choices; a notice when the stored
  choice falls back to WAV (today it does so silently, the format text shows WAV).
- The observed FLAC ratio fed back into the estimate during a take (`FormatInputs::flac_ratio`).
- `decision_delay_ms` for Opus stays libopusenc's 2 s (a power cut loses the sync interval plus up to 2 s); lowering
  it needs a listening check.
- The take's notes as COMMENT and SCENE/TAKE tags.
- The list shows 0 s for a FLAC take whose STREAMINFO says "unknown" (only a take being recorded, or one whose repair
  failed).
- The packaging (Recommends or Depends for libflac/libopusenc, see above) is the owner's step.

## The Format picker (done)

Code: `src/bind/view_map.cpp` (`format_picker()`, `size_per_minute_text()`, `formats_footer_text()`,
`selectable_format()`), `src/ui/page_settings.cpp` (`build_format_rows()`), `Controller::set_format_choice()`.
Tests: `tests/bind/test_map.cpp` (size text, entries, selection rule), `tests/bind/test_binder.cpp`, `tests/app/test_formats.cpp`
(`t_set_format_choice`); scenes `tests/bind/scenes/fmt*.scene`, `sq480-w-format`, `sq720-w-format`. Board test: B19.

- The snapshot carries `Snapshot::format_inputs` (the capture format the recorder uses, state `Known` while the card
  delivers, else `NoCard`; the libraries found). `format_picker()` builds the list with `list_formats()` from it and
  maps each entry: name and size on line 1, the model's detail and, when the free space is known, the hours that fit
  there (`hours_for_space`) on line 2; an entry that is not compatible shows `why_not` on line 2 and is not selectable.
- Size text: 1 MB = 10^6 bytes; from 1 MB `5.8 MB/min` (one decimal under 10, none from 10); under 1 MB `190 kB/min`
  rounded to 5 kB; `about ` in front of an estimate (FLAC); a bitrate target (Opus) and an exact size (WAV) have none;
  no card: blank. The footer says which capture format the sizes are for.
- The row's value is the effective choice: `WAV 48 kHz 16-bit`, `FLAC`, `Opus 32 kbps`; `WAV (FLAC unavailable)` when the
  stored choice cannot be used (library missing, rate or channels the codec does not take). The current entry of the
  picker is the effective one.
- Choosing: the binder maps the entry index to the model's key and calls `set_format_choice(format, preset)`, which
  stores both through the settings store (WAV and FLAC with an empty preset, Opus with its preset) and refreshes the
  recorder's format text and hours left. It is refused while a take or the countdown runs (the row is dimmed then
  too) and for a key the model does not have. The controller does not check the card: the picker does not offer an
  incompatible entry, and a stored one that stops being compatible falls back to WAV when a take starts.
- Demo and scenes: `CASSETTE_DEMO_CARD=stereo` (44.1 kHz stereo only) and `CASSETTE_DEMO_CODECS=none|flac|opus|both`
  (which libraries the picker finds) make the cases reproducible without hardware.

## Next: phase C, playback and editing

0. Remove the gate: `playback_refusal()` in takes/recordings and its callers in the controller (load, play_take,
   preview_take, open_target, open_trim); the app tests that expect the notice (`tests/app/test_formats.cpp`) then
   expect playback.
1. Decoders (decoder.hpp designed): FLAC through libFLAC's stream decoder, Opus through libopusfile, WAV behind the
   same interface; the player, peaks and trim read through it. Opus plays at 48 kHz whatever the input rate. The
   player's progress and the list read the length from the stream (`read_stream_info()` already gives it).
2. Peaks for compressed takes: decode once on the worker thread (FLAC is fast; Opus decodes at about 30x real time on
   a Pi 3 class CPU per report 058: a one-hour take takes a couple of minutes on the zero8, so show progress or build
   the peaks while recording, from the PCM the writer already has).
3. Trim/split: FLAC by re-encoding (simplest: decode and encode the kept range; or keep whole frames and re-encode
   the edge frames); Opus by cutting at pages and setting pre-skip / end trimming for sample accuracy, or re-encoding
   (a second generation of lossy coding).
4. Notes editing in place: the FLAC padding and the OpusTags padding allow it without rewriting the file.
5. The edit names already keep the source's extension (`next_edit_names`); trim_split must write the results in the
   source's format (or WAV, if the owner prefers: then the names need the target format).
