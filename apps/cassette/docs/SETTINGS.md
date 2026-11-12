# Cassette: the settings file (`settings.json`)

Cassette keeps its settings in `~/.config/cassette/settings.json`. You do not need to edit it: every key below is
changed from the app (the row or control named in the last column), and the file is written at once when you change
something. This page is for reading the file, for resetting one setting, or for the one setting the app cannot change
(the folder to a place other than `~/Recordings`, see [Folder](#folder)). The source of truth is
`src/core/takes/settings.hpp` and `settings.cpp`; the rows are in the [user guide](USER-GUIDE.md#settings).

How the file behaves:

- **All keys are optional.** A missing key, a missing or unreadable file, or a value that is not valid for its key gives the default.
  Cassette never refuses to start because of this file. A number out of range is clamped (the table says to what).
- **Delete the file to reset everything**, or delete one key to reset that setting. Close Cassette first: it writes the file when a
  setting changes, so an edit made while it runs can be overwritten.
- **Written safely**: to a temporary file, flushed, then renamed over the old one, so a power cut leaves the old or the new
  file, never half of one.
- **Keys Cassette does not know are kept** when it writes the file again (at the top level and inside a card entry), so a
  newer or older version does not lose them. `"version": 1` is written by the app; do not change it.

## Keys

| Key | Values | Default (key absent) | Set in the app by |
|---|---|---|---|
| `format` | `"wav"`, `"flac"` or `"opus"` | `"wav"` | Settings > Codec |
| `format_preset` | a preset name: `"speech24"`, `"speech32"`, `"music64"`, `"music128"` (Opus presets); absent for WAV and FLAC | the codec's own default (Opus: `speech32`) | Settings > Codec (the quick presets) |
| `rate` | capture sample rate in Hz, 8000 to 768000; `0` or absent = the card's own rate for the codec | `0` | Settings > Sample rate |
| `quality` | WAV and FLAC: bit depth, `16` or `24`; Opus: bit rate in kbps (16, 24, 32, 48, 64, 96, 128); `0` or absent = the default (a preset's bit rate, else 16-bit, else Opus 32 kbps) | `0` | Settings > Bit depth or Bit rate |
| `folder` | the folder for takes, `~` allowed, 1 to 1024 characters, no control characters | `"~/Recordings"` | Settings > Folder (offers the current folder and `~/Recordings`) |
| `prefix` | the first part of new take names, 1 to 24 characters of letters, digits, spaces, `-`, `_` | `"take"` | Settings > File prefix |
| `sync_interval_s` | seconds between safe writes of a running take, 5 to 30 (clamped) | `10` | Settings > Sync every (5, 10, 20, 30) |
| `auto_split` | `true`: a take that reaches 4 GiB goes on in a new part; `false`: one RF64 file | `true` | Settings > Auto-split |
| `record_timer_s` | countdown before REC: `0`, `3`, `10` or `20` (another number is moved to the nearest of these) | `0` | Settings > Record timer |
| `rew_fwd` | `"scan"` (hold: 5x scan) or `"jump"` (tap: 5 s jump) | `"scan"` | Settings > REW / FWD |
| `low_cut` | `true` or `false`: 80 Hz high-pass | `false` | Settings > Low cut 80 Hz |
| `limiter` | `true` or `false`: peak limiter | `false` | Settings > Limiter |
| `soft_gain_db` | software gain in dB, 0 to 24 (clamped) | `0` | the SOFT control on the recorder screen |
| `monitor` | `true` or `false`: hear the input while the recorder runs | `false` | Settings > Monitor |
| `monitor_level` | 0 to 100, in steps of 5 (rounded to the nearest step) | `50` | Settings > Monitor level |
| `monitor_output` | where the monitor plays: `""` or absent = automatic (the card's own playback side, or the Bluetooth headset that is the input); `"bt:AA:BB:CC:DD:EE:FF"` for a Bluetooth device; an ALSA device name | automatic | Settings > Monitor to |
| `wave_fit` | `true`: the Edit page's waveform is scaled to fit (FIT); absent: true level (TRUE). Written only when `true` | absent (TRUE) | the TRUE / FIT chip on the Edit page (key F) |
| `card` | the chosen input card, as Cassette describes it: name, USB path and `vid:pid` joined with `\|`, or `bt:AA:BB:CC:DD:EE:FF` for a Bluetooth headset (a text Cassette writes; copy it, do not type it) | absent: the first capture card | Settings > Input |
| `output_device` | the playback device: an ALSA device name (`hw:CARD=...,DEV=0`, `default`) or `"bt:AA:BB:CC:DD:EE:FF"` | absent: automatic | Settings > Output |
| `bt_names` | an object `{ "AA:BB:CC:DD:EE:FF": "last name seen" }`: the names of Bluetooth devices, so a paired one that is not connected still shows its name. Capital hex digits with colons; at most 16 entries (extra entries are ignored); not written when empty | absent | automatic, not a setting |
| `cards` | an object with one entry per sound card (see below) | absent | Input level, Auto gain (card), Mic mute (card) |
| `version` | `1` | written by the app | not a setting |

### Folder

Settings > Folder offers the current folder and `~/Recordings`. To keep takes somewhere else, close Cassette and set
`"folder"` to the path, for example `"/media/usb/Recordings"` (or `"~/Music/Takes"`), then start it again. Make sure the folder exists and
your user can write to it. A value that is empty, longer than 1024 characters or holds a control character is ignored (the default is used).

### The `cards` entries

The mixer of a sound card goes back to its own values at every reboot, so Cassette keeps the level, auto gain and mute per
card and applies them again at start. The entry is named by the same text as `card`:

| Key inside an entry | Values | Meaning |
|---|---|---|
| `level_db` | a number of dB, -120 to 60 (clamped; the card's mixer limits it further) | the recording level; absent = the card has no level control |
| `auto_gain` | `true` or `false` | the card's own automatic gain |
| `mute` | `true` or `false` | the card's microphone mute |

### Not in this file

The take number (the number the next take gets) is `~/Recordings/.take-counter`, not a setting (see
[Take number](USER-GUIDE.md#take-number)). The other files Cassette keeps are listed in
[Files and where they live](USER-GUIDE.md#files-and-where-they-live).
