/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * A scriptable fake sound server for the Cassette pulse tests: the device, card and server lists, a library that is
 * missing or a server that is not running, and the server's side of every stream the code opens (the tests play the
 * server): deliver frames, stall (a profile switch), remove a device (kill, or move when the "server" ignores DONT_MOVE as
 * pipewire-pulse did in P5), suspend, attach a new stream elsewhere, consume playback audio (underruns when short), lose
 * the connection. Synthetic samples only. The streams are the production PulseCapture / PulsePlayback; only their server
 * end (a PulseStreamLink) is fake. Every server-side call takes the fake's mutex (its "main loop lock"), as the real
 * callbacks run under libpulse's lock.
 */
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "pulse/pulse_api.hpp"

namespace vinyl {

struct FakeStream {
    bool capture = true;
    std::string requested;   /* the device the code asked for */
    std::string attached;    /* where the fake server has it ("" once it is gone) */
    uint32_t rate = 0;
    unsigned channels = 0;
    uint32_t flags = 0;
    PulseBufferAttr attr;
    std::vector<std::pair<std::string, std::string>> props;
    PulseCapture *cap = nullptr;   /* valid while open (cleared by the link's close) */
    PulsePlayback *play = nullptr;
    bool open = true;              /* false once the code closed it */
    uint64_t latency_us = 20000;
    uint64_t delivered_after_close = 0; /* frames the server tried to deliver after the close (dropped by the fake) */
    unsigned identity_queries = 0;
    std::vector<int16_t> drained;  /* playback: what drain() played out at once (the end of a take), in order */
};

class FakePulse : public PulseApi {
public:
    /* scripting */
    bool lib_missing = false;      /* connect -> kPulseMissing */
    bool server_down = false;      /* connect -> kPulseNoServer */
    bool server_hangs = false;     /* connect -> kPulseTimeout */
    std::atomic<int> call_delay_ms{0}; /* a HUNG server: connect, the lists and card_by_name take this long to answer (a real sleep, taken
                                        * before the fake's lock so the streams keep running) */
    bool honour_dont_move = true;  /* removing a device kills its streams; false: moves them to the default source/sink */
    std::string attach_elsewhere;  /* non-empty: the next stream is attached to this device instead (identity check) */
    bool defer_ready = false;      /* open returns before the server accepts the stream (ready() later) */
    int connect_error_stream = 0;  /* non-zero: the next stream fails at the connect with this error */
    std::atomic<double> clock{0};  /* the streams' clock, seconds */
    PulseServerInfo server;
    std::vector<PulseDevice> sources, sinks;
    std::vector<PulseCard> cards;
    std::vector<FakeStream> streams; /* in open order; read them under lock() when a server thread runs */
    unsigned callbacks = 0;          /* change callbacks made */
    std::atomic<unsigned> listings{0}; /* list_sources calls (one per re-listing) */

    FakePulse();
    ~FakePulse() override;

    /* PulseApi */
    int connect(const std::string &app_name, int timeout_ms) override;
    void disconnect() override;
    bool connected() override;
    std::string last_error() override;
    int server_info(PulseServerInfo &out) override;
    int list_sources(std::vector<PulseDevice> &out) override;
    int list_sinks(std::vector<PulseDevice> &out) override;
    int list_cards(std::vector<PulseCard> &out) override;
    int card_by_name(const std::string &name, PulseCard &out) override;
    void set_change_callback(PulseChangeCallback cb) override;
    int open_capture(const PulseCaptureRequest &req, std::unique_ptr<PulseCapture> &out, std::string *why) override;
    int open_playback(const PulsePlaybackRequest &req, std::unique_ptr<PulsePlayback> &out, std::string *why) override;

    /* the server's side, driven by the tests (any thread) */
    void advance(double seconds) { clock.store(clock.load() + seconds); }
    /* capture: delivers frames (interleaved S16) as the server's read callback would. false when the stream is closed */
    bool deliver(size_t stream, const int16_t *samples, size_t frames);
    bool deliver_value(size_t stream, int16_t value, size_t frames); /* every sample = value */
    void hole(size_t stream, size_t frames);
    /* the server accepts a deferred stream (defer_ready) on its attached device */
    void ready(size_t stream);
    /* playback: the server plays `frames`; fewer queued = one underflow. Returns the frames it got. */
    size_t consume(size_t stream, size_t frames, std::vector<int16_t> *out = nullptr);
    void underflow(size_t stream);
    /* devices */
    void add_source(const PulseDevice &d);
    void remove_source(const std::string &name); /* kills its streams (or moves them: honour_dont_move = false) */
    void add_sink(const PulseDevice &d);
    void remove_sink(const std::string &name);
    /* VINYL: a sink's volume or mute changes (a volume key, a headset's buttons), with the server's change event */
    void set_sink_volume(const std::string &name, int pct, bool muted = false);
    /* a card appears (a new index unless it has one) or goes away, with the server's change event */
    uint32_t add_card(const PulseCard &c);
    void remove_card(const std::string &name);
    void suspend(const std::string &device, bool on);
    /* the server moves one stream (whatever the flags say) */
    void move(size_t stream, const std::string &to);
    void fail(size_t stream, int code);
    void set_profile(const std::string &card, const std::string &profile);
    /* the connection is lost: every stream Killed(kPulseDisconnected), a Disconnected change */
    void crash();
    std::recursive_mutex &lock() { return mu_; }

private:
    friend class FakeLink;
    void notify(PulseChange::What what, PulseChange::Kind kind, uint32_t index);
    FakeStream *at(size_t i);
    void nap();
    void lose(const std::string &device, bool sink);

    std::recursive_mutex mu_;
    PulseChangeCallback cb_;
    bool connected_ = false;
    std::string err_;
    uint32_t next_index_ = 100;
};

/* Presets from the HackBerry probe (study 12; names as WirePlumber gives them, synthetic otherwise). */
PulseDevice fake_bt_source(const std::string &mac, uint32_t rate = 16000, uint32_t card = 70);
PulseDevice fake_bt_sink(const std::string &mac, uint32_t card = 70, const std::string &form = "headset");
PulseDevice fake_bt_monitor(const std::string &mac, uint32_t card = 70);
PulseDevice fake_usb_source(const std::string &id, uint32_t rate = 48000, unsigned channels = 1, uint32_t card = 50);
PulseDevice fake_usb_sink(const std::string &id, uint32_t card = 50);
/* A Bluetooth card: music (a2dp-sink, a2dp-sink-sbc_xq) and call (headset-head-unit, -cvsd) profiles. */
PulseCard fake_bt_card(const std::string &mac, uint32_t index, bool music_available, const std::string &active,
                       bool has_call = true);
/* A HackBerry-like set (study 12): the Evolve 65 in call mode (music available), the XWF speaker on A2DP, the Beats Fit Pro
 * limited to call profiles (P6), a USB headset, monitors and the dummy output; server name and versions as on the board.
 * For cassette-probe --server --fake and the report tests. */
void fake_hackberry(FakePulse &f);

}  // namespace vinyl
