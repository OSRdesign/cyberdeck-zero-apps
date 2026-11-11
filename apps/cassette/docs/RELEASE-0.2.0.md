# Cassette 0.2.0: release notes

Cassette 0.2.0 adds one feature to 0.1.0: **merging takes**. Everything else (recording, the formats, the meter, the deck,
the Edit page, the layouts) is as in [0.1.0](RELEASE-0.1.0.md).

## What is new

- **MERGE joins several takes into one new take, in the order you choose.** In the recordings list, select the take that
  should come first and press MERGE (key M). The list turns into merge mode: tap the other takes in the order you want
  (each gets the next number), press MERGE again for the merge box, and choose KEEP ORIGINALS or DELETE ORIGINALS. The
  strip at the top shows how many takes are chosen, their length and size together. The new take is named like a new
  recording (the next take number, the first take's prefix, the date and time of the merge).
- **Takes of one kind only.** The first take you choose decides: the others must have the same codec (WAV, FLAC or Opus),
  sample rate and channels, and for WAV and FLAC the same bit depth. A take that cannot join is dimmed and says why
  (`Different format: FLAC 44.1 kHz`, `Different rate`, `Different channels`, `Different bit depth`, `Not a Cassette take`,
  `Damaged take`, `Playing: stop it first`). Nothing is ever converted.
- **How each format is joined.**
  - **WAV**: the sound is copied as it is behind a new header (RF64 past 4 GiB). Sample exact.
  - **FLAC**: decoded and written again as one file at the same rate and depth. Lossless and sample exact; it costs CPU
    (a few minutes per hour of stereo on the slowest boards, not measured on a board yet).
  - **Ogg Opus**: no re-encoding; the packets of every take are copied into one stream.
- **Safe.** The new take is written under a temporary name, synced and read back before it gets its name; a cancel, an
  error or a power cut leaves your takes as they were. The originals are deleted (if you chose that) only after the new
  take is complete. The merge needs the takes' total size plus 5 % free; a progress box shows the work and Esc stops it.

## Opus joins: a short seam

An Ogg Opus file can leave out sound only at its very start and very end, so at each join the new take keeps the padding
each take had when it played alone. That adds a seam of about 12 to 20 ms between takes straight from the recorder (always
under 40 ms), and the new take is that much longer than the takes together. With the speech presets (24 and 32 kbps) the
join is a quiet gap. With the music presets (64 and 128 kbps) the first 100 ms of each later take decode a little
differently, and a short blip at each join is likely audible (measured on synthetic signals; to be confirmed by ear on
real recordings). If the join must be clean, record the takes as WAV or FLAC. Opus takes of different bit rates can be
merged.

## Also in this version

- The user guide's HackBerry photo now shows a fresh capture taken while recording (Opus 16 kHz, lamps lit). No program
  change.

## Known limits

- Merge is for takes of one format and rate; there is no conversion and no resampling, and no merge across formats.
- The seam of an Opus join cannot be removed (see above); Cassette's reader plays one stream, not chained streams.
- The time a FLAC merge takes on a Pi Zero 2 W is estimated, not measured. The limits of 0.1.0 otherwise stand: no
  recording in the background, no Opus on a card that records at 44.1 kHz only, no Bluetooth audio.

More: [`README.md`](../README.md), [`USER-GUIDE.md`](USER-GUIDE.md), [`FORMATS.md`](FORMATS.md).
