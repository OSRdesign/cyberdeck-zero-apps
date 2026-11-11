/*
 * SPDX-License-Identifier: MIT
 */
#include "controller.hpp"

#include "../alsa/capture.hpp"
#include "../alsa/card.hpp"
#include "../alsa/mixer.hpp"
#include "../alsa/open_policy.hpp"
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
#include "../playback/sink.hpp"
#include "../repair.hpp"
#include "../ring.hpp"
#include "../takes/atomic_file.hpp"
#include "../takes/recordings.hpp"
#include "../writer.hpp"
#include "record_gate.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace cassette {
namespace {

constexpr uint64_t kSpaceRefreshMs = 2000;
constexpr uint64_t kPlayLevelStaleMs = 150; /* no new played period for this long: the playback level falls */

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

    ~Impl() override { shutdown(); }

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

    /* playback */
    std::unique_ptr<PlaybackSink> sink;
    std::unique_ptr<Player> player;
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
        if (!have_card || !cap || via_server || take_active || active_offer.rates.empty()) return;
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

    void refresh_cards_locked() {
        cards = list_capture_cards(*cap_api(), false);
        sel = -1;
        if (have_card)
            for (size_t i = 0; i < cards.size(); ++i)
                if (cards[i].info.id == active.id) sel = static_cast<int>(i);
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
    bool monitor_wanted() const {
        if (!started || down || !cap || !cap->running() || !mtap || !have_card || !active.playback) return false;
        if (!store.values().monitor || app_left) return false;
        if (playing_family() || tr.previewing || tr.open || !preview_name.empty()) return false;
        if (player && player->state() != PlayerState::Stopped) return false; /* the Player holds (or opens) the card */
        return true;
    }
    void monitor_stop() {
        if (mon) mon->stop();
        mon.reset();
    }
    /* Brings the monitor in line with the state; called from publish() (every command and every service tick). */
    void monitor_sync() {
        if (mon && !mon->running()) { /* the sink died (a device error) */
            monitor_stop();
            mon_failed = true;
            add_notice(Notice::Info, "Monitor unavailable");
        }
        if (!monitor_wanted()) {
            if (mon) monitor_stop();
            return;
        }
        if (!mon && !mon_failed) {
            std::unique_ptr<PlaybackSink> sk;
            const std::string dev = pcm_device(active);
            if (cfg.make_sink) sk = cfg.make_sink(dev);
            else sk.reset(new AlsaPlaybackSink(*cap_api(), dev));
            std::unique_ptr<Monitor> m(new Monitor(*mtap));
            m->set_level_pct(store.values().monitor_level);
            if (m->start(std::move(sk)) < 0) {
                mon_failed = true;
                add_notice(Notice::Info, "Monitor unavailable");
                return;
            }
            mon = std::move(m);
        }
        if (mon) mon->set_level_pct(store.values().monitor_level);
    }

    void close_capture() {
        monitor_stop();
        cap.reset();
        proc.reset();
        mixer.reset();
        mstate = MixerState();
    }

    /* Finds the wanted card and opens the capture path. true when the meter is running. */
    bool try_open_capture() {
        refresh_cards_locked();
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

    void on_capture_lost(const CaptureStats &st) {
        (void)st;
        lost = true;
        want_text = active_key; /* wait for this very card to come back */
        cap_message = "The sound card was unplugged";
        lost_text = "The sound card was unplugged";
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

    void rebuild_output() {
        if (take_active) return;
        player.reset();
        sink.reset();
        plan = choose_output(cap_api()->cards(), have_card ? active.id : std::string(), store.values().output_device);
        if (cfg.make_sink) sink = cfg.make_sink(plan.chosen.device);
        else sink.reset(new AlsaPlaybackSink(*cap_api(), plan.chosen.device));
        player.reset(new Player(*ops, *sink, Player::Mode::Threaded, &dlibs));
    }

    /* ================================================================================================================ */
    /* recording */

    bool begin_take() {
        stop_preview();
        if (take_active) return false;
        if (merge_state.load() == 1) { /* the merge holds the next take number until it ends */
            add_notice(Notice::Info, "Wait until the merge has finished");
            return false;
        }
        if (!cap || !cap->running()) {
            prob = ProblemSnap();
            prob.message = "No sound card is recording: the take cannot start";
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
        gate.attach(ring.get());
        /* what the screen must say about the capture path (the planned notices of study 058) */
        if (via_server) {
            add_notice(Notice::Info, "Recording through the sound server: the card is in use");
        } else {
            const bool running = cfg.server_running ? cfg.server_running() : sound_server_running_here();
            if (running) add_notice(Notice::Info, "Other apps cannot use this card while Cassette records");
        }
        return true;
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
            prob.message = "The sound card was unplugged: the take was closed and kept";
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

    bool open_target() {
        stop_preview();
        if (refuse_missing_codec(play_target())) return false;
        monitor_stop(); /* the Player is about to open the card's playback side: the monitor lets go first */
        if (!player || play_target().empty()) return false;
        const std::string path = folder + "/" + play_target();
        const int r = player->open(path);
        if (r < 0) {
            add_notice(Notice::Info, "This take cannot be played: " + decode_error_text(r, path));
            return false;
        }
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
            if (!open_target() || player->play() < 0) pend.push_back(DeckEvent::Stop);
            break;
        case DeckCommand::CueLast:
            if (!open_target()) pend.push_back(DeckEvent::Stop);
            break;
        case DeckCommand::PausePlayback:
            if (player) player->pause();
            break;
        case DeckCommand::ResumePlayback:
            if (player) player->resume();
            break;
        case DeckCommand::StopPlayback:
            if (player) player->stop();
            break;
        case DeckCommand::BeginScan:
            if (player) player->scan(c.dir, kScanSpeed);
            break;
        case DeckCommand::EndScan:
            if (player) player->end_scan();
            break;
        case DeckCommand::Jump: {
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
                lost_text = "The sound output was lost";
                run_deck(DeckEvent::DeviceLost);
            }
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
        if (!cap && now - last_retry_ms >= cfg.replug_retry_ms) {
            last_retry_ms = now;
            if (try_open_capture() && !take_active && !playing_family() && deck.state() == DeckState::Idle && !tr.previewing)
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
        publish();
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
        s.card.via_server = cap && via_server;
        for (const CaptureCard &c : cards) s.card.labels.push_back(c.label);
        s.card.selected = sel;
        if (sel >= 0 && sel < static_cast<int>(cards.size())) s.card.label = cards[static_cast<size_t>(sel)].label;
        else if (have_card) s.card.label = active.name;
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
        s.card.has_playback = s.card.present && have_card && active.playback;
        s.card.monitor_active = mon && mon->running();
        s.card.output_label = plan.chosen.label;
        for (const OutputOption &o : plan.choices) {
            if (o.device == plan.chosen.device) s.card.output_selected = static_cast<int>(s.card.output_labels.size());
            s.card.output_labels.push_back(o.label);
            s.card.output_devices.push_back(o.device);
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
        if (s.card.present && fmt.rate && fmt.channels && fmt.bits) fill_inputs(s.format_inputs);
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
    if (index < 0 || index >= static_cast<int>(d.cards.size())) return false;
    const std::string key = key_of(d.cards[static_cast<size_t>(index)].info);
    if (d.deck.state() != DeckState::Idle) d.run_deck(DeckEvent::Stop);
    d.close_capture();
    d.want_text = key;
    d.store.values().card = key;
    d.lost = false;
    const bool ok = d.try_open_capture();
    d.rebuild_output();
    d.persist();
    d.publish();
    return ok;
}
bool Controller::select_output(const std::string &device) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() != DeckState::Idle || d.tr.previewing) return false;
    d.store.values().output_device = device;
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
