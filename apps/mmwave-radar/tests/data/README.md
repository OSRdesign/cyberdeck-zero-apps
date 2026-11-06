# Test fixtures (our own recordings)

Recorded on 2026-11-06 from the owner's HLK-LD2452 on a CH340 USB-serial board at 9600 baud 8N1 (study report 056,
section 3.5). They are our own data, MIT like the rest of the app.

- `ld2452-9600-walk-15s.bin`: 8700 raw bytes = 290 data frames; `.csv`: the same frames decoded (the golden file).
- `ld2452-9600-config-session.bin`: 1778 raw bytes of one read-only configuration session (a data frame cut short by
  the command answer, the damaged first byte, silence, the end answer, the resumed stream).

- `empty-pi3a-30s.bin`, `empty-pi3a-20s.bin` (Pi 3A+) and `empty-deck-30s.bin` (the deck): our own recordings of the same
  sensor at 9600 baud with nobody in front of it, 17432, 11651 and 13800 bytes (581, 388 and 460 frames). Every slot is
  empty in all of them, apart from a 0.13 s blip at the start of the deck file. Used by `test_ghost.cpp`.
