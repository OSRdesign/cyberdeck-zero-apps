# Cassette: the sound-server backend (PipeWire / PulseAudio through libpulse)

Design note for the second audio backend (task T1 of the Bluetooth release, 2026-11-11). It follows the Bluetooth
study (Option A: Cassette reaches Bluetooth devices through the sound server, `libpulse` loaded at run time like the
codecs) and its HackBerry measurements P1-P9 and items C15-C19.

**Status (2026-11-11).** The module is done and tested on its own: `src/core/pulse/` (no LVGL), tests in `tests/pulse/`,
the board CLI `tools/pulse_capture_test.cpp`. **T2 (capture) is wired into the controller** (section "What T2 uses"
below): a paired Bluetooth headset is an input next to the ALSA cards. **T3 (playback and the live monitor over Bluetooth, and
the worker thread for the server's questions) is wired in too** (section "What T3 uses"); **T4, the UI, shows it** (section
"What T4 shows"). The ALSA path
(including its `(server)` fallback through ALSA `default`, `alsa/open_policy.*`) is exactly as before.

## Files

| File | What it is |
|---|---|
| `src/core/pulse/pulse_abi.hpp` | the libpulse C ABI Cassette uses, declared by hand (constants, structures, callbacks): no pulseaudio header is needed to build |
| `src/core/pulse/pulse_lib.hpp/.cpp` | the function table, filled with `dlopen("libpulse.so.0")` / `dlsym` on first use; "libpulse not installed (libpulse0)" when it is missing |
| `src/core/pulse/pulse_api.hpp` | the seam: `PulseApi` (connect, lists, changes, open), the device and card records, the stream classes `PulseCapture` / `PulsePlayback`, the events, the error values, the open parameters shared by every implementation |
| `src/core/pulse/pulse_stream.cpp` | the shared parts: error texts, name and profile helpers, flags, properties, formats, buffer attributes from milliseconds, and the two stream classes (ring, events, silence detector, start timeout) |
| `src/core/pulse/real_pulse.cpp` | the production `PulseApi` on libpulse (threaded main loop) |
| `src/core/pulse/pulse_probe.hpp/.cpp` | a pure-data summary of the lists for choosers and the probe: Bluetooth or not, MAC, kind hint, music profile available, active profile |
| `src/core/pulse/pulse_pcm.hpp/.cpp` | a capture stream seen as an `AlsaPcm`, so the existing capture thread (`alsa/capture.*`) runs on it unchanged; T2: the optional `PulseLeadGate` (the connecting gate, below) |
| `src/core/pulse/bt_input.hpp/.cpp` | T2: the Bluetooth INPUT model: `bt:<MAC>` identity, which source to open (virtual or real), the call rate, the profile hint, per device |
| `src/core/pulse/bt_output.hpp/.cpp` | T3: the Bluetooth OUTPUT model: which sink to open for `bt:<MAC>`, label, kind, rate now, profile hint, whether a music profile is available |
| `src/core/playback/pulse_sink.hpp/.cpp` | T3: `PulsePlaybackSink`, a `PlaybackSink` on a `PulsePlayback` stream: the Player and the Monitor use it like the ALSA sink |
| `src/core/app/pulse_worker.hpp/.cpp` | T3: the thread that asks the server its questions (scan, card profile, identity) so the controller never waits for it |
| `tests/pulse/fake_pulse.hpp/.cpp` | `FakePulse`: a scriptable server for the unit tests |
| `tests/pulse/test_*.cpp`, `run.sh` | the unit tests (fake server), the real-library group, the ABI check runner |
| `tests/pulse/check_pulse_abi.cpp` | compile-time comparison of `pulse_abi.hpp` and `pulse_lib.hpp` with the real headers |
| `tests/pulse/real_pulse_test.cpp` | the real-server smoke test (lists; records when a source is named) |
| `tools/pulse_capture_test.cpp` | `pulse-capture-test`, the board CLI (lists, watches, records, plays a tone); not packaged |

The app build compiles the module (everything under `src/` is built) but links nothing new: `readelf -d` of the arm64
binary lists the same NEEDED entries as 0.2.0 (no libpulse, no `pa_` import: the library is only `dlopen`ed, and only when a
sound server runs).

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

## What T2 uses (the controller, `src/core/app/controller.*`)

Everything below is in the controller; the UI (T4) only reads the snapshot. `ControllerConfig::pulse` is the `PulseApi`
(`make_real_pulse()` in `bind/bound_app.cpp` for the real app, a `FakePulse` in the tests, null in the demo: then the
snapshot says "Bluetooth audio is not set up" and nothing else changes).

**Connection and discovery.** The controller connects (`connect("Cassette", 2000)`) only when `server_running()` (the
existing `ControllerConfig::server_running`, default `sound_server_running_here()`) is true, at start and then at most
every `pulse_retry_ms` (5 s) while there is no connection; it never starts a server. `kPulseMissing` / `kPulseNoServer` or no
running server: `CardSnap::bt_status` = "Bluetooth audio is not set up", no Bluetooth entries, USB cards unaffected;
`kPulseTimeout` or a failing list: "Bluetooth audio is not responding". It lists sources, sinks and cards
(`pulse_probe`) when the Settings page opens (`refresh_cards`), on every capture (re)open attempt and, after a server change,
from the service tick: the change callback only sets two atomics (`bt_dirty`, `bt_gone`), the controller thread re-lists at
most every `pulse_refresh_ms` (200 ms). A lost connection (`PulseChange::Kind::Disconnected`) is reconnected at once. Paired
but not connected devices are not listed: the sound server only knows connected ones (launcher facts, `bt_audio_dev`, are T8).

**The input list.** `bt_inputs()` (bt_input.cpp) gives one entry per Bluetooth device that has a microphone: a non-monitor source,
or a card offering a call profile (`headset-head-unit*`; without a source now it is listed unusable, "The headset microphone
is off"). Speakers (no microphone profile) and monitors are never listed. The chooser order is the ALSA cards, then the
Bluetooth devices; `CardSnap::labels` carries both and `CardSnap::inputs` has one `InputSnap` per label; `select_card(index)`
takes an index into them. A chosen headset that is not there stays in the list as a placeholder (`connected` false, its last
label). Identity `bt:AA:BB:CC:DD:EE:FF` (upper case) is the `card` setting and the key of the per-card map (`cards`); an old
settings file loads unchanged (the value is just a string). No hardware level: `has_level` is false, `set_level_db` refuses,
the soft gain works (`InputSnap::hw_level`, `CardSnap::bluetooth`).

**Which node, which rate.** The real source (`bluez_input.<MAC with underscores>.0`) when the card is on a call profile, else the
music-mode VIRTUAL source (`bluez_input.<MAC with colons>`, 48 kHz float: never opened at its own rate). The rate is asked
for explicitly, S16 mono: the real source's own rate when it is 8000 or 16000, else 16000 when the card offers
`headset-head-unit` (mSBC), 8000 when only `-cvsd` is available or active; the server resamples. `open_capture` (T1) does the
lookup, DONT_MOVE and the identity check at the open; `PulseCapture::source_identity_ok()` is checked again before a take
starts (refused: "The headset microphone could not be confirmed: the take did not start.") and every 3 s during one (a silent
relink ends and keeps the take like a loss). The stream becomes an `OpenResult` through `pulse_open_result(stream, 50, gate)`
and runs on the existing `Capture` / `ProcessingSink` / `RecordGate` / writer path: WAV, FLAC and Opus writers are unchanged at
8 and 16 kHz.

**Connecting (study C5, C18).** From music mode the stream says `Running` after ~25 ms but the first second is digital zero (the
profile switch). `PulseLeadGate` (pulse_pcm.hpp) sits in the capture PCM: while it is closed the leading frames that are exactly
zero are dropped (not written, not recording time), and audio is handed over from the first non-zero sample, which opens the
gate. The controller also opens it when the card's active profile is a call profile and the stream has run 300 ms
(`kBtCallLinkMs`; the card is asked every 100 ms): the zeros are then the headset's own. `CaptureSnap::connecting` is true
from the open until the gate opens. REC while connecting is allowed: the take starts at the first real audio and its time is 0
until then. The silence detector (T1: 3 s of exact zeros) is read as a state only once the gate is open
(`stats().silent && gate open`), so the profile switch can never raise it.

**No sound (C15).** `CaptureSnap::no_sound` and the notice "The headset is connected but sends no sound." (Info) on the rising
edge; the take keeps running, nothing auto-stops; the state and the notice clear when the first sound arrives (the Beats Fit Pro
case, P3).

**Loss.** `Killed`, `Moved` (and a lost connection) end the stream; the capture thread stops with `Unplugged` after the frames
before the end were written, the existing DeviceLost path closes and keeps the take: notice and `ProblemSnap::message` "The
Bluetooth headset disconnected: the take was closed and kept." ("... disconnected." when no take ran; "The sound server
stopped ..." for a lost connection). The controller never reopens on another device; `CaptureSnap::bt_gone` is true until the
chosen headset is back. REC with the chosen headset absent: "The Bluetooth headset is not connected."

**Reconnect.** The existing `replug_retry_ms` loop retries; a re-listing after a server change (the device-added event) tries at once.
A headset that connects after the app started appears in the list (the change flag); a chosen one is opened without a restart
("The Bluetooth headset is back" when it had been lost).

**Mic open policy (decision 5, C6).** The Bluetooth microphone is open on the recorder and settings screens, closed on the
list and the Edit page (`Controller::set_page(UiPage)`, called by the binder on every screen change) and while a take plays
or previews, so the headset returns to music mode; it opens again (connecting again) on return. Never closed during a take
or the countdown; closed after `AppLeave`. `CaptureSnap::mic_closed` says it is closed on purpose (`card.present` is false then).
USB cards keep their behaviour.

**Formats (C12).** `FormatInputs::speech_only` (set for a Bluetooth input) dims the Opus presets above 32 kbps ("Bluetooth call
audio is speech only: Opus up to 32 kbps"); speech presets, FLAC and WAV record at 8 / 16 kHz; the Sample rate row has the one
rate (`rates` = {16000}); a stored music preset falls back to WAV like any incompatible choice.

## What T3 uses (playback and the live monitor over Bluetooth)

Everything below is in the controller, `playback/pulse_sink.*`, `playback/output.*`, `playback/monitor.*` and `app/pulse_worker.*`; the UI (T4) reads the
snapshot. Nothing changes for an ALSA output or an ALSA monitor: the Player, the Monitor and their tests are the same, the new behaviour is
reached only through a `bt:<MAC>` choice.

### The server is asked on a worker thread

`PulseWorker` (`app/pulse_worker.*`) owns one thread and three kinds of job, each with one request in flight and one answer waiting:

| Job | Does | The controller uses it for |
|---|---|---|
| scan | (re)connect when the controller decided it is time (`pulse_retry_ms`, a lost connection at once), install the change callback, list sources, sinks and cards | `bt_list`, the Bluetooth outputs, `bt_status`; every re-listing |
| card | `card_by_name` | the connecting gate (call profile up), the wait for the music profile, the playback quality |
| identity | `source_identity_ok()` of the open capture stream | the 3 s guard during a take |

The controller's mutex is **never held across these round trips** (a hung server answers in up to 3 s per call). `bt_request_scan()` only posts the job; the
service tick takes the answer (`bt_apply_scan()`), rebuilds the lists and re-publishes. `start()` waits at most 0.3 s for the first scan so a chosen headset
opens at once; after that nothing waits. Measured on the fake with a server that takes 1.5 s per call: the slowest of 40+ commands (`refresh_cards`,
`set_soft_gain_db`, `snapshot`, `set_page`) stayed under 120 ms (a few ms in practice), `start()` returned in 0.3 s, a take recorded on the USB card through the hang.
What still waits for the server, by the user's own action: the open of a stream (`open_capture`, `open_playback`: playback and the monitor ask for a 0.4 s accept
timeout, a slower server gets a stream that is still connecting) and the identity check at the start of a take. Shutdown stops the worker first (a call in progress
is bounded by the library's 3 s).

### Output choices

`choose_output()` takes the Bluetooth outputs (`bt_outputs(probe)`, from the same scan as the inputs) and puts them between the cards and `Default output`: id `bt:AA:BB:CC:DD:EE:FF`
(upper case), label = the server's description, `kind`, `rate` (the sink's rate now: 48000 in music mode, 16000 or 8000 in call mode), `hint_profile` (`music`, `call`,
`unknown`), `connected`, `usable`, `reason`. The automatic choice is unchanged (the capture card's own playback side, else the default output). `output_device` accepts
`bt:<MAC>` (stored in upper case); a chosen output that is absent stays as a placeholder (`connected` false, the last label) and PLAY says "The Bluetooth output is not connected.";
nothing Bluetooth is listed while the server is unreachable (the same rule as the inputs), except that placeholder. Old settings files load unchanged.

### Playback

`PulsePlaybackSink` is a `PlaybackSink` over `PulsePlayback`; the Player is not aware of it.

- **Rate and format.** The stream is the TAKE's rate and channel count (1, or 2; more than two: the first two), S16 (24-bit takes are converted by the Player): the server resamples,
  the plughw rule does not apply. Verified sample-exact at 8, 16, 44.1 and 48 kHz.
- **Buffers.** `buffer_ms` 60 (the server target, `tlength`), client ring 100 ms, a 20 ms period. A 60 ms target keeps pause, seek and the 5x scan quick (what is queued is
  discarded on a change) while the 100 ms ring rides out a stalled SD card or a busy decoder; the server adds its own A2DP latency on top. T1's 100 ms default would make the
  counter and a pause lag more, 40 ms would risk underruns on a Zero 2 W under load. Constants: `PulseSinkOptions`.
- **DONT_MOVE** on the stream (T1), the sink looked up by name just before the open (`prepare_output()`), never `default`.
- **Underruns** are the server's: the next `write()` after the stream's underflow counter moved returns `-EPIPE` once and `recover()` succeeds, so `Player::underruns()` counts them as for ALSA
  (`snapshot().playback.underruns`). Underflows while nothing is written (a pause) are not underruns.
- **The counter** is the frame being heard: `delay()` = the stream's ring plus the server's latency report (which includes the A2DP device's own latency), and the Player already
  rewinds its position by it (the counter does not run ahead of the sound).
- **Pause: drop, keep the stream.** The stream API has no pause. Pause = stop writing and `drop()` (the client ring and the server buffer are flushed, `pa_stream_flush`); the stream stays
  open, the counter rewinds by what was queued, resume writes again from the counter. No cork and no reopen: a reopen costs a sink wake-up (the A2DP sink suspends when idle) and
  would be one more place to lose the stream. Seek, the 5x scan and the 5 s jump are the Player's own (they drop and rewrite), unchanged.
- **Disconnect during playback.** The stream ends (`Killed` / `Moved`), `write()` fails with `-ENODEV`, the Player reports `DeviceLost` and the deck pauses at the frame heard with the notice
  "The Bluetooth output disconnected." (Info kind `CardLost`); while paused the controller notices the ended stream itself (`Player::release_sink()`: the sink is closed, the position kept).
  Nothing is ever opened on another sink. PLAY again re-resolves the sink: gone means the notice "The Bluetooth output is not connected." and the deck stops. A server that takes nothing
  for 3 s counts as lost (`-EIO`), so the Player thread is never stuck.

### The same headset as input and output: waiting for the music profile (study C19)

PLAY (or REW / FWD from idle) with `output_device` = the headset that is also the input (or whose microphone stream is open):

1. The deck goes to Playing as usual; the controller defers the real start. `snapshot().playback.state` is `switching_to_music`, `switch_elapsed_s` runs, `switch_timeout_s` is 4.0,
   `playback.quality` is `call`. The microphone closes at once (T2 policy: playback is a closed-microphone state).
2. Every 100 ms the card's profile is read on the worker. Playback starts when a MUSIC profile is the active one. A microphone that was open also needs `saw_call` (a call reading was
   seen) or 0.9 s of waiting (the switch down takes 0.65 s, P4: an early "music" reading is not yet the answer). PAUSE, RESUME, REW / FWD (a scan) and STOP during the wait change what
   will happen at the end or cancel it; nothing is opened before.
3. After 4 s without music (`kMusicWaitMaxMs`) it plays anyway, on whatever sink exists: `quality` `call`, `switch_timed_out` true. PLAY is never stuck.
4. **No wait** when the headset has no AVAILABLE music profile (another A2DP device holds the link, P6; `music_available` false), when the output is another device (a speaker), or when the
   profile already is music: it plays at once. For the call-only headset `quality` is `call`.

While a take plays through a Bluetooth output the card's profile is read about once a second and `quality` follows it (`music`, `call`, `unknown`; `""` for any other output).

### The live monitor over Bluetooth

**Target.** The setting `monitor_output` (`""` automatic, `bt:<MAC>`, or an ALSA device as the output list spells it; absent from the file = automatic; default `""`):

| Input | `monitor_output` | Plays to | `monitor.route` |
|---|---|---|---|
| ALSA card with a playback side | `""` | the card's own playback side (as before) | `card` |
| ALSA card without one | `""` | nothing (as before) | `""` |
| Bluetooth headset | `""` | the SAME headset's sink (call mode: the mSBC / CVSD mono sink) | `same_headset` |
| any | `bt:<MAC>` | that sink (the same headset's: `same_headset`; another device: `other_output`) | |
| any | an ALSA device | that device (the capture card's own: `card`; else `other_output`) | |

The `Monitor` setting stays Off by default (a Bluetooth input starts with it off: it runs only when the user turned it on; the global default is unchanged). A local microphone to a Bluetooth sink needs the
explicit `monitor_output = bt:<MAC>`.

**Path.** The capture stream's frames go through `ProcessingSink` to the `MonitorTap` ring (as for ALSA), the monitor thread reads it and writes a `PulsePlaybackSink` opened with
`pulse_monitor_sink_options()`: server target 40 ms, client ring 20 ms, 16 ms period, role `production`, DONT_MOVE, the capture's rate (16000 / 8000: the server resamples to the sink). The capture side is
the T1 stream with 20 ms fragments (`PulseCapturePcm::readi` hands over whatever is there, not 1024-frame periods). `MonitorOptions` (monitor.hpp) switch on what a server sink needs: `prefill_ms` 10
(silence after an open or an underrun, instead of 512 frames), `max_queue_ms` 2000 (the stream's delay includes the device's own latency, never a reason to drop), and the drift control below.
The ALSA monitor uses the defaults of `MonitorOptions` and behaves exactly as before.

**Drift policy.** The capture stream and the playback stream run on different clocks (the server rate-matches each to its own device, so the drift is small: the policy is a safety net, tested on a fake
with 1 % and 2 % drift):

- the sink is SLOWER than the capture: the tap's ring backlog grows. Above `drift_target_ms + drift_band_ms` (15 + 5 ms) the thread drops ONE frame of the next chunk (a skip: at most 1 frame in 128 to 256,
  i.e. under 0.8 % speed change, inaudible on speech) until the backlog is inside the band; the hard resync above `max_backlog_ms` (60 ms: the oldest audio is dropped) stays as the last resort;
- the sink is FASTER: it runs dry. The underrun is recovered (`write()` returns `-EPIPE` once) and the device is refilled with `prefill_ms` (10 ms) of silence: an insert of 10 ms every margin/drift seconds
  (a 40 ms margin and 100 ppm of drift: one gap in 400 s);
- counted in `Monitor::stats()`: `drift_skips`, `underruns`, `resyncs`, `backlog_frames`.

**Delay figure.** `monitor.delay_ms` = the capture stream's latency (`PulseCapture::latency`, the server's report; a card's capture counts its 1024-frame period) + the tap ring's fill + the playback stream's delay
(ring + the server's report, `PulsePlaybackSink::delay_ms`). First value 0.3 s after the monitor starts, then once a second; 0 while it is not running. `monitor.notice` is `delay` when the route is
`other_output` to a Bluetooth sink on a music profile (A2DP: about 0.2-0.3 s, "you will hear an echo"), or while the figure is above 150 ms; else `""`. The notice for an A2DP target is already there before the
monitor is switched on (the Settings row can say it). The measured figure for the Evolve 65 is the owner's (a step in the T3 report); the expectation is 80-150 ms on the same headset.

**When it runs.** `monitor_wanted()`: capture running, Monitor setting on, a target exists, not after AppLeave, no playback / preview / trim (as before), and for a Bluetooth input the microphone is NOT connecting
(`CaptureSnap::connecting` false: the profile switch is over). So it is off while connecting, off during playback (the microphone is closed then), and starts by itself after the profile switch when the microphone
comes back. If the monitor's Bluetooth sink disappears (the stream ends, or the lists drop it) the monitor stops with the notice "The monitor output disconnected: the recording goes on."; the capture and the
take are never touched (the tap never blocks: a full ring drops); it restarts by itself when the sink is back while Monitor is still on. A headset that has sidetone: nothing to do (the guide says so).

### Snapshot fields for the UI (T4)

All additive; a UI that ignores them compiles and behaves as before.

- `card.outputs[]` (`OutputSnap`: `id`, `label`, `bluetooth`, `kind`, `rate`, `connected`, `usable`, `hint_profile`, `reason`), parallel to `output_labels` / `output_devices`; `card.output_selected` as before.
  `select_output(id)` accepts `bt:<MAC>`.
- `playback.state` (`idle`, `switching_to_music`, `playing` = a take plays, is paused or scans: the deck says which), `playback.quality` (`""`, `music`, `call`, `unknown`), `playback.bluetooth`,
  `playback.switch_elapsed_s`, `playback.switch_timeout_s` (4.0), `playback.switch_timed_out`, `playback.underruns`.
- `monitor.active`, `monitor.route` (`same_headset`, `other_output`, `card`, `""`), `monitor.delay_ms`, `monitor.notice` (`""`, `delay`), `monitor.output` (the setting), `monitor.available`, `monitor.target_label`,
  `monitor.reason` (no target), `monitor.outputs[]` (the choice list: `[0]` = automatic with id `""`, then every output; an absent choice is listed with `connected` false), `monitor.output_selected`;
  `set_monitor_output(id)` (false for a bad `bt:` address).
- `card.has_playback` now means "the Monitor settings make sense": a target exists (the capture card's playback side, the same headset, or an explicit target).

## What T4 shows (the UI)

The binding (`bind/view_map.*`) turns the snapshot fields above into the view model; the texts are built in `ui/ui_format.*`
(`bt_input_entry`, `bt_output_entry`, `bt_input_footer`, `monitor_delay_text`, `switching_text`, the notice constants), so the
demo states (`CASSETTE_DEMO_BT`, `ui/demo.hpp`) use the same words. Tests: `tests/ui/test_bt.cpp`, `tests/bind/test_bt_map.cpp`;
scenes `tests/scenes/bt1-*` to `bt12-*` (the seven layouts each).

- **Pickers.** `SettingRow::choice_entries` (one `FormatEntry` per choice) on Input, Output and Monitor to: a Bluetooth device is the
  Format picker's two-line row (name, rate on the right for an input, line 2 the kind and quality, or the reason, dimmed, "Not
  available: <reason>" on a tap), every other entry the plain one-line row as before (no `choice_entries` at all when no Bluetooth
  device is listed). The Input picker's footer (`formats_footer`): the speech-quality line, or "Bluetooth audio is not set up on this
  board" when `bt_status` is the not-set-up text (the Input row then opens even with no card). Input level is a greyed Info row
  ("Not applicable") for a Bluetooth input.
- **Monitor.** `SettingRow::detail` / `detail_warn`: the delay line under Monitor (amber with `monitor.notice == "delay"`). Monitor to
  appears where Bluetooth is in play (a Bluetooth input or output, or a target chosen earlier); without a target it is drawn greyed
  (`SettingRow::dimmed`) with `monitor.reason` as its value and still opens. `set_monitor_output` is the Port's.
- **Recorder.** `RecorderVm::bluetooth` (BT for IN), `RecorderVm::standing_notice` (connecting, no sound: in the notice place while no
  timed notice shows; a tap hides it until it changes), `InfoVm::play_status` (the switching line: the landscape strip's time slot; the info box is too narrow for it, so portrait and square show it in the notice place like a standing notice). The
  controller's own notices (the headset gone, not connected, refusals) pass through the timed notice queue unchanged; the binding adds
  the call-quality notice once per play (`CallQualityWatch`).

### Constants (one place each)

`PulseSinkOptions` (playback 60 / 100 / 20 ms, accept timeout 0.4 s, stuck 3 s), `pulse_monitor_sink_options()` (40 / 20 / 16 ms), `MonitorOptions` (prefill 10 ms, drift 15 + 5 ms),
`kMusicWaitMaxMs` 4000, `kMusicWaitMinMs` 900, `kQualityPollMs` 1000, `kMonitorNoticeDelayMs` 150 (controller.cpp).

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
- the ABI check against `PULSE_INCLUDES` (or `/usr/include` with libpulse-dev);
- T2: `test_bt_input.cpp` (identity, music-mode virtual source, call-mode real source, the rate rules, devices without a source,
  the connecting gate) here, and `tests/app/test_bt.cpp` on the whole controller with FakePulse (`tests/app/run.sh`, or
  `test-cassette-app --bt` for that group alone): list, music-mode open at 16 kHz, call mode, CVSD 8 kHz, the profile-switch
  zeros dropped, the call-profile rule and the no-sound notice, killed and moved mid-take, identity refusals, absent headset,
  hot-plug and reconnect, server missing / not running / hung / late, the mic policy on every page and in playback, settings
  round trip and an old file, Opus presets at 16 kHz;
- T3: `test_bt_output.cpp` (the output model) here; `tests/playback/test_pulse_sink.cpp` (the Player on `PulsePlaybackSink`: sample-exact at 8 / 16 / 44.1 / 48 kHz,
  the position counter, underruns, pause / resume, scan, jump, a sink that goes away, a server that takes nothing) and `test_pulse_monitor.cpp` (the monitor with drifting
  playback clocks: same clocks, 1 % slow, 2 % fast, the sink lost); `tests/playback/test_output.cpp` (the output list with Bluetooth, placeholders, no server);
  `tests/takes/test_settings.cpp` (`bt:` output, `monitor_output`, an old file); `tests/app/test_bt_out.cpp` on the whole controller (`test-cassette-app --bt` runs it with T2's):
  the output list and its placeholder, playback at 8 / 16 / 48 kHz, pause and scan, a disconnect while playing and while paused, the wait for the music profile against a
  fake card that follows the microphone (650 ms down, 1850 ms up), the 4 s timeout, the call-only headset, STOP during the wait, the monitor on the same headset (data,
  buffers, delay from the fakes' latencies, the notice), not while connecting, off during playback and back, a local microphone to a Bluetooth speaker, a lost monitor sink while
  the take records on, and a server that takes 1.5 s per call (the controller stays responsive).

A private root needs no install: `apt-get download libpulse-dev libpulse0 libsndfile1 libasyncns0 libflac12t64 libogg0
libopus0 libvorbis0a libvorbisenc2 libmpg123-0t64 libmp3lame0`, `dpkg-deb -x` each into one folder, then
`PULSE_INCLUDES=<root>/usr/include PULSE_LIBPATHS=<root>/usr/lib/x86_64-linux-gnu:<root>/usr/lib/x86_64-linux-gnu/pulseaudio`.

The board CLI: `make -C tests/pulse BUILD=<dir> CXX=aarch64-linux-gnu-g++ CXXFLAGS="-std=c++17 -O2" cli`, copy
`pulse-capture-test` to the board, run it as the desktop user (no sudo). `--source NAME` records (listen-only unless
`--wav FILE`), `--sink NAME --tone` plays, `--watch S` prints the changes, `--no-lookup` shows the server's own handling
of a name.
