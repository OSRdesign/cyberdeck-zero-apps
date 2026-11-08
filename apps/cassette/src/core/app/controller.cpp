/*
 * SPDX-License-Identifier: MIT
 */
#include "controller.hpp"

#include "../alsa/capture.hpp"
#include "../alsa/card.hpp"
#include "../alsa/mixer.hpp"
#include "../alsa/open_policy.hpp"
#include "../dsp/processing_sink.hpp"
#include "../edit/peaks.hpp"
#include "../edit/trim_split.hpp"
#include "../playback/deck.hpp"
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

std::string format_text(const AudioFormat &f) {
    char b[64];
    std::snprintf(b, sizeof b, "%gk/%u", f.rate / 1000.0, static_cast<unsigned>(f.bits));
    std::string s = b;
    if (f.channels == 2) s += " stereo";
    else if (f.channels > 2) s += " " + std::to_string(f.channels) + "ch";
    return s + " WAV";
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

int zone_int(Zone z) { return static_cast<int>(z); }

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

    /* capture pipeline: destruction order is cap, proc (the gate outlives them) */
    RecordGate gate;
    std::unique_ptr<ProcessingSink> proc;
    std::unique_ptr<Capture> cap;
    AudioFormat fmt;
    bool via_server = false;

    /* playback */
    std::unique_ptr<PlaybackSink> sink;
    std::unique_ptr<Player> player;
    OutputPlan plan;

    /* the take being recorded */
    std::unique_ptr<Ring> ring;
    std::unique_ptr<Writer> writer;
    bool take_active = false, paused = false, unplug_flag = false;
    std::string take_name;
    uint64_t pause_total = 0, pause_start = 0;
    uint32_t base_overruns = 0;
    uint64_t base_lost = 0;
    ProblemSnap prob;

    /* takes */
    std::vector<TakeRow> rows;
    unsigned takes_rev = 0;
    std::string target;
    SpaceInfo space;
    uint64_t last_space_ms = 0;

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
        deck.set_has_last_take(!rows.empty()); /* the list is the truth (a take that failed to start leaves none) */
        return r;
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
            r.format_text = t.damaged ? std::string() : format_text(t.fmt);
            r.date_text = date_text(t);
            r.damaged = t.damaged;
            r.repaired = t.repaired;
            rows.push_back(r);
        }
        bool found = false;
        for (const TakeRow &r : rows) found = found || r.name == target;
        if (!found) target = rows.empty() ? std::string() : rows.front().name;
        for (TakeRow &r : rows) r.current = r.name == target;
        deck.set_has_last_take(!rows.empty());
        ++takes_rev;
    }

    bool has_take(const std::string &name) const {
        for (const TakeRow &r : rows)
            if (r.name == name) return true;
        return false;
    }

    void refresh_space() {
        AudioFormat f = fmt;
        if (!(f.rate && f.channels && f.bits)) f = AudioFormat();
        if (folder_space(*ops, folder, f, space) < 0) space = SpaceInfo();
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

    void close_capture() {
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
        OpenResult r = open_capture(*cap_api(), info);
        active = info;
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
        proc.reset(new ProcessingSink(fmt, &gate));
        apply_dsp_settings();
        std::unique_ptr<Capture> c(new Capture(std::move(r), *proc));
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
        player.reset(new Player(*ops, *sink, Player::Mode::Threaded));
    }

    /* ================================================================================================================ */
    /* recording */

    bool begin_take() {
        stop_preview();
        if (take_active) return false;
        if (!cap || !cap->running()) {
            prob = ProblemSnap();
            prob.message = "No sound card is recording: the take cannot start";
            add_notice(Notice::Info, prob.message);
            return false;
        }
        const Settings &s = store.values();
        make_dirs(*ops, folder);
        uint64_t number = 0;
        int r = reserve_take_number(*ops, folder, number);
        prob = ProblemSnap();
        if (r < 0) {
            prob.write_error = true;
            prob.disk_full = r == -ENOSPC;
            prob.message = std::string("The take cannot be created: ") + std::strerror(-r);
            add_notice(Notice::Info, prob.message);
            return false;
        }
        const TakeName tn = new_take_name(s.prefix, number, *cstate, *clk);
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
        return true;
    }

    void end_take() {
        if (!take_active) return;
        gate.detach();
        writer->stop();
        const WriterStatus st = writer->status();
        prob.frames_dropped = ring->frames_dropped();
        if (cap) {
            const CaptureStats cs = cap->stats();
            prob.overruns = cs.overruns - base_overruns;
            prob.frames_lost = cs.frames_lost_xrun - base_lost;
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

    bool open_target() {
        stop_preview();
        if (!player || target.empty()) return false;
        const int r = player->open(folder + "/" + target);
        if (r < 0) {
            add_notice(Notice::Info, "This take cannot be played: " + std::string(std::strerror(-r)));
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
        case DeckCommand::Jump:
            if (player) player->jump(c.dir, kJumpSeconds);
            break;
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
        refresh_takes_locked();
    }

    void start_peaks(const std::string &path, uint64_t frames) {
        peaks_cancel.store(false);
        peaks_state.store(1);
        peaks_done.store(0);
        peaks_total.store(frames);
        peaks_mem.store(false);
        peaks_th = std::thread([this, path] {
            PeakBuildOptions o;
            o.progress = [this](uint64_t done, uint64_t) { peaks_done.store(done); };
            o.cancel = &peaks_cancel;
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

    void tick() {
        std::lock_guard<std::mutex> g(mu);
        if (!started || down) return;
        const uint64_t now = clk->mono_ms();
        run_deck(DeckEvent::TimerTick);
        PlayerEvent ev;
        while (player && player->poll_event(ev)) {
            if (ev == PlayerEvent::TakeEnded || ev == PlayerEvent::ReachedEnd) run_deck(DeckEvent::TakeEnded);
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
        if (now - last_space_ms >= kSpaceRefreshMs) refresh_space();
        publish();
    }

    void publish() {
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
            const MeterSnapshot m = proc->levels();
            s.meter.channels = m.channels;
            for (int i = 0; i < 2; ++i) {
                s.meter.ch[i].peak_db = m.ch[i].peak_db;
                s.meter.ch[i].rms_db = m.ch[i].rms_db;
                s.meter.ch[i].hold_db = m.ch[i].hold_db;
                s.meter.ch[i].bar = m.ch[i].bar;
                s.meter.ch[i].hold_bar = m.ch[i].hold_bar;
                s.meter.ch[i].peak_lamp = m.ch[i].peak_lamp;
                s.meter.ch[i].zone = zone_int(m.ch[i].zone);
            }
            s.meter.clips = static_cast<unsigned>(proc->clip_count());
        }
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
        s.card.output_label = plan.chosen.label;
        for (const OutputOption &o : plan.choices) {
            if (o.device == plan.chosen.device) s.card.output_selected = static_cast<int>(s.card.output_labels.size());
            s.card.output_labels.push_back(o.label);
            s.card.output_devices.push_back(o.device);
        }
        /* take */
        s.take.recording = take_active;
        s.take.name = take_active ? take_name : target;
        s.take.format_text = format_text(f);
        s.take.free_bytes = space.free_bytes;
        s.take.hours_left = space.hours_left;
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
        d.stop_preview();
        d.run_deck(de);
        d.join_save(); /* a save in progress is cancelled: the temporaries go, the original stays */
        d.join_peaks();
        d.persist();
        d.publish();
        return true;
    }
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
bool Controller::set_folder(const std::string &folder) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if (d.take_active || d.deck.state() == DeckState::Countdown || d.save_state.load() == 1) return false;
    if (folder.empty() || folder.size() > 1024) return false;
    const std::string expanded = expand_home(folder, d.cfg.home);
    if (make_dirs(*d.ops, expanded) < 0) return false;
    if (d.tr.open) d.close_trim_locked();
    if (d.deck.state() != DeckState::Idle) d.run_deck(DeckEvent::Stop);
    d.store.values().folder = folder;
    d.folder = expanded;
    d.target.clear();
    d.refresh_takes_locked();
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
    d.stop_preview();
    if (d.playing_family()) d.run_deck(DeckEvent::Stop);
    d.target = name;
    for (TakeRow &r : d.rows) r.current = r.name == name;
    ++d.takes_rev;
    const bool ok = d.run_deck(DeckEvent::PlayPause);
    d.publish();
    return ok;
}
int Controller::rename_take(const std::string &name, const std::string &new_prefix, std::string *new_name) {
    Locked l(p_->mu);
    Impl &d = *p_;
    if ((d.tr.open && d.tr.take == name) || (d.take_active && d.take_name == name) ||
        (d.target == name && d.deck.state() != DeckState::Idle))
        return -EBUSY;
    std::string out;
    const int r = cassette::rename_take(*d.ops, d.folder, name, new_prefix, out);
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
    if ((d.tr.open && d.tr.take == name) || (d.take_active && d.take_name == name) ||
        (d.target == name && d.deck.state() != DeckState::Idle))
        return -EBUSY;
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

}  // namespace cassette
