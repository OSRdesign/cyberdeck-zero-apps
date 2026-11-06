# Test fixtures (our own recordings)

Recorded on 2026-11-06 from the owner's HLK-LD2452 on a CH340 USB-serial board at 9600 baud 8N1 (study report 056,
section 3.5). They are our own data, MIT like the rest of the app.

- `ld2452-9600-walk-15s.bin`: 8700 raw bytes = 290 data frames; `.csv`: the same frames decoded (the golden file).
- `ld2452-9600-config-session.bin`: 1778 raw bytes of one read-only configuration session (a data frame cut short by
  the command answer, the damaged first byte, silence, the end answer, the resumed stream).

- `empty-pi3a-30s.bin`, `empty-pi3a-20s.bin` (Pi 3A+) and `empty-deck-30s.bin` (the deck): our own recordings of the same
  sensor at 9600 baud with nobody in front of it, 17432, 11651 and 13800 bytes (581, 388 and 460 frames). Every slot is
  empty in all of them, apart from a 0.13 s blip at the start of the deck file. Used by `test_ghost.cpp`.

## HLK-LD2451 recordings (2026-11-06)

Our own recordings of the owner's HLK-LD2451 (study `ld2451-compat.md`) on a CH340 USB-serial adapter at 115200 baud
8N1 on a Raspberry Pi 3A+, listen only (nothing was sent), taken with the capture script of the study; MIT like the
rest of the app. `.ts.csv`: for each chunk read (`offset,len,t` in seconds), when the sensor's bytes arrived (the
first chunk of each file holds the frames that had queued up before the capture started). The frame counts are
asserted in `test_ld2451.cpp`.

- `ld2451-room-300s.bin` (2758 bytes): 300 s in a room about 4 m deep, a person walking away from and toward the
  sensor; 242 frames, 194 empty (`F4 F3 F2 F1 00 00 F8 F7 F6 F5`, one every 1.4 s) and 48 with one target (every 0.14 s).
  The legs that go away carry direction byte 0, the legs toward the sensor byte 1. A few readings of 6-10 m are
  reflections.
- `ld2451-walk-240s.bin` (2334 bytes): 240 s, a walk; 201 frames, 155 empty, 46 with one target.
- `ld2451-115200-60s.bin` (745 bytes): 60 s, the first capture; 57 frames, 32 empty, 25 with one target.
- `room` and `walk` hold two stray bytes (`F4 F3`) at offset 40 that belong to no frame; the decoder skips them.
- Gaps seen: 1.39-1.67 s between empty frames, up to 2.65 s between two target frames of one pass, never more than
  2.8 s between any two frames (the basis of the driver's `quiet_ms` 5 s and `drop_ms` 3 s).
