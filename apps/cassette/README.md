# Cassette (`cassette`)

A sound recorder for the cyberdeck: WAV (Broadcast Wave) takes written so that a power cut loses at most one sync
interval of audio. Plan: launcher `docs/dev/tasks/016-cassette-recorder.md`; study: report 058.

Status 0.0.1: only `src/core` (no UI, no package yet): the SPSC ring, the WAV/BWF/RF64 header, the writer thread,
the journal and the repair step. The core is LVGL-free and needs no sound hardware.

Run the tests: `wsl -e sh apps/cassette/tests/run.sh`
