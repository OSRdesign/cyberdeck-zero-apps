# Cassette: the sound-server backend (PipeWire / PulseAudio through libpulse)

Design note for the second audio backend (task T1 of the Bluetooth release, 2026-11-11). It follows the Bluetooth
study (Option A: Cassette reaches Bluetooth devices through the sound server, `libpulse` loaded at run time like the
codecs) and its HackBerry measurements P1-P9 and items C15-C19.

**Status (2026-11-11).** The module is done and tested on its own: `src/core/pulse/` (no LVGL), tests in `tests/pulse/`,
the board CLI `tools/pulse_capture_test.cpp`. **Nothing calls it yet**: the controller, the settings, the pickers and the
UI are unchanged and the ALSA path (including its `(server)` fallback through ALSA `default`, `alsa/open_policy.*`) is
exactly as before. T2 wires capture into the controller, T3 playback and the monitor, following the contract at the end
of this note.

## Files

| File | What it is |
|---|---|
| `src/core/pulse/pulse_abi.hpp` | the libpulse C ABI Cassette uses, declared by hand (constants, structures, callbacks): no pulseaudio header is needed to build |
| `src/core/pulse/pulse_lib.hpp/.cpp` | the function table, filled with `dlopen("libpulse.so.0")` / `dlsym` on first use; "libpulse not installed (libpulse0)" when it is missing |
| `src/core/pulse/pulse_api.hpp` | the seam: `PulseApi` (connect, lists, changes, open), the device and card records, the stream classes `PulseCapture` / `PulsePlayback`, the events, the error values, the open parameters shared by every implementation |
| `src/core/pulse/pulse_stream.cpp` | the shared parts: error texts, name and profile helpers, flags, properties, formats, buffer attributes from milliseconds, and the two stream classes (ring, events, silence detector, start timeout) |
| `src/core/pulse/real_pulse.cpp` | the production `PulseApi` on libpulse (threaded main loop) |
| `src/core/pulse/pulse_probe.hpp/.cpp` | a pure-data summary of the lists for choosers and the probe: Bluetooth or not, MAC, kind hint, music profile available, active profile |
| `src/core/pulse/pulse_pcm.hpp/.cpp` | a capture stream seen as an `AlsaPcm`, so the existing capture thread (`alsa/capture.*`) runs on it unchanged |
| `tests/pulse/fake_pulse.hpp/.cpp` | `FakePulse`: a scriptable server for the unit tests |
| `tests/pulse/test_*.cpp`, `run.sh` | the unit tests (fake server), the real-library group, the ABI check runner |
| `tests/pulse/check_pulse_abi.cpp` | compile-time comparison of `pulse_abi.hpp` and `pulse_lib.hpp` with the real headers |
| `tests/pulse/real_pulse_test.cpp` | the real-server smoke test (lists; records when a source is named) |
| `tools/pulse_capture_test.cpp` | `pulse-capture-test`, the board CLI (lists, watches, records, plays a tone); not packaged |

The app build compiles the module (everything under `src/` is built) but links nothing new: `readelf -d` of the arm64
binary lists the same NEEDED entries as 0.2.0 (no libpulse, no `pa_` import), and until T2 calls the module the linker
leaves it out (the arm64 binary is byte-for-byte the size of 0.2.0's).

## Why libpulse, loaded at run time

- One client library reaches PipeWire (through `pipewire-pulse`, present on the desktop images: the HackBerry runs
  PipeWire 1.4.2 with it) and PulseAudio (the FZ-M1 tablet) alike.
- It has the flag Cassette needs most: `PA_STREAM_DONT_MOVE` (below), plus server-side introspection of cards and
  profiles (C17).
- `dlopen` keeps the binary free of a NEEDED entry: on a board without a sound server (the appliance boards) Cassette
  starts and records through ALSA exactly as before; `connect()` answers `kPulseMissing` ("libpulse not installed
  (libpulse0)") or `kPulseNoServer` ("No sound server is running (PipeWire or PulseAudio)").
- `connect()` uses `PA_CONTEXT_NOAUTOSPAWN`: it never starts a PulseAudio daemon. The HackBerry has the `pulseaudio`
  package installed but not running; an autospawned PulseAudio would take the Bluetooth endpoints away from PipeWire.

The ABI is checked like the codecs' (`tests/codec/check_codec_abi.cpp`): `tests/pulse/check_pulse_abi.cpp` compares every
constant, structure size and field offset, and the shape of every function and callback with the real headers
(PulseAudio 16.1 checked, from a private root: `PULSE_INCLUDES`). libpulse's ABI has been stable since 1.0 and only
appends fields to the info structures, which Cassette reads through the library's pointer.

## Threading model

```
libpulse main-loop thread (pa_threaded_mainloop, one per connection)
   read callback   -> PulseCapture::feed_frames  -> ring (lock-free, bounded) --+--> owner's capture thread: read()
   write callback  <- PulsePlayback::take        <- ring (lock-free, bounded) <-+--- owner's playback thread: write()
   state / moved / suspended / underflow callbacks -> the stream's event queue -----> owner's thread: poll_event()
   subscribe callback -> the owner's change callback (set a flag, re-list later)
```

- Every owner call into `RealPulse` takes the main-loop lock and waits for the server in 1 ms naps with the lock
  dropped, bounded by a timeout (3 s for an introspection round trip, `ready_timeout_ms` for an open): a hung server
  never hangs the app.
- The **data path** never allocates, never locks a mutex and never blocks: the read callback peeks and copies into the
  stream's ring (`cassette::Ring`, the same SPSC ring the recorder uses) and counts what does not fit; the playback pump
  copies from the ring into `pa_stream_write`. The silence detector runs in the same callback (a backward scan of each
  chunk for the last non-zero sample).
- The **state path** (ready, moved, suspended, killed, failed) is rare and pushes onto a small mutex-guarded queue.
- A waiting `read()` is woken by the producer through a condition variable notified without its mutex; a missed wake
  costs at most one 5 ms slice.
- **Shutdown barrier:** `PulseStreamLink::close()` (called by `PulseCapture::close()` / `PulsePlayback::close()` and the
  destructors) clears the stream's libpulse callbacks under the main-loop lock before it disconnects. Once it returns no
  feed call can reach the stream. `PulseApi::disconnect()` and `set_change_callback()` are the same barrier for the change
  callback: they swap it under the main-loop lock, so after they return the old callback is not running and is not called
  again. A stream that outlives `disconnect()` holds the (dead) connection until it is closed and ends with
  `Killed(kPulseDisconnected)`.
- Under ThreadSanitizer and AddressSanitizer + UBSan the suite is clean (`tests/pulse/run.sh SAN=thread`,
  `SAN=address,undefined`).

## Buffer sizing

All buffer sizes are asked for in **milliseconds** and converted to bytes for the stream's own rate and channels
(`pulse_ms_to_bytes`: whole frames, rounded up). The old frame-sized constants were three times longer in time at 16 kHz
than at 48 kHz (study C4); here a fragment is 20 ms at every rate.

| Stream | Field | Default | Why |
|---|---|---|---|
| capture | `fragsize` | `fragment_ms` = 20 ms | the server hands over about this much at a time (640 bytes at 16 kHz mono); the server may grant less (WSLg's PulseAudio gave 10 ms) |
| capture | `maxlength` | `server_max_ms` = 2000 ms (at least two fragments) | the server-side limit while the client is late |
| capture | client ring | `ring_ms` = 2000 ms | what a late reader (an SD-card stall on the writer side) can lag; overflow drops the newest frames and is counted (`frames_overflow`) |
| playback | `tlength` | `buffer_ms` = 100 ms (the monitor wants 20-40 ms) | the server-side target; prebuf, minreq, maxlength left to the server |
| playback | client ring | `ring_ms`, 0 = `buffer_ms` | what `write()` fills; the pump moves it to the server as soon as there is room |

Flags on every stream: `DONT_MOVE`, `ADJUST_LATENCY` (the server sizes its own buffers to the target),
`INTERPOLATE_TIMING` and `AUTO_TIMING_UPDATE` (latency figures without a round trip); `FAIL_ON_SUSPEND` on request.

## Events

Every stream reports a single ordered sequence of `PulseEvent`s through `poll_event()` (the owner polls, like
`Player::poll_event`; nothing is called back on the server's thread):

| Event | When | The stream |
|---|---|---|
| `Connecting` | always first (t = 0) | open, no frame yet |
| `Running` | capture: the first frame arrived (`t` = its time); again after `Suspended`. Playback: the server accepted the stream | live |
| `StartTimeout` (code `kPulseTimeout`) | capture only: no frame `first_frame_timeout_ms` (3000 ms) after the open | **still open**: `Running` follows if frames come later |
| `SilenceDetected` | capture only: `silence_seconds` (3 s) of samples that are exactly 0 | live |
| `SilenceCleared` | capture only: the first non-zero sample after it | live |
| `Suspended` | the server suspended the device | live, no frames until `Running` |
| `Moved` (code `kPulseMoved`, `device` = where it went) | the server attached the stream to another device, at the open or later | **ended**; no frame of the other device is handed over |
| `Killed` (code `kPulseKilled`, `kPulseNoDevice` or `kPulseDisconnected`) | the device went away, or the connection to the server was lost | **ended** |
| `Error` (code) | any other failure | **ended** |

The first end wins (a kill after a move changes nothing). After an end, `read()` still returns every frame received
before it, then the end code. The silence events carry the exact frame of the transition (`frame`) and its stream time;
when several transitions happen between two polls they are all reported, in order.

### DONT_MOVE: never another microphone

P5 measured the risk: `parecord` with an explicit device name was **moved by pipewire-pulse to another source** when the
Evolve 65 was switched off mid-take (speech to 13 s, then zeros, the file looked complete) and when the device was not
there at the start (26 s of zeros from the speaker's monitor). Cassette guards in four layers:

1. **Look up first.** `open_capture()` asks the server for the exact source; absent means `kPulseNoDevice` and no stream
   at all. Monitor sources are refused (`kPulseRefused`): they are never a microphone.
2. **`PA_STREAM_DONT_MOVE`** on every stream, plus the PipeWire node properties `node.dont-reconnect=true`,
   `node.dont-fallback=true` and `node.dont-move=true` (pipewire-pulse copies stream properties onto its node; WirePlumber
   then destroys the stream when its target goes away instead of relinking it).
3. **The move callback ends the stream** (`Moved`), disconnects it, and every frame that arrives after the move is
   discarded (counted in `frames_after_end`).
4. **The identity guard** `source_identity_ok()` asks the server which source the stream's source output is attached to
   *now* (`pa_context_get_source_output_info` -> source index -> name; falls back to `pa_stream_get_device_name`), so a
   silent relink without a move notification is caught too (study C16). The same check runs when the server accepts the
   stream: attached elsewhere means `kPulseMoved` from the open (closed again) or a `Moved` event.

Measured on the HackBerry (PipeWire 1.4.2, 2026-11-11): a capture connected to a source name that does not exist, with
the lookup skipped so the server alone decides (`pulse-capture-test --no-lookup`), was **refused by pipewire-pulse**
(stream failed, "no such entity" -> `kPulseNoDevice`, no source output left behind), where P5's `parecord` without
these flags ran on another source. The mid-take case (the headset switched off during a capture) needs the owner and is
in the board steps of the T1 report.

### Silence detection (P3)

The Beats Fit Pro connects, its source runs (PipeWire reports RUNNING) and it delivers **pure digital zeros**: the call
audio link never opened (P3, P7). A take from it would be silent and look fine. The detector counts the run of samples
that are **exactly 0** in every channel; at `silence_seconds` it reports `SilenceDetected`. A low level is not silence:
one LSB of noise (+/-1) never triggers it, and neither does a single non-zero sample every 2.9 s. Frames that never come
are not silence either: that is `Connecting` / `StartTimeout`. 3 s is above the ~1 s of zeros the Evolve 65 delivers
right after the profile switch (P4: "the first second of the file is silent"), so a working headset does not trigger
it. Measured on the HackBerry: a real capture from PipeWire's dummy monitor (digital silence) gave `SilenceDetected` at
t = 3.035 s, frame 48000 at 16 kHz.

### Start delay and profiles (P4, P6)

- From music mode the headset microphone costs about **0.65 s** to open (A2DP -> headset-head-unit), and the profile
  returns to A2DP **1.85 s** after the microphone closes (P4). The stream is `Connecting` (with `connecting_seconds()`)
  until the first frame; `StartTimeout` comes only at 3 s, so a normal switch never shows as an error.
- Only **one A2DP device at a time** on the HackBerry (P6): with the XWF speaker connected, the Evolve 65 had call
  profiles only. `list_cards()` reports every profile with its `available` flag and the active one; `pulse_probe()`
  turns that into `music_available` / `active_is_music` / `active_is_call` per Bluetooth card (C17), so the output
  chooser can say "call quality" when no music profile is available.
- The default source follows the most recently connected headset (P8): Cassette never opens "default"; every stream
  names its exact device (`"default"`, `@DEFAULT_SOURCE@` and empty names are refused).

## The contract for T2 (capture) and T3 (playback, monitor)

**Connection.** One `std::unique_ptr<PulseApi> api = make_real_pulse()` per controller. Call
`api->connect("Cassette", 2000)` only when `sound_server_running_here()` (alsa/open_policy.hpp) is true or at the
user's request; treat `kPulseMissing` and `kPulseNoServer` as "Bluetooth audio is not set up on this board" (no error).
Never call it from the change callback.

**Discovery.** `list_sources()`, `list_sinks()`, `list_cards()`, `server_info()` (each one round trip, at most 3 s),
then `pulse_probe(...)` for the facts a chooser needs: `bluetooth`, `mac` (the identity to store: `bt:<MAC>`, study
C2; the node name changes with the profile, the MAC does not), `kind`, `rate`, `monitor` (never offer a monitor as an
input), and per card `music_available`, `active_is_music`, `active_is_call`. Follow changes with
`set_change_callback([&](const PulseChange &) { dirty = true; })` and re-list from `tick()` when `dirty`; a
`PulseChange::Kind::Disconnected` means the server went away (reconnect later).

**Capture (T2).**

```cpp
PulseCaptureRequest rq;
rq.source = node_name;            // from list_sources(), found by MAC
// rq.rate = 0, rq.channels = 0:   the source's own (16000 / 8000 for a headset); no resampling
std::unique_ptr<PulseCapture> cap;
std::string why;
int r = api->open_capture(rq, cap, &why);   // kPulseNoDevice: not there (never another device); kPulseMoved: refused
std::shared_ptr<PulseCapture> stream(std::move(cap));   // keep a share for the events
OpenResult o = pulse_open_result(stream);               // via_server = true, format = rate/channels/16
// then exactly as today: MonitorTap / ProcessingSink on o.format, Capture(std::move(o), *proc, copt), start()
```

- The capture thread reads through `PulseCapturePcm::readi` (50 ms waits). A lost, moved or disconnected device ends
  `Capture` with `CaptureError::Unplugged`, so the existing `on_capture_lost()` / `DeviceLost` path ends and keeps the
  take, after the last frames received before the end were written (C9). Write the Bluetooth wording from the event.
- From `tick()`, drain `stream->poll_event(ev)`:
  - `Connecting` / `StartTimeout`: show the connecting state (C5, C18); do not count time as recorded before `Running`
    (hold REC, or start the take at the first frame: owner decision).
  - `SilenceDetected` / `SilenceCleared`: the "no signal" notice and meter state (C15).
  - `Moved` / `Killed` / `Error`: the capture thread stops by itself; the event says why.
- Call `stream->source_identity_ok()` before starting a take and, if wanted, every few seconds during it (one server
  round trip, from the owner's thread only).
- Reopen after a loss: on a `PulseChange` (source new) or the existing `replug_retry_ms` timer, re-list and open the
  node with the same MAC (C10). An ended stream never resumes; open a new one.
- `stream->format()` gives the granted fragment and the device's own rate; `stream->latency(l)` the server's latency
  (`l.total_ms`, `l.device_ms`).

**Playback and monitor (T3).**

```cpp
PulsePlaybackRequest rq;
rq.sink = sink_name;   rq.rate = take_rate;   rq.channels = take_channels;   // the server resamples to the sink
rq.buffer_ms = 100;                                                           // the monitor: 20-40
std::unique_ptr<PulsePlayback> pb;
int r = api->open_playback(rq, pb, &why);
long n = pb->write(frames, count, 100);   // > 0 queued, 0 no room in time, < 0 ended (the end code)
pb->drain(2000);  pb->drop();  pb->delay_frames();  pb->underruns();  pb->latency(l);  pb->poll_event(ev);
```

- `write()` blocks at most `timeout_ms` while the ring is full and is the natural pacing of a playback thread; a
  `PlaybackSink` adapter over these calls (write / recover / delay / drop / drain / close) is T3's to write.
- Underruns are counted, not returned (the server handles them: with the default prebuf it waits for a full target
  before it plays again). A monitor that sees `underruns()` grow can raise `buffer_ms`.
- `delay_frames()` = the ring plus the server's latency report: the playback counter's compensation over A2DP (study,
  monitor design point 5). `sink_identity_ok()` mirrors the capture guard.
- There is no pause: `drop()` and a new `write()` (or close and reopen).

**Do not:** open "default"; open a monitor source as an input; call the PulseApi or a stream from the change callback;
call `close()` while another thread is inside `read()` / `write()` of the same stream (stop that thread first, as
`Capture::stop()` does); keep a raw `PulseCapture *` once it is handed to `pulse_open_result` (keep the `shared_ptr`).

## Tests

`wsl -e sh tests/pulse/run.sh [SAN=thread|SAN=address,undefined]`:

- the unit tests on `FakePulse` (always): library missing, server down or hanging, the error mapping, the millisecond
  maths, flags and properties; listing, the change callback and the Bluetooth summary on a HackBerry-like set; capture at
  8000, 16000 and 48000 Hz (rate 0 = native) and the refusals; DONT_MOVE honoured (kill) and ignored (move: `Moved`, no
  foreign frames); suspend and `fail_on_suspend`; the identity guard (silent relink, attached elsewhere at the open,
  deferred acceptance); errors and a lost server; the start timeout; silence thresholds (exact 3 s boundary, +/-1 LSB
  noise, sparse samples, stereo with one silent channel, several transitions between polls, off); the bounded ring's
  overflow count; playback write / underruns / drop / drain / delay; threads (a feeding server and a reader, close
  racing both, no change callback after `disconnect()`); the existing `Capture` thread running on a pulse stream;
- the real-library group when `libpulse.so.0` loads (`PULSE_LIBPATHS`): every symbol, and `kPulseNoServer` for a server
  that is not there;
- `real-pulse-test`: the real server's lists (skipped without libpulse or a server; on a WSLg PC it lists WSLg's
  PulseAudio);
- the ABI check against `PULSE_INCLUDES` (or `/usr/include` with libpulse-dev).

A private root needs no install: `apt-get download libpulse-dev libpulse0 libsndfile1 libasyncns0 libflac12t64 libogg0
libopus0 libvorbis0a libvorbisenc2 libmpg123-0t64 libmp3lame0`, `dpkg-deb -x` each into one folder, then
`PULSE_INCLUDES=<root>/usr/include PULSE_LIBPATHS=<root>/usr/lib/x86_64-linux-gnu:<root>/usr/lib/x86_64-linux-gnu/pulseaudio`.

The board CLI: `make -C tests/pulse BUILD=<dir> CXX=aarch64-linux-gnu-g++ CXXFLAGS="-std=c++17 -O2" cli`, copy
`pulse-capture-test` to the board, run it as the desktop user (no sudo). `--source NAME` records (listen-only unless
`--wav FILE`), `--sink NAME --tone` plays, `--watch S` prints the changes, `--no-lookup` shows the server's own handling
of a name.
