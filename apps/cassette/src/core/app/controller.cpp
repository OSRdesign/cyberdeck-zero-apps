/*
 * SPDX-License-Identifier: MIT
 */
#include "controller.hpp"

#include "../alsa/capture.hpp"
#include "../alsa/card.hpp"
#include "../alsa/mixer.hpp"
#include "../alsa/open_policy.hpp"
#include "../pulse/bt_input.hpp"
#include "../pulse/bt_output.hpp"
#include "../pulse/pulse_pcm.hpp"
#include "../pulse/pulse_probe.hpp"
#include "../codec/decoder.hpp"
#include "../codec/flac_encoder.hpp"
#include "../codec/formats.hpp"
#include "../codec/opus_encoder.hpp"
#include "../codec/stream_scan.hpp"
#include "../dsp/processing_sink.hpp"
#include "../edit/merge.hpp"
#include "../edit/peaks.hpp"
#include "../edit/trim_split.hpp"
#include "../playback/deck.hpp"
#include "../playback/monitor.hpp"
#include "../playback/output.hpp"
#include "../playback/player.hpp"
#include "../playback/pulse_sink.hpp"
#include "../playback/sink.hpp"
#include "../repair.hpp"
#include "../ring.hpp"
#include "../takes/atomic_file.hpp"
#include "../takes/recordings.hpp"
#include "../writer.hpp"
#include "pulse_worker.hpp"
#include "record_gate.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace cassette {
namespace {

constexpr uint64_t kSpaceRefreshMs = 2000;
constexpr uint64_t kPlayLevelStaleMs = 150; /* no new played period for this long: the playback level falls */

/* Bluetooth input (docs/PULSE.md, study C5 C9 C15 C16 C18) */
const char kBtNotSetUp[] = "Bluetooth audio is not set up";
const char kBtNotResponding[] = "Bluetooth audio is not responding";
const char kBtNotConnected[] = "The Bluetooth headset is not connected.";
const char kBtNoSound[] = "The headset is connected but sends no sound.";
const char kBtLostTake[] = "The Bluetooth headset disconnected: the take was closed and kept.";
const char kBtLostIdle[] = "The Bluetooth headset disconnected.";
const char kBtServerLostTake[] = "The sound server stopped: the take was closed and kept.";
const char kBtServerLostIdle[] = "The sound server stopped.";
const char kBtBack[] = "The Bluetooth headset is back";
const char kBtIdentity[] = "The headset microphone could not be confirmed: the take did not start.";
constexpr uint64_t kBtCallLinkMs = 300;    /* a call profile and this long running: the zeros are the headset's own (C18) */
constexpr uint64_t kBtProfilePollMs = 100; /* while connecting: how often the card's profile is asked */
constexpr uint64_t kBtIdentityMs = 3000;   /* during a take: how often the stream's source is checked (C16) */
constexpr uint64_t kBtIdentityRetryMs = 1000; /* T9: the server may report the stream attached to NO source for a moment (seen on the
                                               * HackBerry while WirePlumber relinks the virtual source at a profile switch): such an
                                               * answer is asked again for this long before REC is refused or a take is ended */
constexpr uint64_t kBtIdentityRetryStepMs = 100; /* ... this often (at REC: on the controller's thread, the user's own action) */

/* Bluetooth output and monitor (docs/PULSE.md "What T3 uses", study C7 C8 C17 C19) */
const char kBtOutNotConnected[] = "The Bluetooth output is not connected.";
const char kBtOutLost[] = "The Bluetooth output disconnected.";
const char kBtMonitorGone[] = "The monitor output disconnected: the recording goes on.";
const char kBtMonitorNotThere[] = "The monitor output is not connected.";
constexpr uint64_t kMusicWaitMaxMs = 4000;  /* PLAY waits at most this long for the music profile, then plays at call quality */
constexpr uint64_t kMusicWaitMinMs = 900;   /* a microphone that was open: the switch to call takes 0.65 s, so a music reading before
                                             * this is not yet the answer (P4) */
constexpr uint64_t kSinkWaitMs = 3000;      /* T9: a Bluetooth sink that is missing while its device is there (PipeWire destroys the
                                             * node at every profile switch and creates a new one a moment later; a reconnect too):
                                             * PLAY waits this long for it, then says the output is not connected */
constexpr uint64_t kBtRelistMs = 1000;      /* T9: while the chosen output (or the chosen input) has no usable node, or the server failed
                                             * a listing, the lists are read again this often even without a server event */
constexpr uint64_t kBtRelistWaitMs = 250;   /* ... and this often while PLAY waits for a missing sink */
constexpr uint64_t kQualityPollMs = 1000;   /* while a take plays through a Bluetooth output: how often the card's profile is read */
constexpr uint64_t kMonitorDelayFirstMs = 300; /* the first delay figure after the monitor starts, then once a second */
constexpr uint64_t kMonitorDelayMs = 1000;
constexpr double kMonitorNoticeDelayMs = 150.0; /* above this the monitor says 'delay' */

AudioFormat play_meter_format() {
    AudioFormat f;
    f.rate = 48000;
    f.channels = 2;
    f.bits = 16;
    return f;
}

void copy_meter(const MeterSnapshot &m, MeterSnap &o) {
    o.channels = m.channels;
    for (int i = 0; i < 2; ++i) {
        o.ch[i].peak_db = m.ch[i].peak_db;
        o.ch[i].rms_db = m.ch[i].rms_db;
        o.ch[i].hold_db = m.ch[i].hold_db;
        o.ch[i].bar = m.ch[i].bar;
        o.ch[i].hold_bar = m.ch[i].hold_bar;
        o.ch[i].peak_lamp = m.ch[i].peak_lamp;
        o.ch[i].zone = static_cast<int>(m.ch[i].zone);
    }
}

std::string format_text(const AudioFormat &f, const char *tag = "WAV") {
    char b[64];
    std::snprintf(b, sizeof b, "%gk/%u", f.rate / 1000.0, static_cast<unsigned>(f.bits));
    std::string s = b;
    if (f.channels == 2) s += " stereo";
    else if (f.channels > 2) s += " " + std::to_string(f.channels) + "ch";
    return s + " " + tag;
}

/* The production encoders: FLAC and Ogg Opus on the libraries loaded with dlopen. */
std::unique_ptr<Encoder> real_encoder(const FormatChoice &c, const AudioFormat &capture) {
    if (c.key.format == RecFormat::Flac) {
        FlacConfig fc;
        fc.fmt = capture;
        fc.level = c.flac_level;
        return std::unique_ptr<Encoder>(new FlacEncoder(real_flac(), fc));
    }
    if (c.key.format == RecFormat::Opus) {
        OpusConfig oc;
        oc.fmt = capture;
        oc.bitrate_bps = c.opus_bitrate_bps;
        oc.application = c.opus_app;
        oc.force_mono = c.opus_force_mono;
        return std::unique_ptr<Encoder>(new OpusEncoder(real_opusenc(), oc));
    }
    return nullptr;
}

std::string secs_text(double s) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f s", s);
    return b;
}

/* "name|by-id|vid:pid" -> a descriptor for select_card(). */
CardDescriptor parse_descriptor(const std::string &text) {
    CardDescriptor d;
    const size_t a = text.find('|');
    if (a == std::string::npos) {
        d.name = text;
        return d;
    }
    const size_t b = text.find('|', a + 1);
    d.name = text.substr(0, a);
    d.usb_by_id = text.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (b != std::string::npos) d.usb_id = text.substr(b + 1);
    return d;
}

std::string key_of(const CardInfo &c) { return card_descriptor(c.name, c.usb_by_id, c.usb_id); }

}  // namespace

/* ======================================================================================================================== */

struct Controller::Impl : public DeckCommands {
    explicit Impl(ControllerConfig c)
        : cfg(std::move(c)),
          ops(cfg.ops ? cfg.ops : &posix_file_ops()),
          clk(cfg.clock ? cfg.clock : &system_clock()),
          default_state(*ops),
          cstate(cfg.clock_state ? cfg.clock_state : &default_state),
          store(*ops, cfg.settings_path.empty() ? default_settings_path(cfg.home) : cfg.settings_path),
          deck(*clk, *this) {}

    ~Impl() override {
        shutdown();
        if (pw) pw->stop();
        if (cfg.pulse && bt_cb_set) cfg.pulse->set_change_callback(nullptr);
    }

    /* ---- configuration and fixed members ---- */
    ControllerConfig cfg;
    FileOps *ops;
    Clock *clk;
    TimesyncClockState default_state;
    ClockState *cstate;

    /* ---- everything below is guarded by mu unless noted ---- */
    mutable std::mutex mu;
    SettingsStore store;
    Deck deck;
    bool started = false, down = false;
    std::string folder; /* the recordings folder, "~" expanded */
    std::deque<DeckEvent> pend;
    std::vector<Notice> notices;
    bool settings_error_shown = false;
    std::string lost_text = "The sound card was unplugged";
    std::string unplug_msg;       /* ProblemSnap::message of a take ended by a lost input (empty: the card text) */

    /* cards */
    std::vector<CaptureCard> cards;
    int sel = -1;                 /* index of the active card in `cards`, -1 when none */
    CardInfo active;              /* the card in use (valid when have_card) */
    bool have_card = false;
    std::string want_text;        /* the descriptor of the card to use (saved choice, or the card to wait for after a loss) */
    std::string active_key;
    bool lost = false;
    uint64_t last_retry_ms = 0;
    std::string cap_message;
    std::unique_ptr<Mixer> mixer;
    MixerState mstate;

    /* capture pipeline: destruction order is cap, proc (the gate and the monitor tap outlive them) */
    std::unique_ptr<MonitorTap> mtap;   /* made with the capture path; its ring is shared with the monitor thread */
    std::unique_ptr<Monitor> mon;       /* running only while monitor_wanted() */
    bool app_left = false;              /* AppLeave (Esc on the recorder): the monitor stays closed until the next key or setting */
    bool mon_failed = false;            /* the sink could not be opened or died: no retry until the user toggles or the card returns */
    RecordGate gate;
    std::unique_ptr<ProcessingSink> proc;
    std::unique_ptr<Capture> cap;
    AudioFormat fmt;
    bool via_server = false;

    /* Bluetooth input through the sound server (cfg.pulse). Under mu, except the two flags the change callback sets. */
    std::vector<BtInput> bt_list;                 /* what the server lists now, plus the chosen one when it is absent */
    std::map<std::string, std::string> bt_labels; /* the last label seen for an id (a headset that went away keeps its name) */
    std::string bt_status = kBtNotSetUp;          /* "" when the server answered */
    bool bt_cb_set = false, bt_connect_tried = false, bt_changed = false;
    uint64_t bt_last_connect_ms = 0, bt_last_refresh_ms = 0;
    std::atomic<bool> bt_dirty{false}, bt_gone{false}; /* set by the server's change callback, read by the service tick */
    /* everything that asks the server a question runs on the worker (docs/PULSE.md): the scan (connect + the three lists), the
     * card poll and the identity check; the service tick takes the answers */
    std::unique_ptr<PulseWorker> pw;
    std::vector<BtInput> bt_scan_in;              /* what the last scan listed (no placeholder) */
    std::vector<BtOutput> bt_outs;                /* the Bluetooth sinks the last scan listed */
    bool bt_is_connected = false;                 /* the worker's last word on the connection */
    bool bt_rescan = false;                       /* a change arrived while a scan was running */
    uint64_t bt_scan_ms = 0;                      /* when the applied scan was requested */
    uint64_t bt_scan_asked_ms = 0;                /* when the scan in flight (or the last one) was requested */
    std::string bt_digest;                        /* what the applied scan listed, as text: a re-listing that changed nothing is no
                                                   * reason to try the input again (T9: no scan loop while the input is absent) */
    std::string bt_card_asked;                    /* the card of the pending card poll, and when it was asked */
    uint64_t bt_card_ask_ms = 0;
    CardResult bt_card;                           /* the latest card poll answer ... */
    uint64_t bt_card_ms = 0;                      /* ... and when it was asked (answers older than an event of interest are ignored) */
    const void *bt_ident_for = nullptr;           /* the stream of the pending identity check */
    bool active_is_bt = false;                    /* the chosen input is a Bluetooth headset (open or not) */
    std::shared_ptr<PulseCapture> bt_stream;      /* the open microphone stream (events, identity); null when closed */
    std::shared_ptr<PulseLeadGate> bt_gate;       /* the connecting gate shared with the capture PCM */
    std::string bt_card_name;
    uint64_t bt_running_ms = 0, bt_profile_poll_ms = 0, bt_ident_ms = 0;
    uint64_t bt_ident_unknown_ms = 0;             /* during a take: since when the stream has been reported on no source (0: not) */
    bool bt_silent = false;                       /* the no-sound notice is in force */
    bool bt_policy_closed = false;                /* the microphone is closed on purpose (list, Edit page, playback) */
    UiPage page = UiPage::Recorder;

    /* playback */
    std::unique_ptr<PlaybackSink> sink;
    PulsePlaybackSink *bt_sink = nullptr;  /* `sink` when the output is a Bluetooth device (owned by `sink`) */
    std::string built_device;              /* the device `sink` was built for */
    std::unique_ptr<Player> player;
    /* PLAY with the headset still on the call profile (it is also the input): nothing plays until the music profile is back. T9: also
     * PLAY to a Bluetooth output whose device is there but whose sink node is not (a profile switch re-creates it): nothing plays until
     * it is listed again (need_music false), at most kSinkWaitMs */
    struct PlayWait {
        bool active = false;
        enum After { Play, Cued, ScanFwd, ScanRev } after = Play;
        uint64_t start_ms = 0, last_poll_ms = 0;
        bool mic_was_open = false, saw_call = false, cue_origin = false;
        bool need_music = true;   /* false: only the sink is waited for */
        uint64_t missing_ms = 0;  /* since when the sink has been missing (0: it is listed) */
        uint64_t music_ms = 0;    /* when the card poll that first said music was asked (0: not yet) */
        uint64_t rescan_ms = 0;   /* an open found no node: the next try waits for a listing asked after this (0: none) */
    } pwait;
    bool pb_timed_out = false;             /* the last PLAY gave up waiting for the music profile */
    std::string pb_quality;                /* "" / "music" / "call" / "unknown" for the output in use */
    uint64_t pb_quality_ms = 0;
    /* the live monitor: its target and its figures */
    std::string mon_key;                   /* route + device the running monitor plays to */
    bool mon_lost_bt = false;              /* the monitor's Bluetooth sink went away: restart when it is back */
    double mon_delay_ms = 0;
    uint64_t mon_started_ms = 0, mon_delay_at_ms = 0;
    /* the decoders of FLAC / Opus takes (Player, peaks, trim): resolved once, at the first take that needs them
     * (cfg.decoders, else the real libraries with dlopen); written under mu before any user reads it */
    DecoderLibs dlibs;
    bool dlibs_ready = false;
    OutputPlan plan;
    /* the playback meter (service thread, under mu): fed from the Player's level tap while a take plays or is paused */
    Meter play_meter{play_meter_format()};
    bool play_meter_on = false;
    uint32_t play_seq = 0;
    uint16_t play_ch = 1;
    float play_db[2] = {Meter::kFloorDb, Meter::kFloorDb};
    uint64_t play_new_ms = 0, play_tick_ms = 0;
    /* the last 5 s jump, for ScanSnap::jump_dir */
    uint64_t jump_ms = 0;
    int jump_dir = 0;

    /* the take being recorded */
    std::unique_ptr<Ring> ring;
    std::unique_ptr<Writer> writer;
    bool take_active = false, paused = false, unplug_flag = false;
    std::string take_name;
    uint64_t pause_total = 0, pause_start = 0;
    uint32_t base_overruns = 0;
    uint64_t base_lost = 0;
    std::atomic<uint64_t> take_lost{0}; /* running estimate of the take's lost frames: the writer keeps it in the journal */
    bool lost_noticed = false;          /* the "some audio was lost" notice was raised for this take */
    ProblemSnap prob;

    /* takes */
    std::vector<TakeRow> rows;
    unsigned takes_rev = 0;
    std::string target;       /* the LOADED take: the one PLAY, REW and FWD act on; empty = nothing in the deck */
    std::string preview_name; /* a take of the list playing without being loaded (preview_take); empty otherwise */
    unsigned load_seq = 0;    /* counts load() calls: the UI plays its cassette animation when this moves */
    SpaceInfo space;
    uint64_t last_space_ms = 0;
    /* the file format a take gets now: the stored choice when the capture format and the libraries allow it
     * (effective_choice), else WAV; recomputed with the free space (every 2 s) and when a take starts */
    FormatChoice rec_choice;  /* valid when rec_compressed */
    bool rec_compressed = false;
    CaptureOffer active_offer;    /* what the open card offered (empty: it was busy, recording through the sound server) */
    bool ignore_prefs = false;    /* the card refused the chosen rate or depth: open with its own defaults until the next choice */

    /* trim page */
    struct Trim {
        bool open = false;
        std::string take, path;
        uint64_t frames = 0;
        uint32_t rate = 48000;
        uint64_t in = 0, out = 0, pos = 0, wstart = 0;
        std::vector<uint64_t> splits;
        int sel = -1, zoom = 0;
        bool previewing = false;
        int grace = 0;
        bool replace_kind = false;
    } tr;
    /* workers (atomics, or guarded by wmu) */
    std::mutex wmu;
    std::string peaks_error;
    EditResult save_result;
    std::atomic<int> peaks_state{0};
    std::atomic<uint64_t> peaks_done{0}, peaks_total{0};
    std::atomic<bool> peaks_cancel{false}, peaks_mem{false};
    std::thread peaks_th;
    std::atomic<int> save_state{0};
    std::atomic<uint64_t> save_done{0}, save_total{1};
    std::atomic<bool> save_cancel{false};
    std::thread save_th;
    std::string save_msg;
    std::vector<std::string> save_outputs;
    int save_seen = 0;
    std::mutex pyr_mu;
    std::shared_ptr<PeakPyramid> pyr;
    /* the merge worker (atomics, or guarded by wmu; merge_names, merge_out, merge_msg ... by mu) */
    MergeResult merge_result;
    std::atomic<int> merge_state{0};
    std::atomic<uint64_t> merge_done{0}, merge_total{1};
    std::atomic<bool> merge_cancel{false};
    std::thread merge_th;
    int merge_seen = 0;
    unsigned merge_seq = 0;
    std::vector<std::string> merge_names; /* the takes of the running merge (in_use) */
    std::string merge_out, merge_msg;
    bool merge_deleted_all = false;
    int merge_count = 0;

    /* service thread */
    std::thread svc;
    std::atomic<bool> quit{false};

    /* published state: never taken together with mu by a reader */
    mutable std::mutex pub_mu;
    Snapshot pub;
    std::vector<TakeRow> pub_rows;

    /* ================================================================================================================ */
    /* small helpers */

    void add_notice(int kind, const std::string &text) {
        Notice n;
        n.kind = kind;
        n.text = text;
        notices.push_back(n);
    }

    bool run_deck(DeckEvent e) {
        const bool r = deck.handle(e);
        while (!pend.empty()) {
            const DeckEvent x = pend.front();
            pend.pop_front();
            deck.handle(x);
        }
        deck.set_has_last_take(!target.empty()); /* the loaded take is the truth (a take that failed to start leaves none) */
        return r;
    }

    /* The take the player opens: a previewed take of the list, else the loaded one. */
    std::string play_target() const { return preview_name.empty() ? target : preview_name; }
    /* Is `name` in use (playing or previewing, recording, open for trimming)? rename and delete refuse such a take. */
    bool in_use(const std::string &name) const {
        return (tr.open && tr.take == name) || (take_active && take_name == name) ||
               (deck.state() != DeckState::Idle && play_target() == name) ||
               (merge_state.load() == 1 && std::find(merge_names.begin(), merge_names.end(), name) != merge_names.end());
    }
    void load_newest_if_empty() {
        if (target.empty() && !rows.empty()) target = rows.front().name;
        for (TakeRow &r : rows) r.current = r.name == target;
        deck.set_has_last_take(!target.empty());
    }

    void persist() {
        const int r = store.save();
        if (r < 0 && !settings_error_shown) {
            settings_error_shown = true;
            add_notice(Notice::Info, std::string("The settings could not be saved: ") + std::strerror(-r));
        } else if (r == 0) {
            settings_error_shown = false;
        }
    }

    bool recording_family() const {
        const DeckState s = deck.state();
        return s == DeckState::Countdown || s == DeckState::Recording || s == DeckState::RecordingPaused;
    }
    bool playing_family() const {
        const DeckState s = deck.state();
        return s == DeckState::Playing || s == DeckState::PlayingPaused || s == DeckState::ScanningForward ||
               s == DeckState::ScanningReverse;
    }

    /* ================================================================================================================ */
    /* takes list */

    static std::string date_text(const TakeInfo &t) {
        char b[48];
        if (!t.origination_date.empty()) return t.origination_date + " " + t.origination_time.substr(0, 5);
        if (t.named && t.tn.has_date) {
            std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", t.tn.when.year, t.tn.when.month, t.tn.when.day,
                          t.tn.when.hour, t.tn.when.minute);
            return b;
        }
        return "";
    }

    void refresh_takes_locked() {
        std::vector<TakeInfo> v;
        list_takes(*ops, folder, v);
        rows.clear();
        for (const TakeInfo &t : v) {
            TakeRow r;
            r.name = t.name;
            r.label = t.label;
            r.path = t.path;
            r.seconds = t.seconds;
            r.frames = t.frames;
            r.size = t.size;
            r.rate = t.fmt.rate;
            r.channels = t.fmt.channels;
            r.bits = t.fmt.bits;
            r.format_text = t.damaged ? std::string() : format_text(t.fmt, format_tag(t.format));
            r.format_tag = format_tag(t.format);
            r.date_text = date_text(t);
            r.damaged = t.damaged;
            r.repaired = t.repaired;
            r.named = t.named;
            rows.push_back(r);
        }
        /* a loaded take that is gone (deleted) leaves the deck empty; start(), a new folder and the end of a recording
         * load the newest take themselves (load_newest_if_empty) */
        bool found = false;
        for (const TakeRow &r : rows) found = found || r.name == target;
        if (!found) target.clear();
        for (TakeRow &r : rows) r.current = r.name == target;
        deck.set_has_last_take(!target.empty());
        ++takes_rev;
    }

    bool has_take(const std::string &name) const {
        for (const TakeRow &r : rows)
            if (r.name == name) return true;
        return false;
    }

    CodecAvailability codecs_now() const { return cfg.codecs ? cfg.codecs() : codec_availability_here(); }

    /* The model's inputs for the capture that runs now: its format and what the card offers (the Rate and Quality pickers). */
    void fill_inputs(FormatInputs &in) const {
        in.codecs = codecs_now();
        in.speech_only = active_is_bt; /* Bluetooth call audio: no Opus music presets */
        if (!(fmt.rate && fmt.channels && fmt.bits)) return;
        in.state = CaptureState::Known;
        in.capture = fmt;
        in.rates = active_offer.rates;
        if (active_offer.rates.empty()) { /* busy: the server's format is all there is */
            in.has16 = fmt.bits == 16;
            in.has24 = fmt.bits == 24;
        } else {
            in.has16 = active_offer.has_format(SampleFormat::S16_LE);
            in.has24 = active_offer.has_format(SampleFormat::S24_3LE) || active_offer.has_format(SampleFormat::S32_LE);
        }
    }

    /* The rate and depth to open a card with: the stored choice (codec, rate, quality) on what the card offers. */
    void prefer_capture(const CaptureOffer &o, uint32_t &rate, unsigned &bits) const {
        rate = 0;
        bits = 0;
        if (ignore_prefs) return;
        const FormatInputs in = inputs_for_offer(o, codecs_now());
        if (in.state != CaptureState::Known) return;
        const RecParams p = effective_params(in, stored_of(store.values()));
        rate = p.rate;
        bits = p.bits;
    }

    void update_rec_choice() {
        rec_compressed = false;
        const Settings &s = store.values();
        if (s.format == RecFormat::Wav || !(fmt.rate && fmt.channels && fmt.bits)) return; /* WAV: no library is loaded */
        FormatInputs in;
        fill_inputs(in);
        const FormatChoice c = effective_for(in, stored_of(s));
        if (c.compatible && c.key.format != RecFormat::Wav) {
            /* the capture that runs may differ from the resolved one (a card busy: the server's 48 kHz): the encoder must
             * accept what actually arrives */
            const bool ok = c.key.format == RecFormat::Flac ? flac_format_ok(fmt) : opus_format_ok(fmt);
            if (!ok) return;
            rec_choice = c;
            rec_compressed = true;
        }
    }

    /* After a choice of codec, rate or quality: reopens the capture when the rate or depth it runs at is not the one the
     * stored choice asks for now. Nothing running (no card, a busy card, a take): nothing to do, the next open follows
     * the choice. A card that refuses what it offered is opened with its own defaults and the choice stays stored. */
    void apply_capture_choice() {
        if (!have_card || !cap || via_server || active_is_bt || take_active || active_offer.rates.empty()) return;
        uint32_t rate = 0;
        unsigned bits = 0;
        prefer_capture(active_offer, rate, bits);
        if (!rate || (rate == fmt.rate && bits == fmt.bits)) return;
        if (deck.state() != DeckState::Idle) run_deck(DeckEvent::Stop);
        close_capture();
        lost = false;
        if (!try_open_capture() && !cap) {
            ignore_prefs = true;
            if (try_open_capture()) add_notice(Notice::Info, "The card would not record at that rate: using its own");
        }
        rebuild_output();
    }

    void refresh_space() {
        AudioFormat f = fmt;
        if (!(f.rate && f.channels && f.bits)) f = AudioFormat();
        if (folder_space(*ops, folder, f, space) < 0) space = SpaceInfo();
        update_rec_choice();
        if (rec_compressed) space.hours_left = hours_for_space(space.free_bytes, rec_choice.size);
        last_space_ms = clk->mono_ms();
    }

    /* ================================================================================================================ */
    /* cards, mixer, capture path */

    /* The index of the chosen input in the combined list (ALSA cards, then the Bluetooth devices). */
    void fix_sel() {
        sel = -1;
        if (!have_card) return;
        if (active_is_bt) {
            for (size_t i = 0; i < bt_list.size(); ++i)
                if (bt_list[i].id == active_key) sel = static_cast<int>(cards.size() + i);
        } else {
            for (size_t i = 0; i < cards.size(); ++i)
                if (cards[i].info.id == active.id) sel = static_cast<int>(i);
        }
    }

    void refresh_cards_locked() {
        cards = list_capture_cards(*cap_api(), false);
        bt_request_scan();
        bt_apply_scan();
        bt_rebuild();
        fix_sel();
    }

    /* ---- Bluetooth: discovery (the server is asked on the worker; the answers are taken here) ---- */

    PulseWorker *worker() {
        if (!pw && cfg.pulse) pw.reset(new PulseWorker(*cfg.pulse));
        return pw.get();
    }

    /* Asks the worker to (re)connect when one runs and there is no connection (at most once per cfg.pulse_retry_ms) and to list
     * sources, sinks and cards. Never waits for the server. */
    void bt_request_scan() {
        bt_last_refresh_ms = clk->mono_ms();
        PulseWorker *w = worker();
        if (!w) {
            bt_status = kBtNotSetUp;
            bt_scan_in.clear();
            bt_outs.clear();
            return;
        }
        ScanRequest rq;
        const bool lost_conn = bt_gone.exchange(false); /* the connection was lost: connect again at once */
        if (lost_conn) {
            rq.disconnect_first = true;
            bt_cb_set = false;
            bt_connect_tried = false;
            bt_is_connected = false;
        }
        if (!bt_is_connected) {
            const bool running = cfg.server_running ? cfg.server_running() : sound_server_running_here();
            const uint64_t now = clk->mono_ms();
            if (!running) {
                bt_status = kBtNotSetUp;
                bt_scan_in.clear();
                bt_outs.clear();
                if (lost_conn) bt_gone.store(true);
                return;
            }
            if (bt_connect_tried && now - bt_last_connect_ms < cfg.pulse_retry_ms) { /* the last answer stands */
                if (lost_conn) bt_gone.store(true);
                return;
            }
            bt_connect_tried = true;
            bt_last_connect_ms = now;
            rq.connect = true;
        }
        if (!bt_cb_set) { /* follow the server's changes (a new device, a lost connection) from the service tick */
            rq.callback = [this](const PulseChange &c) {
                if (c.kind == PulseChange::Kind::Disconnected) bt_gone.store(true);
                bt_dirty.store(true);
            };
        }
        if (!w->request_scan(rq)) { /* one is running: ask again soon */
            if (lost_conn) bt_gone.store(true);
            bt_rescan = true;
        } else {
            bt_scan_asked_ms = clk->mono_ms();
        }
    }

    /* A fresh listing as soon as possible (now when none is running, else right after it). */
    void bt_refresh_soon() {
        PulseWorker *w = worker();
        if (w && !w->scan_pending()) bt_request_scan();
        else bt_rescan = true;
    }

    /* The applied scan as text: the entries and the facts the controller acts on. */
    std::string bt_scan_digest() const {
        std::string d = bt_status + "|";
        for (const BtInput &b : bt_scan_in)
            d += b.id + "," + b.source + "," + std::to_string(b.rate) + "," + (b.usable ? "u" : "-") + b.hint_profile + "," + b.label + ";";
        d += "|";
        for (const BtOutput &b : bt_outs)
            d += b.id + "," + b.sink + "," + b.sink_profile + "," + std::to_string(b.rate) + "," + (b.usable ? "u" : "-") + b.hint_profile +
                 (b.music_available ? "m" : "-") + "," + b.label + ";";
        return d;
    }

    /* Takes the worker's finished scan, if any, into bt_scan_in / bt_outs / bt_status. true when one was applied. */
    bool bt_apply_scan() {
        ScanResult r;
        if (!pw || !pw->take_scan(r)) return false;
        bt_scan_ms = bt_scan_asked_ms; /* the request's time: an answer counts from when the server was asked */
        if (r.callback_set) bt_cb_set = true;
        bt_is_connected = r.connected;
        if (r.connect_result != 1 && r.connect_result != 0) {
            bt_status = (r.connect_result == kPulseMissing || r.connect_result == kPulseNoServer) ? kBtNotSetUp : kBtNotResponding;
            bt_scan_in.clear();
            bt_outs.clear();
        } else if (!r.connected || !r.listed) {
            bt_status = kBtNotResponding;
            bt_scan_in.clear();
            bt_outs.clear();
        } else {
            bt_status.clear();
            const PulseProbe probe = pulse_probe(r.sources, r.sinks, r.cards);
            bt_scan_in = bt_inputs(probe);
            bt_outs = bt_outputs(probe);
            for (const BtInput &b : bt_scan_in)
                if (b.usable) bt_labels[b.id] = b.label;
            for (const BtOutput &b : bt_outs)
                if (b.usable) bt_labels[b.id] = b.label;
        }
        /* only a scan that lists something else is a reason to try the input again: each try asks for a new scan, so "every scan"
         * kept the server busy at the tick rate while the chosen input was absent (T9); the replug timer still retries */
        std::string digest = bt_scan_digest();
        if (digest != bt_digest) bt_changed = true;
        bt_digest = std::move(digest);
        if (mon_lost_bt && monitor_target().ok) {
            mon_lost_bt = false;
            mon_failed = false; /* the monitor's output is back */
        }
        refresh_plan();
        return true;
    }

    /* bt_list = what the last scan listed, plus a placeholder for a chosen headset that is not listed (connected false). */
    void bt_rebuild() {
        bt_list = bt_scan_in;
        const std::string mac = bt_mac_of_id(want_text);
        if (mac.empty()) return;
        for (const BtInput &b : bt_list)
            if (b.mac == mac) return;
        BtInput ph;
        ph.mac = mac;
        ph.id = bt_id_of(mac);
        auto l = bt_labels.find(ph.id);
        ph.label = l != bt_labels.end() ? l->second : std::string("Bluetooth headset");
        ph.kind = "headset";
        ph.reason = "Not connected";
        bt_list.push_back(ph);
    }

    /* Startup only: waits a little for the first scan so a chosen headset opens at once. */
    void bt_wait_scan(unsigned ms) {
        PulseWorker *w = worker();
        if (!w) return;
        bt_request_scan();
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (w->scan_pending() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        bt_apply_scan();
        bt_rebuild();
    }

    const BtInput *bt_find(const std::string &mac) const {
        for (const BtInput &b : bt_list)
            if (b.mac == mac) return &b;
        return nullptr;
    }

    /* Is the Bluetooth microphone wanted open now? Open on the recorder (and settings) screen, closed on the list and
     * Edit pages and during playback so the headset returns to music mode (decision 5); never closed during a take. */
    bool bt_mic_wanted() const {
        if (take_active || recording_family()) return true;
        if (page == UiPage::List || page == UiPage::Edit) return false;
        if (tr.open || tr.previewing || !preview_name.empty() || playing_family()) return false;
        if (player && player->state() != PlayerState::Stopped) return false;
        if (app_left) return false;
        return true;
    }

    AlsaApi *cap_api() const { return cfg.api; }

    void save_card_state() {
        if (!have_card) return;
        CardState cs;
        cs.has_level = mixer && mixer->caps().has_level && mstate.level_valid;
        cs.level_db = mstate.level_db;
        cs.auto_gain = mstate.agc_valid && mstate.agc;
        cs.mute = mstate.mute_valid && mstate.mute;
        store.set_card_state(active_key, cs);
        persist();
    }

    void apply_dsp_settings() {
        if (!proc) return;
        const Settings &s = store.values();
        proc->set_soft_gain_db(s.soft_gain_db);
        proc->set_highpass(s.low_cut);
        proc->set_limiter(s.limiter);
    }

    /* ---- live monitor ---- */

    /* Where the monitor plays (docs/PULSE.md "Monitor routes"): settings monitor_output ("" = automatic). */
    struct MonTarget {
        bool ok = false;        /* a target exists now */
        bool bt = false;        /* a Bluetooth sink (through the sound server) */
        std::string route;      /* "same_headset", "other_output", "card"; "" when there is none */
        std::string device;     /* ALSA device, or the sink's node name */
        std::string id;         /* the output's id as the lists spell it */
        std::string label;
        bool a2dp = false;      /* a Bluetooth sink on a music profile: the delay is a few tenths of a second */
        std::string why;        /* not ok: why */
    };

    MonTarget monitor_target() const {
        MonTarget t;
        if (!have_card) {
            t.why = "No input";
            return t;
        }
        const std::string want = store.values().monitor_output;
        const std::string in_mac = active_is_bt ? bt_mac_of_id(active_key) : std::string();
        std::string mac = bt_mac_of_id(want);
        if (want.empty() && active_is_bt) mac = in_mac;
        if (!mac.empty()) { /* a Bluetooth sink: the same headset, or another device */
            t.bt = true;
            t.id = bt_id_of(mac);
            t.route = (!in_mac.empty() && in_mac == mac) ? "same_headset" : "other_output";
            auto l = bt_labels.find(t.id);
            t.label = l != bt_labels.end() ? l->second : std::string("Bluetooth device");
            const BtOutput *o = bt_status.empty() ? bt_out_find(mac) : nullptr;
            if (!o || !o->usable || !o->connected) {
                t.why = o && o->connected ? o->reason : std::string(kBtMonitorNotThere);
                return t;
            }
            t.ok = true;
            t.device = o->sink;
            t.label = o->label;
            t.a2dp = o->hint_profile == "music" || (o->hint_profile == "unknown" && !o->call_offered);
            return t;
        }
        if (want.empty()) { /* automatic, an ALSA input: its own playback side */
            if (!active.playback) {
                t.why = "The sound card has no playback side";
                return t;
            }
            t.ok = true;
            t.route = "card";
            t.device = pcm_device(active);
            t.id = t.device;
            t.label = active.name;
            return t;
        }
        /* an explicit ALSA device */
        t.id = want;
        t.device = want;
        t.route = (!active_is_bt && want == pcm_device(active)) ? "card" : "other_output";
        t.label = want;
        const std::vector<OutputOption> &opts = plan.choices.empty() ? std::vector<OutputOption>{plan.chosen} : plan.choices;
        for (const OutputOption &o : opts)
            if (!o.bluetooth && o.device == want) {
                t.ok = true;
                t.label = o.label;
            }
        if (!t.ok) t.why = kBtMonitorNotThere;
        return t;
    }

    bool monitor_wanted() const {
        if (!started || down || !cap || !cap->running() || !mtap || !have_card) return false;
        if (!store.values().monitor || app_left) return false;
        if (playing_family() || tr.previewing || tr.open || !preview_name.empty()) return false;
        if (player && player->state() != PlayerState::Stopped) return false; /* the Player holds (or opens) the card */
        if (active_is_bt && !(bt_gate && bt_gate->open.load() && bt_stream && !bt_stream->ended())) return false; /* connecting */
        return monitor_target().ok;
    }
    void monitor_stop() {
        if (mon) mon->stop();
        mon.reset();
        mon_key.clear();
        mon_delay_ms = 0;
    }
    /* Brings the monitor in line with the state; called from publish() (every command and every service tick). */
    void monitor_sync() {
        if (mon && !mon->running()) { /* the sink died (a device error, the headset went away) */
            const bool was_bt = mon_key.compare(0, 3, "bt:") == 0;
            monitor_stop();
            mon_failed = true;
            if (was_bt) {
                mon_lost_bt = true;
                add_notice(Notice::Info, kBtMonitorGone);
            } else {
                add_notice(Notice::Info, "Monitor unavailable");
            }
        }
        if (mon && mon_key.compare(0, 3, "bt:") == 0 && store.values().monitor && !app_left && cap && cap->running() &&
            !monitor_target().ok) { /* its Bluetooth output is gone from the lists: say so, the recording is not touched */
            monitor_stop();
            mon_failed = true;
            mon_lost_bt = true;
            add_notice(Notice::Info, kBtMonitorGone);
            return;
        }
        if (!monitor_wanted()) {
            if (mon) monitor_stop();
            return;
        }
        const MonTarget t = monitor_target();
        const std::string key = (t.bt ? "bt:" : "alsa:") + t.device;
        if (mon && mon_key != key) monitor_stop(); /* the target changed: play to the new one */
        if (!mon && !mon_failed) {
            std::unique_ptr<PlaybackSink> sk;
            MonitorOptions mo;
            if (t.bt) {
                if (!cfg.pulse) return;
                std::unique_ptr<PulsePlaybackSink> ps(new PulsePlaybackSink(*cfg.pulse, pulse_monitor_sink_options()));
                ps->set_sink(t.device);
                sk = std::move(ps);
                mo.prefill_ms = 10;
                mo.drift_control = true;
                mo.max_queue_ms = 2000; /* the stream's delay includes the device's own latency: never a reason to drop */
            } else if (cfg.make_sink) {
                sk = cfg.make_sink(t.device);
            } else {
                sk.reset(new AlsaPlaybackSink(*cap_api(), t.device));
            }
            std::unique_ptr<Monitor> m(new Monitor(*mtap));
            m->set_level_pct(store.values().monitor_level);
            if (m->start(std::move(sk), mo) < 0) {
                mon_failed = true;
                add_notice(Notice::Info, "Monitor unavailable");
                return;
            }
            mon = std::move(m);
            mon_key = key;
            mon_delay_ms = 0;
            mon_started_ms = clk->mono_ms();
            mon_delay_at_ms = 0;
        }
        if (mon) mon->set_level_pct(store.values().monitor_level);
    }

    /* The monitor's delay figure (capture latency + ring + playback latency), refreshed about once a second while it runs. For a
     * Bluetooth input the capture and playback figures are the server's own reports; a card's capture is its nominal period. */
    void monitor_delay_tick(uint64_t now) {
        if (!mon || !mon->running()) {
            mon_delay_ms = 0;
            return;
        }
        if (mon_delay_at_ms && now - mon_delay_at_ms < kMonitorDelayMs) return;
        if (!mon_delay_at_ms && now - mon_started_ms < kMonitorDelayFirstMs) return;
        mon_delay_at_ms = now;
        const MonitorStats st = mon->stats();
        const double rate = fmt.rate ? static_cast<double>(fmt.rate) : 48000.0;
        double cap_ms = 1000.0 * 1024.0 / rate;
        PulseLatency l;
        if (bt_stream && bt_stream->latency(l) == 0) cap_ms = l.total_ms;
        const double ring_ms = 1000.0 * static_cast<double>(st.backlog_frames) / rate;
        double play_ms = st.latency_ms;
        if (PlaybackSink *sk = mon->sink())
            if (PulsePlaybackSink *ps = dynamic_cast<PulsePlaybackSink *>(sk)) {
                double d = 0;
                if (ps->delay_ms(d)) play_ms = d;
            }
        mon_delay_ms = cap_ms + ring_ms + play_ms;
    }

    void close_capture() {
        monitor_stop();
        cap.reset();
        proc.reset();
        mixer.reset();
        mstate = MixerState();
        bt_stream.reset(); /* the capture's PCM closed the stream; ours was the second share */
        bt_gate.reset();
        bt_silent = false;
        notices.erase(std::remove_if(notices.begin(), notices.end(), [](const Notice &n) { return n.text == kBtNoSound; }),
                      notices.end());
    }

    /* Finds the wanted card and opens the capture path. true when the meter is running. */
    bool try_open_capture() {
        refresh_cards_locked();
        if (!bt_mac_of_id(want_text).empty()) return try_open_bt();
        active_is_bt = false;
        bt_policy_closed = false;
        std::vector<CardInfo> infos;
        for (const CaptureCard &c : cards) infos.push_back(c.info);
        int pos = -1;
        if (!want_text.empty()) {
            const Selection s = cassette::select_card(infos, parse_descriptor(want_text));
            if (s.found()) pos = s.pos;
        }
        if (pos < 0 && !lost && !infos.empty()) pos = 0; /* never opened or no saved choice: the first capture card */
        if (pos < 0) {
            have_card = false;
            sel = -1;
            cap_message = infos.empty() ? "No sound card with a microphone input" : "The chosen sound card is not connected";
            return false;
        }
        const CardInfo info = infos[static_cast<size_t>(pos)];
        OpenOptions oopt;
        oopt.prefer = [this](const CaptureOffer &o, uint32_t &rate, unsigned &bits) { prefer_capture(o, rate, bits); };
        OpenResult r = open_capture(*cap_api(), info, oopt);
        active = info;
        active_offer = r.offer;
        have_card = true;
        active_key = key_of(info);
        sel = pos;
        if (!r.ok()) {
            cap_message = r.message.empty() ? std::string("The sound card could not be opened") : r.message;
            return false;
        }
        /* the mixer: saved state back on the card (it resets at every boot) */
        mixer.reset(new Mixer(*cap_api(), mixer_device(info)));
        mixer->discover();
        CardState cs;
        if (store.card_state(active_key, cs)) {
            MixerSetting ms;
            ms.set_level = cs.has_level && mixer->caps().has_level;
            ms.level_db = cs.level_db;
            ms.set_agc = mixer->caps().has_agc;
            ms.agc = cs.auto_gain;
            ms.set_mute = mixer->caps().has_mute;
            ms.mute = cs.mute;
            mixer->reapply(ms);
        }
        mstate = mixer->read();
        fmt = r.format;
        via_server = r.via_server;
        mon_failed = false;
        mtap.reset(new MonitorTap(fmt));
        proc.reset(new ProcessingSink(fmt, &gate));
        proc->set_tap(mtap.get());
        apply_dsp_settings();
        CaptureOptions copt;
        copt.now = cfg.capture_now;
        std::unique_ptr<Capture> c(new Capture(std::move(r), *proc, std::move(copt)));
        if (!c->start()) {
            cap_message = "The sound card stopped answering";
            proc.reset();
            mixer.reset();
            return false;
        }
        cap = std::move(c);
        cap_message.clear();
        if (lost) {
            lost = false;
            add_notice(Notice::CardBack, "The sound card is back");
        }
        return true;
    }

    /* The chosen input is a Bluetooth headset (want_text "bt:<MAC>"): opens its microphone through the sound server on the
     * exact source, at the call rate asked for explicitly, S16 mono, never moved (T1). false: not there, not usable, closed
     * on purpose (bt_policy_closed) or refused; cap_message says why. */
    bool try_open_bt() {
        const std::string mac = bt_mac_of_id(want_text);
        const BtInput *b = bt_find(mac);
        active = CardInfo();
        active.id = bt_id_of(mac);
        active.name = b ? b->label : std::string("Bluetooth headset");
        active.capture = true;
        active_key = active.id;
        active_offer = CaptureOffer();
        active_is_bt = true;
        have_card = true;
        via_server = false;
        fix_sel();
        if (!b || !b->connected) {
            cap_message = kBtNotConnected;
            return false;
        }
        if (!b->usable) {
            cap_message = b->reason;
            return false;
        }
        if (!bt_mic_wanted()) { /* the list, the Edit page, playback: the headset goes back to music mode */
            bt_policy_closed = true;
            cap_message.clear();
            return false;
        }
        bt_policy_closed = false;
        PulseCaptureRequest rq;
        rq.source = b->source;
        rq.rate = b->rate;
        rq.channels = 1;
        std::unique_ptr<PulseCapture> pc;
        std::string why;
        const int r = cfg.pulse ? cfg.pulse->open_capture(rq, pc, &why) : kPulseState;
        if (r != 0 || !pc) {
            if (r == kPulseNoDevice) cap_message = kBtNotConnected;
            else if (r == kPulseMoved) cap_message = "The sound server attached the microphone to another device: not used";
            else cap_message = "The Bluetooth microphone could not be opened: " + (why.empty() ? pulse_error_text(r) : why);
            return false;
        }
        std::shared_ptr<PulseCapture> stream(std::move(pc));
        std::shared_ptr<PulseLeadGate> lead(new PulseLeadGate());
        OpenResult r2 = pulse_open_result(stream, 50, lead);
        mixer.reset(); /* no hardware level: software gain only */
        mstate = MixerState();
        fmt = r2.format;
        active_offer.rates = {fmt.rate};
        active_offer.min_channels = active_offer.max_channels = fmt.channels;
        active_offer.formats = {SampleFormat::S16_LE};
        mon_failed = false;
        mtap.reset(new MonitorTap(fmt));
        proc.reset(new ProcessingSink(fmt, &gate));
        proc->set_tap(mtap.get());
        apply_dsp_settings();
        CaptureOptions copt;
        copt.now = cfg.capture_now;
        std::unique_ptr<Capture> c(new Capture(std::move(r2), *proc, std::move(copt)));
        if (!c->start()) {
            cap_message = "The Bluetooth microphone stopped answering";
            proc.reset();
            return false;
        }
        cap = std::move(c);
        bt_stream = stream;
        bt_gate = lead;
        bt_card_name = b->card;
        bt_running_ms = bt_profile_poll_ms = 0;
        bt_ident_ms = clk->mono_ms();
        bt_silent = false;
        cap_message.clear();
        if (lost) {
            lost = false;
            add_notice(Notice::CardBack, kBtBack);
        }
        return true;
    }

    void on_capture_lost(const CaptureStats &st) {
        (void)st;
        lost = true;
        want_text = active_key; /* wait for this very card to come back */
        unplug_msg.clear();
        if (active_is_bt) {
            const bool server = bt_stream && bt_stream->end_code() == kPulseDisconnected;
            cap_message = kBtNotConnected;
            lost_text = take_active ? (server ? kBtServerLostTake : kBtLostTake) : (server ? kBtServerLostIdle : kBtLostIdle);
            unplug_msg = lost_text;
        } else {
            cap_message = "The sound card was unplugged";
            lost_text = "The sound card was unplugged";
        }
        if (recording_family()) {
            unplug_flag = true;
            run_deck(DeckEvent::DeviceLost); /* ends the take (kept) or cancels the countdown, then the notice */
        } else {
            add_notice(Notice::CardLost, lost_text);
        }
        unplug_flag = false;
        close_capture();
        last_retry_ms = clk->mono_ms();
    }

    /* The output list and the choice in use, from the cards, the saved choice and (when the server answered) the Bluetooth sinks.
     * Does not touch the Player; rebuild_output() does when the chosen device changed and nothing plays. */
    void refresh_plan() {
        const std::string saved = store.values().output_device;
        const std::string mac = bt_mac_of_id(saved);
        auto l = mac.empty() ? bt_labels.end() : bt_labels.find(bt_id_of(mac));
        plan = choose_output(cap_api()->cards(), have_card ? active.id : std::string(), saved, bt_status.empty() ? &bt_outs : nullptr,
                             l != bt_labels.end() ? l->second : std::string());
        if (plan.chosen.device != built_device && !take_active && !playing_family() && !pwait.active && !tr.previewing &&
            (!player || player->state() == PlayerState::Stopped))
            rebuild_output();
    }

    void rebuild_output() {
        if (take_active) return;
        player.reset();
        sink.reset();
        bt_sink = nullptr;
        plan = choose_output(cap_api()->cards(), have_card ? active.id : std::string(), store.values().output_device,
                             bt_status.empty() ? &bt_outs : nullptr,
                             [&] {
                                 auto l = bt_labels.find(bt_id_of(bt_mac_of_id(store.values().output_device)));
                                 return l != bt_labels.end() ? l->second : std::string();
                             }());
        built_device = plan.chosen.device;
        if (plan.chosen.bluetooth && cfg.pulse) { /* a Bluetooth sink, through the sound server at the take's rate */
            std::unique_ptr<PulsePlaybackSink> ps(new PulsePlaybackSink(*cfg.pulse));
            bt_sink = ps.get();
            sink = std::move(ps);
        } else if (plan.chosen.bluetooth) { /* no sound server in this build (the demo): never opens (resolve_output refuses) */
            sink.reset(new AlsaPlaybackSink(*cap_api(), kDefaultOutput));
        } else if (cfg.make_sink) {
            sink = cfg.make_sink(plan.chosen.device);
        } else {
            sink.reset(new AlsaPlaybackSink(*cap_api(), plan.chosen.device));
        }
        player.reset(new Player(*ops, *sink, Player::Mode::Threaded, &dlibs));
    }

    const BtOutput *bt_out_find(const std::string &mac) const { return bt_output_find(bt_outs, mac); }

    /* ================================================================================================================ */
    /* recording */

    bool begin_take() {
        stop_preview();
        if (take_active) return false;
        if (merge_state.load() == 1) { /* the merge holds the next take number until it ends */
            add_notice(Notice::Info, "Wait until the merge has finished");
            return false;
        }
        if (!cap && active_is_bt) try_open_capture(); /* REC while the headset microphone was closed or lost */
        if (!cap || !cap->running()) {
            prob = ProblemSnap();
            prob.message = active_is_bt ? std::string(kBtNotConnected) : "No sound card is recording: the take cannot start";
            add_notice(Notice::Info, prob.message);
            return false;
        }
        if (bt_stream && !bt_identity_at_start()) { /* the stream is not on the headset's source (C16) */
            prob = ProblemSnap();
            prob.message = kBtIdentity;
            add_notice(Notice::Info, prob.message);
            return false;
        }
        const Settings &s = store.values();
        make_dirs(*ops, folder);
        uint64_t number = 0;
        int r = reserve_take_number(*ops, folder, number, s.prefix);
        prob = ProblemSnap();
        if (r < 0) {
            prob.write_error = true;
            prob.disk_full = r == -ENOSPC;
            prob.message = std::string("The take cannot be created: ") + std::strerror(-r);
            add_notice(Notice::Info, prob.message);
            return false;
        }
        update_rec_choice(); /* FLAC / Opus when the stored choice can record on this card, else WAV */
        TakeName tn = new_take_name(s.prefix, number, *cstate, *clk);
        if (rec_compressed) tn.format = rec_choice.key.format;
        const std::string first = format_take_name(tn);
        WriterConfig wc;
        wc.fmt = fmt;
        wc.take = file_stem(first);
        wc.recordings_dir = folder;
        const std::string dir = folder;
        wc.namer = [dir, tn](int part) {
            TakeName t = tn;
            t.part = static_cast<uint32_t>(part) + 1;
            return dir + "/" + format_take_name(t);
        };
        wc.lost_frames = &take_lost;
        take_lost.store(0);
        lost_noticed = false;
        wc.device_name = active.name;
        wc.device_id = active.usb_by_id.empty() ? active.id : active.usb_by_id;
        wc.sync_interval_s = static_cast<uint32_t>(s.sync_interval_s);
        wc.sync_interval_ms = cfg.writer_sync_ms;
        wc.auto_split = s.auto_split;
        wc.min_free_bytes = cfg.min_free_bytes;
        if (cfg.split_bytes) wc.split_bytes = cfg.split_bytes;
        if (cstate->synchronized()) {
            const int64_t now = clk->wall_s();
            DateTime dt;
            if (datetime_from_wall(now, dt)) {
                char b[32];
                const int sec = static_cast<int>(((now % 60) + 60) % 60);
                std::snprintf(b, sizeof b, "%04d-%02d-%02d", dt.year, dt.month, dt.day);
                wc.bwf.origination_date = b;
                std::snprintf(b, sizeof b, "%02d:%02d:%02d", dt.hour, dt.minute, sec);
                wc.bwf.origination_time = b;
                wc.bwf.time_reference =
                    static_cast<uint64_t>(dt.hour * 3600 + dt.minute * 60 + sec) * static_cast<uint64_t>(fmt.rate);
            }
        }
        wc.bwf.description = "Cassette " + wc.take;
        if (rec_compressed) {
            /* a fresh encoder per part; the Vorbis comments: the part's name, the date when the clock is set (D15) */
            const FormatChoice choice = rec_choice;
            const AudioFormat capture = fmt;
            auto make = cfg.make_encoder;
            wc.make_encoder = [choice, capture, make](int) {
                return make ? make(choice, capture) : real_encoder(choice, capture);
            };
            const std::string date = wc.bwf.origination_date;
            wc.tags = [tn, date](int part) {
                TakeName t = tn;
                t.part = static_cast<uint32_t>(part) + 1;
                Tags g{{"TITLE", file_stem(format_take_name(t))}};
                if (!date.empty()) g.push_back({"DATE", date});
                g.push_back({"ENCODER", "Cassette"});
                return g;
            };
            const bool flac = choice.key.format == RecFormat::Flac;
            wc.codec = flac ? "flac" : "opus";
            wc.codec_preset = choice.key.preset;
            wc.bitrate_bps = choice.opus_bitrate_bps;
            wc.force_mono = !flac && choice.opus_force_mono && fmt.channels == 2;
            if (!cfg.make_encoder) {
                if (flac) {
                    if (const FlacLib *l = real_flac()) wc.encoder_name = l->name + " " + l->version;
                } else if (const OpusEncLib *l = real_opusenc()) {
                    wc.encoder_name = l->name + " " + l->version;
                }
            }
        }
        ring.reset(new Ring(fmt, 8.0));
        writer.reset(new Writer(*ring, *ops, *clk, wc));
        r = writer->start();
        if (r < 0) {
            const WriterStatus st = writer->status();
            prob.disk_full = st.reason == StopReason::LowSpace;
            prob.write_error = !prob.disk_full;
            prob.message = st.reason_text.empty() ? std::string("The take cannot start: ") + std::strerror(-r) : st.reason_text;
            add_notice(Notice::Info, prob.message);
            writer.reset();
            ring.reset();
            return false;
        }
        writer->start_thread();
        const CaptureStats cs = cap->stats();
        base_overruns = cs.overruns;
        base_lost = cs.frames_lost_xrun;
        pause_total = pause_start = 0;
        paused = false;
        take_name = base_name(first);
        target = take_name;
        take_active = true;
        bt_ident_unknown_ms = 0;
        gate.attach(ring.get());
        /* what the screen must say about the capture path (the planned notices of study 058) */
        if (active_is_bt) {
            /* no card notice: the headset's own state is on the recorder screen (connecting, no sound) */
        } else if (via_server) {
            add_notice(Notice::Info, "Recording through the sound server: the card is in use");
        } else {
            const bool running = cfg.server_running ? cfg.server_running() : sound_server_running_here();
            if (running) add_notice(Notice::Info, "Other apps cannot use this card while Cassette records");
        }
        return true;
    }

    /* C16 at the start of a take: the stream must be on the headset's source. Attached to another source, or ended: refused at once.
     * Attached to no source (the server's answer names none): asked again every kBtIdentityRetryStepMs for up to kBtIdentityRetryMs
     * (T9), then refused. Waits on the controller's thread: REC is the user's own action (docs/PULSE.md). */
    bool bt_identity_at_start() {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBtIdentityRetryMs);
        for (;;) {
            std::string actual;
            if (bt_stream->source_identity_ok(&actual)) return true;
            if (!actual.empty() || bt_stream->ended()) return false;
            if (std::chrono::steady_clock::now() >= end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(kBtIdentityRetryStepMs));
        }
    }

    void end_take() {
        if (!take_active) return;
        gate.detach();
        if (cap) take_lost.store(cap->stats().frames_lost_xrun - base_lost); /* the closing journal carries the final count */
        writer->stop();
        const WriterStatus st = writer->status();
        prob.frames_dropped = ring->frames_dropped();
        if (cap) {
            const CaptureStats cs = cap->stats();
            prob.overruns = cs.overruns - base_overruns;
            prob.frames_lost = cs.frames_lost_xrun - base_lost;
            prob.lost_seconds = fmt.rate ? static_cast<double>(prob.frames_lost) / fmt.rate : 0.0;
        }
        if (unplug_flag) {
            prob.unplugged = true;
            prob.message = unplug_msg.empty() ? std::string("The sound card was unplugged: the take was closed and kept") : unplug_msg;
        }
        if (st.state == WriterState::Failed || st.reason == StopReason::LowSpace) {
            prob.disk_full = st.reason == StopReason::LowSpace;
            prob.write_error = !prob.disk_full;
            prob.message = st.reason_text;
            add_notice(Notice::Info, "Recording stopped: " + st.reason_text + ". The take was kept.");
        }
        writer.reset();
        ring.reset();
        take_active = false;
        paused = false;
        refresh_takes_locked();
        load_newest_if_empty(); /* the new take stays loaded; a take that was not kept leaves the newest one */
        refresh_space();
    }

    uint64_t elapsed_frames() const {
        if (!take_active || !ring) return 0;
        const uint64_t w = ring->frames_written();
        const uint64_t upto = paused ? pause_start : w;
        return upto > pause_total ? upto - pause_total : 0;
    }

    /* ================================================================================================================ */
    /* Deck commands */

    const DecoderLibs &decoders() {
        if (!dlibs_ready) {
            dlibs = cfg.decoders ? cfg.decoders() : real_decoder_libs();
            dlibs_ready = true;
        }
        return dlibs;
    }

    /* A FLAC / Opus take whose decoder library is not installed: true (and one notice "This take cannot be played:
     * FLAC not installed (libflac14)") instead of opening it. WAV and takes with their library: false. */
    bool refuse_missing_codec(const std::string &name, const char *what = "played") {
        const Container c = container_of_name(name);
        if (c != Container::Flac && c != Container::OggOpus) return false;
        const DecoderLibs &d = decoders();
        if ((c == Container::Flac && d.flac) || (c == Container::OggOpus && d.opus)) return false;
        add_notice(Notice::Info, std::string("This take cannot be ") + what + ": " + decode_error_text(kCodecMissing, name));
        return true;
    }

    /* ---- playback through a Bluetooth output ---- */

    static const char *profile_class(const std::string &p) {
        return pulse_profile_is_music(p) ? "music" : (pulse_profile_is_call(p) ? "call" : "unknown");
    }

    /* The chosen Bluetooth output's entry in the last scan, found by MAC; null when the output is not Bluetooth, the server is not
     * reachable or the device is not listed (switched off, out of range). */
    const BtOutput *bt_out_chosen() const {
        if (!plan.chosen.bluetooth || !bt_status.empty()) return nullptr;
        return bt_out_find(bt_mac_of_id(plan.chosen.device));
    }
    /* A device that is listed with a sink node now. */
    static bool sink_listed(const BtOutput *o) { return o && o->connected && o->usable && !o->sink.empty(); }

    /* The Bluetooth sink to play through: the node of the chosen device's MAC in the last scan (its name is the same in both profiles
     * and after a reconnect; PulsePlaybackSink looks it up again by name when it opens). Sets the quality shown. false when the scan
     * lists no sink for it now. A card or the default output: nothing to do. No notice: the callers decide (wait, or say why). */
    bool resolve_output() {
        if (!plan.chosen.bluetooth) {
            pb_quality.clear();
            return true;
        }
        const BtOutput *o = bt_out_chosen();
        if (!bt_sink || !sink_listed(o)) return false;
        bt_sink->set_sink(o->sink);
        pb_quality = o->hint_profile;
        return true;
    }

    /* The device of the chosen Bluetooth output is listed (a card or a sink), even when its sink node is missing for a moment. */
    bool bt_out_device_there() const {
        const BtOutput *o = bt_out_chosen();
        return o && o->connected;
    }

    /* RESUME / scan of a take that is open: its stream is the proof while it is open (a re-listing in between may miss the node for a
     * moment); else the output is resolved again from the last scan. false (and the notice): it cannot play now. */
    bool prepare_resume() {
        if (plan.chosen.bluetooth && bt_sink && bt_sink->is_open() && !bt_sink->ended()) return true;
        if (resolve_output()) return true;
        add_notice(Notice::Info, kBtOutNotConnected);
        return false;
    }

    /* The card's profile class as far as is known now: the last card poll when it is newer than the last scan, else the scan. */
    std::string bt_profile_now(const BtOutput &o) const {
        if (bt_card.found && bt_card.name == o.card && bt_card_ms >= bt_scan_ms) return profile_class(bt_card.card.active_profile);
        return o.hint_profile;
    }

    /* PLAY with the headset still (or about to be) on the call profile: it must go back to music first (study C19). Not needed for a
     * speaker, another device, or a headset whose music profile is not available (it plays at call quality at once, P6). The device
     * must be listed; its sink may be missing for a moment (T9: the node is re-created at each profile switch). */
    bool bt_music_wait_needed() {
        if (!plan.chosen.bluetooth || !active_is_bt) return false;
        const BtOutput *o = bt_out_chosen();
        if (!o || !o->connected) return false;
        if (!(o->music_offered && o->music_available)) return false;
        if (bt_mac_of_id(active_key) != o->mac) return false;
        const bool mic_open = cap && bt_stream;
        return mic_open || bt_profile_now(*o) != "music";
    }

    void bt_ask_card(const std::string &name, uint64_t now) {
        PulseWorker *w = worker();
        if (w && !name.empty() && w->request_card(name)) {
            bt_card_asked = name;
            bt_card_ask_ms = now;
        }
    }

    void begin_wait(PlayWait::After after, bool cue_origin, bool need_music) {
        stop_preview();
        monitor_stop();
        pwait = PlayWait();
        pwait.active = true;
        pwait.after = after;
        pwait.cue_origin = cue_origin;
        pwait.need_music = need_music;
        pwait.start_ms = clk->mono_ms();
        pwait.mic_was_open = cap && bt_stream;
        pb_timed_out = false;
        const BtOutput *o = bt_out_chosen();
        pb_quality = need_music ? std::string("call") : (o ? o->hint_profile : std::string("unknown"));
        if (o && need_music) bt_ask_card(o->card, pwait.start_ms);
        pwait.last_poll_ms = pwait.start_ms;
        if (!sink_listed(o)) {
            pwait.missing_ms = pwait.start_ms ? pwait.start_ms : 1;
            bt_refresh_soon(); /* ask the server now, not only when it sends an event */
        }
    }

    /* Why a take did not start: a codec, a file or the Player (Failed), or the Bluetooth sink was not there (SinkMissing: listed
     * without a node, or gone between the listing and the open, kPulseNoDevice). */
    enum class Start { Ok, Failed, SinkMissing };

    /* Opens the take and does `after` (play, cue, scan). */
    Start open_and_start(PlayWait::After after) {
        stop_preview();
        if (refuse_missing_codec(play_target())) return Start::Failed;
        monitor_stop(); /* the Player is about to open the card's playback side: the monitor lets go first */
        if (!player || play_target().empty()) return Start::Failed;
        if (!resolve_output()) return Start::SinkMissing;
        const std::string path = folder + "/" + play_target();
        int r = player->open(path);
        if (r < 0) {
            add_notice(Notice::Info, "This take cannot be played: " + decode_error_text(r, path));
            return Start::Failed;
        }
        switch (after) {
        case PlayWait::Play: r = player->play(); break;
        case PlayWait::ScanFwd: r = player->scan(Direction::Forward, kScanSpeed); break;
        case PlayWait::ScanRev: r = player->scan(Direction::Reverse, kScanSpeed); break;
        case PlayWait::Cued: r = 0; break;
        }
        if (r >= 0) return Start::Ok;
        return plan.chosen.bluetooth && r == -ENODEV ? Start::SinkMissing : Start::Failed;
    }

    /* PLAY / CUE of a deck that is not playing, without a wait or at the end of one. A Bluetooth sink that is missing while its device is
     * listed is waited for (T9) within the wait's bounds; a device that is not listed at all: the notice. false: the deck stops. */
    bool start_or_wait(PlayWait::After after, bool cue_origin, const PlayWait *from) {
        const Start s = open_and_start(after);
        if (s == Start::Ok) return true;
        if (s == Start::SinkMissing) {
            const uint64_t now = clk->mono_ms();
            const bool device = bt_out_device_there();
            if (!from && device) { /* a fresh PLAY: the node is being re-created */
                begin_wait(after, cue_origin, false);
                pwait.missing_ms = now ? now : 1;
                pwait.rescan_ms = now ? now : 1;
                bt_refresh_soon();
                return true;
            }
            if (from && now - from->start_ms < kMusicWaitMaxMs + kSinkWaitMs &&
                (device || (from->missing_ms && now - from->missing_ms < kSinkWaitMs))) { /* the end of a wait: back to waiting */
                pwait = *from;
                pwait.active = true;
                pwait.need_music = false; /* music was reached (or given up): only the node is missing */
                if (!pwait.missing_ms) pwait.missing_ms = now ? now : 1;
                pwait.rescan_ms = now ? now : 1; /* the next try: on a listing asked after this failure */
                bt_refresh_soon();
                return true;
            }
            add_notice(Notice::Info, kBtOutNotConnected);
        }
        return false;
    }

    /* The wait is over (music is back, or the time is up, and the sink is listed): open the take and do what the deck asked meanwhile. */
    void finish_wait() {
        const PlayWait w = pwait;
        pwait = PlayWait();
        if (!start_or_wait(w.after, w.cue_origin, &w)) run_deck(DeckEvent::Stop);
        else if (!pwait.active && pb_timed_out) pb_quality = "call"; /* played on whatever sink exists */
    }

    /* The wait is given up: the sink never came back. */
    void give_up_wait() {
        pwait = PlayWait();
        add_notice(Notice::Info, kBtOutNotConnected);
        run_deck(DeckEvent::Stop);
    }

    /* Once per service tick: follows the wait for the music profile and for the sink node. */
    void bt_wait_tick(uint64_t now) {
        if (!pwait.active) return;
        if (!playing_family()) { /* Stop was pressed (or the deck went idle) */
            pwait.active = false;
            return;
        }
        const BtOutput *o = bt_out_chosen();
        const bool hard = now - pwait.start_ms >= kMusicWaitMaxMs + kSinkWaitMs; /* the longest a PLAY ever waits */
        /* after an open that found no node: only a listing asked since then tells whether it is back */
        const bool listed = sink_listed(o) && (!pwait.rescan_ms || bt_scan_ms >= pwait.rescan_ms || hard);
        if (!listed) { /* a profile switch (the node is re-created), a reconnect, or the headset went away: bounded */
            if (!pwait.missing_ms) {
                pwait.missing_ms = now ? now : 1;
                bt_refresh_soon();
            }
            if (now - pwait.missing_ms >= kSinkWaitMs || hard) {
                give_up_wait();
                return;
            }
        } else {
            pwait.missing_ms = 0;
        }
        bool music = false;
        if (pwait.need_music && o) {
            if (now - pwait.last_poll_ms >= kBtProfilePollMs) {
                pwait.last_poll_ms = now;
                bt_ask_card(o->card, now);
            }
            if (bt_card.found && bt_card.name == o->card && bt_card_ms >= pwait.start_ms) {
                music = pulse_profile_is_music(bt_card.card.active_profile);
                if (pulse_profile_is_call(bt_card.card.active_profile)) pwait.saw_call = true;
            }
            if (music && !pwait.music_ms) {
                pwait.music_ms = bt_card_ms ? bt_card_ms : 1;
                bt_refresh_soon(); /* the music node is new: list it */
            }
        }
        if (!listed) return;
        if (!pwait.need_music) { /* only the node was missing: it is back */
            finish_wait();
            return;
        }
        const bool timed_out = now - pwait.start_ms >= kMusicWaitMaxMs;
        const bool min_ok = pwait.saw_call || !pwait.mic_was_open || now - pwait.start_ms >= kMusicWaitMinMs;
        bool ready = (music && min_ok) || timed_out;
        /* music: open the NEW node, not the call node on its way out (a listing asked after the music reading, and the node itself says
         * it is not the call one) */
        if (ready && music && !hard && (bt_scan_ms < pwait.music_ms || o->sink_profile == "call")) ready = false;
        if (ready) {
            pb_timed_out = timed_out && !music;
            finish_wait();
        }
    }

    /* While a take plays through a Bluetooth output: the profile now (music, or call quality), read about once a second. */
    void bt_quality_tick(uint64_t now) {
        if (!plan.chosen.bluetooth || !playing_family() || pwait.active) return;
        const BtOutput *o = bt_out_chosen();
        if (!o) return;
        if (now - pb_quality_ms >= kQualityPollMs) {
            pb_quality_ms = now;
            bt_ask_card(o->card, now);
        }
        if (bt_card.found && bt_card.name == o->card) pb_quality = profile_class(bt_card.card.active_profile);
    }

    /* PLAY / REW / FWD of a deck that is not playing: waits for the music profile when the headset needs it, or for the sink node when
     * the device is listed without one (true: waiting, or refused). */
    bool defer_for_output(PlayWait::After after, bool cue_origin) {
        if (!plan.chosen.bluetooth) return false;
        const bool music = bt_music_wait_needed();
        const BtOutput *o = bt_out_chosen();
        const bool missing = o && o->connected && !sink_listed(o);
        if (!music && !missing) return false;
        if (refuse_missing_codec(play_target()) || !player || play_target().empty()) {
            pend.push_back(DeckEvent::Stop);
            return true;
        }
        begin_wait(after, cue_origin, music);
        return true;
    }

    void command(const DeckCommand &c) override {
        switch (c.kind) {
        case DeckCommand::StartRecording:
            if (!begin_take()) pend.push_back(DeckEvent::Stop);
            break;
        case DeckCommand::PauseRecording:
            if (take_active && !paused) {
                pause_start = ring->frames_written();
                writer->pause();
                paused = true;
            }
            break;
        case DeckCommand::ResumeRecording:
            if (take_active && paused) {
                writer->resume();
                pause_total += ring->frames_written() - pause_start;
                paused = false;
            }
            break;
        case DeckCommand::StopRecording: end_take(); break;
        case DeckCommand::PlayLast:
            if (defer_for_output(PlayWait::Play, false)) break;
            if (!start_or_wait(PlayWait::Play, false, nullptr)) pend.push_back(DeckEvent::Stop);
            break;
        case DeckCommand::CueLast:
            if (defer_for_output(PlayWait::Cued, true)) break;
            if (!start_or_wait(PlayWait::Cued, true, nullptr)) pend.push_back(DeckEvent::Stop);
            break;
        case DeckCommand::PausePlayback:
            if (pwait.active) { /* still waiting for the music profile: it will open cued */
                pwait.after = PlayWait::Cued;
                break;
            }
            if (player) player->pause();
            break;
        case DeckCommand::ResumePlayback:
            if (pwait.active) {
                pwait.after = PlayWait::Play;
                break;
            }
            if (!prepare_resume()) { /* the Bluetooth output went away while paused: say so, stop */
                pend.push_back(DeckEvent::Stop);
                break;
            }
            if (player && player->resume() < 0 && plan.chosen.bluetooth) { /* the stream could not open: the output is gone */
                add_notice(Notice::Info, kBtOutNotConnected);
                pend.push_back(DeckEvent::Stop);
            }
            break;
        case DeckCommand::StopPlayback:
            pwait = PlayWait();
            if (player) player->stop();
            break;
        case DeckCommand::BeginScan:
            if (pwait.active) {
                pwait.after = c.dir == Direction::Forward ? PlayWait::ScanFwd : PlayWait::ScanRev;
                break;
            }
            if (!prepare_resume()) {
                pend.push_back(DeckEvent::Stop);
                break;
            }
            if (player && player->scan(c.dir, kScanSpeed) < 0 && plan.chosen.bluetooth) {
                add_notice(Notice::Info, kBtOutNotConnected);
                pend.push_back(DeckEvent::Stop);
            }
            break;
        case DeckCommand::EndScan:
            if (pwait.active) {
                pwait.after = pwait.cue_origin ? PlayWait::Cued : PlayWait::Play;
                break;
            }
            if (player) player->end_scan();
            break;
        case DeckCommand::Jump: {
            if (pwait.active) break;
            /* the hubs flash only when the jump moved the tape (not at the start or the end) */
            const uint64_t before = player ? player->position() : 0;
            if (player) player->jump(c.dir, kJumpSeconds);
            if (player && player->position() != before) {
                jump_ms = clk->mono_ms();
                jump_dir = c.dir == Direction::Forward ? 1 : -1;
            }
            break;
        }
        case DeckCommand::StartCountdown:
        case DeckCommand::CancelCountdown: break; /* the Deck runs the count; the snapshot shows it */
        case DeckCommand::DeviceLostNotice: add_notice(Notice::CardLost, lost_text); break;
        }
    }

    /* ================================================================================================================ */
    /* trim page */

    void join_peaks() {
        peaks_cancel.store(true);
        if (peaks_th.joinable()) peaks_th.join();
    }
    void join_save() {
        save_cancel.store(true);
        if (save_th.joinable()) save_th.join();
        merge_save();
    }

    void stop_preview() {
        if (!tr.previewing) return;
        tr.previewing = false;
        if (player) player->stop();
    }

    void close_trim_locked() {
        stop_preview();
        join_peaks();
        join_save();
        {
            std::lock_guard<std::mutex> g(pyr_mu);
            pyr.reset();
        }
        peaks_state.store(0);
        save_state.store(0);
        save_seen = 0;
        save_msg.clear();
        save_outputs.clear();
        tr = Trim();
    }

    int max_zoom() const {
        const uint64_t floor_frames = std::max<uint64_t>(1, std::min<uint64_t>(tr.frames, tr.rate / 10));
        int z = 0;
        while (z < 40 && (tr.frames >> (z + 1)) >= floor_frames) ++z;
        return z;
    }
    uint64_t visible() const {
        const uint64_t floor_frames = std::max<uint64_t>(1, std::min<uint64_t>(tr.frames, tr.rate / 10));
        return std::max<uint64_t>(floor_frames, tr.frames >> tr.zoom);
    }
    void recenter() {
        const uint64_t v = visible();
        uint64_t s = tr.pos > v / 2 ? tr.pos - v / 2 : 0;
        if (tr.frames > v && s > tr.frames - v) s = tr.frames - v;
        if (tr.frames <= v) s = 0;
        tr.wstart = s;
    }
    /* the marker moved: keep it inside the window (page-wise), else recentre */
    void fit_window() {
        const uint64_t v = visible();
        if (tr.pos < tr.wstart || tr.pos > tr.wstart + v) recenter();
    }

    void prune_splits() {
        std::vector<uint64_t> keep;
        for (uint64_t s : tr.splits)
            if (s > tr.in && s < tr.out) keep.push_back(s);
        tr.splits = keep;
        if (tr.sel >= static_cast<int>(tr.splits.size())) tr.sel = -1;
    }

    /* A finished save: join the worker, take its result, refresh the list. */
    void merge_save() {
        const int st = save_state.load();
        if (st < 2 || st == save_seen) return;
        if (save_th.joinable()) save_th.join();
        EditResult r;
        {
            std::lock_guard<std::mutex> g(wmu);
            r = save_result;
        }
        save_seen = st;
        save_msg = st == 4 ? "Cancelled" : r.message;
        save_outputs.clear();
        if (st == 2)
            for (const std::string &p : r.outputs) save_outputs.push_back(base_name(p));
        /* a loaded take replaced by the save stays loaded under its new name (the first part), without a new load */
        if (st == 2 && tr.replace_kind && target == tr.take && !save_outputs.empty()) target = save_outputs.front();
        refresh_takes_locked();
    }

    /* ================================================================================================================ */
    /* merge takes */

    void join_merge() {
        merge_cancel.store(true);
        if (merge_th.joinable()) merge_th.join();
        merge_finish();
    }

    /* A finished merge: join the worker, take its result, refresh the list. */
    void merge_finish() {
        const int st = merge_state.load();
        if (st < 2 || st == merge_seen) return;
        if (merge_th.joinable()) merge_th.join();
        MergeResult r;
        {
            std::lock_guard<std::mutex> g(wmu);
            r = merge_result;
        }
        merge_seen = st;
        merge_msg = st == 4 ? std::string("cancelled") : st == 3 ? r.message : r.warning;
        merge_deleted_all = false;
        if (st == 2) {
            merge_out = base_name(r.output);
            merge_deleted_all = r.deleted.size() == merge_names.size();
            for (const std::string &p : r.deleted)
                if (target == base_name(p)) target = merge_out; /* a loaded original went: the new take stays in the deck */
        }
        merge_names.clear();
        refresh_takes_locked();
        refresh_space();
    }

    MergeOptions merge_options() {
        MergeOptions o;
        o.ops = ops;
        o.block_bytes = cfg.save_block_bytes;
        o.decoders = &decoders();
        if (cfg.make_encoder) { /* the tests' fixture encoder for a FLAC result, else FlacEncoder on libFLAC */
            auto make = cfg.make_encoder;
            o.make_flac_encoder = [make](const AudioFormat &f) {
                FormatChoice c;
                c.id = "flac";
                c.key.format = RecFormat::Flac;
                c.flac_level = 5;
                return make(c, f);
            };
        }
        return o;
    }

    /* Why `name` cannot be merged now (the take list's facts and what is in use), "" when it can. */
    std::string merge_take_why(const std::string &name) const {
        const TakeRow *row = nullptr;
        for (const TakeRow &r : rows)
            if (r.name == name) row = &r;
        if (!row) return "A take to merge is no longer in the list";
        if (!row->named) return "Not available: Not a Cassette take";
        if (row->damaged) return "Not available: Damaged take";
        if (take_active && take_name == name) return "Not available: Being recorded";
        if (tr.open && tr.take == name) return "Not available: Open on the Edit page";
        if (in_use(name)) return "Not available: Playing: stop it first";
        return "";
    }

    /* The request of a merge of `names`: the checks the controller owns (merge.hpp's plan does the rest), the new take's name,
     * comments and bext as a new recording gets them. "" or the notice. */
    std::string merge_request(const std::vector<std::string> &names, bool del, MergeRequest &req, uint64_t &number) {
        if (merge_state.load() == 1) return "A merge is running";
        if (names.size() < 2) return "Merging needs two takes";
        if (take_active || recording_family()) return "Stop the recording to merge takes";
        for (const std::string &n : names) {
            const std::string why = merge_take_why(n);
            if (!why.empty()) return why;
        }
        TakeName t0;
        if (!parse_take_name(names.front(), t0)) return "Not available: Not a Cassette take";
        if (t0.format == RecFormat::Flac) { /* decoded and encoded again: libFLAC both ways (Opus needs no library) */
            const bool dec = decoders().flac != nullptr;
            const bool enc = cfg.make_encoder || real_flac() != nullptr;
            if (!dec || !enc) return "This take cannot be merged: " + decode_error_text(kCodecMissing, names.front());
        }
        number = peek_take_number(*ops, folder, t0.prefix);
        TakeName nt = new_take_name(t0.prefix, number, *cstate, *clk);
        nt.format = t0.format;
        req = MergeRequest();
        req.output_name = format_take_name(nt);
        for (const std::string &n : names) req.sources.push_back(folder + "/" + n);
        req.delete_originals = del;
        const std::string stem = file_stem(req.output_name);
        uint32_t rate = 48000;
        for (const TakeRow &r : rows)
            if (r.name == names.front() && r.rate) rate = r.rate;
        req.bwf.description = "Cassette " + stem;
        if (cstate->synchronized()) {
            const int64_t now = clk->wall_s();
            DateTime dt;
            if (datetime_from_wall(now, dt)) {
                char b[32];
                const int sec = static_cast<int>(((now % 60) + 60) % 60);
                std::snprintf(b, sizeof b, "%04d-%02d-%02d", dt.year, dt.month, dt.day);
                req.bwf.origination_date = b;
                std::snprintf(b, sizeof b, "%02d:%02d:%02d", dt.hour, dt.minute, sec);
                req.bwf.origination_time = b;
                req.bwf.time_reference = static_cast<uint64_t>(dt.hour * 3600 + dt.minute * 60 + sec) * rate;
            }
        }
        req.tags = {{"TITLE", stem}};
        if (!req.bwf.origination_date.empty()) req.tags.push_back({"DATE", req.bwf.origination_date});
        req.tags.push_back({"ENCODER", "Cassette"});
        return "";
    }

    void start_peaks(const std::string &path, uint64_t frames) {
        peaks_cancel.store(false);
        peaks_state.store(1);
        peaks_done.store(0);
        peaks_total.store(frames);
        peaks_mem.store(false);
        decoders(); /* resolved here, before the worker reads it */
        peaks_th = std::thread([this, path] {
            PeakBuildOptions o;
            o.progress = [this](uint64_t done, uint64_t) { peaks_done.store(done); };
            o.cancel = &peaks_cancel;
            o.decoders = &dlibs;
            std::string err;
            std::unique_ptr<PeakPyramid> p = PeakPyramid::open_or_build(*ops, path, &err, o);
            if (p) {
                std::shared_ptr<PeakPyramid> sp(std::move(p));
                peaks_mem.store(sp->in_memory());
                {
                    std::lock_guard<std::mutex> g(pyr_mu);
                    pyr = sp;
                }
                peaks_state.store(2);
            } else if (err == "cancelled") {
                peaks_state.store(0);
            } else {
                {
                    std::lock_guard<std::mutex> g(wmu);
                    peaks_error = err;
                }
                peaks_state.store(3);
            }
        });
    }

    /* ================================================================================================================ */
    /* the service tick and the snapshot */

    /* The playback meter, once per service tick: a new played period feeds its peak; paused (or no period for
     * kPlayLevelStaleMs) the level falls at the meter's 20 dB/s; out of the playing states the meter is cleared. */
    void update_play_meter(uint64_t now) {
        if (!playing_family() || !player) {
            if (play_meter_on) {
                play_meter.reset();
                play_meter_on = false;
            }
            return;
        }
        float pk[2] = {0.0f, 0.0f};
        uint16_t ch = 0;
        const uint32_t q = player->level(pk, &ch);
        if (!play_meter_on) { /* a take started: what the tap holds is from before */
            play_meter.reset();
            play_meter_on = true;
            play_seq = q;
            play_db[0] = play_db[1] = Meter::kFloorDb;
            play_ch = player->format().channels >= 2 ? 2 : 1;
            play_new_ms = play_tick_ms = now;
            play_meter.feed_peaks(play_db, play_ch, now * 1000);
            return;
        }
        const double dt = static_cast<double>(now - play_tick_ms) / 1000.0;
        play_tick_ms = now;
        if (q != play_seq) {
            play_seq = q;
            play_new_ms = now;
            if (ch) play_ch = ch;
            for (int c = 0; c < 2; ++c) play_db[c] = Meter::to_db(pk[c]);
            play_meter.feed_peaks(play_db, play_ch, now * 1000);
        } else if (deck.state() == DeckState::PlayingPaused || now - play_new_ms > kPlayLevelStaleMs) {
            for (float &d : play_db) d = std::max(Meter::kFloorDb, static_cast<float>(d - Meter::kHoldFallDbPerSec * dt));
            play_meter.feed_peaks(play_db, play_ch, now * 1000);
        }
    }

    /* The Bluetooth input's upkeep, once per service tick: re-lists after a server change, follows the open stream (the
     * connecting gate, the no-sound state, the identity during a take) and closes the microphone when it is not wanted. */
    void bt_tick(uint64_t now) {
        bt_changed = false;
        PulseApi *pa = cfg.pulse;
        if (!pa) return;
        if (bt_apply_scan()) {
            bt_rebuild();
            fix_sel();
        }
        const bool dirty = bt_dirty.load() || bt_gone.load() || bt_rescan;
        /* T9: the lists follow the server even when an event is missed or a listing failed: read again while the chosen output has no
         * node (often while PLAY waits for it) or the last listing failed */
        const bool unresolved = bt_is_connected && (bt_status == kBtNotResponding || (plan.chosen.bluetooth && !sink_listed(bt_out_chosen())));
        const bool relist = pwait.active && pwait.missing_ms ? now - bt_last_refresh_ms >= kBtRelistWaitMs
                                                             : unresolved && now - bt_last_refresh_ms >= kBtRelistMs;
        if (dirty ? now - bt_last_refresh_ms >= cfg.pulse_refresh_ms
                  : ((!bt_is_connected && now - bt_last_refresh_ms >= cfg.pulse_retry_ms) || relist)) {
            bt_dirty.store(false);
            bt_rescan = false;
            bt_request_scan();
        }
        PulseWorker *w = worker();
        CardResult cr;
        if (w && w->take_card(cr)) {
            bt_card = cr;
            bt_card_ms = bt_card_ask_ms;
        }
        IdentityResult ir;
        if (w && w->take_identity(ir) && bt_ident_for && bt_stream.get() == bt_ident_for) {
            bt_ident_for = nullptr;
            /* a silent relink without a move notice (C16): another source ends the take at once; NO source is asked again soon and
             * ends it only when it lasts kBtIdentityRetryMs (T9: a moment of it at a profile switch is the server's, not a loss) */
            if (ir.ok || !take_active) {
                bt_ident_unknown_ms = 0;
            } else if (bt_stream && !bt_stream->ended()) {
                if (!ir.actual.empty()) {
                    bt_ident_unknown_ms = 0;
                    on_capture_lost(CaptureStats());
                } else {
                    if (!bt_ident_unknown_ms) bt_ident_unknown_ms = now ? now : 1;
                    if (now - bt_ident_unknown_ms >= kBtIdentityRetryMs) {
                        bt_ident_unknown_ms = 0;
                        on_capture_lost(CaptureStats());
                    } else {
                        /* ask again in 0.2 s (the regular check comes kBtIdentityMs after bt_ident_ms) */
                        if (now >= kBtIdentityMs) bt_ident_ms = now - kBtIdentityMs + 2 * kBtIdentityRetryStepMs;
                    }
                }
            }
        }
        if (bt_stream && bt_gate && cap && cap->running()) {
            PulseStreamEvent ev;
            while (bt_stream->poll_event(ev))
                if (ev.kind == PulseEvent::Running && !bt_running_ms) bt_running_ms = now ? now : 1;
            /* connecting ends at the first non-zero sample (the PCM opens the gate), or when the card is on a call profile
             * and the stream has run a while: the zeros are then the headset's own (study C18). The profile is read on the
             * worker; an answer counts when it was asked after the link had time to come up. */
            if (!bt_gate->open.load() && bt_running_ms && now - bt_running_ms >= kBtCallLinkMs) {
                if (now - bt_profile_poll_ms >= kBtProfilePollMs) {
                    bt_profile_poll_ms = now;
                    bt_ask_card(bt_card_name, now);
                }
                if (bt_card.found && bt_card.name == bt_card_name && bt_card_ms >= bt_running_ms + kBtCallLinkMs &&
                    pulse_profile_is_call(bt_card.card.active_profile))
                    bt_gate->open.store(true);
            }
            /* no sound: the silence detector is armed only once connecting has ended (its 3 s of zeros then say the
             * headset sends nothing); it is a state, so a first sound clears it */
            const bool silent = bt_gate->open.load() && bt_stream->stats().silent;
            if (silent && !bt_silent) {
                add_notice(Notice::Info, kBtNoSound);
            } else if (!silent && bt_silent) {
                notices.erase(std::remove_if(notices.begin(), notices.end(), [](const Notice &n) { return n.text == kBtNoSound; }),
                              notices.end());
            }
            bt_silent = silent;
            if (take_active && now - bt_ident_ms >= kBtIdentityMs) {
                bt_ident_ms = now;
                if (!bt_stream->ended() && w && w->request_identity(bt_stream)) bt_ident_for = bt_stream.get();
            }
        }
        if (cap && active_is_bt && !bt_mic_wanted()) { /* the list, the Edit page, playback: let the headset back to music mode */
            close_capture();
            bt_policy_closed = true;
        }
    }

    void tick() {
        std::lock_guard<std::mutex> g(mu);
        if (!started || down) return;
        const uint64_t now = clk->mono_ms();
        run_deck(DeckEvent::TimerTick);
        PlayerEvent ev;
        while (player && player->poll_event(ev)) {
            if (ev == PlayerEvent::TakeEnded || ev == PlayerEvent::ReachedEnd || ev == PlayerEvent::ReachedStart)
                run_deck(DeckEvent::TakeEnded);
            else if (ev == PlayerEvent::DeviceLost && playing_family()) {
                lost_text = plan.chosen.bluetooth ? kBtOutLost : "The sound output was lost";
                run_deck(DeckEvent::DeviceLost);
            }
        }
        if (playing_family() && bt_sink && bt_sink->ended() && player) { /* the headset went away (paused, or between writes) */
            player->release_sink(); /* closed, the position kept: the deck then pauses there, never another sink */
            lost_text = kBtOutLost;
            if (deck.state() == DeckState::PlayingPaused) add_notice(Notice::CardLost, kBtOutLost); /* the deck ignores it paused */
            else run_deck(DeckEvent::DeviceLost);
        }
        if (cap && !cap->running()) {
            const CaptureStats st = cap->stats();
            if (st.stop_reason != CaptureError::None) on_capture_lost(st);
        }
        if (take_active && writer) {
            const WriterState ws = writer->status().state;
            if (ws == WriterState::Failed || ws == WriterState::Stopped) run_deck(DeckEvent::Stop);
            if (take_active && ring && cap) {
                const CaptureStats cs = cap->stats();
                prob.frames_dropped = ring->frames_dropped();
                prob.overruns = cs.overruns - base_overruns;
                prob.frames_lost = cs.frames_lost_xrun - base_lost;
                prob.lost_seconds = fmt.rate ? static_cast<double>(prob.frames_lost) / fmt.rate : 0.0;
                take_lost.store(prob.frames_lost, std::memory_order_relaxed);
                if (prob.frames_lost > 0 && !lost_noticed) {
                    lost_noticed = true;
                    add_notice(Notice::Info, "Some audio was lost: the card or the disk could not keep up");
                }
                if (!cs.message.empty() && cs.last_error != CaptureError::None) prob.message = cs.message;
            }
        }
        bt_tick(now);
        bt_wait_tick(now);
        bt_quality_tick(now);
        const bool bt_hold = active_is_bt && bt_policy_closed && !bt_mic_wanted();     /* closed on purpose, still not wanted */
        const bool bt_reopen = active_is_bt && bt_policy_closed && bt_mic_wanted();    /* back on the recorder: open at once */
        if (!cap && !bt_hold && (bt_reopen || bt_changed || now - last_retry_ms >= cfg.replug_retry_ms)) {
            last_retry_ms = now;
            if (try_open_capture() && !bt_reopen && !take_active && !playing_family() && deck.state() == DeckState::Idle &&
                !tr.previewing)
                rebuild_output();
        }
        if (tr.previewing && player) {
            const uint64_t p = player->position();
            ++tr.grace;
            if (p >= tr.out) {
                tr.pos = tr.out;
                stop_preview();
            } else if (tr.grace > 2 && player->state() == PlayerState::Stopped) {
                tr.pos = tr.out;
                tr.previewing = false;
            } else {
                tr.pos = p;
            }
            fit_window();
        }
        merge_save();
        merge_finish();
        if (now - last_space_ms >= kSpaceRefreshMs) refresh_space();
        update_play_meter(now);
        monitor_delay_tick(now);
        publish();
    }

    static OutputSnap output_snap(const OutputOption &o) {
        OutputSnap x;
        x.id = o.device;
        x.label = o.label;
        x.bluetooth = o.bluetooth;
        x.kind = o.kind;
        x.rate = o.rate;
        x.connected = o.connected;
        x.usable = o.usable;
        x.hint_profile = o.hint_profile;
        x.reason = o.reason;
        return x;
    }

    void publish() {
        if (deck.state() == DeckState::Idle) preview_name.clear(); /* the preview is over */
        monitor_sync();
        Snapshot s;
        s.started = started;
        s.folder = folder;
        s.settings = store.values();
        s.takes_revision = takes_rev;
        /* deck */
        s.deck.state = static_cast<int>(deck.state());
        s.deck.state_name = deck_state_name(deck.state());
        auto key = [&](DeckKey k, KeySnap &o) {
            o.enabled = deck.key_enabled(k);
            o.active = deck.key_active(k);
        };
        key(DeckKey::Rew, s.deck.rew);
        key(DeckKey::PlayPause, s.deck.play_pause);
        key(DeckKey::Fwd, s.deck.fwd);
        key(DeckKey::Stop, s.deck.stop);
        key(DeckKey::Rec, s.deck.rec);
        s.deck.rec_latched = deck.rec_latched();
        s.deck.pause_icon = deck.shows_pause_icon();
        /* time */
        const AudioFormat f = (fmt.rate && fmt.channels) ? fmt : AudioFormat();
        if (take_active) s.time.elapsed_s = static_cast<double>(elapsed_frames()) / f.rate;
        if (player) {
            s.time.position_s = player->position_s();
            s.time.length_s = player->length_s();
        }
        if (deck.countdown().active()) {
            const Countdown::Remaining r = deck.countdown().remaining();
            s.time.countdown_seconds = static_cast<int>(r.seconds);
            s.time.countdown_fraction = r.fraction;
        }
        /* meter */
        if (proc) {
            copy_meter(proc->levels(), s.meter);
            s.meter.clips = static_cast<unsigned>(proc->clip_count());
        }
        if (play_meter_on && playing_family()) copy_meter(play_meter.snapshot(), s.play_meter);
        /* card */
        s.card.present = cap && cap->running();
        s.card.via_server = cap && via_server && !active_is_bt;
        for (const CaptureCard &c : cards) {
            s.card.labels.push_back(c.label);
            InputSnap in;
            in.id = key_of(c.info);
            in.label = c.label;
            s.card.inputs.push_back(in);
        }
        for (const BtInput &b : bt_list) {
            s.card.labels.push_back(b.label);
            InputSnap in;
            in.id = b.id;
            in.label = b.label;
            in.bluetooth = true;
            in.kind = b.kind;
            in.rate = b.rate;
            in.connected = b.connected;
            in.usable = b.usable;
            in.hint_profile = b.hint_profile;
            in.reason = b.reason;
            in.hw_level = false;
            s.card.inputs.push_back(in);
        }
        s.card.bt_status = bt_status;
        s.card.selected = sel;
        if (sel >= 0 && sel < static_cast<int>(s.card.labels.size())) s.card.label = s.card.labels[static_cast<size_t>(sel)];
        else if (have_card) s.card.label = active.name;
        if (have_card && active_is_bt) {
            s.card.bluetooth = true;
            const BtInput *b = bt_find(bt_mac_of_id(active_key));
            s.card.bt_rate = b ? b->rate : 0;
            s.capture.connecting = cap && cap->running() && bt_gate && !bt_gate->open.load();
            s.capture.no_sound = bt_silent;
            s.capture.mic_closed = bt_policy_closed && !cap;
            s.capture.bt_gone = lost || !b || !b->connected;
        }
        s.card.message = s.card.present ? std::string() : cap_message;
        if (mixer) {
            s.card.has_level = mixer->caps().has_level;
            s.card.has_agc = mixer->caps().has_agc;
            s.card.has_mute = mixer->caps().has_mute;
            if (s.card.has_level) {
                s.card.level_db = mstate.level_db;
                s.card.level_min_db = mixer->level_range().db_min;
                s.card.level_max_db = mixer->level_range().db_max;
                s.card.level_inferred = mixer->level_range().db_inferred;
            }
            s.card.auto_gain = mstate.agc_valid && mstate.agc;
            s.card.mute = mstate.mute_valid && mstate.mute;
        }
        s.card.soft_gain_db = store.values().soft_gain_db;
        /* T9: the Bluetooth microphone closed on purpose, the headset there: what depends on the capture stays as it was open (the
         * format it records, the Monitor rows), so nothing looks like a missing input */
        const bool mic_held = s.capture.mic_closed && !s.capture.bt_gone;
        const MonTarget mt = monitor_target();
        s.card.has_playback = (s.card.present || mic_held) && have_card && mt.ok;
        s.card.monitor_active = mon && mon->running();
        s.card.output_label = plan.chosen.label;
        for (const OutputOption &o : plan.choices) {
            if (o.device == plan.chosen.device) s.card.output_selected = static_cast<int>(s.card.output_labels.size());
            s.card.output_labels.push_back(o.label);
            s.card.output_devices.push_back(o.device);
            s.card.outputs.push_back(output_snap(o));
        }
        /* playback */
        s.playback.bluetooth = plan.chosen.bluetooth;
        s.playback.switch_timeout_s = static_cast<double>(kMusicWaitMaxMs) / 1000.0;
        s.playback.switch_timed_out = pb_timed_out;
        if (pwait.active) {
            s.playback.state = "switching_to_music";
            s.playback.switch_elapsed_s = static_cast<double>(clk->mono_ms() - pwait.start_ms) / 1000.0;
        } else if (playing_family()) {
            s.playback.state = "playing";
        }
        if (plan.chosen.bluetooth)
            s.playback.quality = (pwait.active || playing_family()) && !pb_quality.empty()
                                     ? pb_quality
                                     : (plan.chosen.hint_profile.empty() ? std::string("unknown") : plan.chosen.hint_profile);
        if (player && playing_family()) s.playback.underruns = player->underruns();
        /* monitor */
        MonitorSnap &m = s.monitor;
        m.active = mon && mon->running();
        m.route = mt.route;
        m.available = mt.ok;
        m.target_label = mt.ok ? mt.label : std::string();
        m.reason = mt.ok ? std::string() : mt.why;
        m.output = store.values().monitor_output;
        m.delay_ms = m.active ? mon_delay_ms : 0.0;
        if (mt.ok && mt.route == "other_output" && mt.a2dp) m.notice = "delay";
        if (m.active && m.delay_ms > kMonitorNoticeDelayMs) m.notice = "delay";
        {
            OutputSnap autosnap;
            autosnap.label = "Automatic";
            m.outputs.push_back(autosnap);
            const std::vector<OutputOption> opts = plan.choices.empty() ? std::vector<OutputOption>{plan.chosen} : plan.choices;
            for (const OutputOption &o : opts) m.outputs.push_back(output_snap(o));
            m.output_selected = 0;
            if (!m.output.empty()) {
                m.output_selected = -1;
                const std::string want_mac = bt_mac_of_id(m.output);
                const std::string want = want_mac.empty() ? m.output : bt_id_of(want_mac);
                for (size_t i = 1; i < m.outputs.size(); ++i)
                    if (m.outputs[i].id == want) m.output_selected = static_cast<int>(i);
                if (m.output_selected < 0) { /* chosen earlier, not there now: a placeholder entry */
                    OutputSnap ph;
                    ph.id = want;
                    auto l = bt_labels.find(want);
                    ph.label = l != bt_labels.end() ? l->second : (want_mac.empty() ? want : std::string("Bluetooth device"));
                    ph.bluetooth = !want_mac.empty();
                    ph.kind = ph.bluetooth ? "other" : "";
                    ph.connected = false;
                    ph.usable = false;
                    ph.reason = "Not connected";
                    m.outputs.push_back(ph);
                    m.output_selected = static_cast<int>(m.outputs.size() - 1);
                }
            }
        }
        /* take */
        s.take.recording = take_active;
        s.take.name = take_active ? take_name : target;
        s.take.playing_name = playing_family() ? play_target() : std::string();
        s.take.load_seq = load_seq;
        s.take.format_text = format_text(f, rec_compressed ? format_tag(rec_choice.key.format) : "WAV");
        s.take.free_bytes = space.free_bytes;
        s.take.hours_left = space.hours_left;
        s.take.opus_kbps = rec_compressed && rec_choice.key.format == RecFormat::Opus ? rec_choice.opus_bitrate_bps / 1000 : 0;
        /* the Format picker's inputs */
        s.format_inputs.codecs = codecs_now();
        if ((s.card.present || mic_held) && fmt.rate && fmt.channels && fmt.bits) fill_inputs(s.format_inputs);
        /* problems */
        s.problems = prob;
        s.notices = notices;
        /* scan */
        const DeckState ds = deck.state();
        if (ds == DeckState::ScanningForward || ds == DeckState::ScanningReverse) {
            s.scan.active = true;
            s.scan.direction = ds == DeckState::ScanningForward ? 1 : -1;
            s.scan.speed_text = std::to_string(kScanSpeed) + "x";
        }
        if (jump_dir != 0 && playing_family() && clk->mono_ms() - jump_ms < kJumpFlashMs) s.scan.jump_dir = jump_dir;
        /* trim */
        if (tr.open) {
            TrimSnap &t = s.trim;
            t.open = true;
            t.take = tr.take;
            t.frames = tr.frames;
            t.rate = tr.rate;
            t.in_frame = tr.in;
            t.out_frame = tr.out;
            t.splits = tr.splits;
            t.selected_split = tr.sel;
            t.position = tr.pos;
            t.zoom = tr.zoom;
            t.max_zoom = max_zoom();
            t.window_start = tr.wstart;
            t.visible_frames = visible();
            t.peaks_state = peaks_state.load();
            const uint64_t pt = peaks_total.load();
            t.peaks_progress = pt ? static_cast<float>(static_cast<double>(peaks_done.load()) / static_cast<double>(pt)) : 0.f;
            if (t.peaks_state == 2) t.peaks_progress = 1.f;
            t.peaks_in_memory = peaks_mem.load();
            if (t.peaks_state == 3) {
                std::lock_guard<std::mutex> g(wmu);
                t.peaks_error = peaks_error;
            }
            t.previewing = tr.previewing;
            t.kept_seconds = tr.rate ? static_cast<double>(tr.out - tr.in) / tr.rate : 0.0;
            t.save_state = save_state.load();
            const uint64_t st = save_total.load();
            t.save_progress = st ? static_cast<float>(static_cast<double>(save_done.load()) / static_cast<double>(st)) : 0.f;
            if (t.save_state == 2) t.save_progress = 1.f;
            t.save_message = save_msg;
            t.save_outputs = save_outputs;
            t.save_replaced = tr.replace_kind && t.save_state == 2;
        }
        /* merge: a result the service tick has not taken yet still shows as running */
        {
            const int st = merge_state.load();
            MergeSnap &m = s.merge;
            m.state = (st >= 2 && st != merge_seen) ? 1 : st;
            m.seq = merge_seq;
            const uint64_t mt = merge_total.load();
            m.progress = mt ? static_cast<float>(static_cast<double>(merge_done.load()) / static_cast<double>(mt)) : 0.f;
            if (m.state == 2) m.progress = 1.f;
            m.sources = merge_count;
            m.output = merge_out;
            m.message = merge_msg;
            m.originals_deleted = merge_deleted_all;
        }
        std::lock_guard<std::mutex> g(pub_mu);
        pub = s;
        pub_rows = rows;
    }

    /* ================================================================================================================ */
    /* startup and shutdown */

    void do_start() {
        std::lock_guard<std::mutex> g(mu);
        if (started) return;
        store.load();
        Settings &s = store.values();
        folder = expand_home(s.folder, cfg.home);
        make_dirs(*ops, folder);
        deck.set_rew_fwd_mode(s.rew_fwd == ScanMode::Jump ? RewFwdMode::Jump5s : RewFwdMode::FastScan);
        deck.set_record_timer(static_cast<RecordTimer>(s.record_timer_s));
        cleanup_temporaries(*ops, folder);
        const RepairReport rep = repair_all(*ops, folder);
        for (const RepairedTake &t : rep.repaired) {
            std::string text = "Recovered " + t.take + " after an interrupted recording (" + secs_text(t.seconds) + " kept";
            if (t.missing_seconds >= 1) text += ", about " + secs_text(t.missing_seconds) + " at the end lost";
            text += ")";
            add_notice(Notice::Repaired, text);
        }
        for (const std::string &e : rep.errors) add_notice(Notice::Info, e);
        refresh_takes_locked();
        load_newest_if_empty(); /* the last take is loaded at launch, without a load animation */
        want_text = s.card;
        if (cfg.pulse) bt_wait_scan(300); /* the first scan: a chosen headset opens at once (startup only) */
        try_open_capture();
        rebuild_output();
        refresh_space();
        last_retry_ms = clk->mono_ms();
        started = true;
        publish();
        svc = std::thread([this] {
            while (!quit.load()) {
                tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg.service_period_ms));
            }
        });
    }

    void shutdown() {
        if (!started && !svc.joinable()) return;
        quit.store(true);
        if (svc.joinable()) svc.join();
        std::lock_guard<std::mutex> g(mu);
        if (down) return;
        down = true;
        if (pw) pw->stop(); /* no more questions to the server (a call in progress is bounded by the server's own timeout) */
        if (cfg.pulse && bt_cb_set) { /* no change callback after this: it points into this object */
            cfg.pulse->set_change_callback(nullptr);
            bt_cb_set = false;
        }
        if (take_active || deck.state() != DeckState::Idle) run_deck(DeckEvent::AppLeave);
        end_take();
        close_trim_locked();
        join_merge(); /* a merge in progress is cancelled: the partial new take goes, the originals stay */
        player.reset();
        sink.reset();
        close_capture();
        persist();
    }
};

/* ======================================================================================================================== */
/* the public face: each command takes the lock, acts and republishes */

namespace {
struct Locked {
    explicit Locked(std::mutex &m) : g(m) {}
    std::lock_guard<std::mutex> g;
};
}  // namespace

Controller::Controller(ControllerConfig cfg) : p_(new Impl(std::move(cfg))) {}
Controller::~Controller() = default;

void Controller::start() { p_->do_start(); }
void Controller::shutdown() { p_->shutdown(); }

Snapshot Controller::snapshot() const {
    std::lock_guard<std::mutex> g(p_->pub_mu);
    return p_->pub;
}
std::vector<TakeRow> Controller::takes() const {
    std::lock_guard<std::mutex> g(p_->pub_mu);
    return p_->pub_rows;
}
void Controller::ack_notices() {
    Locked l(p_->mu);
    p_->notices.clear();
    p_->publish();
}

std::vector<PeakBar> Controller::peak_window(int bars) const {
    std::vector<PeakBar> out;
    if (bars <= 0) return out;
    uint64_t start = 0, vis = 0;
    {
        std::lock_guard<std::mutex> g(p_->pub_mu);
        if (!p_->pub.trim.open) return out;
        start = p_->pub.trim.window_start;
        vis = p_->pub.trim.visible_frames;
    }
    std::shared_ptr<PeakPyramid> py;
    {
        std::lock_guard<std::mutex> g(p_->pyr_mu);
        py = p_->pyr;
    }
    if (!py || vis == 0) return out;
    const uint64_t fpb = std::max<uint64_t>(1, (vis + static_cast<uint64_t>(bars) - 1) / static_cast<uint64_t>(bars));
    const std::vector<PeakPair> w = py->window(start, fpb, static_cast<size_t>(bars));
    out.reserve(w.size());
    for (const PeakPair &x : w) {
        PeakBar b;
        b.min = x.min;
        b.max = x.max;
        out.push_back(b);
    }
    return out;
}

std::vector<PeakBar> Controller::peak_range(int64_t start_frame, uint64_t frames, int bars) const {
    std::vector<PeakBar> out;
    if (bars <= 0 || frames == 0) return out;
    {
        std::lock_guard<std::mutex> g(p_->pub_mu);
        if (!p_->pub.trim.open) return out;
    }
    std::shared_ptr<PeakPyramid> py;
    {
        std::lock_guard<std::mutex> g(p_->pyr_mu);
        py = p_->pyr;
    }
    if (!py) return out;
    const uint64_t n = static_cast<uint64_t>(bars);
    const uint64_t fpb = std::max<uint64_t>(1, (frames + n - 1) / n);
    out.assign(static_cast<size_t>(bars), PeakBar());
    /* the bars that start before frame 0 stay (0,0): the first bar inside the take is lead */
    uint64_t lead = 0;
    if (start_frame < 0) {
        const uint64_t before = static_cast<uint64_t>(-start_frame);
        lead = (before + fpb - 1) / fpb;
        if (lead >= n) return out;
    }
    const uint64_t first = static_cast<uint64_t>(start_frame + static_cast<int64_t>(lead * fpb));
    const std::vector<PeakPair> w = py->window(first, fpb, static_cast<size_t>(n - lead));
    for (size_t i = 0; i < w.size() && lead + i < n; ++i) {
        out[static_cast<size_t>(lead + i)].min = w[i].min;
        out[static_cast<size_t>(lead + i)].max = w[i].max;
    }
    return out;
}

bool Controller::key_event(KeyEvent e) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.started) return false;
    DeckEvent de = DeckEvent::Esc;
    switch (e) {
    case KeyEvent::Rec: de = DeckEvent::Rec; break;
    case KeyEvent::Stop: de = DeckEvent::Stop; break;
    case KeyEvent::PlayPause: de = DeckEvent::PlayPause; break;
    case KeyEvent::RewDown: de = DeckEvent::RewDown; break;
    case KeyEvent::RewUp: de = DeckEvent::RewUp; break;
    case KeyEvent::FwdDown: de = DeckEvent::FwdDown; break;
    case KeyEvent::FwdUp: de = DeckEvent::FwdUp; break;
    case KeyEvent::Esc: de = DeckEvent::Esc; break;
    case KeyEvent::TapOverlay: de = DeckEvent::TapOverlay; break;
    case KeyEvent::AppLeave: de = DeckEvent::AppLeave; break;
    }
    if (e == KeyEvent::AppLeave) {
        d.app_left = true;
        d.monitor_stop();
        d.stop_preview();
        d.run_deck(de);
        d.join_save(); /* a save in progress is cancelled: the temporaries go, the original stays */
        d.join_merge(); /* the same for a merge: the partial new take goes, the originals stay */
        d.join_peaks();
        d.persist();
        d.publish();
        return true;
    }
    d.app_left = false;
    if (de == DeckEvent::Rec || de == DeckEvent::PlayPause || de == DeckEvent::RewDown || de == DeckEvent::FwdDown)
        d.stop_preview();
    const bool r = d.run_deck(de);
    d.publish();
    return r;
}

/* ---- settings ---- */

bool Controller::set_level_db(double db) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.mixer || !d.mixer->caps().has_level) return false;
    double applied = db;
    const bool ok = d.mixer->set_level_db(db, &applied);
    d.mstate = d.mixer->read();
    d.save_card_state();
    d.publish();
    return ok;
}
bool Controller::set_auto_gain(bool on) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.mixer || !d.mixer->caps().has_agc) return false;
    const bool ok = d.mixer->set_agc(on);
    d.mstate = d.mixer->read();
    d.save_card_state();
    d.publish();
    return ok;
}
bool Controller::set_mute(bool on) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.mixer || !d.mixer->caps().has_mute) return false;
    const bool ok = d.mixer->set_mute(on);
    d.mstate = d.mixer->read();
    d.save_card_state();
    d.publish();
    return ok;
}
bool Controller::set_soft_gain_db(double db) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!std::isfinite(db)) return false;
    d.store.values().soft_gain_db = std::min(kSoftGainMax, std::max(kSoftGainMin, db));
    d.apply_dsp_settings();
    d.persist();
    d.publish();
    return true;
}
bool Controller::set_format(RecFormat f) {
    Locked l(p_->mu);
    if (f != RecFormat::Wav) return false;
    p_->store.values().format = f;
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_format_choice(RecFormat f, const std::string &preset) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() == DeckState::Countdown) return false;
    FormatInputs in; /* the model's own entries: only the key matters here */
    const std::vector<FormatChoice> list = list_formats(in);
    bool known = false;
    for (const FormatChoice &c : list) known = known || (c.key.format == f && c.key.preset == preset);
    if (!known) return false;
    d.store.values().format = f;
    d.store.values().format_preset = preset;
    d.store.values().rate = 0; /* a preset: its codec and bit rate, rate and quality at their defaults */
    d.store.values().quality = 0;
    d.ignore_prefs = false;
    d.persist();
    d.apply_capture_choice();
    d.refresh_space(); /* the format text and the hours left follow the choice */
    d.publish();
    return true;
}
bool Controller::set_rec_params(RecFormat f, const std::string &preset, int rate, int quality) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() == DeckState::Countdown) return false;
    if (!preset.empty() && !(f == RecFormat::Opus && preset_kbps(preset) > 0)) return false;
    if (rate != 0 && (rate < 8000 || rate > 768000)) return false;
    if (quality != 0 && !(f == RecFormat::Opus ? opus_kbps_ok(quality) : (quality == 16 || quality == 24))) return false;
    Settings &s = d.store.values();
    s.format = f;
    s.format_preset = preset;
    s.rate = rate;
    s.quality = quality;
    d.ignore_prefs = false;
    d.persist();
    d.apply_capture_choice();
    d.refresh_space();
    d.publish();
    return true;
}
bool Controller::set_folder(const std::string &folder) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() == DeckState::Countdown || d.save_state.load() == 1 || d.merge_state.load() == 1)
        return false;
    if (folder.empty() || folder.size() > 1024) return false;
    const std::string expanded = expand_home(folder, d.cfg.home);
    if (make_dirs(*d.ops, expanded) < 0) return false;
    if (d.tr.open) d.close_trim_locked();
    if (d.deck.state() != DeckState::Idle) d.run_deck(DeckEvent::Stop);
    d.store.values().folder = folder;
    d.folder = expanded;
    d.target.clear();
    d.preview_name.clear();
    d.refresh_takes_locked();
    d.load_newest_if_empty();
    d.refresh_space();
    d.persist();
    d.publish();
    return true;
}
bool Controller::set_prefix(const std::string &prefix) {
    Locked l(p_->mu);
    if (!valid_prefix(prefix)) return false;
    p_->store.values().prefix = prefix;
    p_->persist();
    p_->publish();
    return true;
}
int Controller::set_next_take_number(uint64_t n, uint64_t *landed) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() == DeckState::Countdown || d.save_state.load() == 1 || d.merge_state.load() == 1)
        return -EBUSY;
    uint64_t got = 0;
    const int r = cassette::set_next_take_number(*d.ops, d.folder, d.store.values().prefix, n, got);
    if (r < 0) return r;
    if (landed) *landed = got;
    return 0;
}
bool Controller::set_sync_interval(int seconds) {
    Locked l(p_->mu);
    if (seconds < kSyncMin || seconds > kSyncMax) return false;
    p_->store.values().sync_interval_s = seconds;
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_auto_split(bool on) {
    Locked l(p_->mu);
    p_->store.values().auto_split = on;
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_record_timer(int seconds) {
    Locked l(p_->mu);
    bool ok = false;
    for (int c : kRecordTimerChoices) ok = ok || c == seconds;
    if (!ok) return false;
    p_->store.values().record_timer_s = seconds;
    p_->deck.set_record_timer(static_cast<RecordTimer>(seconds));
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_scan_mode(ScanMode m) {
    Locked l(p_->mu);
    p_->store.values().rew_fwd = m;
    p_->deck.set_rew_fwd_mode(m == ScanMode::Jump ? RewFwdMode::Jump5s : RewFwdMode::FastScan);
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_low_cut(bool on) {
    Locked l(p_->mu);
    p_->store.values().low_cut = on;
    p_->apply_dsp_settings();
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_monitor(bool on) {
    Locked l(p_->mu);
    p_->store.values().monitor = on;
    p_->mon_failed = false; /* a new try */
    p_->app_left = false;
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_monitor_output(const std::string &id) {
    Locked l(p_->mu);
    Impl &d = *p_;
    const std::string mac = bt_mac_of_id(id);
    if (id.compare(0, 3, "bt:") == 0 && mac.empty()) return false; /* not an address */
    for (unsigned char c : id)
        if (c < 0x20 || c == 0x7F) return false;
    if (id.size() > 256) return false;
    d.store.values().monitor_output = mac.empty() ? id : bt_id_of(mac);
    d.mon_failed = false; /* a new target: a new try */
    d.mon_lost_bt = false;
    d.persist();
    d.publish();
    return true;
}
bool Controller::set_wave_fit(bool on) {
    Locked l(p_->mu);
    p_->store.values().wave_fit = on;
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_monitor_level(int percent) {
    Locked l(p_->mu);
    p_->store.values().monitor_level = monitor_level_clamp(percent);
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::set_limiter(bool on) {
    Locked l(p_->mu);
    p_->store.values().limiter = on;
    p_->apply_dsp_settings();
    p_->persist();
    p_->publish();
    return true;
}
bool Controller::select_card(int index) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() == DeckState::Countdown) return false;
    if (index < 0) return false;
    std::string key;
    if (index < static_cast<int>(d.cards.size())) {
        key = key_of(d.cards[static_cast<size_t>(index)].info);
    } else { /* a Bluetooth device: the entries follow the cards */
        const size_t bi = static_cast<size_t>(index) - d.cards.size();
        if (bi >= d.bt_list.size()) return false;
        const BtInput &b = d.bt_list[bi];
        if (!b.usable) { /* absent or without a microphone: say why, change nothing */
            d.add_notice(Notice::Info, b.connected ? b.reason : std::string(kBtNotConnected));
            d.publish();
            return false;
        }
        key = b.id;
    }
    if (d.deck.state() != DeckState::Idle) d.run_deck(DeckEvent::Stop);
    d.close_capture();
    d.want_text = key;
    d.store.values().card = key;
    d.lost = false;
    d.bt_policy_closed = false;
    const bool ok = d.try_open_capture() || (d.active_is_bt && d.bt_policy_closed);
    d.rebuild_output();
    d.persist();
    d.publish();
    return ok;
}
bool Controller::select_output(const std::string &device) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() != DeckState::Idle || d.tr.previewing) return false;
    const std::string mac = bt_mac_of_id(device);
    if (device.compare(0, 3, "bt:") == 0 && mac.empty()) return false; /* not an address */
    d.store.values().output_device = mac.empty() ? device : bt_id_of(mac);
    d.rebuild_output();
    d.persist();
    d.publish();
    return true;
}
void Controller::refresh_cards() {
    Locked l(p_->mu);
    p_->refresh_cards_locked();
    p_->publish();
}
void Controller::set_page(UiPage page) {
    Locked l(p_->mu);
    p_->page = page;
    p_->publish();
}

/* ---- takes ---- */

void Controller::refresh_takes() {
    Locked l(p_->mu);
    p_->refresh_takes_locked();
    p_->publish();
}
bool Controller::select_take(const std::string &name) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.has_take(name) || d.take_active) return false;
    d.target = name;
    for (TakeRow &r : d.rows) r.current = r.name == name;
    ++d.takes_rev;
    d.publish();
    return true;
}
bool Controller::play_take(const std::string &name) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.has_take(name) || d.take_active || d.deck.state() == DeckState::Countdown) return false;
    if (d.refuse_missing_codec(name)) {
        d.publish();
        return false;
    }
    d.stop_preview();
    if (d.playing_family()) d.run_deck(DeckEvent::Stop);
    d.preview_name.clear();
    d.target = name;
    for (TakeRow &r : d.rows) r.current = r.name == name;
    ++d.takes_rev;
    const bool ok = d.run_deck(DeckEvent::PlayPause);
    d.publish();
    return ok;
}
bool Controller::load(const std::string &name) {
    Locked l(p_->mu);
    Impl &d = *p_;
    /* refused while recording (a take, a paused take, the countdown) or when the take is not in the list */
    if (!d.has_take(name) || d.take_active || d.recording_family()) return false;
    if (d.refuse_missing_codec(name)) {
        d.publish();
        return false;
    }
    d.stop_preview();
    if (d.playing_family()) d.run_deck(DeckEvent::Stop);
    d.preview_name.clear();
    d.target = name;
    for (TakeRow &r : d.rows) r.current = r.name == name;
    d.deck.set_has_last_take(true);
    ++d.takes_rev;
    ++d.load_seq;
    d.publish();
    return true;
}
bool Controller::preview_take(const std::string &name) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.has_take(name) || d.take_active || d.recording_family()) return false;
    if (d.refuse_missing_codec(name)) {
        d.publish();
        return false;
    }
    d.stop_preview();
    if (d.playing_family()) d.run_deck(DeckEvent::Stop);
    d.preview_name = name == d.target ? std::string() : name;
    d.deck.set_has_last_take(true); /* an empty deck still previews (run_deck puts the flag back to the loaded take) */
    const bool ok = d.run_deck(DeckEvent::PlayPause);
    d.publish();
    return ok;
}
int Controller::rename_take(const std::string &name, const std::string &new_prefix, std::string *new_name,
                            RenameRefusal *why) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.in_use(name)) return -EBUSY;
    std::string out;
    const int r = cassette::rename_take(*d.ops, d.folder, name, new_prefix, out, why);
    if (r == 0) {
        if (d.target == name) d.target = out;
        if (new_name) *new_name = out;
        d.refresh_takes_locked();
        d.publish();
    }
    return r;
}
int Controller::delete_take(const std::string &name) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.in_use(name)) return -EBUSY;
    const int r = cassette::delete_take(*d.ops, d.folder, name);
    d.refresh_takes_locked();
    d.publish();
    return r;
}

/* ---- trim page ---- */

bool Controller::open_trim(const std::string &take_name) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.started || d.take_active || d.deck.state() != DeckState::Idle) return false;
    if (d.refuse_missing_codec(take_name, "trimmed")) { /* the peaks and the preview decode it */
        d.publish();
        return false;
    }
    const std::string path = d.folder + "/" + take_name;
    const EditTakeInfo ti = inspect_take(*d.ops, path);
    if (!ti.ok) {
        d.add_notice(Notice::Info, "This take cannot be trimmed: " + ti.error);
        d.publish();
        return false;
    }
    d.close_trim_locked();
    d.tr.open = true;
    d.tr.take = take_name;
    d.tr.path = path;
    d.tr.frames = ti.frames;
    d.tr.rate = ti.fmt.rate ? ti.fmt.rate : 48000;
    d.tr.in = 0;
    d.tr.out = ti.frames;
    d.tr.pos = 0;
    d.recenter();
    d.start_peaks(path, ti.frames);
    d.publish();
    return true;
}
void Controller::close_trim() {
    Locked l(p_->mu);
    p_->close_trim_locked();
    p_->publish();
}
bool Controller::set_position(uint64_t frame) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open) return false;
    d.tr.pos = std::min(frame, d.tr.frames);
    if (d.tr.previewing && d.player) d.player->seek(d.tr.pos);
    d.fit_window();
    d.publish();
    return true;
}
bool Controller::move_position(int64_t delta) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open) return false;
    int64_t np = static_cast<int64_t>(d.tr.pos) + delta;
    np = std::max<int64_t>(0, std::min<int64_t>(np, static_cast<int64_t>(d.tr.frames)));
    d.tr.pos = static_cast<uint64_t>(np);
    if (d.tr.previewing && d.player) d.player->seek(d.tr.pos);
    d.fit_window();
    d.publish();
    return true;
}
bool Controller::set_in() {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open || d.tr.pos >= d.tr.out) return false;
    d.tr.in = d.tr.pos;
    d.prune_splits();
    d.publish();
    return true;
}
bool Controller::set_out() {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open || d.tr.pos <= d.tr.in) return false;
    d.tr.out = d.tr.pos;
    d.prune_splits();
    d.publish();
    return true;
}
bool Controller::add_split() {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open || d.tr.pos <= d.tr.in || d.tr.pos >= d.tr.out) return false;
    for (uint64_t s : d.tr.splits)
        if (s == d.tr.pos) return false;
    d.tr.splits.push_back(d.tr.pos);
    std::sort(d.tr.splits.begin(), d.tr.splits.end());
    d.tr.sel = static_cast<int>(std::find(d.tr.splits.begin(), d.tr.splits.end(), d.tr.pos) - d.tr.splits.begin());
    d.publish();
    return true;
}
bool Controller::remove_split(int n) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open || n < 0 || n >= static_cast<int>(d.tr.splits.size())) return false;
    d.tr.splits.erase(d.tr.splits.begin() + n);
    d.tr.sel = -1;
    d.publish();
    return true;
}
bool Controller::select_split(int n) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open || n < 0 || n >= static_cast<int>(d.tr.splits.size())) return false;
    d.tr.sel = n;
    d.tr.pos = d.tr.splits[static_cast<size_t>(n)];
    if (d.tr.previewing && d.player) d.player->seek(d.tr.pos);
    d.fit_window();
    d.publish();
    return true;
}
bool Controller::set_zoom(int level) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open) return false;
    d.tr.zoom = std::max(0, std::min(level, d.max_zoom()));
    d.recenter();
    d.publish();
    return true;
}
bool Controller::preview(bool on) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open) return false;
    if (!on) {
        d.stop_preview();
        d.publish();
        return true;
    }
    if (d.take_active || d.deck.state() != DeckState::Idle || !d.player || d.tr.out <= d.tr.in) return false;
    const uint64_t from = (d.tr.pos >= d.tr.in && d.tr.pos < d.tr.out) ? d.tr.pos : d.tr.in;
    d.monitor_stop();
    d.decoders();
    if (d.player->open(d.tr.path) < 0) return false;
    d.player->seek(from);
    if (d.player->play() < 0) return false;
    d.tr.pos = from;
    d.tr.previewing = true;
    d.tr.grace = 0;
    d.publish();
    return true;
}
bool Controller::save(SaveKind kind) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (!d.tr.open || d.save_state.load() == 1 || d.tr.out <= d.tr.in) return false;
    d.stop_preview();
    d.merge_save();
    if (d.save_th.joinable()) d.save_th.join();
    EditRequest req;
    req.source_path = d.tr.path;
    req.in_frame = d.tr.in;
    req.out_frame = d.tr.out;
    req.splits = d.tr.splits;
    req.mode = kind == SaveKind::ReplaceOriginal ? SaveMode::ReplaceOriginal : SaveMode::NewFiles;
    d.tr.replace_kind = kind == SaveKind::ReplaceOriginal;
    d.save_cancel.store(false);
    d.save_done.store(0);
    d.save_total.store(1);
    d.save_state.store(1);
    d.save_seen = 0;
    d.save_msg.clear();
    d.save_outputs.clear();
    Impl *dp = &d;
    const std::string dir = d.folder;
    d.decoders(); /* resolved before the worker reads it */
    d.save_th = std::thread([dp, req, dir] {
        EditOptions o;
        o.ops = dp->ops;
        o.names = [dp, dir](const std::string &source, int count) {
            std::vector<std::string> out;
            if (reserve_edit_names(*dp->ops, dir, source, static_cast<unsigned>(count), out) < 0) out.clear();
            return out;
        };
        o.on_original_deleted = [dp](const std::string &path) { dp->ops->unlink(PeakPyramid::cache_path(path)); };
        o.progress = [dp](uint64_t done, uint64_t total) {
            dp->save_total.store(total ? total : 1);
            dp->save_done.store(done);
        };
        o.cancel = &dp->save_cancel;
        o.block_bytes = dp->cfg.save_block_bytes;
        o.decoders = &dp->dlibs;
        if (dp->cfg.make_encoder) { /* a FLAC result through the configured encoder (the tests' fixture), else FlacEncoder */
            auto make = dp->cfg.make_encoder;
            o.make_flac_encoder = [make](const AudioFormat &f) {
                FormatChoice c;
                c.id = "flac";
                c.key.format = RecFormat::Flac;
                c.flac_level = 5;
                return make(c, f);
            };
        }
        EditResult r = trim_split(req, o);
        {
            std::lock_guard<std::mutex> g(dp->wmu);
            dp->save_result = r;
        }
        dp->save_state.store(r.ok ? 2 : (r.error == EditError::Cancelled ? 4 : 3));
    });
    d.publish();
    return true;
}
void Controller::cancel_save() {
    p_->save_cancel.store(true);
}

/* ---- merge takes ---- */

MergeInfo Controller::plan_merge(const std::vector<std::string> &names) {
    Locked l(p_->mu);
    Impl &d = *p_;
    MergeInfo info;
    info.count = static_cast<int>(names.size());
    MergeRequest req;
    uint64_t number = 0;
    std::string why = d.merge_request(names, false, req, number);
    if (why.empty()) {
        const MergePlan p = cassette::plan_merge(req, d.merge_options());
        if (!p.ok) {
            why = p.error == MergeError::NoSpace ? p.message : "Cannot merge: " + p.message;
        } else {
            info.ok = true;
            info.name = req.output_name;
            info.seconds = p.fmt.rate ? static_cast<double>(p.frames) / p.fmt.rate : 0.0;
            info.bytes = p.estimate_bytes;
        }
    }
    info.message = why;
    return info;
}

bool Controller::start_merge(const std::vector<std::string> &names, bool delete_originals) {
    Locked l(p_->mu);
    Impl &d = *p_;
    MergeRequest req;
    uint64_t number = 0;
    std::string why = d.merge_request(names, delete_originals, req, number);
    if (why.empty()) { /* the engine's checks too (compatible, the name, the free space), before anything starts */
        const MergePlan p = cassette::plan_merge(req, d.merge_options());
        if (!p.ok) why = p.error == MergeError::NoSpace ? p.message : "Cannot merge: " + p.message;
    }
    if (!why.empty()) {
        d.add_notice(Notice::Info, why);
        d.publish();
        return false;
    }
    d.merge_finish();
    if (d.merge_th.joinable()) d.merge_th.join();
    d.merge_cancel.store(false);
    d.merge_done.store(0);
    d.merge_total.store(1);
    d.merge_seen = 0;
    ++d.merge_seq;
    d.merge_names = names;
    d.merge_count = static_cast<int>(names.size());
    d.merge_out = req.output_name;
    d.merge_msg.clear();
    d.merge_deleted_all = false;
    MergeOptions o = d.merge_options(); /* the decoders are resolved here, before the worker reads them */
    Impl *dp = &d;
    const std::string dir = d.folder;
    d.merge_state.store(1);
    d.merge_th = std::thread([dp, req, number, dir, o]() mutable {
        o.progress = [dp](uint64_t done, uint64_t total) {
            dp->merge_total.store(total ? total : 1);
            dp->merge_done.store(done);
        };
        o.cancel = &dp->merge_cancel;
        /* the number is used only by a merge that succeeded: the counter moves on once the new take is in place */
        o.on_committed = [dp, dir, number](const std::string &) { note_take_number_used(*dp->ops, dir, number); };
        o.delete_original = [dp, dir](const std::string &path) { return cassette::delete_take(*dp->ops, dir, base_name(path)); };
        MergeResult r = merge_takes(req, o);
        {
            std::lock_guard<std::mutex> g(dp->wmu);
            dp->merge_result = r;
        }
        dp->merge_state.store(r.ok ? 2 : (r.error == MergeError::Cancelled ? 4 : 3));
    });
    d.publish();
    return true;
}

void Controller::cancel_merge() {
    p_->merge_cancel.store(true);
}

}  // namespace cassette
