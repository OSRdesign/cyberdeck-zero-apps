/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the controller (see vinyl_app.hpp).
 */
#include "vinyl_app.hpp"

#include "gfx.hpp"
#include "vinyl_view.hpp"

#include "alsa/alsa_api.hpp"
#include "app/pulse_worker.hpp"
#include "app/sink_watch.hpp"
#include "decode/decode_libs.hpp"
#include "demo/demo_world.hpp"
#include "engine/engine.hpp"
#include "engine/outputs.hpp"
#include "input/media_keys.hpp"
#include "launcher/launcher_link.hpp"
#include "library/browse.hpp"
#include "library/tag_jobs.hpp"
#include "pulse/pulse_api.hpp"
#include "state/resume.hpp"
#include "transport/transport_press.hpp"
#include "volume/output_volume.hpp"

#include "app.hpp"
#include "keyboard_input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace vinyl {
namespace ui {

namespace {

bool env_on(const char *name) {
    const char *v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

std::string env_str(const char *name) {
    const char *v = std::getenv(name);
    return v ? v : "";
}

std::string mmss(uint64_t ms) {
    const uint64_t s = ms / 1000;
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%u:%02u:%02u", unsigned(s / 3600), unsigned((s / 60) % 60), unsigned(s % 60));
    else std::snprintf(b, sizeof b, "%02u:%02u", unsigned(s / 60), unsigned(s % 60));
    return b;
}

std::string khz(uint32_t rate) {
    char b[16];
    if (rate % 1000 == 0) std::snprintf(b, sizeof b, "%u", rate / 1000);
    else std::snprintf(b, sizeof b, "%.1f", rate / 1000.0);
    return b;
}

/* evdev codes of the letter keys used (the physical key, not its text: the guidelines' rule) */
enum : uint32_t {
    K_MINUS = 12, K_EQUAL = 13, K_Q = 16, K_R = 19, K_U = 22, K_O = 24, K_S = 31, K_D = 32, K_X = 45, K_C = 46,
    K_B = 48, K_COMMA = 51, K_DOT = 52, K_SPACE = 57, K_UP = 103, K_LEFT = 105, K_RIGHT = 106, K_DOWN = 108, K_DELETE = 111,
};

/* The LVGL key the toolkit's keypad sends after a key event (cp0_keyboard_lvgl_input.c): the semantic key where it has
 * one. In the navigation context the CardputerZero's F / X / Z / C are also Up / Down / Left / Right (its arrows), so a
 * press of C (Clear all) reaches the keypad as Right, X (Remove) as Down. 0: not an arrow. */
uint32_t arrow_echo(const key_item &it) {
    if (!it.semantic_key || it.semantic_key == it.key_code) return 0;
    switch (it.semantic_key) {
    case K_UP: return LV_KEY_UP;
    case K_DOWN: return LV_KEY_DOWN;
    case K_LEFT: return LV_KEY_LEFT;
    case K_RIGHT: return LV_KEY_RIGHT;
    default: return 0;
    }
}

}  // namespace

struct VinylApp::Impl final : TransportHost {
    lv_obj_t *screen = nullptr;
    lv_obj_t *focus = nullptr;
    lv_timer_t *timer = nullptr;
    std::unique_ptr<View> view;

    /* the world */
    bool demo = false, lite = false;
    DemoWorld demo_world;
    std::unique_ptr<PulseApi> pulse;
    std::unique_ptr<AlsaApi> alsa;
    std::unique_ptr<SinkProvider> sinks;
    std::unique_ptr<PulseWorker> worker;
    SinkWatch sink_watch; /* the server's sink changes between the scans (board test B9) */
    std::string vol_logged; /* the last volume line logged */
    std::unique_ptr<Engine> engine;
    TagJobs jobs;
    MediaKeyReader keys;
    std::unique_ptr<LauncherStatusReader> launcher;
    std::unique_ptr<LauncherRequests> requests;
    LauncherStatus status;
    OutputWorld world;
    OutputTarget target;

    /* state */
    Queue queue;
    ResumeState resume;
    std::string resume_file;
    bool dirty = false;
    uint64_t last_save = 0;
    std::vector<Root> roots;
    std::string folder;      /* "" = the list of roots */
    std::vector<Entry> entries;
    uint64_t rows_gen = 1;
    uint64_t qgen = 1; /* every change of the queue's order or contents */
    uint64_t queue_set_gen = 1; /* a new queue, or the queue page opened again: it scrolls to the playing track */
    struct RowsCache {
        uint64_t key = ~0ull;
        std::shared_ptr<std::vector<RowView>> rows;
    };
    RowsCache pane_cache, queue_cache, browser_cache, output_cache;
    uint64_t out_gen = 1;
    int br_sel = 0, qu_sel = 0, out_sel = 0, pane_sel = -1;
    int qu_scroll = -1, br_scroll = -1;
    Page back_from_queue = Page::Now, back_from_output = Page::Now;
    EngineSnapshot snap;
    uint64_t track_gen = 0;
    std::string cover_for;
    std::shared_ptr<const PixelImage> cover;
    int auto_skips = 0;
    uint64_t tags_seen = UINT64_MAX;
    uint64_t notice_gen = 0;
    std::string call_noticed;  /* the sink the call-quality notice was shown for */
    bool dim_sent = false;
    uint64_t paused_since = 0;
    bool was_hidden = false;
    uint64_t last_scan = 0;
    unsigned scan_seq = 0;
    std::vector<OutputRow> out_rows;
    /* the transport press (core/transport): previous / next from the arrows on now playing, the comma and dot keys, the
     * on-screen buttons and the media keys, all the same */
    TransportPress transport{*this};
    bool scan_muted = false;
    /* the playing output's volume (D11 as changed 2026-11-12): a sink level asked for and shown until the server's event
     * brings it (1.5 s at most), the card mixer Vinyl reads when there is no server */
    int pending_level = -1;
    std::string pending_sink;
    uint64_t pending_until = 0;
    std::string mixer_card, mixer_choice_card;
    MixerChoice mixer_choice;
    MixerLevel mixer_level;
    uint64_t last_mixer_read = 0;

    /* ------------------------------------------------------------------------------------------ start */

    void start(lv_obj_t *scr) {
        screen = scr;
        demo = env_on("VINYL_DEMO");
        lite = env_on("VINYL_DEMO_LITE");
        if (demo) {
            /* the design-QA variants: no music at all, or CJK names and tags */
            const std::string variant = env_on("VINYL_DEMO_EMPTY") ? "empty"
                                        : env_on("VINYL_DEMO_CJK")  ? "cjk"
                                        : env_on("VINYL_DEMO_LONG") ? "long"
                                                                    : "";
            make_demo_world(demo_world, lite, env_str("VINYL_DEMO_STATE"), variant);
            if (demo_world.empty) roots.clear();
            else roots = {Root{"Music", demo_world.music}};
            resume_file = demo_world.config + "/vinyl/state.json";
            sinks = make_demo_sink_provider(variant);
            world = demo_output_world(lite, variant);
            status = demo_launcher_status();
            if (env_on("VINYL_DEMO_MUTED")) /* the output muted (by the launcher's key, another app): Muted everywhere */
                for (PulseDevice &d : world.sinks) d.muted = true;
            if (env_on("VINYL_DEMO_NOVOLUME")) { /* the server reports no level for its sinks: "--", nothing to change */
                status.volume = -1;
                for (PulseDevice &d : world.sinks) d.volume_pct = -1;
            }
        } else {
            roots = system_roots();
            resume_file = resume_path();
            pulse = make_real_pulse();
            alsa = make_real_alsa();
            sinks = make_real_sink_provider(pulse.get(), *alsa);
            worker = std::make_unique<PulseWorker>(*pulse);
            launcher = std::make_unique<LauncherStatusReader>();
            requests = std::make_unique<LauncherRequests>();
            keys.start();
        }
        std::fprintf(stderr, "[vinyl] %s, %d root(s), state %s\n", demo ? "demonstration world" : "started", static_cast<int>(roots.size()),
                     resume_file.c_str());
        engine = std::make_unique<Engine>(real_decode_libs(), *sinks);

        Actions a;
        a.back = [this] { back(); };
        a.toggle = [this] { toggle(); };
        a.prev = [this] { /* a tap: press and release at once */
            transport_key(false, 1);
            transport_key(false, 0);
        };
        a.next = [this] {
            transport_key(true, 1);
            transport_key(true, 0);
        };
        a.shuffle = [this] { shuffle(); };
        a.repeat = [this] { repeat(); };
        a.button = [this](const std::string &id) { button(id); };
        a.seek_to = [this](double f) { seek_frac(f); };
        a.row = [this](Page p, int i) { row(p, i); };
        a.queue_button = [this](int i) { queue_button(i); };
        a.queue_clear = [this] {
            std::fprintf(stderr, "[vinyl] the queue cleared (asked, Clear)\n");
            queue_edit(3);
        };
        a.mini_open = [this] { go(Page::Now); };
        a.transport = [this](bool next, int phase) {
            const uint64_t now = mono_ms_now();
            const TransportKey k = next ? TransportKey::Next : TransportKey::Prev;
            if (phase == 0) transport.press(k, now);
            else if (phase == 1) transport.release(k, now);
            else transport.cancel(k, now);
            refresh(true);
        };
        a.volume_step = [this](int d) { volume_step(d); };
        a.volume_unavailable = [this] { notice(Notice::Kind::Info, "Volume is not available on this board"); };
        view = std::make_unique<View>(screen, a);
        std::fprintf(stderr, "[vinyl] canvas %dx%d, class %s\n", cdk::screen_w(), cdk::screen_h(),
                     layout::class_name(view->size_class()));

        /* the keys: an invisible focus object for the navigation keys, the screen for the physical key codes */
        focus = lv_obj_create(screen);
        plain(focus);
        lv_obj_set_size(focus, 1, 1);
        lv_obj_add_event_cb(focus, key_cb, LV_EVENT_KEY, this);
        if (LV_EVENT_KEYBOARD != 0) lv_obj_add_event_cb(screen, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
        if (lv_group_t *g = cdk::input_group()) {
            lv_group_add_obj(g, focus);
            lv_group_focus_obj(focus);
        }

        /* the saved state */
        load_resume(resume_file, resume);
        target = OutputTarget::from_key(resume.output);
        engine->set_output(target);
        apply_gain();
        Page first = Page::Browser;
        if (!resume.items.empty()) {
            queue_from_resume(resume, queue);
            ++qgen;
            if (queue.current() >= 0) {
                engine->load(queue.current(), *queue.current_path(), resume.position_ms, demo && env_str("VINYL_DEMO_STATE") == "playing");
                first = Page::Now;
            }
            jobs.want_tags(queue.items());
        }
        std::string f = resume.browse;
        if (f.empty() || !root_of(roots, f)) f = roots.size() == 1 ? roots.front().path : "";
        open_folder(f, "");
        if (const std::string p = env_str("VINYL_DEMO_PAGE"); demo && !p.empty()) {
            if (p == "browser") first = Page::Browser;
            else if (p == "queue") first = Page::Queue;
            else if (p == "output") first = Page::Output;
            else if (p == "notices") first = Page::Notices;
            else first = Page::Now;
        }
        back_from_queue = back_from_output = Page::Now;
        if (first == Page::Queue || first == Page::Output) refresh_outputs(true);
        go(first);
        if (worker) scan(true);
        timer = lv_timer_create(timer_cb, 100, this);
        tick();
        if (const std::string n = env_str("VINYL_DEMO_NOTICE"); demo && !n.empty()) demo_notice(n);
    }

    void stop_all() {
        if (timer) lv_timer_delete(timer);
        timer = nullptr;
        save(true);
        keys.stop();
        if (engine) engine->shutdown();
        jobs.stop();
        if (worker) worker->stop();
        if (pulse) pulse->disconnect();
        view.reset();
        if (demo) remove_demo_world(demo_world);
    }

    /* ------------------------------------------------------------------------------------------ pages */

    void go(Page p) {
        if (p == Page::Queue) {
            qu_sel = std::max(0, queue.current());
            qu_scroll = qu_sel;
            ++queue_set_gen;
        }
        if (p == Page::Output) {
            refresh_outputs(true);
            out_sel = 0;
            for (size_t i = 0; i < out_rows.size(); ++i)
                if (out_rows[i].chosen) out_sel = static_cast<int>(i);
        }
        if (p == Page::Browser) br_scroll = std::max(0, br_sel - 1);
        view->show_page(p);
        refresh(true);
    }

    void back() {
        switch (view->page()) {
        case Page::Now: go(Page::Browser); break;
        case Page::Queue: go(back_from_queue); break;
        case Page::Output: go(back_from_output); break;
        case Page::Notices: go(Page::Now); break;
        case Page::Browser:
            std::fprintf(stderr, "[vinyl] Esc at the browser: quit\n");
            cdk::request_quit(0);
            break;
        }
    }

    void button(const std::string &id) {
        if (id == "queue") {
            back_from_queue = view->page();
            go(Page::Queue);
        } else if (id == "output") {
            back_from_output = view->page();
            go(Page::Output);
        } else if (id == "browse") {
            go(Page::Browser);
        }
    }

    /* ------------------------------------------------------------------------------------------ playback */

    void play_pos(int pos, bool play) {
        if (pos < 0 || pos >= queue.size()) return;
        queue.set_current(pos);
        ++qgen;
        auto_skips = 0;
        engine->load(pos, queue.at(pos), 0, play);
        dirty = true;
    }

    void toggle() {
        if (snap.state == PlayState::Idle && queue.current() >= 0) {
            if (snap.pos == queue.current() && !snap.info.label.empty()) engine->play();
            else play_pos(queue.current(), true);
            return;
        }
        if (snap.state == PlayState::Idle && queue.current() < 0 && !queue.empty()) {
            play_pos(0, true);
            return;
        }
        engine->toggle();
        dirty = true;
    }

    /* ---- the transport host (core/transport/transport_press.hpp): the module decides, these do */
    bool has_track() const override { return snap.pos >= 0 && !snap.path.empty(); }
    uint64_t position_ms() const override { return snap.pos_ms(); }
    uint64_t duration_ms() const override { return snap.total_ms(); }
    void seek(uint64_t ms) override { engine->seek_ms(ms); }
    bool go_next() override {
        const int n = transport_next_pos(queue);
        if (n < 0) return false;
        play_pos(n, snap.state == PlayState::Playing); /* playing or paused as it was */
        return true;
    }
    bool go_prev() override {
        const int n = transport_prev_pos(queue);
        if (n < 0) return false;
        play_pos(n, snap.state == PlayState::Playing);
        return true;
    }
    void mute(bool on) override {
        scan_muted = on;
        apply_gain();
    }

    /* a previous / next key (state as the keyboard's: 1 press, 0 release, 2 auto-repeat, which the module ignores) */
    void transport_key(bool next, int state) {
        const uint64_t now = mono_ms_now();
        const TransportKey k = next ? TransportKey::Next : TransportKey::Prev;
        if (state == 1) transport.press(k, now);
        else if (state == 0) transport.release(k, now);
        refresh(true);
    }

    /* the heard position, or the scan's while a long press scans */
    uint64_t shown_pos_ms() const { return transport.scanning() ? transport.scan_position() : snap.pos_ms(); }

    void shuffle() {
        queue.set_shuffle(!queue.shuffle());
        ++qgen;
        update_next();
        dirty = true;
        refresh(true);
    }

    void repeat() {
        queue.cycle_repeat();
        update_next();
        dirty = true;
        refresh(true);
    }

    void seek_frac(double f) {
        if (!has_track() || snap.total_ms() == 0) return;
        engine->seek_ms(static_cast<uint64_t>(f * snap.total_ms()));
    }

    void seek_by(int64_t ms) {
        if (!has_track()) return;
        int64_t t = static_cast<int64_t>(snap.pos_ms()) + ms;
        if (t < 0) t = 0;
        if (snap.total_ms() && static_cast<uint64_t>(t) >= snap.total_ms()) t = static_cast<int64_t>(snap.total_ms()) - 1000;
        engine->seek_ms(static_cast<uint64_t>(std::max<int64_t>(0, t)));
    }

    void update_next() {
        const int pos = snap.pos >= 0 ? snap.pos : queue.current();
        const int n = queue.next_after(pos, false);
        engine->set_next(n, n >= 0 ? queue.at(n) : std::string());
    }

    /* ------------------------------------------------------------------------------------------ browser */

    void open_folder(const std::string &path, const std::string &select_path) {
        entries.clear();
        folder = path;
        if (path.empty()) {
            entries = root_entries(roots);
        } else {
            const bool top = roots.size() == 1 && roots.front().path == path;
            const int r = list_folder(path, top, entries);
            if (r < 0) std::fprintf(stderr, "[vinyl] cannot list %s: %s\n", path.c_str(), std::strerror(-r));
        }
        br_sel = entries.empty() ? -1 : 0;
        std::vector<std::string> tracks;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].kind == Entry::Kind::Track) tracks.push_back(entries[i].path);
            if (!select_path.empty() && entries[i].path == select_path) br_sel = static_cast<int>(i);
        }
        /* the playing track is the selection when it is here */
        if (select_path.empty() && queue.current_path())
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].path == *queue.current_path()) br_sel = static_cast<int>(i);
        jobs.want_tags(tracks);
        br_scroll = std::max(0, br_sel - 2);
        ++rows_gen;
        resume.browse = path;
        dirty = true;
    }

    void browser_act(int i) {
        if (i < 0 || i >= static_cast<int>(entries.size())) return;
        const Entry e = entries[static_cast<size_t>(i)];
        switch (e.kind) {
        case Entry::Kind::Up: {
            const std::string old = folder;
            std::string up = parent_folder(roots, folder);
            if (up.empty() && roots.size() > 1) up = "";
            else if (up.empty()) up = folder;
            open_folder(up, old);
            refresh(true);
            break;
        }
        case Entry::Kind::Root:
        case Entry::Kind::Folder:
            open_folder(e.path, "");
            refresh(true);
            break;
        case Entry::Kind::Playlist: {
            std::ifstream f(e.path);
            std::stringstream ss;
            ss << f.rdbuf();
            const size_t slash = e.path.rfind('/');
            std::vector<std::string> items = parse_m3u(ss.str(), slash == std::string::npos ? "." : e.path.substr(0, slash), file_exists);
            if (items.empty()) {
                notice(Notice::Kind::Info, "This playlist has no track Vinyl can find.");
                return;
            }
            queue.set(items, 0, e.path);
            ++qgen;
            ++queue_set_gen;
            jobs.want_tags(queue.items());
            play_pos(queue.current() >= 0 ? queue.current() : 0, true);
            go(Page::Now);
            break;
        }
        case Entry::Kind::Track: {
            if (queue.current_path() && *queue.current_path() == e.path && queue.source() == folder) {
                go(Page::Now);
                return;
            }
            std::vector<std::string> tracks = folder_tracks(entries);
            int start = 0;
            for (size_t k = 0; k < tracks.size(); ++k)
                if (tracks[k] == e.path) start = static_cast<int>(k);
            queue.set(tracks, start, folder);
            ++qgen;
            ++queue_set_gen;
            jobs.want_tags(queue.items());
            play_pos(queue.current(), true);
            go(Page::Now);
            break;
        }
        }
    }

    /* ------------------------------------------------------------------------------------------ rows and keys */

    void row(Page p, int i) {
        if (p == Page::Browser) {
            br_sel = i;
            browser_act(i);
        } else if (p == Page::Queue) {
            if (i == qu_sel) play_pos(i, true);
            else qu_sel = i;
            refresh(true);
        } else if (p == Page::Now) { /* the large class's queue pane */
            if (i == pane_sel) play_pos(i, true);
            else pane_sel = i;
            refresh(true);
        } else if (p == Page::Output) {
            out_sel = i;
            choose_output(i);
        }
    }

    void queue_button(int i) {
        if (queue.empty()) return;
        if (i == 3) { /* Clear all asks first (design QA run 3): "Clear the queue?", Cancel the default */
            view->ask_clear_queue();
            return;
        }
        queue_edit(i);
    }

    void queue_edit(int i) {
        if (queue.empty()) return;
        const int cur = queue.current();
        if (i == 0 && queue.move_up(qu_sel)) --qu_sel;
        else if (i == 1 && queue.move_down(qu_sel)) ++qu_sel;
        else if (i == 2) {
            const bool was_current = qu_sel == cur;
            queue.remove(qu_sel);
            if (queue.empty()) {
                engine->stop();
                qu_sel = 0;
            } else {
                qu_sel = std::min(qu_sel, queue.size() - 1);
                if (was_current) {
                    if (queue.current() >= 0) play_pos(queue.current(), snap.state == PlayState::Playing);
                } else if (queue.current() >= 0) {
                    /* the engine's positions follow the queue's */
                    engine_track_moved();
                }
            }
        } else if (i == 3) {
            queue.clear_except_current();
            qu_sel = 0;
            if (queue.current() >= 0) engine_track_moved();
            else engine->stop();
        }
        update_next();
        ++qgen;
        dirty = true;
        refresh(true);
    }

    /* the queue changed around the playing track: the engine keeps playing it; only its position number changed */
    void engine_track_moved() {
        /* the engine reports positions as they were loaded: reload paused/playing at the same time when it differs */
        if (snap.pos != queue.current() && queue.current_path() && snap.path == *queue.current_path()) {
            engine->load(queue.current(), *queue.current_path(), snap.pos_ms(), snap.state == PlayState::Playing);
        }
    }

    void choose_output(int i) {
        if (i < 0 || i >= static_cast<int>(out_rows.size())) return;
        const OutputRow &r = out_rows[static_cast<size_t>(i)];
        if (r.dimmed && !r.chosen) {
            notice(Notice::Kind::Info, r.label + " is not connected: connect it in Settings, Bluetooth.");
            refresh(true);
            return;
        }
        target = r.target;
        engine->set_output(target);
        resume.output = target.key();
        call_noticed.clear();
        dirty = true;
        refresh_outputs(false);
        refresh(true);
    }

    static void key_cb(lv_event_t *e) { static_cast<Impl *>(lv_event_get_user_data(e))->on_key(lv_event_get_key(e)); }
    static void keyboard_cb(lv_event_t *e) {
        const auto *item = static_cast<const key_item *>(lv_event_get_param(e));
        if (item) static_cast<Impl *>(lv_event_get_user_data(e))->on_keyboard(*item);
    }

    void list_move(int &sel, int n, int d) {
        if (n <= 0) return;
        sel = std::clamp(sel + d, 0, n - 1);
        refresh(true);
    }

    /* the arrow the keypad sends right after a letter Vinyl acted on (arrow_echo): taken once and dropped, so C (Clear
     * all) never also moves the dialog's highlight to Clear, nor X (Remove) the queue's selection */
    uint32_t swallow_echo = 0;

    void on_key(uint32_t k) {
        const Page p = view->page();
        const uint32_t echo = swallow_echo;
        swallow_echo = 0;
        if (echo && k == echo) return;
        if (view->dialog_shown() && view->dialog_key(k)) return; /* "Clear the queue?" takes the keys while it shows */
        if (k == LV_KEY_ESC) {
            if (view->notice_shown()) view->hide_notice();
            back();
            return;
        }
        if (p == Page::Browser) {
            if (k == LV_KEY_UP) list_move(br_sel, static_cast<int>(entries.size()), -1);
            else if (k == LV_KEY_DOWN) list_move(br_sel, static_cast<int>(entries.size()), 1);
            else if (k == LV_KEY_ENTER || k == LV_KEY_RIGHT) browser_act(br_sel);
            else if (k == LV_KEY_LEFT || k == LV_KEY_BACKSPACE) {
                if (!entries.empty() && entries.front().kind == Entry::Kind::Up) browser_act(0);
            }
        } else if (p == Page::Queue) { /* Left goes back, Right acts as Enter (design QA D-29) */
            if (k == LV_KEY_UP) list_move(qu_sel, queue.size(), -1);
            else if (k == LV_KEY_DOWN) list_move(qu_sel, queue.size(), 1);
            else if (k == LV_KEY_ENTER || k == LV_KEY_RIGHT) play_pos(qu_sel, true);
            else if (k == LV_KEY_LEFT) back();
            else if (k == LV_KEY_DEL) queue_button(2);
        } else if (p == Page::Output) {
            if (k == LV_KEY_UP) list_move(out_sel, static_cast<int>(out_rows.size()), -1);
            else if (k == LV_KEY_DOWN) list_move(out_sel, static_cast<int>(out_rows.size()), 1);
            else if (k == LV_KEY_ENTER || k == LV_KEY_RIGHT) choose_output(out_sel);
            else if (k == LV_KEY_LEFT) back();
        } else if (p == Page::Now) { /* Left / Right: now_arrow() (the keyboard path, press and release) */
            if (k == LV_KEY_UP && view->size_class() == layout::SizeClass::Large)
                list_move(pane_sel, queue.size(), pane_sel < 0 ? queue.current() : -1);
            else if (k == LV_KEY_DOWN && view->size_class() == layout::SizeClass::Large)
                list_move(pane_sel, queue.size(), pane_sel < 0 ? queue.current() + 1 : 1);
            else if (k == LV_KEY_ENTER) {
                if (pane_sel >= 0 && pane_sel != queue.current()) play_pos(pane_sel, true);
                else toggle();
            }
        }
    }

    void media(const MediaEvent &e) {
        switch (e.action) {
        case MediaAction::Toggle:
            if (e.down) toggle();
            break;
        case MediaAction::Next: transport_key(true, e.down ? 1 : 0); break; /* held: a scan */
        case MediaAction::Prev: transport_key(false, e.down ? 1 : 0); break;
        case MediaAction::Stop:
            if (e.down) engine->stop();
            break;
        default: break;
        }
        std::fprintf(stderr, "[vinyl] media key: %s%s\n", media_action_name(e.action), e.down ? "" : " (released)");
    }

    /* Now playing's Left / Right: the transport press (owner ruling 2026-11-12; the one exception to the Left = back,
     * Right = Enter rule of design QA D-29, kept here and in the transport module). Shift+Left / Shift+Right seek 10 s
     * back / ahead (each press and its auto-repeat). A release always reaches the module (Shift let go first, or the page
     * changed while the key was held). */
    void now_arrow(const key_item &it) {
        const bool right = it.key_code == K_RIGHT;
        if (it.key_state == 0) {
            transport_key(right, 0);
            return;
        }
        if (view->page() != Page::Now) return;
        if (it.mods & KBD_MOD_SHIFT) seek_by(right ? 10000 : -10000);
        else transport_key(right, it.key_state);
    }

    void on_keyboard(const key_item &it) {
        const MediaAction ma = media_action_for(it.key_code);
        if (ma != MediaAction::None) {
            MediaEvent me;
            if (keys.accept_other(ma, it.key_state, me)) media(me);
            return;
        }
        if (it.key_state == 1) swallow_echo = 0;
        if (view->dialog_shown()) return; /* the dialog takes its keys through the keypad (on_key) */
        if (it.key_code == K_LEFT || it.key_code == K_RIGHT) {
            now_arrow(it); /* the other pages take the arrows from the LVGL keypad (on_key) */
            return;
        }
        if (it.key_code == K_COMMA || it.key_code == K_DOT) { /* previous / next on every page: the transport press */
            transport_key(it.key_code == K_DOT, it.key_state);
            return;
        }
        if (it.key_state != 1) return;
        const Page p = view->page();
        switch (it.key_code) {
        case K_SPACE: toggle(); break;
        case K_S: shuffle(); break;
        case K_R: repeat(); break;
        case K_Q:
            if (p != Page::Queue) button("queue");
            break;
        case K_O:
            if (p != Page::Output) button("output");
            break;
        case K_B:
            if (p != Page::Browser) go(Page::Browser);
            break;
        case K_U:
            if (p == Page::Queue) queue_button(0);
            break;
        case K_D:
            if (p == Page::Queue) queue_button(1);
            break;
        case K_X:
            if (p == Page::Queue) {
                swallow_echo = arrow_echo(it);
                queue_button(2);
            }
            break;
        case K_C:
            if (p == Page::Queue) {
                swallow_echo = arrow_echo(it);
                queue_button(3);
            }
            break;
        case K_MINUS: volume_step(-kVolumeStep); break; /* the playing output's volume (D11 as changed) */
        case K_EQUAL: volume_step(kVolumeStep); break;
        default: break;
        }
    }

    /* ------------------------------------------------------------------------------------------ volume (D11) */

    /* D11 as the owner changed it (2026-11-12): the volume group and the - / = keys set the volume of the output Vinyl
     * plays on, and the group shows its real level (core/volume/output_volume.hpp). */
    struct VolTarget {
        enum class Kind { None, Sink, Mixer } kind = Kind::None;
        std::string sink;      /* Sink: the sink's name */
        unsigned channels = 2; /* Sink: its channels */
        std::string card;      /* Mixer: "hw:CARD=<id>" or "default" */
        std::string key;       /* output_volume_key: a card without a mixer keeps its software level under it */
    };

    /* the sink of the server that a target plays to, "" when it is not there (a Bluetooth device switched off) */
    std::string sink_for_target() const {
        if (target.kind == OutputTarget::Kind::Auto) return world.default_sink;
        if (target.kind == OutputTarget::Kind::Pulse) return target.id;
        if (target.kind == OutputTarget::Kind::Bt)
            for (const PulseDevice &d : world.sinks) {
                std::string mac = pulse_mac_from_name(d.name);
                for (char &c : mac) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (!mac.empty() && mac == target.id && d.name.compare(0, 6, "bluez_") == 0) return d.name;
            }
        return "";
    }

    /* the output Vinyl plays on (or will play on: nothing is open while paused at start) */
    VolTarget volume_target() const {
        VolTarget t;
        std::string sink;
        if (!snap.output_name.empty() && snap.output_pulse) sink = snap.output_name;
        else if (snap.output_name.empty() && world.server && target.kind != OutputTarget::Kind::Alsa) sink = sink_for_target();
        if (!sink.empty()) {
            t.kind = VolTarget::Kind::Sink;
            t.sink = sink;
            for (const PulseDevice &d : world.sinks)
                if (d.name == sink && d.channels > 0) t.channels = d.channels;
            t.key = output_volume_key(sink, true, target);
            return t;
        }
        if (!snap.output_name.empty() && !snap.output_pulse) {
            t.kind = VolTarget::Kind::Mixer;
            t.card = mixer_card_for(snap.output_name);
        } else if (target.kind == OutputTarget::Kind::Alsa || (!world.server && target.kind == OutputTarget::Kind::Auto)) {
            t.kind = VolTarget::Kind::Mixer;
            t.card = mixer_card_for(target.kind == OutputTarget::Kind::Alsa ? target.id : std::string("default"));
        }
        t.key = output_volume_key(snap.output_name, snap.output_pulse, target);
        return t;
    }

    /* an ALSA output without a playback mixer (or no mixer could be read): Vinyl's software gain */
    bool software_volume(const VolTarget &t) const { return t.kind == VolTarget::Kind::Mixer && !mixer_level.has_mixer; }

    /* reads the playing card's mixer (at most every 300 ms, at once when forced) */
    void read_output_mixer(uint64_t now, bool force) {
        const VolTarget t = volume_target();
        if (t.kind != VolTarget::Kind::Mixer || !alsa) {
            mixer_level = MixerLevel();
            return;
        }
        if (!force && t.card == mixer_card && now - last_mixer_read < 300) return;
        last_mixer_read = now;
        if (t.card != mixer_card) mixer_choice_card.clear();
        mixer_card = t.card;
        read_mixer(*alsa, t.card, mixer_choice_card, mixer_choice, mixer_level);
    }

    /* the level the chip shows: the sink's own, the mixer's, or the software level; -1 = not known */
    int output_level(bool *muted = nullptr) const {
        if (muted) *muted = false;
        const VolTarget t = volume_target();
        if (t.kind == VolTarget::Kind::Sink) {
            int pct = -1;
            bool m = false;
            if (!sink_volume_of(world, t.sink, pct, m)) return -1;
            if (pending_level >= 0 && pending_sink == t.sink && mono_ms_now() < pending_until && pct != pending_level)
                return pending_level; /* asked for, the server's event not back yet */
            if (muted) *muted = m;
            return pct;
        }
        if (t.kind == VolTarget::Kind::Mixer) {
            if (software_volume(t)) return app_volume_of(resume, t.key);
            if (muted) *muted = mixer_level.muted;
            return mixer_level.pct;
        }
        return -1;
    }

    /* the group and the keys can change it */
    bool output_level_adjustable() const {
        const VolTarget t = volume_target();
        if (t.kind == VolTarget::Kind::Sink) return output_level() >= 0 && (demo || worker);
        return t.kind == VolTarget::Kind::Mixer;
    }

    /* the engine's gain: silent while a long press scans; the software level only for a card without a mixer; else
     * unity (the output's own volume does the work, Vinyl's stream stays at 100 %) */
    void apply_gain() {
        if (!engine) return;
        const VolTarget t = volume_target();
        engine->set_gain(scan_muted ? 0.0f : software_volume(t) ? volume_gain(app_volume_of(resume, t.key)) : 1.0f);
    }

    /* the group's and the keys' "-" / "+" (core/volume: sink_press, mixer_press), kVolumeStep at a time: while muted "-" does
     * nothing and "+" brings the sound back at its level; "-" at the card's floor mutes */
    void volume_step(int d) {
        const VolTarget t = volume_target();
        bool muted = false;
        const int cur = output_level(&muted);
        if (t.kind == VolTarget::Kind::Sink) {
            const VolumePress p = sink_press(cur, muted, d);
            if (p.kind != VolumePress::Kind::Set) return;
            if (demo) {
                for (PulseDevice &s2 : world.sinks)
                    if (s2.name == t.sink) {
                        s2.volume_pct = p.pct;
                        if (p.pct > 0) s2.muted = false;
                    }
            } else if (worker) {
                worker->request_sink_level(t.sink, t.channels, p.pct); /* above 0 % it also un-mutes */
                pending_level = p.pct;
                pending_sink = t.sink;
                pending_until = mono_ms_now() + 1500;
            }
            std::fprintf(stderr, "[vinyl] volume: %s set to %d %%%s\n", t.sink.c_str(), p.pct, muted ? " (un-muted)" : "");
        } else if (t.kind == VolTarget::Kind::Mixer) {
            if (software_volume(t)) {
                set_app_volume(resume, t.key, volume_after_step(app_volume_of(resume, t.key), d));
                dirty = true;
                apply_gain();
                std::fprintf(stderr, "[vinyl] volume: software %d %% for %s (no playback mixer)\n", app_volume_of(resume, t.key),
                             t.key.c_str());
            } else if (alsa) {
                const VolumePress p = mixer_press(mixer_choice, mixer_level, d);
                int r = 0;
                if (p.kind == VolumePress::Kind::Set) r = write_mixer(*alsa, t.card, mixer_choice, p.pct);
                else if (p.kind == VolumePress::Kind::Mute) r = write_mixer_switch(*alsa, t.card, mixer_choice, false);
                else if (p.kind == VolumePress::Kind::Unmute) r = write_mixer_switch(*alsa, t.card, mixer_choice, true);
                else return;
                if (r < 0) std::fprintf(stderr, "[vinyl] volume: %s %s: %s\n", t.card.c_str(), mixer_choice.volume.c_str(), alsa_error_text(r).c_str());
                if (p.kind == VolumePress::Kind::Set && r >= 0) { /* what was written and what the control holds now */
                    long back = 0;
                    const int rr = alsa->mixer_get(t.card, mixer_choice.volume, back); /* a fresh read of the control */
                    std::fprintf(stderr, "[vinyl] volume: %s\n",
                                 mixer_set_log_text(t.card, mixer_choice, p.pct, mixer_raw_for(mixer_choice, p.pct), rr, back).c_str());
                } else if (r >= 0)
                    std::fprintf(stderr, "[vinyl] volume: set mixer %s '%s' %s\n", t.card.c_str(), mixer_choice.sw.c_str(),
                                 p.kind == VolumePress::Kind::Mute ? "off (muted)" : "on (un-muted)");
                read_output_mixer(mono_ms_now(), true);
            }
        }
        refresh(true);
    }

    /* ------------------------------------------------------------------------------------------ notices */

    void notice(Notice::Kind k, const std::string &text) {
        Notice n;
        n.kind = k;
        n.text = text;
        n.gen = ++notice_gen;
        view->show_notice(n);
        std::fprintf(stderr, "[vinyl] notice: %s\n", text.c_str());
    }

    void demo_notice(const std::string &n) {
        if (n == "ffmpeg") notice(Notice::Kind::Info, "This format needs the FFmpeg libraries. Install them from Settings, Apps.");
        else if (n == "bt") notice(Notice::Kind::Bluetooth, "Bluetooth audio is not set up on this board.");
        else if (n == "call") notice(Notice::Kind::Call, "Call quality (16 kHz): the headset is in call mode.");
        else if (n == "lost") notice(Notice::Kind::Danger, "Evolve 65 disconnected. Paused.");
    }

    /* ------------------------------------------------------------------------------------------ outputs */

    /* false: a scan is already running (ask again later) */
    bool scan(bool connect) {
        if (!worker) return false;
        ScanRequest rq;
        rq.connect = connect;
        rq.app_name = "Vinyl";
        rq.connect_timeout_ms = 1500;
        rq.callback = sink_watch.callback(); /* the server's change events, once per connection (B9) */
        return worker->request_scan(rq);
    }

    void refresh_outputs(bool rescan) {
        if (!demo) {
            if (rescan) scan(true);
            if (!world.server && alsa) world.alsa_cards = alsa->cards();
            world.facts = status.facts;
        }
        out_rows = build_output_rows(world, target);
    }

    void take_scan() {
        if (!worker) return;
        ScanResult r;
        if (!worker->take_scan(r)) return;
        sink_watch.scanned(r);
        world.server = r.connected;
        world.sinks.clear();
        for (const PulseDevice &s : r.sinks)
            if (!s.is_monitor) world.sinks.push_back(s);
        world.cards = r.cards;
        if (r.have_info) world.default_sink = r.info.default_sink;
        for (const PulseDevice &s : world.sinks)
            if (!s.bt_address.empty() && !s.description.empty()) {
                std::string mac = s.bt_address;
                for (char &c : mac) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                world.bt_names[mac] = s.description;
            }
        if (!world.server && alsa) world.alsa_cards = alsa->cards();
        world.facts = status.facts;
        out_rows = build_output_rows(world, target);
        if (r.seq != scan_seq) {
            scan_seq = r.seq;
            if (view->page() == Page::Output) refresh(true);
        }
    }

    /* the output in use as the chip shows it, and its quality word */
    std::string output_quality() const {
        if (!snap.output_pulse) return "";
        for (const PulseDevice &s : world.sinks)
            if (s.name == snap.output_name) return bt_quality(s, world.cards);
        return "";
    }

    bool output_is_bt() const {
        if (snap.output_pulse && snap.output_name.compare(0, 12, "bluez_output") == 0) return true;
        if (snap.output_name.empty()) {
            if (target.kind == OutputTarget::Kind::Bt) return true;
            if (target.kind == OutputTarget::Kind::Auto && world.default_sink.compare(0, 12, "bluez_output") == 0) return true;
        }
        return false;
    }

    std::string output_label() const {
        if (!snap.output_label.empty()) return snap.output_label;
        if (target.kind == OutputTarget::Kind::Auto) {
            for (const PulseDevice &s : world.sinks)
                if (s.name == world.default_sink) return sink_label(s); /* the device's name, not the profile words */
            return world.server ? "" : "Default output";
        }
        for (const OutputRow &r : out_rows)
            if (r.target == target) return r.label;
        return "";
    }

    /* ------------------------------------------------------------------------------------------ the views */

    TrackTags tags_of(const std::string &path) {
        TrackTags t;
        if (!jobs.tags(path, t)) t = TrackTags();
        return t;
    }

    std::string vol_text() const {
        bool muted = false;
        const int v = output_level(&muted);
        return volume_chip_text(v, muted);
    }

    /* one log line when the level shown or the control changes (for the board probes: which volume is in force) */
    void log_volume() {
        const VolTarget t = volume_target();
        bool m = false;
        const int v = output_level(&m);
        std::string line;
        if (t.kind == VolTarget::Kind::Sink) line = "sink " + t.sink + " " + volume_log_text(v);
        else if (t.kind == VolTarget::Kind::Mixer && software_volume(t))
            line = "software " + volume_log_text(v) + " for " + t.key + " (" + t.card + ": no playback mixer)";
        else if (t.kind == VolTarget::Kind::Mixer) {
            char db[32] = "";
            if (mixer_choice.has_db) std::snprintf(db, sizeof db, ", %.2f dB", mixer_level.cdb / 100.0);
            line = "mixer " + t.card + " '" + mixer_choice.volume + "' " + volume_log_text(v) + (v >= 0 ? db : "") +
                   (v >= 0 ? ", raw=" + std::to_string(mixer_level.raw) : std::string());
        } else line = "no output";
        if (m && v >= 0) line += " (muted)";
        if (line == vol_logged) return;
        vol_logged = line;
        std::fprintf(stderr, "[vinyl] volume: %s\n", line.c_str());
    }

    /* "FLAC 44.1 kHz / 16 bit", short "FLAC 44.1 / 16"; with_khz (the 640x480 draft): "AAC 44.1 kHz / 161k" */
    std::string format_text(bool short_form, bool with_khz = false) const {
        const TrackInfo &i = snap.info;
        if (i.label.empty() || !i.out.rate) return "";
        std::string s = i.label + " " + khz(i.out.rate);
        if (!short_form || with_khz) s += " kHz";
        if (!i.lossy && i.source_bits) s += short_form ? " / " + std::to_string(i.source_bits) : " / " + std::to_string(i.source_bits) + " bit";
        else if (i.lossy) {
            unsigned kbps = i.bitrate_kbps;
            if (!kbps) {
                TrackTags t;
                if (jobs.tags(snap.path, t)) kbps = t.bitrate_kbps;
            }
            if (kbps) s += short_form ? " / " + std::to_string(kbps) + "k" : " / " + std::to_string(kbps) + " kbps";
        }
        return s;
    }

    uint64_t tag_duration_ms(const std::string &path) {
        TrackTags t;
        return jobs.tags(path, t) ? t.duration_ms : 0;
    }

    /* what the queue rows depend on: the tags read so far, the queue's order and contents, the playing track and (the
     * queue page's playing row) the second of the position */
    uint64_t rows_key(bool progress) const {
        uint64_t k = tags_seen * 0x9E3779B97F4A7C15ull;
        k ^= qgen * 0xC2B2AE3D27D4EB4Full;
        k ^= static_cast<uint64_t>(queue.current() + 1) * 0x165667B19E3779F9ull;
        if (progress) k ^= (snap.pos_ms() / 1000 + 1) * 0x27D4EB2F165667C5ull;
        return k;
    }

    RowView queue_row(int pos, bool show_progress) {
        RowView r;
        const std::string &path = queue.at(pos);
        TrackTags t;
        const bool known = jobs.tags(path, t);
        const bool playing = pos == queue.current();
        r.text = playing ? (known && !t.title.empty() ? t.title : title_from_path(path)) : artist_title(known ? t : TrackTags(), path);
        r.mark = RowView::Mark::Dot;
        r.playing = playing;
        const uint64_t d = known ? t.duration_ms : 0;
        if (playing && show_progress && d) r.value = mmss(snap.pos_ms()) + "/" + mmss(d);
        else r.value = d ? mmss(d) : "";
        return r;
    }

    NowView now_view() {
        NowView v;
        const bool sm = view->size_class() == layout::SizeClass::Small;
        const layout::NowGeom g = layout::now_geom(view->size_class());
        const int cur = queue.current();
        v.has_track = has_track() || cur >= 0;
        char cnt[32];
        if (queue.empty()) std::snprintf(cnt, sizeof cnt, "--/--");
        else std::snprintf(cnt, sizeof cnt, "%02d/%02d", cur + 1, queue.size());
        v.count = cnt;
        v.vol = vol_text();
        v.vol_bt = output_is_bt();
        bool vol_muted = false;
        v.vol_level = output_level(&vol_muted);
        v.vol_muted = vol_muted;
        v.vol_adjustable = output_level_adjustable();
        v.queue_empty = queue.empty();
        const std::string path = !snap.path.empty() ? snap.path : (queue.current_path() ? *queue.current_path() : "");
        if (!path.empty()) {
            TrackTags t;
            const bool known = jobs.tags(path, t);
            v.title = known && !t.title.empty() ? t.title : title_from_path(path);
            v.artist = known ? t.artist : "";
            std::string al = known ? t.album : "";
            const std::string y = known ? t.year() : "";
            if (!al.empty() && !y.empty()) al += ", " + y;
            else if (al.empty()) al = y;
            v.album = al;
        } else {
            /* on two lines: one line is too narrow for the hint in the square and large info column (design QA D-18) */
            v.title = queue.empty() ? "Nothing playing" : "";
            v.artist = queue.empty() ? "Pick a folder" : "";
            v.album = queue.empty() ? "or a playlist" : "";
        }
        v.format = format_text(!sm || g.short_format, g.format_khz);
        const std::string ol = output_label();
        const std::string q = output_quality();
        v.output = ol;
        if (g.quality_in_chip && !q.empty() && !ol.empty()) v.output += ", " + q;
        v.output_bt = output_is_bt();
        /* the Gapless chip of the square, large and medium drafts ("Gapless, RG -3.2 dB"), the end of the small draft's
         * format line ("AAC 44.1 / 161k, gapless"): when the next track continues on the same stream (ReplayGain is a P2
         * feature) */
        if (layout::shows_gapless(g)) {
            const int n = queue.next_after(cur, false);
            if (n >= 0 && n != cur && snap.info.out.rate) {
                TrackTags nt;
                if (jobs.tags(queue.at(n), nt) && nt.rate == snap.info.out.rate) v.extra_chip = "Gapless";
            }
            if (g.gapless_in_format && !v.extra_chip.empty()) {
                if (!v.format.empty()) v.format += ", gapless";
                v.extra_chip.clear();
            }
        }
        v.pos_ms = shown_pos_ms();
        v.total_ms = snap.total_ms() ? snap.total_ms() : tag_duration_ms(path);
        v.playing = snap.state == PlayState::Playing;
        v.shuffle = queue.shuffle();
        v.repeat = queue.repeat();
        const int n = queue.next_after(cur, true);
        if (n >= 0 && n != cur) v.upnext = artist_title(tags_of(queue.at(n)), queue.at(n));
        v.cover = cover;
        v.track_gen = track_gen;
        if (g.has_queue_pane) {
            char h[48];
            std::snprintf(h, sizeof h, "QUEUE, %d TRACK%s", queue.size(), queue.size() == 1 ? "" : "S");
            v.queue_heading = h;
            const uint64_t key = rows_key(false);
            if (key != pane_cache.key || !pane_cache.rows) {
                auto rows = std::make_shared<std::vector<RowView>>();
                rows->reserve(static_cast<size_t>(queue.size()));
                for (int i = 0; i < queue.size(); ++i) rows->push_back(queue_row(i, false));
                pane_cache.rows = rows;
                pane_cache.key = key;
            }
            v.queue_rows = pane_cache.rows;
            v.queue_ver = pane_cache.key;
            v.queue_current = cur;
            v.queue_selected = pane_sel >= 0 ? pane_sel : (cur >= 0 ? cur + 1 < queue.size() ? cur + 1 : cur : -1);
        }
        return v;
    }

    ListView browser_view() {
        ListView v;
        v.left = "Browse";
        v.vol = vol_text();
        v.vol_bt = output_is_bt();
        v.heading = folder.empty() ? "MUSIC" : breadcrumb(roots, folder);
        v.sort = "A-Z";
        const std::string *playing = queue.current_path();
        const uint64_t bkey = tags_seen * 0x9E3779B97F4A7C15ull ^ (rows_gen << 24) ^ std::hash<std::string>()(playing ? *playing : "");
        if (bkey == browser_cache.key && browser_cache.rows) {
            v.rows = browser_cache.rows;
            v.rows_ver = bkey;
        } else {
        auto out = std::make_shared<std::vector<RowView>>();
        for (const Entry &e : entries) {
            RowView r;
            switch (e.kind) {
            case Entry::Kind::Up:
            case Entry::Kind::Root:
            case Entry::Kind::Folder:
                r.text = e.name;
                r.mark = RowView::Mark::Chevron;
                break;
            case Entry::Kind::Playlist:
                r.text = e.name;
                r.mark = RowView::Mark::Chevron;
                break;
            case Entry::Kind::Track: {
                TrackTags t;
                const bool known = jobs.tags(e.path, t);
                r.text = known && !t.title.empty() ? t.title : title_from_path(e.path);
                r.value = known && t.duration_ms ? mmss(t.duration_ms) : "";
                r.mark = RowView::Mark::Dot;
                r.playing = playing && *playing == e.path;
                break;
            }
            }
            out->push_back(r);
        }
        if (out->empty()) { /* information rows (no mark); the first start's hint on two rows, whole at 320 px wide */
            for (const char *t : folder.empty() ? std::vector<const char *>{"No music folder", "Copy music to ~/Music"}
                                                : std::vector<const char *>{"No music in this folder"}) {
                RowView r;
                r.text = t;
                r.mark = RowView::Mark::None;
                r.dimmed = true;
                out->push_back(r);
            }
        }
        browser_cache.rows = out;
        browser_cache.key = bkey;
        v.rows = out;
        v.rows_ver = bkey;
        }
        v.selected = br_sel;
        v.rows_gen = rows_gen;
        v.scroll_to = br_scroll;
        const std::string path = !snap.path.empty() ? snap.path : (playing ? *playing : "");
        v.mini.shown = !path.empty();
        if (v.mini.shown) {
            TrackTags t;
            const bool known = jobs.tags(path, t);
            v.mini.title = known && !t.title.empty() ? t.title : title_from_path(path);
            const uint64_t total = snap.total_ms() ? snap.total_ms() : (known ? t.duration_ms : 0);
            /* the artist (the view gives it the dots) and the times, whole, h:mm:ss for a track of an hour or more */
            v.mini.sub = known ? t.artist : "";
            const bool hours = (total ? total : shown_pos_ms()) >= 3600000;
            v.mini.time = layout::track_time_text(shown_pos_ms(), hours, true) + " / " +
                          (total ? layout::track_time_text(total, hours, true) : std::string("--:--"));
            v.mini.progress = total ? static_cast<double>(shown_pos_ms()) / total : 0;
            v.mini.playing = snap.state == PlayState::Playing;
        }
        return v;
    }

    ListView queue_view() {
        ListView v;
        v.left = "Queue";
        v.vol = vol_text();
        v.vol_bt = output_is_bt();
        const int cur = queue.current();
        uint64_t left_ms = 0;
        bool all_known = true;
        for (int i = std::max(0, cur); i < queue.size(); ++i) {
            const uint64_t d = tag_duration_ms(queue.at(i));
            if (!d) all_known = false;
            left_ms += (i == cur) ? (d > snap.pos_ms() ? d - snap.pos_ms() : 0) : d;
        }
        char h[96];
        const unsigned mins = static_cast<unsigned>((left_ms + 30000) / 60000); /* to the nearest minute */
        if (queue.empty()) std::snprintf(h, sizeof h, "EMPTY QUEUE");
        else if (mins >= 60) std::snprintf(h, sizeof h, "%d OF %d, %u H %u MIN LEFT%s", cur + 1, queue.size(), mins / 60, mins % 60, all_known ? "" : "+");
        else std::snprintf(h, sizeof h, "%d OF %d, %u MIN LEFT%s", cur + 1, queue.size(), mins, all_known ? "" : "+");
        v.heading = h;
        const uint64_t key = rows_key(true);
        if (key != queue_cache.key || !queue_cache.rows) {
            auto rows = std::make_shared<std::vector<RowView>>();
            rows->reserve(static_cast<size_t>(queue.size()));
            for (int i = 0; i < queue.size(); ++i) rows->push_back(queue_row(i, true));
            queue_cache.rows = rows;
            queue_cache.key = key;
        }
        v.rows = queue_cache.rows;
        v.rows_ver = key;
        v.selected = qu_sel;
        v.rows_gen = queue_set_gen + 1000000;
        v.scroll_to = qu_scroll;
        return v;
    }

    ListView output_view() {
        ListView v;
        v.left = "Output";
        v.show_vol = false;
        auto rows = std::make_shared<std::vector<RowView>>();
        for (const OutputRow &r : out_rows) {
            RowView rv;
            rv.text = r.label;
            rv.value = r.value;
            rv.mark = RowView::Mark::Dot; /* a tap chooses it: the action dot (blue: the chosen one), no chevron (D-45) */
            rv.playing = r.chosen;
            rv.dimmed = r.dimmed;
            rv.mono_value = false;
            rows->push_back(rv);
        }
        v.rows = rows;
        uint64_t okey = 1469598103934665603ull;
        for (const RowView &rv : *rows)
            okey = (okey ^ std::hash<std::string>()(rv.text + "\x1f" + rv.value + (rv.playing ? "p" : "") + (rv.dimmed ? "d" : ""))) * 1099511628211ull;
        v.rows_ver = okey;
        v.selected = out_sel;
        v.rows_gen = out_rows.size() + 2000000;
        if (bluetooth_not_set_up(world)) {
            v.hint = "Bluetooth audio is not set up on this board.";
            v.hint_notice = true;
        } else {
            v.hint = "Pairing is in Settings. Vinyl never switches output by itself while a track plays.";
        }
        return v;
    }

    void refresh(bool force) {
        if (cdk::hidden()) return;
        const Page p = view->page();
        (void)force;
        if (p == Page::Now) view->update_now(now_view());
        else if (p == Page::Browser) {
            view->update_list(p, browser_view());
            br_scroll = -1;
        } else if (p == Page::Queue) {
            view->update_list(p, queue_view());
            qu_scroll = -1;
        } else if (p == Page::Output) view->update_list(p, output_view());
    }

    /* ------------------------------------------------------------------------------------------ the 100 ms tick */

    static void timer_cb(lv_timer_t *t) { static_cast<Impl *>(lv_timer_get_user_data(t))->tick(); }

    void tick() {
        const uint64_t now = mono_ms_now();
        /* engine events */
        EngineEvent ev;
        while (engine->poll_event(ev)) on_engine(ev);
        snap = engine->snapshot();
        /* media keys from the headset and keyboard nodes */
        for (const MediaEvent &e : keys.take()) media(e);
        /* a held previous / next becomes a scan, and the scan moves */
        transport.tick(now);
        /* the launcher's files, the sound server's lists */
        if (launcher && launcher->poll(now)) {
            status = launcher->status();
            world.facts = status.facts;
            out_rows = build_output_rows(world, target);
        }
        take_scan();
        /* a sink changed (a volume key, a headset's buttons, a reconnect): look again now, not at the next periodic
         * scan; a scan already running keeps the change for the next tick */
        if (worker && sink_watch.take_change()) {
            if (scan(true)) last_scan = now;
            else sink_watch.keep_change();
        }
        if (worker && now - last_scan > (view->page() == Page::Output ? 2000u : 5000u)) {
            last_scan = now;
            scan(true);
        }
        /* the playing output's volume: the card's mixer without a server (the server's sinks come with the scans) */
        read_output_mixer(now, false);
        apply_gain();
        log_volume();
        /* cover */
        TagJobs::CoverResult cr;
        if (jobs.take_cover(cr) && cr.path == cover_for) {
            cover = cr.image;
            refresh(true);
        }
        /* call quality while playing on a headset in call mode (D8: keep playing, say so) */
        if (snap.state == PlayState::Playing && output_quality() == "call quality" && call_noticed != snap.output_name) {
            call_noticed = snap.output_name;
            notice(Notice::Kind::Call, "Call quality (16 kHz): the headset is in call mode.");
        }
        /* dim allow while playing in front on a battery board (D16) */
        const bool hidden = cdk::hidden();
        if (was_hidden && !hidden) dim_sent = false; /* the launcher took the panel back meanwhile */
        if (!was_hidden && hidden) { /* a key held while the launcher takes the keyboards back never sends its release */
            transport.cancel(TransportKey::Next, now);
            transport.cancel(TransportKey::Prev, now);
        }
        was_hidden = hidden;
        /* only where the launcher answers dim on this board (board.state requests: a battery board with backlight levels;
         * APP-CONTRACT 5c and 14, design QA D-72) */
        if (requests && status.has_request("dim")) {
            if (snap.state == PlayState::Playing) {
                paused_since = 0;
                if (!hidden && !dim_sent) {
                    requests->send("dim allow");
                    dim_sent = true;
                }
            } else {
                if (!paused_since) paused_since = now;
                if (dim_sent && now - paused_since > 60000) {
                    requests->send("dim deny");
                    dim_sent = false;
                }
            }
        }
        /* save as it goes: the launcher may close a hidden app */
        if (snap.pos >= 0) {
            resume.position_ms = snap.pos_ms();
            if (snap.state == PlayState::Playing && now - last_save > 5000) dirty = true;
        }
        save(false);
        /* tags arrived: the rows show titles and durations */
        const uint64_t tg = jobs.gen();
        const bool tags_changed = tg != tags_seen;
        tags_seen = tg;
        if (!hidden) {
            view->tick(snap.state == PlayState::Playing);
            refresh(tags_changed);
        }
    }

    void on_engine(const EngineEvent &ev) {
        switch (ev.kind) {
        case EngineEvent::Kind::Started:
            if (ev.pos >= 0 && ev.pos < queue.size() && queue.at(ev.pos) == ev.path) queue.set_current(ev.pos);
            ++qgen;
            ++track_gen;
            cover.reset();
            cover_for = ev.path;
            jobs.want_cover(ev.path, cover_side());
            jobs.want_tags({ev.path});
            pane_sel = -1;
            update_next();
            dirty = true;
            break;
        case EngineEvent::Kind::Advanced: {
            const int n = queue.next_after(ev.pos, false);
            engine->set_next(n, n >= 0 ? queue.at(n) : std::string());
            if (n >= 0) jobs.want_tags({queue.at(n)});
            break;
        }
        case EngineEvent::Kind::QueueEnd: dirty = true; break;
        case EngineEvent::Kind::OpenFailed:
            notice(Notice::Kind::Info, ev.text);
            if (ev.pos >= 0 && ev.pos < queue.size()) queue.set_current(ev.pos);
            ++qgen;
            if (ev.automatic && ++auto_skips < 10) {
                const int n = queue.next_after(ev.pos, true);
                if (n >= 0 && n != ev.pos) {
                    queue.set_current(n);
                    ++qgen;
                    engine->load(n, queue.at(n), 0, true);
                }
            }
            dirty = true;
            break;
        case EngineEvent::Kind::OutputLost:
            notice(Notice::Kind::Danger, (ev.text.empty() ? std::string("The output") : ev.text) + " disconnected. Paused.");
            dirty = true;
            break;
        case EngineEvent::Kind::OutputFailed: {
            std::string what = output_label();
            if (what.empty()) what = "The output";
            notice(Notice::Kind::Info, what + " cannot play: " + ev.text + ". Choose another in Output.");
            break;
        }
        }
    }

    int cover_side() const { return std::max(16, view->cover_side()); }

    void save(bool force) {
        if (!dirty && !force) return;
        resume_from_queue(queue, resume);
        resume.output = target.key();
        const int r = save_resume(resume_file, resume);
        if (r < 0 && force) std::fprintf(stderr, "[vinyl] cannot save %s: %s\n", resume_file.c_str(), std::strerror(-r));
        dirty = false;
        last_save = mono_ms_now();
    }
};

VinylApp::VinylApp(lv_obj_t *screen) : d_(std::make_unique<Impl>()) { d_->start(screen); }

VinylApp::~VinylApp() {
    if (d_->screen && LV_EVENT_KEYBOARD != 0) lv_obj_remove_event_cb_with_user_data(d_->screen, Impl::keyboard_cb, d_.get());
    d_->stop_all();
    release_fonts();
}

} // namespace ui
} // namespace vinyl
