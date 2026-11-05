# Test fixtures (our own recordings)

Recorded on 2026-11-06 from the owner's HLK-LD2452 on a CH340 USB-serial board at 9600 baud 8N1 (study report 056,
section 3.5). They are our own data, MIT like the rest of the app.

- `ld2452-9600-walk-15s.bin`: 8700 raw bytes = 290 data frames; `.csv`: the same frames decoded (the golden file).
- `ld2452-9600-config-session.bin`: 1778 raw bytes of one read-only configuration session (a data frame cut short by
  the command answer, the damaged first byte, silence, the end answer, the resumed stream).
