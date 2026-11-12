/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette: the sound-server seam (PipeWire through pipewire-pulse, or PulseAudio), the second audio backend next to the
 * ALSA one (alsa/alsa_api.hpp). PulseApi is everything the app needs from the server: connect, list the sources, sinks and
 * cards with the facts a Bluetooth device carries, follow changes, and open one capture or playback stream on an EXACT
 * device. real_pulse.cpp implements it through libpulse.so.0 loaded at run time (pulse_lib.hpp: no link-time dependency);
 * tests/pulse/fake_pulse.* implements it for the PC tests. LVGL-free. The design and the contract for the controller are in
 * docs/PULSE.md.
 *
 * Streams. PulseCapture and PulsePlayback are concrete: the bounded ring, the event logic, the silence detector and the
 * start-timeout live here (pulse_stream.cpp) and are the same whichever server end feeds them. The server end is a
 * PulseStreamLink (real_pulse.cpp, or the fake) that calls the feed_*() methods from its own thread:
 *   - the data path (feed_frames, take) never allocates, never locks a mutex and never blocks;
 *   - the state path (feed_ready, feed_moved, feed_suspended, feed_end) is rare and may take a short mutex;
 *   - after PulseStreamLink::close() returns no feed call happens any more (close() is the shutdown barrier).
 * Every stream is opened with PA_STREAM_DONT_MOVE and the PipeWire properties node.dont-reconnect / node.dont-fallback, so
 * the server ends the stream instead of moving it to another device (study P5: pipewire-pulse silently moved a parecord
 * to another source mid-take and at start); a move that happens anyway ends the stream with Moved, and no frame from the
 * other device is ever handed to the owner.
 *
 * Error values are negative: the kPulse* values below (pulse_error_text explains them).
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace vinyl {

class Ring;

constexpr int kPulseMissing = -100200;      /* libpulse.so.0 is not installed or lacks a symbol */
constexpr int kPulseNoServer = -100201;     /* no sound server answers for this user (not running, or refused) */
constexpr int kPulseNoDevice = -100202;     /* the source or sink named does not exist (now) */
constexpr int kPulseKilled = -100203;       /* the server ended the stream: its device went away (or was suspended,
                                               with fail_on_suspend) */
constexpr int kPulseMoved = -100204;        /* the stream was attached to another device than the one asked for */
constexpr int kPulseDisconnected = -100205; /* the connection to the sound server was lost */
constexpr int kPulseTimeout = -100206;      /* the server did not answer in time (or no frame came, StartTimeout) */
constexpr int kPulseFailed = -100207;       /* any other server error (PulseApi::last_error() has the server's text) */
constexpr int kPulseState = -100208;        /* a call out of order: not connected, stream closed */
constexpr int kPulseRefused = -100209;      /* the server refused the parameters (rate, channels, buffer) */
std::string pulse_error_text(int err);
/* The libpulse error code (pa_error_code_t, positive) as a Cassette value. */
int pulse_error_from_pa(int pa_errno);

constexpr uint32_t kPulseNoIndex = 0xFFFFFFFFu;  /* PA_INVALID_INDEX */
constexpr uint32_t kPulseDefault = 0xFFFFFFFFu;  /* a buffer field the server chooses */

/* ------------------------------------------------------------------------------------------------ devices and cards */

enum class PulseDeviceState { Unknown, Running, Idle, Suspended };

/* One source (microphone, or a sink's monitor) or one sink, with the facts Cassette needs. The property fields are empty
 * when the server does not set them; both spellings in use are read (PipeWire's api.bluez5.* / device.form-factor and
 * PulseAudio's bluetooth.* / device.form_factor). */
struct PulseDevice {
    uint32_t index = kPulseNoIndex; /* this session's index: NEVER stored, a device is found again by name */
    std::string name;               /* "bluez_input.70_BF_92_53_6B_8B.0", "alsa_input.usb-..." */
    std::string description;        /* "Evolve 65" */
    uint32_t rate = 0;              /* the device's own sample spec (record at this rate: no resampling) */
    unsigned channels = 0;
    std::string format;             /* "s16le", "float32le", ... */
    bool is_monitor = false;        /* sources: the monitor of a sink (what the sink plays, never a microphone) */
    std::string monitor_of;         /* sources: the sink a monitor belongs to */
    uint32_t card = kPulseNoIndex;  /* the owner card's index, kPulseNoIndex when none */
    PulseDeviceState state = PulseDeviceState::Unknown;
    std::string bt_address;         /* "70:BF:92:53:6B:8B": api.bluez5.address, else device.string when it is a MAC */
    std::string form_factor;        /* device.form-factor / device.form_factor: "headset", "headphone", "speaker", ... */
    std::string bus;                /* device.bus: "bluetooth", "usb", "pci", ... */
    std::string bt_profile;         /* api.bluez5.profile / bluetooth.protocol: "headset-head-unit", "a2dp-sink" */
    std::string bt_codec;           /* api.bluez5.codec / bluetooth.codec: "msbc", "cvsd", "sbc", ... */
    std::string device_description; /* device.description */
    std::string device_api;         /* device.api: "bluez5", "alsa" */
    int volume_pct = -1;            /* VINYL: sinks: the loudest channel in percent of the normal volume (pa_cvolume_max,
                                       as pactl shows it); -1 = not reported */
    bool muted = false;             /* VINYL: sinks */
};
/* VINYL: a pa_cvolume's loudest channel in percent of PA_VOLUME_NORM (0x10000), rounded. */
int pulse_volume_pct(const uint32_t *values, unsigned channels);

struct PulseProfile {
    std::string name;        /* "a2dp-sink", "a2dp-sink-sbc_xq", "headset-head-unit", "headset-head-unit-cvsd", "off" */
    std::string description; /* "High Fidelity Playback (A2DP Sink, codec SBC)" */
    bool available = true;   /* false: the server knows it cannot work now (another A2DP device holds the link, P6) */
    uint32_t priority = 0;
    uint32_t n_sinks = 0, n_sources = 0;
};

struct PulseCard {
    uint32_t index = kPulseNoIndex;
    std::string name;        /* "bluez_card.70_BF_92_53_6B_8B" */
    std::string description; /* device.description */
    std::string driver;
    std::string bt_address, bus, form_factor, device_api;
    std::vector<PulseProfile> profiles;
    std::string active_profile; /* "" when the server reports none */
};

/* A music profile: A2DP towards the headset or speaker ("a2dp-sink", "a2dp-sink-sbc_xq", PulseAudio "a2dp_sink"). */
bool pulse_profile_is_music(const std::string &profile);
/* A call profile, the only one with a microphone ("headset-head-unit", "headset-head-unit-cvsd",
 * "headset-head-unit-msbc", PulseAudio "headset_head_unit" / "handsfree_head_unit"). */
bool pulse_profile_is_call(const std::string &profile);
/* "AA:BB:CC:DD:EE:FF" when `s` is a Bluetooth address (case kept), else "". */
std::string pulse_parse_mac(const std::string &s);
/* The MAC in a node or card name: "bluez_input.70_BF_92_53_6B_8B.0" -> "70:BF:92:53:6B:8B"; "" when none. */
std::string pulse_mac_from_name(const std::string &name);
/* The libpulse sample format name ("s16le") of a pa_sample_format_t value. */
const char *pulse_format_name(int pa_format);

struct PulseServerInfo {
    std::string server_name;    /* "PulseAudio (on PipeWire 1.4.2)" or "pulseaudio" */
    std::string server_version; /* "15.0.0" (pipewire-pulse reports the protocol level) */
    std::string default_source, default_sink;
    uint32_t rate = 0;          /* the default sample rate */
    bool on_pipewire = false;   /* the server name says PipeWire */
    std::string library_version; /* libpulse's own version */
};

/* A server change (pa_context_subscribe). Only the index is known: re-list to see what it is now. */
struct PulseChange {
    enum class What { Source, Sink, Card, Server };
    enum class Kind { New, Change, Remove, Disconnected /* the connection to the server was lost (What::Server) */ };
    What what = What::Server;
    Kind kind = Kind::Change;
    uint32_t index = kPulseNoIndex;
};
using PulseChangeCallback = std::function<void(const PulseChange &)>;

/* ------------------------------------------------------------------------------------------------ streams */

enum class PulseEvent {
    Connecting,      /* opened, no frame yet (capture) / not ready yet (playback); always the first event */
    Running,         /* capture: the first frame arrived (again after Suspended); playback: the stream is ready */
    Suspended,       /* the server suspended the device: no frames until it resumes (then Running) */
    Moved,           /* the server attached the stream to `device`, not the one asked for: the stream is ENDED */
    Killed,          /* the device went away (kPulseKilled, kPulseNoDevice) or the server connection was lost
                        (kPulseDisconnected): ENDED */
    Error,           /* any other failure (code): ENDED */
    StartTimeout,    /* capture: no frame within first_frame_timeout_ms of the open; the stream stays open and may still
                        start (Running follows then) */
    SilenceDetected, /* capture: silence_seconds of samples that are exactly zero (digital silence, not a low level) */
    SilenceCleared,  /* capture: a non-zero sample after SilenceDetected */
};
const char *pulse_event_name(PulseEvent e);

struct PulseStreamEvent {
    PulseEvent kind = PulseEvent::Connecting;
    int code = 0;       /* Killed / Error: the error value; StartTimeout: kPulseTimeout */
    std::string device; /* Moved: the device the server attached the stream to; else the device asked for */
    double t = 0;       /* seconds since the open (the stream's clock): when it happened, or when it was noticed */
    uint64_t frame = 0; /* capture: frames the server had delivered by then */
};

enum class PulseStreamState { Connecting, Running, Suspended, Ended };
const char *pulse_state_name(PulseStreamState s);

/* pa_buffer_attr, in bytes; kPulseDefault lets the server choose. */
struct PulseBufferAttr {
    uint32_t maxlength = kPulseDefault;
    uint32_t tlength = kPulseDefault;
    uint32_t prebuf = kPulseDefault;
    uint32_t minreq = kPulseDefault;
    uint32_t fragsize = kPulseDefault;
};

/* Bytes of `ms` milliseconds of S16 audio: whole frames, rounded up (1 ms at 8000 Hz mono = 16 bytes). */
uint32_t pulse_ms_to_bytes(uint32_t ms, uint32_t rate, unsigned channels, unsigned sample_bytes = 2);
/* Frames in `bytes` of S16 audio (rounded down). VINYL: sample_bytes 4 for S32 playback streams. */
uint64_t pulse_bytes_to_frames(uint64_t bytes, unsigned channels, unsigned sample_bytes = 2);

struct PulseCaptureRequest {
    std::string source;              /* the EXACT source name; never empty, never "default" (refused) */
    uint32_t rate = 0;               /* 0: the source's own rate (no resampling: 8000 CVSD, 16000 mSBC, 48000 USB...) */
    unsigned channels = 0;           /* 0: the source's own count, limited to 1..2 */
    uint32_t fragment_ms = 20;       /* the server hands over about this much at a time (fragsize) */
    uint32_t server_max_ms = 2000;   /* the server-side buffer limit (maxlength) */
    uint32_t ring_ms = 2000;         /* the client ring the reader drains; when a late reader lets it fill, the frames
                                        that do not fit are dropped (counted: frames_overflow) */
    bool fail_on_suspend = false;    /* PA_STREAM_FAIL_ON_SUSPEND: a suspended source kills the stream */
    double silence_seconds = 3.0;    /* SilenceDetected after this long of exactly-zero samples; 0 turns it off */
    uint32_t first_frame_timeout_ms = 3000; /* StartTimeout after this long without a frame (P4: 0.65 s switch) */
    uint32_t ready_timeout_ms = 2000; /* open waits this long for the server to accept the stream (see open_capture) */
    std::string stream_name = "Vinyl recording"; /* VINYL */
    std::string role = "production"; /* media.role */
    /* Diagnostics only (the probe CLI): false skips the source lookup (and with it the native rate, 48000 unless `rate`
     * says, and the monitor refusal), so the stream is connected to a name that may not exist and the SERVER's own
     * handling of a missing device can be observed (with DONT_MOVE it must refuse or kill the stream, never attach it
     * elsewhere). The app always looks up first. */
    bool lookup_first = true;
};

struct PulsePlaybackRequest {
    std::string sink;                /* the EXACT sink name; never empty, never "default" (refused) */
    uint32_t rate = 48000;           /* the source rate: the server resamples to the sink's rate */
    unsigned channels = 2;           /* 1 or 2 */
    unsigned sample_bytes = 2;       /* VINYL: 2 = s16le, 4 = s32le (24-bit and 32-bit sources, decision D13) */
    uint32_t buffer_ms = 100;        /* the server-side target (tlength); the monitor wants about 20-40 ms */
    uint32_t ring_ms = 0;            /* the client ring write() fills; 0 = buffer_ms (at least 10 ms) */
    bool fail_on_suspend = false;
    uint32_t ready_timeout_ms = 2000;
    std::string stream_name = "Vinyl"; /* VINYL */
    std::string role = "music";
};

/* What a stream got. S16LE interleaved (VINYL: or S32LE for a playback stream asked with sample_bytes 4). */
struct PulseStreamFormat {
    uint32_t rate = 0;
    unsigned channels = 0;
    unsigned sample_bytes = 2;  /* VINYL: 2 (s16le) or 4 (s32le) */
    uint32_t device_rate = 0;   /* the device's own rate at the open */
    unsigned device_channels = 0;
    PulseBufferAttr requested;  /* what was asked for */
    PulseBufferAttr granted;    /* what the server set (pa_stream_get_buffer_attr), once ready; else = requested */
    std::string device;         /* the device the stream is attached to, once ready ("" before) */
    uint32_t flags = 0;         /* the pa_stream_flags_t used */
};

/* The server's latency report for a stream. */
struct PulseLatency {
    double total_ms = 0;     /* pa_stream_get_latency: capture: how old a frame is when it is read; playback: how
                                long a frame written now takes to be heard */
    double device_ms = 0;    /* the device's own part (timing info source_usec / sink_usec), 0 when unknown */
    double transport_ms = 0; /* the client-server transport part, 0 when unknown */
    bool timing_info = false; /* device_ms / transport_ms come from a timing report */
};

/* The server end of one stream (real_pulse.cpp, or the test fake). */
class PulseStreamLink {
public:
    virtual ~PulseStreamLink() = default;
    /* The device the stream is attached to now, in the server's own view (the real one asks the server); "" when unknown or
     * the stream is gone. */
    virtual std::string device_name() = 0;
    virtual int latency(PulseLatency &out) = 0;
    /* Playback: audio was queued, hand it to the server now. */
    virtual void kick() {}
    /* Playback: discard what is queued (the ring's consumer side and the server buffer). */
    virtual void flush() {}
    /* Playback: hand the server everything still in the ring, then wait until it has played it (at most timeout_ms in
     * all): 0, kPulseTimeout or an error. */
    virtual int drain(int timeout_ms) {
        (void)timeout_ms;
        return 0;
    }
    /* Disconnects the stream. After it returns, no feed call reaches the owner any more. Idempotent. */
    virtual void close() = 0;
};

struct PulseCaptureStats {
    PulseStreamState state = PulseStreamState::Connecting;
    uint64_t frames_in = 0;        /* frames the server delivered (before the ring) */
    uint64_t frames_read = 0;      /* frames the owner took with read() */
    uint64_t frames_overflow = 0;  /* frames the ring had no room for (the reader was late) */
    uint64_t frames_hole = 0;      /* frames the server reported as lost on its side (holes) */
    uint64_t frames_after_end = 0; /* frames discarded because the stream had ended (a move: never another mic's audio) */
    uint64_t zero_run = 0;         /* frames in the current run of exactly-zero samples */
    bool silent = false;           /* SilenceDetected is in force */
    double first_frame_s = -1;     /* seconds from the open to the first frame; -1 = none yet */
    int end_code = 0;              /* why it ended (0 while open) */
    size_t ring_frames = 0;        /* the ring's capacity */
};

/* One capture stream. The owner reads frames on its capture thread (read) and polls events on its own thread
 * (poll_event, state, source_identity_ok, latency, stats): those two sides may run at the same time. close() (or the
 * destructor) must not race with read(). */
class PulseCapture {
public:
    /* `now` is the stream clock in seconds (steady; the fake passes its own). */
    PulseCapture(const PulseCaptureRequest &req, const PulseStreamFormat &fmt, std::function<double()> now);
    ~PulseCapture();
    PulseCapture(const PulseCapture &) = delete;
    PulseCapture &operator=(const PulseCapture &) = delete;

    /* ---- the owner */
    /* Copies up to max_frames S16 frames into dst. Waits up to timeout_ms for the first one (0: does not wait). Returns the
     * frames copied (> 0), 0 when none came in time (Connecting, Suspended, or a gap), or, once the stream has ENDED and
     * every frame received before the end has been read, the end code (kPulseKilled, kPulseMoved, ...). kPulseState after
     * close(). */
    long read(void *dst, size_t max_frames, int timeout_ms);
    /* The next event, in order (see PulseEvent): false when none is pending. */
    bool poll_event(PulseStreamEvent &ev);
    PulseStreamState state() const;
    /* Seconds since the open while Connecting (no frame yet), else 0. */
    double connecting_seconds() const;
    /* True while the server says the stream is attached to exactly the requested source (study C16). `actual` gets the
     * server's answer ("" when unknown). False after a move, a kill or close(). */
    bool source_identity_ok(std::string *actual = nullptr);
    int latency(PulseLatency &out);
    PulseCaptureStats stats() const;
    /* rate and channels never change; `granted` and `device` are filled when the server accepts the stream */
    PulseStreamFormat format() const;
    const PulseCaptureRequest &request() const { return req_; }
    bool ended() const { return ended_.load(std::memory_order_acquire); }
    int end_code() const { return end_code_.load(std::memory_order_acquire); }
    /* Disconnects (PulseStreamLink::close) and wakes a waiting read(). Idempotent. */
    void close();

    /* ---- the server end (PulseApi implementations only) */
    void attach(std::unique_ptr<PulseStreamLink> link);
    /* Data path: S16 frames as the server delivered them. Never allocates or blocks. */
    void feed_frames(const void *frames, size_t nframes);
    void feed_hole(size_t nframes);
    /* The server accepted the stream on `device` (granted: the buffer it set). false: the device is not the requested one
     * (the stream has ended with Moved): the caller disconnects the stream. */
    bool feed_ready(const std::string &device, const PulseBufferAttr &granted);
    /* The server moved the stream to `device`: ends it (Moved). Always false (disconnect). */
    bool feed_moved(const std::string &device);
    void feed_suspended(bool on);
    /* The stream failed or was killed (code: kPulseKilled, kPulseNoDevice, kPulseDisconnected -> Killed; else Error). */
    void feed_end(int code);

private:
    double now_s() const { return now_() - t0_; }
    void end_with(PulseEvent kind, int code, const std::string &device);

    PulseCaptureRequest req_;
    PulseStreamFormat fmt_;
    std::function<double()> now_;
    double t0_;
    unsigned channels_;
    uint64_t silence_frames_; /* threshold; 0 = off */
    std::unique_ptr<Ring> ring_;
    std::unique_ptr<PulseStreamLink> link_;

    /* written by the server end */
    std::atomic<bool> first_frame_{false};
    std::atomic<double> first_frame_t_{-1.0};
    std::atomic<uint64_t> frames_in_{0}, holes_{0}, after_end_{0}, zero_run_{0}, frames_read_{0};
    std::atomic<bool> silent_{false};
    std::atomic<uint32_t> silence_flips_{0};
    std::atomic<uint64_t> flip_frame_{0};
    std::atomic<bool> ended_{false};
    std::atomic<int> end_code_{0};
    std::atomic<bool> suspended_{false};
    std::atomic<bool> closed_{false};

    /* read() waits here; feed_frames notifies without taking the mutex (a missed wake costs at most one 5 ms slice) */
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;

    /* the state path */
    mutable std::mutex mu_;
    std::deque<PulseStreamEvent> queue_;
    uint64_t queue_dropped_ = 0;

    /* poll_event's own bookkeeping */
    std::mutex poll_mu_;
    bool rep_connecting_ = false, rep_running_ = false, rep_timeout_ = false;
    uint32_t rep_flips_ = 0;
};

struct PulsePlaybackStats {
    PulseStreamState state = PulseStreamState::Connecting;
    uint64_t frames_written = 0;   /* frames the owner queued with write() */
    uint64_t frames_to_server = 0; /* frames handed to the server */
    uint64_t frames_dropped = 0;   /* frames discarded by drop() */
    uint64_t underruns = 0;        /* the server ran out of audio (pa underflow callback) */
    int end_code = 0;
    size_t ring_frames = 0;
};

/* One playback stream. The owner writes on its playback thread and polls on its own thread. */
class PulsePlayback {
public:
    PulsePlayback(const PulsePlaybackRequest &req, const PulseStreamFormat &fmt, std::function<double()> now);
    ~PulsePlayback();
    PulsePlayback(const PulsePlayback &) = delete;
    PulsePlayback &operator=(const PulsePlayback &) = delete;

    /* ---- the owner */
    /* Queues up to nframes S16 frames; waits up to timeout_ms while the ring is full. Returns the frames queued (> 0,
     * possibly fewer), 0 when there was no room in time, or the end code once the stream has ended. */
    long write(const void *frames, size_t nframes, int timeout_ms);
    /* Waits until everything queued has been played (at most timeout_ms): 0, kPulseTimeout, or the end code. */
    int drain(int timeout_ms);
    /* Discards what is queued, at once. */
    void drop();
    /* Frames queued but not heard yet: the ring plus the server's latency report (>= 0). */
    long delay_frames();
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    bool poll_event(PulseStreamEvent &ev);
    PulseStreamState state() const;
    /* True while the server says the stream plays to exactly the requested sink. */
    bool sink_identity_ok(std::string *actual = nullptr);
    int latency(PulseLatency &out);
    PulsePlaybackStats stats() const;
    PulseStreamFormat format() const;
    bool ended() const { return ended_.load(std::memory_order_acquire); }
    int end_code() const { return end_code_.load(std::memory_order_acquire); }
    void close();

    /* ---- the server end */
    void attach(std::unique_ptr<PulseStreamLink> link);
    /* Data path: the consumer end of the ring (up to max_frames into dst). Never allocates or blocks. Serialised by the
     * server end (one consumer at a time). */
    size_t take(void *dst, size_t max_frames);
    /* The consumer end: discards everything queued (drop()); returns the frames discarded. */
    size_t discard_queued();
    size_t queued_frames() const;
    void feed_underflow();
    bool feed_ready(const std::string &device, const PulseBufferAttr &granted);
    bool feed_moved(const std::string &device);
    void feed_suspended(bool on);
    void feed_end(int code);

private:
    double now_s() const { return now_() - t0_; }
    void end_with(PulseEvent kind, int code, const std::string &device);
    void push(PulseEvent kind, int code, const std::string &device);

    PulsePlaybackRequest req_;
    PulseStreamFormat fmt_;
    std::function<double()> now_;
    double t0_;
    std::unique_ptr<Ring> ring_;
    std::unique_ptr<PulseStreamLink> link_;

    std::atomic<uint64_t> written_{0}, to_server_{0}, dropped_{0}, underruns_{0};
    std::atomic<bool> ended_{false};
    std::atomic<int> end_code_{0};
    std::atomic<bool> suspended_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> closed_{false};

    std::mutex wait_mu_;
    std::condition_variable wait_cv_;

    mutable std::mutex mu_;
    std::deque<PulseStreamEvent> queue_;
    bool rep_connecting_ = false;
};

/* ------------------------------------------------------------------------------------------------ the server */

class PulseApi {
public:
    virtual ~PulseApi() = default;
    /* Connects to the user's sound server (never starts one: PA_CONTEXT_NOAUTOSPAWN, so an installed but stopped
     * PulseAudio is not spawned next to PipeWire). 0, kPulseMissing (libpulse not installed), kPulseNoServer (no server
     * runs), kPulseTimeout, kPulseFailed. last_error() says it in words. Calling it while connected returns 0. */
    virtual int connect(const std::string &app_name, int timeout_ms) = 0;
    /* Drops the connection: open streams end with Killed (kPulseDisconnected); the change callback is cleared and is NOT
     * called after this returns. Idempotent. */
    virtual void disconnect() = 0;
    virtual bool connected() = 0;
    virtual std::string last_error() = 0;
    virtual int server_info(PulseServerInfo &out) = 0;
    virtual int list_sources(std::vector<PulseDevice> &out) = 0;
    virtual int list_sinks(std::vector<PulseDevice> &out) = 0;
    virtual int list_cards(std::vector<PulseCard> &out) = 0;
    virtual int card_by_name(const std::string &name, PulseCard &out) = 0;
    /* Follows source / sink / card / server changes (and the loss of the connection). The callback runs on the server's
     * thread with the server's lock held: it must return quickly, must NOT call this PulseApi or its streams, and must not
     * wait for a lock a thread calling the PulseApi may hold. Set a flag and re-list from your own thread. An empty
     * function stops it; once set_change_callback returns, the previous callback is not running and is not called again. */
    virtual void set_change_callback(PulseChangeCallback cb) = 0;
    /* Opens a capture stream on req.source (looked up first: kPulseNoDevice when absent, the stream is never created).
     * Waits up to req.ready_timeout_ms for the server to accept it: a refusal returns its error (kPulseNoDevice,
     * kPulseRefused, ...); a stream the server attached elsewhere returns kPulseMoved (closed again); a stream still
     * pending after the wait is returned anyway in state Connecting (its acceptance or failure arrives as events). `why`
     * gets a sentence on failure. */
    virtual int open_capture(const PulseCaptureRequest &req, std::unique_ptr<PulseCapture> &out,
                             std::string *why = nullptr) = 0;
    virtual int open_playback(const PulsePlaybackRequest &req, std::unique_ptr<PulsePlayback> &out,
                              std::string *why = nullptr) = 0;
};

/* The production implementation: loads libpulse.so.0 on the first connect(). Never null. */
std::unique_ptr<PulseApi> make_real_pulse();

/* ---- the parts of a stream open that are the same for every implementation (tested on the fake, used by the real one) */

/* The stream flags: DONT_MOVE always, ADJUST_LATENCY, INTERPOLATE_TIMING, AUTO_TIMING_UPDATE, FAIL_ON_SUSPEND on request.
 * VINYL: playback also START_UNMUTED (a mute the server restored never silences a new stream). */
uint32_t pulse_capture_flags(const PulseCaptureRequest &req);
uint32_t pulse_playback_flags(const PulsePlaybackRequest &req);

/* VINYL: the app's identity towards the sound server (board probe 2026-11-12: the copied code said "cassette"). */
constexpr const char *kPulseAppId = "vinyl";
/* The context's properties: application.name, application.id, application.process.binary and application.icon_name. */
std::vector<std::pair<std::string, std::string>> pulse_context_props(const std::string &app_name);
/* The stream properties: media.name, media.role, application.name, application.id, and PipeWire's node.dont-reconnect,
 * node.dont-fallback and node.dont-move (= true). VINYL, board test B7 and the probe of 2026-11-12: the servers restore a
 * stream's volume by media role ("sink-input-by-media-role:music"), shared by every app playing music, so a reconnect
 * could bring back another app's level. A stream gets its own restore key (module-stream-restore.id
 * "sink-input-by-application-id:vinyl", "source-output-by-..." for capture: PulseAudio's module-stream-restore uses it
 * before the role) and WirePlumber's state.restore-props / state.restore-target = false (it applies no stored volume or
 * target to Vinyl's stream); the playback stream is then opened at its own volume (pulse_norm_volume). */
std::vector<std::pair<std::string, std::string>> pulse_stream_props(const std::string &stream_name, const std::string &role,
                                                                    const std::string &app_name, bool playback = true);
/* VINYL: the initial volume of every playback stream Vinyl opens (passed to pa_stream_connect_playback): 100 % (0 dB) on
 * each channel. Vinyl's own level is its software gain (decision D11), so the stream itself stays at unity and no
 * restored value can make it louder or quieter after a reconnect. */
struct PulseVolume {
    unsigned channels = 0;
    uint32_t values[32] = {};
};
PulseVolume pulse_norm_volume(unsigned channels);
/* The capture format for a request on `source`: rate 0 -> the source's rate; channels 0 -> the source's, limited to 1..2.
 * false with `why` when the request cannot be served (an empty or "default" name, a monitor source, no rate). */
bool pulse_capture_format(const PulseCaptureRequest &req, const PulseDevice &source, PulseStreamFormat &fmt,
                          std::string *why);
bool pulse_playback_format(const PulsePlaybackRequest &req, const PulseDevice &sink, PulseStreamFormat &fmt,
                           std::string *why);
/* Buffer attributes from milliseconds: capture fragsize = fragment_ms, maxlength = server_max_ms; playback tlength =
 * buffer_ms (prebuf, minreq, maxlength left to the server). */
PulseBufferAttr pulse_capture_attr(const PulseCaptureRequest &req, uint32_t rate, unsigned channels);
PulseBufferAttr pulse_playback_attr(const PulsePlaybackRequest &req, uint32_t rate, unsigned channels);

}  // namespace vinyl
