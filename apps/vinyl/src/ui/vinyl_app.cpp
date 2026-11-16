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
#include "drives/drives.hpp"
#include "demo/demo_world.hpp"
#include "engine/engine.hpp"
#include "engine/outputs.hpp"
#include "input/media_keys.hpp"
#include "nav/key_echo.hpp"
#include "nav/nav_rules.hpp"
#include "launcher/launcher_link.hpp"
#include "library/browse.hpp"
#include "library/library_ctl.hpp"
#include "library/media_guard.hpp"
#include "library/lib_model.hpp"
#include "library/roots.hpp"
#include "library/schema.hpp"
#include "library/tag_jobs.hpp"
#include "menu/menu_model.hpp"
#include "pulse/pulse_api.hpp"
#include "screen/crash_guard.hpp"
#include "screen/knock.hpp"
#include "settings/settings.hpp"
#include "sleep/sleep_timer.hpp"
#include "state/positions.hpp"
#include "state/resume.hpp"
#include "state/write_policy.hpp"
#include "transport/transport_press.hpp"
#include "util/atomic_file.hpp"
#include "volume/hold_repeat.hpp"
#include "volume/output_volume.hpp"

#include "app.hpp"
#include "build_info.hpp"
#include "keyboard_input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unistd.h>

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

/* evdev codes of the keys used (the physical key, not its text: the guidelines' rule); the letters' commands are
 * core/menu shortcut_command, the table the Keys page shows (menu::key_help) */
enum : uint32_t {
    K_Y = 21, K_COMMA = 51, K_DOT = 52, K_LEFT = 105, K_RIGHT = 106,
};

bool is_playlist_source(const std::string &src) {
    std::string e;
    const size_t dot = src.rfind('.');
    if (dot != std::string::npos) e = src.substr(dot + 1);
    for (char &c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == "m3u" || e == "m3u8";
}

std::string parent_dir(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

/* Knock-knock: the launcher's screen request, given by the device main (cp0_app_backlight); null in the headless harness */
int (*g_backlight_call)(int) = nullptr;

const char *call_code_word(int code) { return code == 0 ? "done" : code > 0 ? "refused" : "no service"; }

}  // namespace

void set_backlight_call(int (*call)(int on)) { g_backlight_call = call; }

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
    /* P2b: the Library (its database, its worker and scanner; null without it: Browse is the folder list as in 0.2.0) */
    std::unique_ptr<LibraryCtl> lib;
    bool browser_from_lib = false; /* the Browser came from the hub's Folders row */
    bool choose_mode = false;      /* ... in choose mode (Music folders > Add a folder) */
    bool pending_escape = false;   /* the search field's Esc, run at the next tick (outside the key event) */
    std::string play_path;         /* the track heard: its play count at its end */
    uint64_t gone_checked = 0;
    std::vector<DriveBase> drive_bases = default_drive_bases(); /* where the queue's files may be on a drive */
    FakeDrives *demo_fake = nullptr;                            /* the scenes: the drive that is "pulled" */
    uint64_t demo_pull_at = 0;
    uint64_t demo_next_at = 0; /* the scenes: the next track starts by itself after a while (VINYL_DEMO_NEXT_AFTER ms) */
    bool lib_on() const { return lib && lib->available(); }
    /* "Mount on request in Vinyl" (owner, 2026-11-14): the removable drives, mounted by udisks off the UI thread */
    std::unique_ptr<DriveManager> drives;
    std::string demo_drive_dir; /* the scenes: a drive Vinyl mounted (VINYL_DEMO_DRIVES=vinyl) */
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
    bool dirty = false; /* an event state.json must hold (state_policy: written at the next tick) */
    WritePolicy state_policy; /* report 060 finding 3: events at once, the position at most once a minute */
    /* P2a: the settings (their own file, written on a change), the positions in long files (their own small file), the
     * sleep timer (not saved), the Menu pages */
    SettingsStore settings{posix_file_ops()};
    Positions positions;
    std::string positions_file;
    WritePolicy pos_policy;
    std::string long_path; /* the long file playing (its position noted every tick) */
    uint64_t long_pos = 0, long_total = 0;
    PlayState last_state = PlayState::Idle;
    SleepTimer sleep;
    bool sleep_armed = false; /* a track mode: nothing named next, the engine fades the track's end */
    bool sleep_silent = false; /* a minute mode ended: silent until the engine has paused */
    std::string about_shown;   /* the About page's THIS OUTPUT line as last drawn */
    std::map<Page, Page> back_to; /* where Esc goes from a Menu page (and the Output page opened from the Menu) */
    std::map<Page, int> menu_sel;
    uint64_t menu_gen = 1;
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
    /* the playing output's volume (D11 as changed 2026-11-12), in its own levels (core/volume, the owner's decision of
     * 2026-11-13): a sink level asked for and shown until the server's event brings it (1.5 s at most), the card mixer
     * Vinyl reads when there is no server, the model of the sink (its card's range) found once per sink */
    int pending_level = -1;
    std::string pending_sink;
    uint64_t pending_until = 0;
    std::string mixer_card, mixer_choice_card;
    MixerChoice mixer_choice;
    MixerLevel mixer_level;
    uint64_t last_mixer_read = 0;
    mutable std::string model_sink;
    mutable int model_card = -2;
    mutable OutputVolumeModel model_of_sink;
    mutable double model_fine = -1; /* the "Volume steps" step the cached sink model was made for */
    int demo_boost = -1;            /* the scenes: a gain boost for the output in use (VINYL_DEMO_BOOST) */
    HoldRepeat vol_hold;            /* the volume group's held "-" / "+" */
    /* the "Volume steps" step in dB (0: Normal) */
    double fine_db() const { return volume_steps_db(settings.get().volume_steps); }
    /* Knock-knock (2026-11-16): the gate and the tap runs, the launcher's screen request on its worker (null: not offered;
     * the scenes' fake with VINYL_DEMO_SCREEN), whether an `off` was ever asked (the exit then sends a last `on`) */
    Knock knock;
    std::unique_ptr<BacklightWorker> backlight;
    bool knock_ever_off = false;
    int touch_slots = -1; /* screen.state's touch_slots, read at start (0: no touch screen, the setting not offered; -1 unknown) */

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
                                        : env_on("VINYL_DEMO_BIGQUEUE") ? "bigqueue"
                                                                    : "";
            make_demo_world(demo_world, lite, env_str("VINYL_DEMO_STATE"), variant);
            if (demo_world.empty) roots.clear();
            else roots = {Root{"Music", demo_world.music}};
            resume_file = demo_world.config + "/vinyl/state.json";
            sinks = make_demo_sink_provider(variant, lite);
            world = demo_output_world(lite, variant);
            status = demo_launcher_status();
            if (env_on("VINYL_DEMO_MUTED")) /* the output muted (the launcher's key, another app, pactl): muted everywhere */
                for (PulseDevice &d : world.sinks) d.muted = true;
            if (env_on("VINYL_DEMO_NOVOLUME")) { /* the server reports no level for its sinks: "--", nothing to change */
                status.volume = -1;
                for (PulseDevice &d : world.sinks) {
                    d.volume_pct = -1;
                    d.volume_raw = -1;
                }
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
            if (g_backlight_call) { /* Knock-knock: offered where board.state's requests has `screen` (the tick looks) */
                int (*call)(int) = g_backlight_call;
                backlight = std::make_unique<BacklightWorker>([call](int on) { return call(on); });
                crash_guard_install(launcher_socket_path());
            }
            /* ... and where the board has a touch screen (screen.state touch_slots, APP-CONTRACT section 4) */
            std::ifstream ss(launcher_state_path("screen.state", env_str("APPLAUNCH_STATE_DIR"), env_str("XDG_RUNTIME_DIR")));
            std::stringstream text;
            text << ss.rdbuf();
            touch_slots = screen_touch_slots(text.str());
        }
        if (demo && env_on("VINYL_DEMO_NOTOUCH")) touch_slots = 0; /* the scenes: a board without a touch screen */
        if (demo && env_on("VINYL_DEMO_SCREEN")) { /* the scenes: a launcher that answers `screen`, a fake backlight */
            const bool refuse = env_str("VINYL_DEMO_SCREEN") == "fail";
            status.requests = "hello,caps,toast,beep,volume,brightness,screen";
            backlight = std::make_unique<BacklightWorker>([refuse](int on) {
                std::fprintf(stderr, "[vinyl] demo backlight: screen %s%s\n", on ? "on" : "off", refuse ? " (refused)" : "");
                return refuse ? 1 : 0;
            });
        }
        std::fprintf(stderr, "[vinyl] %s, %d root(s), state %s\n", demo ? "demonstration world" : "started", static_cast<int>(roots.size()),
                     resume_file.c_str());
        engine = std::make_unique<Engine>(real_decode_libs(), *sinks);
        /* P2a: the settings (their own file), the positions in long files */
        const std::string cfg = demo ? demo_world.config + "/vinyl" : dir_name(resume_file);
        settings.load(cfg + "/settings.json");
        if (demo) { /* the scenes: the settings shown */
            Settings ds = settings.get();
            if (const std::string rg = env_str("VINYL_DEMO_RG"); !rg.empty()) ds.replaygain = replaygain_mode_from(rg, ds.replaygain);
            if (env_on("VINYL_DEMO_NOGAPLESS")) ds.gapless = false;
            if (const std::string vs = env_str("VINYL_DEMO_VOLSTEPS"); !vs.empty()) ds.volume_steps = volume_steps_from(vs, ds.volume_steps);
            if (const std::string bs = env_str("VINYL_DEMO_BOOST"); !bs.empty()) demo_boost = std::atoi(bs.c_str());
            if (env_on("VINYL_DEMO_NOREMEMBER")) ds.remember_long = false;
            if (env_on("VINYL_DEMO_KNOCK")) ds.knock = true;
            settings.set(ds);
        }
        engine->set_gapless(settings.get().gapless);
        engine->set_replaygain(settings.get().replaygain);
        knock.set_enabled(settings.get().knock);
        positions_file = cfg + "/positions.json";
        load_positions(posix_file_ops(), positions_file, positions);
        std::fprintf(stderr, "[vinyl] settings: gapless %s, replaygain %s, remember position in long files %s, knock-knock %s; %d position(s)\n",
                     settings.get().gapless ? "on" : "off", replaygain_mode_name(settings.get().replaygain),
                     settings.get().remember_long ? "on" : "off", settings.get().knock ? "on" : "off", static_cast<int>(positions.size()));

        Actions a;
        a.back = [this] { escape(); }; /* the header's Esc: the same as the Esc key (a modal takes it, D-30) */
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
        a.volume_hold = [this](int d, int phase) { /* the group's button as a held one: a press steps, a hold repeats */
            if (phase == 0) {
                volume_step(d);
                vol_hold.start(d, mono_ms_now());
            } else vol_hold.stop();
        };
        a.volume_unavailable = [this] { notice(Notice::Kind::Info, "Volume is not available on this board"); };
        a.lib_row = [this](Page p, int i) {
            if (lib) lib->row(p, i);
            refresh(true);
        };
        a.lib_hold = [this](Page p, int i) {
            if (p == Page::Browser) browser_hold(i);
            else if (lib) lib->hold(p, i);
            refresh(true);
        };
        a.lib_hold_ok = [this](Page p, int i) {
            return p == Page::Browser && lib_on() && !choose_mode && i >= 0 && i < static_cast<int>(entries.size()) &&
                   entries[static_cast<size_t>(i)].kind == Entry::Kind::Playlist;
        };
        a.name_answer = [this](bool ok, const std::string &t) {
            if (lib) lib->name_answer(ok, t);
            refresh(true);
        };
        a.confirm_answer = [this](bool yes) {
            if (lib) lib->confirm_answer(yes);
            refresh(true);
        };
        a.queue_save = [this] { save_queue(); };
        a.touch = [this](int phase, int x, int y, bool on_cover) { on_touch(phase, x, y, on_cover); };
        a.lib_chip = [this](Page p) {
            if (lib) lib->chip(p);
            refresh(true);
        };
        a.lib_button = [this](Page p, int i) { lib_button(p, i); };
        a.lib_query = [this](const std::string &q) {
            if (lib) lib->query_changed(q);
        };
        a.lib_submit = [this] {
            if (lib) lib->submit();
            refresh(true);
        };
        a.lib_field_escape = [this] { pending_escape = true; };
        a.sheet_pick = [this](int i) {
            if (lib) lib->sheet_pick(i);
            refresh(true);
        };
        a.sheet_dismiss = [this] {
            if (lib) lib->close_sheet();
        };
        view = std::make_unique<View>(screen, a);
        knock.set_slop(KnockRules::slop_for_target(view->touch_target()));
        make_drives();
        make_library();
        const layout::R cr = view->cover_rect();
        std::fprintf(stderr, "[vinyl] canvas %dx%d, class %s, cover square %d,%d %dx%d, touch slots %d\n", cdk::screen_w(),
                     cdk::screen_h(), layout::class_name(view->size_class()), static_cast<int>(cr.x), static_cast<int>(cr.y),
                     static_cast<int>(cr.w), static_cast<int>(cr.h), touch_slots);

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
        if (demo && lite && env_on("VINYL_DEMO_MUTED")) { /* a board without a server: Vinyl's own mute, 60 % kept */
            const std::string key = volume_target().key;
            set_app_volume(resume, key, 60);
            set_app_muted(resume, key, true);
        }
        if (demo && demo_boost >= 0) {
            Settings ds = settings.get();
            ds.set_boost(volume_target().key, demo_boost);
            settings.set(ds);
        }
        apply_gain();
        Page first = lib_on() ? Page::LibHub : Page::Browser;
        if (!resume.items.empty()) {
            queue_from_resume(resume, queue);
            drop_gone_media(true); /* a drive that is not there at start: its entries go before anything is drawn (B50) */
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
            else if (p == "library") first = lib_on() ? Page::LibHub : Page::Browser;
            else if (p == "queue") first = Page::Queue;
            else if (p == "output") first = Page::Output;
            else if (p == "notices") first = Page::Notices;
            else if (p == "menu") first = Page::Menu;
            else if (p == "sleep") first = Page::Sleep;
            else if (p == "playback") first = Page::Playback;
            else if (p == "replaygain") first = Page::ReplayGain;
            else if (p == "boost") first = Page::Boost;
            else if (p == "volumesteps") first = Page::VolumeSteps;
            else if (p == "keys") first = Page::Keys;
            else if (p == "about") first = Page::About;
            else first = Page::Now;
        }
        back_from_queue = back_from_output = Page::Now;
        view->set_keys(menu::key_help());
        if (const std::string sl = env_str("VINYL_DEMO_SLEEP"); demo && !sl.empty()) { /* the scenes: a running timer */
            SleepMode m = SleepMode::Off;
            if (menu::sleep_from_id(sl, m)) {
                const uint64_t now = mono_ms_now();
                sleep.set(m, now);
                if (const std::string left = env_str("VINYL_DEMO_SLEEP_LEFT"); !left.empty() && sleep.timed()) {
                    /* "Stops in 29:12": the timer set as if chosen earlier */
                    const uint64_t want = static_cast<uint64_t>(std::atoll(left.c_str())) * 1000;
                    const uint64_t full = sleep.remaining_ms(now);
                    if (want < full) sleep.set(m, now - (full - want));
                }
            }
        }
        if (first == Page::Sleep || first == Page::Playback || first == Page::Keys || first == Page::About)
            back_to[first] = Page::Menu;
        if (first == Page::ReplayGain || first == Page::Boost || first == Page::VolumeSteps) { /* the scenes: as if opened from Playback, itself from the Menu */
            back_to[first] = Page::Playback;
            back_to[Page::Playback] = Page::Menu;
        }
        if (first == Page::Menu || back_to.count(first)) back_to[Page::Menu] = Page::Now;
        if (first == Page::Queue || first == Page::Output || first == Page::Menu || first == Page::About) refresh_outputs(true);
        if (first == Page::LibHub && lib_on()) lib->enter();
        else go(first);
        if (lib_on() && !demo) lib->start_launch_scan(); /* D27: the scan at app launch */
        state_policy.wrote(mono_ms_now()); /* what is on disk is what was read */
        pos_policy.wrote(mono_ms_now());
        if (worker) scan(true);
        timer = lv_timer_create(timer_cb, 100, this);
        tick();
        if (const std::string f = env_str("VINYL_DEMO_SCRUB"); demo && !f.empty()) /* the scenes: a finger held on the bar */
            view->simulate_scrub(std::atof(f.c_str()));
        if (const std::string n = env_str("VINYL_DEMO_NOTICE"); demo && !n.empty()) demo_notice(n);
        if (const std::string na = env_str("VINYL_DEMO_NEXT_AFTER"); demo && !na.empty()) demo_next_at = mono_ms_now() + static_cast<uint64_t>(std::atoll(na.c_str()));
    }

    void stop_all() {
        if (timer) lv_timer_delete(timer);
        timer = nullptr;
        knock_exit(); /* the light back first (Esc at the root, the launcher's close, SIGTERM from the 3 s Esc, SIGINT, SIGHUP) */
        note_long_file(true);
        save(true);
        save_positions_now(true);
        keys.stop();
        if (engine) engine->shutdown();
        if (drives) drives->shutdown(); /* what Vinyl mounted, Vinyl unmounts; never what the system mounted */
        lib.reset();
        jobs.stop();
        if (worker) worker->stop();
        if (pulse) pulse->disconnect();
        view.reset();
        if (demo) remove_demo_world(demo_world);
    }

    /* ------------------------------------------------------------------------------------------ Knock-knock */

    /* what the gate asked for: the requests on the worker, the notice, the zone and the shield */
    void apply_knock(const ScreenGate::Out &o) {
        if (o.send_off && backlight) {
            backlight->request(false, o.tag);
            knock_ever_off = true;
            crash_guard_arm(true); /* stays armed: a `screen on` from a crashing Vinyl is harmless when the light is on */
            /* nothing held goes on in the dark: a scan of the previous / next key, the volume group's repeat */
            const uint64_t now = mono_ms_now();
            transport.cancel(TransportKey::Next, now);
            transport.cancel(TransportKey::Prev, now);
            vol_hold.stop();
            std::fprintf(stderr, "[vinyl] knock-knock: screen off asked\n");
        }
        if (o.send_on && backlight) {
            backlight->request(true);
            std::fprintf(stderr, "[vinyl] knock-knock: screen on asked\n");
        }
        if (o.failed) {
            notice(Notice::Kind::Info, kKnockUnavailable);
            std::fprintf(stderr, "[vinyl] knock-knock: the screen could not be switched off, it stays on\n");
        }
        sync_knock_view();
    }

    void sync_knock_view() {
        if (!view) return;
        view->set_knock_zone(knock.armable());
        view->set_input_shield(knock.input_blocked());
    }

    /* every touch of the screen (the view: presses and releases of the pointer, moves and lost presses on the zone and the
     * shield) */
    void on_touch(int phase, int x, int y, bool on_cover) {
        const bool was_blocked = knock.input_blocked();
        const ScreenGate::Out o = knock.touch(static_cast<TouchPhase>(std::clamp(phase, 0, 3)), x, y, mono_ms_now(),
                                              on_cover && view->page() == Page::Now);
        if (!o.send_off && !o.send_on && !o.failed) return;
        apply_knock(o);
        if (was_blocked && !knock.input_blocked()) {
            std::fprintf(stderr, "[vinyl] knock-knock: woken by three taps\n");
            refresh(true); /* nothing was drawn while it was off */
        }
    }

    /* the tick: the board's answer to `screen`, the worker's answers */
    void knock_tick() {
        apply_knock(knock.set_available(knock_available(backlight != nullptr, status.has_request("screen"), touch_slots)));
        BacklightWorker::Result r;
        while (backlight && backlight->take(r)) {
            std::fprintf(stderr, "[vinyl] knock-knock: screen %s: %s\n", r.on ? "on" : "off", call_code_word(r.code));
            apply_knock(knock.result(r.on, r.code, r.tag));
        }
    }

    /* the exit: the worker stopped, then `on` at once on this thread if Vinyl ever asked for `off` (one more `on` is
     * harmless: the launcher ignores an `on` without an `off`) */
    void knock_exit() {
        if (!backlight) return;
        const ScreenGate::Out o = knock.restore();
        backlight->stop();
        if (o.send_on || knock_ever_off) {
            const int r = backlight->call_now(true);
            std::fprintf(stderr, "[vinyl] knock-knock: screen on at exit: %s\n", call_code_word(r));
        }
        crash_guard_arm(false);
        backlight.reset();
    }

    /* ------------------------------------------------------------------------------------------ pages */

    void go(Page p) {
        if (lib) {
            if (lib->sheet_open()) lib->close_sheet();
            const bool lib_side = is_lib_page(p) || (p == Page::Browser && browser_from_lib);
            if (lib->in_library() && !lib_side) lib->left(); /* D27: leaving the Library stops the scan */
            else if (!lib->in_library() && is_lib_page(p)) lib->reenter();
        }
        if (p != Page::Browser) choose_mode = false;
        if (p != Page::Browser && !is_lib_page(p)) browser_from_lib = false;
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
        if (is_menu_list(p)) { /* the selection where the page shows the chosen value, else its first row */
            menu_sel[p] = menu::initial_item(menu::rows_of(menu_page(p), menu_state()));
            ++menu_gen;
        }
        if (p == Page::About) view->set_about(about_now());
        view->show_page(p);
        refresh(true);
    }

    static bool is_menu_list(Page p) {
        return p == Page::Menu || p == Page::Sleep || p == Page::Playback || p == Page::ReplayGain || p == Page::Boost ||
               p == Page::VolumeSteps;
    }
    static bool is_menu_page(Page p) { return is_menu_list(p) || p == Page::Keys || p == Page::About; }
    static menu::Page menu_page(Page p) {
        switch (p) {
        case Page::Sleep: return menu::Page::Sleep;
        case Page::Playback: return menu::Page::Playback;
        case Page::ReplayGain: return menu::Page::ReplayGain;
        case Page::Boost: return menu::Page::Boost;
        case Page::VolumeSteps: return menu::Page::VolumeSteps;
        case Page::Keys: return menu::Page::Keys;
        case Page::About: return menu::Page::About;
        default: return menu::Page::Menu;
        }
    }

    /* a Menu page opened from `from` (Esc goes back there) */
    void open_from(Page p, Page from) {
        if (p != from) back_to[p] = from;
        go(p);
    }

    void back() {
        const Page p = view->page();
        switch (p) {
        case Page::Now: open_browse(); break;
        case Page::Queue: go(back_from_queue); break;
        case Page::Output: go(back_from_output); break;
        case Page::Notices: go(Page::Now); break;
        case Page::Browser:
            if (lib_on() && lib->sheet_open()) { /* the import sheet */
                lib->close_sheet();
                break;
            }
            if (choose_mode && lib_on()) { /* back to the Music folders page (or the playlist an export was asked from) */
                choose_mode = false;
                lib->choose_cancelled();
                break;
            }
            if (browser_from_lib && lib_on()) { /* back to the hub */
                browser_from_lib = false;
                lib->resume();
                break;
            }
            std::fprintf(stderr, "[vinyl] Esc at the browser: quit\n");
            cdk::request_quit(0);
            break;
        default: { /* the Menu pages: back where they were opened from (the Menu for its own rows) */
            if (is_lib_page(p) && lib_on()) { /* the Library: back one place; at its first place where it was opened from, or quit */
                if (lib->back()) break;
                auto lt = back_to.find(p);
                if (lt != back_to.end()) {
                    go(lt->second);
                    break;
                }
                std::fprintf(stderr, "[vinyl] Esc at the Library: quit\n");
                cdk::request_quit(0);
                break;
            }
            auto it = back_to.find(p);
            go(it != back_to.end() ? it->second : (p == Page::Menu ? Page::Now : Page::Menu));
            break;
        }
        }
    }

    void button(const std::string &id) {
        const Page p = view->page();
        if (id == "queue") {
            back_from_queue = p;
            go(Page::Queue);
        } else if (id == "output") {
            back_from_output = p;
            go(Page::Output);
        } else if (id == "browse") {
            open_browse();
        } else if (id == "menu") { /* the Menu key, M: Esc goes back to the page it was opened from */
            if (p == Page::Menu) return;
            if (!is_menu_page(p)) back_to[Page::Menu] = p;
            go(Page::Menu);
        } else if (id == "sleep") { /* the large class's sleep-timer key, T */
            if (p != Page::Sleep) open_from(Page::Sleep, p);
        }
    }

    /* ------------------------------------------------------------------------------------------ playback */

    void play_pos(int pos, bool play) {
        if (pos < 0 || pos >= queue.size()) return;
        queue.set_current(pos);
        ++qgen;
        auto_skips = 0;
        /* a file longer than 20 minutes starts where it was left (the Playback setting, On by default) */
        uint64_t start = 0;
        if (settings.get().remember_long) {
            note_long_file(true); /* the one playing now keeps its place first */
            start = positions.get(queue.at(pos));
            if (start) std::fprintf(stderr, "[vinyl] long file: starts at %s (remembered)\n", layout::track_time_text(start, true, false).c_str());
        }
        engine->load(pos, queue.at(pos), start, play);
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
        if (queue.empty()) return; /* drawn dimmed with nothing to play: it does nothing (design QA run 5, D-81) */
        queue.set_shuffle(!queue.shuffle());
        ++qgen;
        update_next();
        dirty = true;
        refresh(true);
    }

    void repeat() {
        if (queue.empty()) return; /* drawn dimmed with nothing to play: it does nothing (design QA run 5, D-81) */
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

    void update_next() { name_next(snap.pos >= 0 ? snap.pos : queue.current()); }

    /* names the track after `pos` to the engine; the sleep timer's track modes name none where playback must stop (the
     * end of this track; the next track of another album, or the queue's end) and let the engine fade that end */
    void name_next(int pos) {
        const int n = queue.next_after(pos, false);
        bool stop = false;
        if (sleep.active() && !sleep.timed() && pos >= 0 && pos < queue.size()) {
            const bool exists = n >= 0;
            const bool same = exists && same_album(queue.at(pos), queue.at(n));
            stop = sleep.stop_after_track(exists, same, is_playlist_source(queue.source()));
        }
        if (stop != sleep_armed) std::fprintf(stderr, "[vinyl] sleep timer: %s\n", stop ? "stops at the end of this track" : "not at this track's end");
        sleep_armed = stop;
        engine->set_end_fade(stop);
        if (stop) engine->set_next(-1, std::string());
        else engine->set_next(n, n >= 0 ? queue.at(n) : std::string());
    }

    /* ------------------------------------------------------------------------------------------ browser */

    void open_folder(const std::string &path, const std::string &select_path) {
        entries.clear();
        folder = path;
        if (path.empty()) {
            if (!demo) roots = system_roots(); /* a stick mounted or pulled since: the list is as the mounts are now */
            entries = root_entries(roots);
            add_drive_entries();
        } else {
            const bool top = roots.size() == 1 && roots.front().path == path && !drives_listed();
            const int r = list_folder(path, top, entries);
            if (r < 0) std::fprintf(stderr, "[vinyl] cannot list %s: %s\n", path.c_str(), std::strerror(-r));
        }
        br_sel = entries.empty() ? -1 : 0;
        while (br_sel >= 0 && br_sel < static_cast<int>(entries.size()) && entries[static_cast<size_t>(br_sel)].kind == Entry::Kind::Heading) ++br_sel;
        if (br_sel >= static_cast<int>(entries.size())) br_sel = -1;
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
        if (browser_from_lib && !choose_mode && lib_on() && !path.empty()) lib->browser_opened(path); /* D27 */
    }

    /* the root list's last rows: the removable drives that are plugged in and not mounted (a tap mounts them), and "mounted" on the
     * mounted ones' rows */
    void add_drive_entries() {
        if (!drives) return;
        const std::vector<DriveInfo> list = drives->list();
        for (Entry &e : entries)
            if (e.kind == Entry::Kind::Root)
                for (const DriveInfo &d : list)
                    if (d.mounted && d.mount_dir == e.path) e.extra = size_text(d.size) + ", mounted";
        bool heading = false;
        for (const DriveInfo &d : list) {
            if (d.mounted) continue;
            if (!heading) {
                heading = true;
                entries.push_back({Entry::Kind::Heading, "USB DRIVES", "", ""});
            }
            entries.push_back({Entry::Kind::Drive, d.name(), d.dev, "not mounted. Tap to mount"});
        }
    }

    /* a removable drive is plugged in: the list of roots is the top of the Browser (a drive that is not mounted is a row there) */
    bool drives_listed() const { return drives && !drives->list().empty(); }

    /* a tap on a drive's row in Folders, or the button Mount in Music folders: the same worker, the same notices */
    void request_mount(const std::string &dev, const std::string &name) {
        if (!drives || !drives->available()) {
            notice(Notice::Kind::Info, "Drives need the udisks2 package.");
            return;
        }
        if (drives->busy()) return;
        notice(Notice::Kind::Info, "Mounting " + name + "...");
        drives->mount_async(dev, name);
    }

    void browser_move(int d) {
        int n = br_sel + d;
        while (n >= 0 && n < static_cast<int>(entries.size()) && entries[static_cast<size_t>(n)].kind == Entry::Kind::Heading) n += d;
        if (n < 0 || n >= static_cast<int>(entries.size())) return;
        br_sel = n;
        refresh(true);
    }

    /* the answers of the drive thread: a mount opens the drive in the Browser and joins the Library, a failure says why */
    void drive_results() {
        DriveManager::Result r;
        while (drives && drives->take(r)) {
            switch (r.kind) {
            case DriveManager::Result::Kind::Mounted:
                std::fprintf(stderr, "[vinyl] drives: %s mounted at %s\n", r.dev.c_str(), r.mount_dir.c_str());
                if (demo && std::none_of(roots.begin(), roots.end(), [&](const Root &x) { return x.path == r.mount_dir; })) roots.push_back(Root{r.name, r.mount_dir});
                if (lib_on()) lib->drive_mounted(r.mount_dir);
                if (lib_on()) lib->mounts_changed(); /* Music folders: the row says "mounted by Vinyl" */
                if (view->page() == Page::LibFolders) {
                    refresh(true);
                } else if (view->page() == Page::Browser && !choose_mode) {
                    open_folder(r.mount_dir, "");
                    refresh(true);
                } else if (view->page() == Page::Browser) {
                    open_folder("", "");
                    refresh(true);
                }
                break;
            case DriveManager::Result::Kind::MountFailed:
                std::fprintf(stderr, "[vinyl] drives: cannot mount %s: %s\n", r.dev.c_str(), r.reason.c_str());
                notice(Notice::Kind::Info, "Could not mount it: " + r.reason + ".");
                break;
            case DriveManager::Result::Kind::Unmounted:
                std::fprintf(stderr, "[vinyl] drives: %s unmounted\n", r.dev.c_str());
                if (demo) roots.erase(std::remove_if(roots.begin(), roots.end(), [&](const Root &x) { return x.path == demo_drive_dir || x.label == r.name; }), roots.end());
                if (lib_on()) lib->mounts_changed();
                drop_gone_media(false); /* what was queued from it goes (B50) */
                if (view->page() == Page::Browser && !folder.empty() && !root_of(roots, folder)) open_folder("", ""); /* it was open: its place is gone */
                else if (view->page() == Page::Browser && folder.empty()) open_folder("", "");
                refresh(true);
                notice(Notice::Kind::Info, r.name + " ejected. You can unplug it.");
                break;
            case DriveManager::Result::Kind::UnmountFailed:
                std::fprintf(stderr, "[vinyl] drives: cannot unmount %s: %s\n", r.dev.c_str(), r.reason.c_str());
                notice(Notice::Kind::Info, "Could not eject it: " + r.reason + ".");
                break;
            }
        }
    }

    /* the drives: the real ones through udisks; the scenes' (VINYL_DEMO_DRIVES = unmounted | denied | nofs | nodisks | vinyl) */
    void make_drives() {
        if (!demo) {
            drives = std::make_unique<DriveManager>(make_udisks_backend());
            return;
        }
        const std::string mode = env_str("VINYL_DEMO_DRIVES");
        if (mode.empty()) return;
        auto fake = std::make_unique<FakeDrives>();
        fake->mount_base = demo_world.root + "/media";
        const std::string base = fake->mount_base;
        auto make_drive_dir = [](const std::string &dir) {
            make_dirs(posix_file_ops(), dir);
            write_demo_wav(dir + "/Flash Song.wav", 90, 44100, "Flash Song", "Flash Artist", "Flash Album", "2020", 1);
        };
        DriveInfo gen;
        gen.dev = "/dev/sdb1";
        gen.label = "Generic Flash Disk";
        gen.fstype = "vfat";
        gen.size = 15500000000ull;
        fake->devices.push_back(gen);
        if (mode == "nodisks") fake->has_udisks = false;
        fake->on_mount = [base, mode, make_drive_dir](const std::string &dev) -> MountReply {
            MountReply r;
            if (mode == "denied") r.reason = "this session is not allowed to mount drives";
            else if (mode == "nofs") r.reason = "the disk has no readable filesystem";
            else {
                r.ok = true;
                r.mount_dir = base + (dev == "/dev/sda1" ? "/SANDISK" : "/Generic Flash Disk");
                make_drive_dir(r.mount_dir);
            }
            return r;
        };
        if (mode == "vinyl") { /* SANDISK, mounted by Vinyl and a root of the library; the generic one plugged in, not mounted */
            DriveInfo sd;
            sd.dev = "/dev/sda1";
            sd.label = "SANDISK";
            sd.fstype = "vfat";
            sd.size = 31000000000ull;
            sd.mounted = true;
            sd.mount_dir = base + "/SANDISK";
            make_drive_dir(sd.mount_dir);
            fake->devices.insert(fake->devices.begin(), sd);
            demo_drive_dir = sd.mount_dir;
            roots.push_back(Root{"SANDISK", sd.mount_dir});
        }
        drive_bases.push_back(DriveBase{base + "/", 1});
        demo_fake = fake.get();
        /* the scenes' saved queues (VINYL_DEMO_DRIVE_QUEUE): "drive" = the three songs of SANDISK (playing), "gone" = two songs of a
         * drive that is not there */
        const std::string dq = env_str("VINYL_DEMO_DRIVE_QUEUE");
        if (!dq.empty()) {
            ResumeState rs;
            const std::string d = dq == "drive" ? base + "/SANDISK" : base + "/GONE";
            for (const char *n : {"Flash Song.wav", "Flash Song 2.wav", "Flash Song 3.wav"}) {
                if (dq == "drive") write_demo_wav(d + "/" + n, 120, 44100, std::string(n, std::strlen(n) - 4), "Flash Artist", "Flash Album", "2020", 1);
                rs.items.push_back(d + "/" + n);
            }
            for (size_t i = 0; i < rs.items.size(); ++i) rs.order.push_back(static_cast<int>(i));
            rs.current = 0;
            rs.output = "auto";
            rs.source = d;
            save_resume(resume_file, rs);
        }
        if (const std::string pa = env_str("VINYL_DEMO_PULL_AFTER"); !pa.empty()) demo_pull_at = mono_ms_now() + static_cast<uint64_t>(std::atoll(pa.c_str()));
        drives = std::make_unique<DriveManager>(std::move(fake));
        if (mode == "vinyl") drives->note_mounted("/dev/sda1");
    }

    void browser_act(int i) {
        if (i < 0 || i >= static_cast<int>(entries.size())) return;
        const Entry e = entries[static_cast<size_t>(i)];
        switch (e.kind) {
        case Entry::Kind::Heading: break;
        case Entry::Kind::Drive: /* a tap mounts it (no password): off the UI thread, then it opens */
            request_mount(e.path, e.name);
            break;
        case Entry::Kind::Up: {
            const std::string old = folder;
            std::string up = parent_folder(roots, folder);
            if (up.empty() && (roots.size() > 1 || drives_listed())) up = ""; /* the list with the drives */
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
            if (choose_mode) return;
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
            if (choose_mode) return;
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
        if (is_lib_page(p)) {
            if (lib) lib->row(p, i);
            refresh(true);
        } else if (p == Page::Browser) {
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
        } else if (is_menu_list(p)) {
            menu_sel[p] = i;
            menu_act(p, i);
        }
    }

    /* ------------------------------------------------------------------------------------------ the Menu pages (P2a) */

    menu::State menu_state() const {
        menu::State st;
        st.sleep = sleep.mode();
        st.sleep_left_ms = sleep.remaining_ms(mono_ms_now());
        st.output = output_label();
        st.output_key = volume_target().key;
        st.library = lib_on();
        st.knock = knock.gate().available(); /* the Knock-knock row where the launcher answers `screen` (and Vinyl can ask it) */
        st.settings = settings.get();
        return st;
    }

    /* a row of a Menu list page tapped, or Enter / Right on it */
    void menu_act(Page p, int i) {
        const std::vector<menu::Row> rows = menu::rows_of(menu_page(p), menu_state());
        if (i < 0 || i >= static_cast<int>(rows.size()) || rows[static_cast<size_t>(i)].kind == menu::Row::Kind::Heading) return;
        const menu::Row &r = rows[static_cast<size_t>(i)];
        if (p == Page::Menu) {
            if (r.id == "sleep") open_from(Page::Sleep, Page::Menu);
            else if (r.id == "playback") open_from(Page::Playback, Page::Menu);
            else if (r.id == "keys") open_from(Page::Keys, Page::Menu);
            else if (r.id == "about") open_from(Page::About, Page::Menu);
            else if (r.id == "library" && lib_on()) { /* the Library settings, first place of the Library: Esc goes back to the Menu */
                back_to[Page::LibSettings] = Page::Menu;
                lib->enter_settings();
            }
            else if (r.id == "output") button("output"); /* Esc goes back to the Menu */
            return;
        }
        if (p == Page::Sleep) { /* a choice closes the page (owner-approved, 2026-11-13): back where it was opened */
            SleepMode m = SleepMode::Off;
            if (menu::sleep_from_id(r.id, m)) set_sleep(m);
            back();
            return;
        }
        Settings st = settings.get();
        if (p == Page::Playback) {
            if (r.id == "gapless") st.gapless = !st.gapless; /* Enter or a tap toggles it */
            else if (r.id == "remember") st.remember_long = !st.remember_long;
            else if (r.id == "knock") st.knock = !st.knock;
            else if (r.id == "replaygain") {
                open_from(Page::ReplayGain, Page::Playback);
                return;
            } else if (r.id == "boost") {
                open_from(Page::Boost, Page::Playback);
                return;
            } else if (r.id == "volume_steps") {
                open_from(Page::VolumeSteps, Page::Playback);
                return;
            }
        } else if (p == Page::Boost) { /* a choice closes the pick list, as ReplayGain's does */
            st.set_boost(volume_target().key, std::atoi(r.id.c_str()));
            apply_settings(st);
            back();
            return;
        } else if (p == Page::VolumeSteps) {
            st.volume_steps = volume_steps_from(r.id, st.volume_steps);
            apply_settings(st);
            back();
            return;
        } else if (p == Page::ReplayGain) {
            st.replaygain = replaygain_mode_from(r.id, st.replaygain);
            apply_settings(st);
            back(); /* a choice closes the pick list (owner-approved, 2026-11-13): Playback shows the new value */
            return;
        }
        apply_settings(st);
        refresh(true);
    }

    void apply_settings(const Settings &st) {
        const Settings was = settings.get();
        const int r = settings.set(st);
        if (r == 1) return;
        if (r < 0) std::fprintf(stderr, "[vinyl] cannot save %s: %s\n", settings.path().c_str(), std::strerror(-r));
        engine->set_gapless(st.gapless);
        engine->set_replaygain(st.replaygain);
        if (was.volume_steps != st.volume_steps) { /* every output's levels again, the nearest dB kept (read back from the output) */
            mixer_choice_card.clear();
            mixer_card.clear();
            model_fine = -1;
            pending_level = -1;
            vol_logged.clear();
            read_output_mixer(mono_ms_now(), true);
            std::fprintf(stderr, "[vinyl] volume steps: %s\n", volume_steps_word(st.volume_steps));
        }
        apply_gain();
        if (was.remember_long && !st.remember_long) long_path.clear();
        apply_knock(knock.set_enabled(st.knock)); /* Off while the screen is off: the light back */
        std::fprintf(stderr, "[vinyl] settings: gapless %s, replaygain %s, gain boost %s (%s), volume steps %s, remember position in long files %s, knock-knock %s\n",
                     st.gapless ? "on" : "off", replaygain_mode_name(st.replaygain), boost_word(st.boost_of(volume_target().key)).c_str(),
                     volume_target().key.c_str(), volume_steps_word(st.volume_steps), st.remember_long ? "on" : "off", st.knock ? "on" : "off");
    }

    /* The sleep timer (core/sleep): the minute modes count on the monotonic clock and fade the gain over their last 10 s
     * (apply_gain); the track modes name no next track at the end of what plays and let the engine fade it (name_next) */
    void set_sleep(SleepMode m) {
        sleep.set(m, mono_ms_now());
        std::fprintf(stderr, "[vinyl] sleep timer: %s\n", sleep_mode_word(m));
        update_next();
        apply_gain();
    }

    /* the timer ended: the music paused, nothing else (the queue kept; a track mode moved to the next track, paused) */
    void sleep_ended() {
        sleep.finish();
        sleep_armed = false;
        engine->set_end_fade(false);
        apply_gain();
        notice(Notice::Kind::Info, kSleepEndedNotice);
        std::fprintf(stderr, "[vinyl] sleep timer: ended, paused\n");
        dirty = true;
    }

    /* the album of a track for "This album or queue": its album tag when both tracks have one, else its folder */
    bool same_album(const std::string &a, const std::string &b) {
        TrackTags ta, tb;
        if (jobs.tags(a, ta) && jobs.tags(b, tb) && !ta.album.empty() && !tb.album.empty()) return ta.album == tb.album;
        return parent_dir(a) == parent_dir(b);
    }

    /* the THIS OUTPUT line of About: the playing output's model */
    menu::About about_now() const {
        OutputVolumeModel m;
        bool muted = false;
        const int step = output_step(&muted, &m);
        const VolTarget t = volume_target();
        std::string label = output_label();
        if (label.empty()) label = t.kind == VolTarget::Kind::None ? "No output" : "The output";
        return menu::about(CDK_APP_VERSION, menu::about_output_line(label, m, step >= 0 || software_volume(t), software_volume(t)));
    }

    /* Esc and the header's Esc (core/nav back_step): the "Clear the queue?" dialog takes them until it closes (design QA
     * run 4, D-30: cancelled, nothing else); else a notice goes with the step back */
    void escape() {
        if (view->name_dialog_shown()) { /* the header's Esc while the name box is up: Cancel */
            view->close_name_dialog();
            if (lib) lib->name_answer(false, "");
            refresh(true);
            return;
        }
        switch (back_step(view->dialog_shown(), view->notice_shown())) {
        case BackStep::CloseModal: view->dialog_key(LV_KEY_ESC); return;
        case BackStep::HideNoticeAndBack: view->hide_notice(); back(); return;
        case BackStep::Back: back(); return;
        }
    }

    void queue_button(int i) {
        if (!(queue_buttons_enabled(queue.size()) & (1u << i))) return; /* drawn dimmed: it does nothing (D-81) */
        if (i == 3) { /* Add: the Library in a multi-select mode that adds to the end of the queue; Done comes back here */
            if (!lib_on()) {
                notice(Notice::Kind::Info, "The Library is not available.");
                return;
            }
            std::vector<std::string> paths;
            for (int pos = 0; pos < queue.size(); ++pos) paths.push_back(queue.at(pos));
            lib->begin_queue_pick(paths);
            refresh(true);
            return;
        }
        if (queue.empty()) return;
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

    /* the LVGL key the toolkit's keypad sends after each key event (core/nav/key_echo: design QA of 0.2.0, D-32; the
     * launcher's decision of 2026-10-05): a letter's or a digit's raw code that is an LVGL key (T = Left, R = Right,
     * W / E = Up / Down, 9 = Enter...) is dropped, and the arrow of an F / X / Z / C alias when Vinyl used the letter
     * (X and C on the queue page), so a shortcut never also moves, opens or goes back */
    KeyEcho key_echo;

    void on_key(uint32_t k) {
        const Page p = view->page();
        if (key_echo.drop(k, mono_ms_now())) return;
        if (knock.input_blocked()) return; /* Knock-knock: the screen is off, no keypad key acts (the media keys come below) */
        if (view->dialog_shown() && view->dialog_key(k)) return; /* "Clear the queue?" takes the keys while it shows */
        if (view->name_dialog_shown()) return; /* the name field types (its own events); no shortcut acts (D-32) */
        if (k == LV_KEY_ESC) {
            escape();
            return;
        }
        if (is_menu_list(p)) { /* the Menu pages: Up / Down move, Enter or Right act, Left goes back (design QA D-29) */
            const std::vector<menu::Row> rows = menu::rows_of(menu_page(p), menu_state());
            int sel = menu_sel.count(p) ? menu_sel[p] : menu::first_item(rows);
            if (k == LV_KEY_UP) sel = menu::step_item(rows, sel, -1);
            else if (k == LV_KEY_DOWN) sel = menu::step_item(rows, sel, 1);
            else if (k == LV_KEY_ENTER || k == LV_KEY_RIGHT) menu_act(p, sel);
            else if (k == LV_KEY_LEFT) back();
            if (view->page() == p) {
                menu_sel[p] = sel;
                refresh(true);
            }
            return;
        }
        if (p == Page::Keys || p == Page::About) { /* nothing acts: Up / Down scroll, Left goes back */
            if (k == LV_KEY_UP) view->scroll_page(p, -1);
            else if (k == LV_KEY_DOWN) view->scroll_page(p, 1);
            else if (k == LV_KEY_LEFT) back();
            return;
        }
        if (lib && lib->sheet_open() && !is_lib_page(p)) { /* the import sheet over the Browser */
            if (k == LV_KEY_LEFT || k == LV_KEY_BACKSPACE) escape();
            else lib->key(k);
            refresh(true);
            return;
        }
        if (is_lib_page(p) && lib) { /* the Library pages: Up / Down move, Enter or Right act, Left goes back (D-29) */
            if (!lib->sheet_open() && (k == LV_KEY_LEFT || k == LV_KEY_BACKSPACE)) escape();
            else lib->key(k);
            refresh(true);
            return;
        }
        if (p == Page::Browser) {
            if (k == LV_KEY_UP) browser_move(-1);
            else if (k == LV_KEY_DOWN) browser_move(1);
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
        bool used = false; /* Vinyl acted on the key as a shortcut (an alias letter's arrow is then dropped too) */
        on_keyboard_key(it, used);
        key_echo.seen(it.key_code, it.semantic_key, static_cast<int>(it.key_state), used, mono_ms_now());
    }

    void on_keyboard_key(const key_item &it, bool &used) {
        const MediaAction ma = media_action_for(it.key_code);
        if (ma != MediaAction::None) {
            MediaEvent me;
            if (keys.accept_other(ma, it.key_state, me)) media(me);
            return;
        }
        /* Knock-knock: while the screen is off only the media keys (above), the volume keys (the launcher's) and - and =
         * (Vinyl's own volume keys, owner decision 2026-11-16) pass; Vinyl acts on no other key (the launcher still sees Esc
         * held 3 s: it reads the keyboards itself) */
        if (!knock.accepts_key(it.key_code)) return;
        if (knock.input_blocked()) {
            if (it.key_state == 1 && !(it.mods & (menu::kModCtrl | menu::kModAlt)) && knock_key_kind(it.key_code) == KeyKind::AppVolume)
                volume_step(it.key_code == menu::kKeyEqual ? 1 : -1);
            return;
        }
        if (view->dialog_shown()) { /* the dialog takes its keys: Enter / Esc through the keypad (on_key), Y here */
            if (it.key_state == 1 && it.key_code == K_Y) view->dialog_key(0, K_Y);
            return;
        }
        /* the search field and the name field type: no shortcut acts, no letter goes anywhere else (D-32) */
        if (lib && is_lib_page(view->page()) && view->search_editing()) return;
        if (view->name_dialog_shown()) return;
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
        if (lib && (is_lib_page(p) || lib->sheet_open()) && lib->letter(it.key_code, it.mods)) { /* the sheet's letters, "/" and Ctrl+F, Ctrl+O */
            used = true;
            refresh(true);
            return;
        }
        if (p == Page::Browser && choose_mode && it.key_code == 22 && !(it.mods & (menu::kModCtrl | menu::kModAlt))) { /* U: use it */
            used = true;
            use_folder();
            return;
        }
        if (p == Page::Browser && !choose_mode && lib_on() && it.key_code == menu::kKeyO && (it.mods & menu::kModCtrl) && !(it.mods & menu::kModAlt) &&
            br_sel >= 0 && br_sel < static_cast<int>(entries.size()) && entries[static_cast<size_t>(br_sel)].kind == Entry::Kind::Playlist) {
            used = true; /* Ctrl+O on a .m3u / .m3u8 row: its sheet (P2c), as the hold */
            browser_hold(br_sel);
            refresh(true);
            return;
        }
        using menu::KeyCmd;
        const KeyCmd cmd = menu::shortcut_command(it.key_code, it.mods, p == Page::Queue); /* Ctrl+O: the Menu */
        used = cmd != KeyCmd::None;
        switch (cmd) {
        case KeyCmd::Toggle: toggle(); break;
        case KeyCmd::Shuffle: shuffle(); break;
        case KeyCmd::Repeat: repeat(); break;
        case KeyCmd::Queue:
            if (p != Page::Queue) button("queue");
            break;
        case KeyCmd::Output:
            if (p != Page::Output) button("output");
            break;
        case KeyCmd::Browse:
            if (p != Page::Browser && !is_lib_page(p)) open_browse();
            break;
        case KeyCmd::Menu: button("menu"); break;   /* M, Ctrl+O (P2a) */
        case KeyCmd::Sleep: button("sleep"); break; /* T (P2a): the Sleep timer page */
        case KeyCmd::QueueUp: queue_button(0); break;
        case KeyCmd::QueueDown: queue_button(1); break;
        case KeyCmd::QueueRemove: queue_button(2); break;
        case KeyCmd::QueueAdd: queue_button(3); break; /* A (the Queue page): add songs from the Library */
        case KeyCmd::QueueSave: save_queue(); break; /* P (the Queue page): save as a playlist */
        case KeyCmd::VolumeDown: volume_step(-1); break; /* the playing output's volume (D11 as changed), one level */
        case KeyCmd::VolumeUp: volume_step(1); break;
        default: break; /* comma and dot: the transport press above */
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
        const bool changed = t.card != mixer_card || !mixer_choice.ok();
        mixer_card = t.card;
        read_mixer(*alsa, t.card, mixer_choice_card, mixer_choice, mixer_level, fine_db());
        if (changed && mixer_choice.ok())
            std::fprintf(stderr, "[vinyl] volume: %s '%s' raw %ld..%ld%s: %s\n", t.card.c_str(), mixer_choice.volume.c_str(),
                         mixer_choice.min, mixer_choice.max, mixer_choice.min_is_mute ? " (min is mute)" : "",
                         volume_model_text(mixer_choice.model).c_str());
    }

    /* A sink's levels (core/volume sink_volume_model): the range of the ALSA card behind it (its proplist), moved under
     * the sink's 0 dB; -60..0 dB for a sink without one (Bluetooth, software sinks). Found once per sink and card. */
    const OutputVolumeModel &sink_model(const PulseDevice &d) const {
        if (d.name == model_sink && d.alsa_card == model_card && model_fine == fine_db()) return model_of_sink;
        model_sink = d.name;
        model_card = d.alsa_card;
        model_fine = fine_db();
        MixerChoice mc;
        bool have = false;
        std::string card;
        if (d.alsa_card >= 0 && alsa) {
            card = "hw:" + std::to_string(d.alsa_card);
            std::vector<CtlInfo> ctls;
            if (alsa->mixer_list(card, ctls) >= 0) have = choose_playback_mixer(ctls, mc, fine_db()) && mc.has_db;
        }
        model_of_sink = sink_volume_model(have ? &mc : nullptr, fine_db());
        std::string from = have ? card + " '" + mc.volume + "' " + volume_model_text(mc.model) : std::string("no card range known");
        std::fprintf(stderr, "[vinyl] volume: sink %s: %s (%s; base volume 0x%x, %u volume steps, flags 0x%x)\n", d.name.c_str(),
                     volume_model_text(model_of_sink).c_str(), from.c_str(), d.base_volume, d.n_volume_steps, d.sink_flags);
        return model_of_sink;
    }

    /* the software level of a card without a mixer: -60..0 dB, kept as its percent */
    static OutputVolumeModel software_model() { return default_volume_model(); }

    /* The playing output's model and level (-1: not known; 0: muted by the control's own mute value or a silent sink). */
    int output_step(bool *muted = nullptr, OutputVolumeModel *model = nullptr) const {
        if (muted) *muted = false;
        const VolTarget t = volume_target();
        if (t.kind == VolTarget::Kind::Sink) {
            const PulseDevice *d = sink_device_of(world, t.sink);
            if (!d || d->volume_raw < 0) return -1;
            const OutputVolumeModel &m = sink_model(*d);
            if (model) *model = m;
            int lv = sink_level_for_volume(m, static_cast<uint32_t>(d->volume_raw));
            if (pending_level >= 0 && pending_sink == t.sink && mono_ms_now() < pending_until && lv != pending_level)
                return pending_level; /* asked for, the server's event not back yet */
            if (muted) *muted = d->muted || lv == 0;
            return lv;
        }
        if (t.kind == VolTarget::Kind::Mixer) {
            if (software_volume(t)) {
                if (model) *model = software_model();
                return software_level(resume, t.key, muted); /* Vinyl's own mute: the level kept */
            }
            if (model) *model = mixer_choice.model;
            const MixerLevel shown = mixer_level_shown(mixer_choice, mixer_level, resume, t.key);
            if (muted) *muted = shown.muted;
            return shown.level;
        }
        return -1;
    }

    /* the percent the group shows: the level's place among the output's levels; -1 = not known */
    int output_level(bool *muted = nullptr) const {
        OutputVolumeModel m;
        const int lv = output_step(muted, &m);
        return lv < 0 ? -1 : m.pct_of(lv);
    }

    /* the group and the keys can change it */
    bool output_level_adjustable() const {
        const VolTarget t = volume_target();
        if (t.kind == VolTarget::Kind::Sink) return output_level() >= 0 && (demo || worker);
        return t.kind == VolTarget::Kind::Mixer;
    }

    /* the engine's gain: silent while a long press scans; the software level only for a card without a mixer (linear in
     * dB over -60..0); silent while Vinyl's own mute holds (a card without a mixer, a control with neither a switch nor
     * a mute value); else unity (the output's own volume does the work, Vinyl's stream stays at 100 %) */
    void apply_gain() {
        if (!engine) return;
        const VolTarget t = volume_target();
        float gain = 1.0f;
        if (software_volume(t)) {
            const OutputVolumeModel m = software_model();
            bool muted = false;
            const int lv = software_level(resume, t.key, &muted);
            gain = muted || lv <= 0 ? 0.0f : volume_gain_db(m.db_of(lv));
        } else if (t.kind == VolTarget::Kind::Mixer && mixer_level.has_mixer && mute_by(mixer_choice) == MuteBy::Vinyl &&
                   mixer_level_shown(mixer_choice, mixer_level, resume, t.key).muted)
            gain = 0.0f;
        /* the sleep timer's minute modes fade over their last 10 s, and stay silent until the engine has paused */
        const float fade = sleep_silent ? 0.0f : sleep.fade_gain(mono_ms_now());
        engine->set_gain(scan_muted ? 0.0f : gain * fade);
        engine->set_boost_db(settings.get().boost_of(t.key)); /* the output in use: its own boost, with the limiter */
    }

    /* the group's and the keys' "-" / "+" (core/volume: sink_press, mixer_press), one level of the output a press: while
     * muted "-" does nothing and "+" brings the sound back at its level; "-" at level 1 mutes */
    void volume_step(int d) {
        const VolTarget t = volume_target();
        bool muted = false;
        OutputVolumeModel m;
        const int cur = output_step(&muted, &m);
        if (t.kind == VolTarget::Kind::Sink) {
            const VolumePress p = sink_press(m, cur, muted, d);
            if (p.kind == VolumePress::Kind::None) return;
            const int level = p.kind == VolumePress::Kind::Mute ? cur : p.level;
            const uint32_t vol = sink_volume_for_db(m.db_of(level));
            const int mute = p.kind == VolumePress::Kind::Mute ? 1 : 0;
            if (demo) {
                for (PulseDevice &s2 : world.sinks)
                    if (s2.name == t.sink) {
                        s2.volume_raw = vol;
                        s2.volume_pct = static_cast<int>((static_cast<uint64_t>(vol) * 100 + 0x8000) / 0x10000);
                        s2.muted = mute != 0;
                    }
            } else if (worker) {
                /* "set sink <name> step i/N P % target X dB"; the worker adds " raw=<set> read back raw=<what the sink
                 * holds>" from a fresh read */
                std::string log = mute ? "set sink " + t.sink + " muted (level " + std::to_string(level) + " kept)"
                                       : volume_set_log_text("sink " + t.sink, m, level, vol, -1, 0);
                if (const size_t cut_at = log.find(" raw="); cut_at != std::string::npos) log.resize(cut_at);
                worker->request_sink_volume(t.sink, t.channels, vol, mute, log);
                if (!mute) {
                    pending_level = level;
                    pending_sink = t.sink;
                    pending_until = mono_ms_now() + 1500;
                }
            }
            if (demo) std::fprintf(stderr, "[vinyl] volume: %s\n", volume_set_log_text("sink " + t.sink, m, mute ? 0 : level, vol, 0, vol).c_str());
        } else if (t.kind == VolTarget::Kind::Mixer) {
            if (software_volume(t)) {
                const OutputVolumeModel sm = software_model();
                const VolumePress p = software_press(resume, t.key, d); /* Vinyl's own mute keeps the level */
                if (p.kind == VolumePress::Kind::None) return;
                dirty = true;
                apply_gain();
                bool muted = false;
                const int level = software_level(resume, t.key, &muted);
                char db[48] = "";
                if (muted) std::snprintf(db, sizeof db, "muted, level %d kept", level);
                else std::snprintf(db, sizeof db, "%.2f dB", sm.db_of(level));
                std::fprintf(stderr, "[vinyl] volume: software step %d/%d %d %% (%s) for %s (no playback mixer)\n", muted ? 0 : level,
                             sm.n, muted ? 0 : sm.pct_of(level), db, t.key.c_str());
            } else if (alsa) {
                const MixerLevel shown = mixer_level_shown(mixer_choice, mixer_level, resume, t.key);
                const VolumePress p = mixer_press(mixer_choice, shown, d);
                const MuteBy by = mute_by(mixer_choice);
                const bool on_off = p.kind == VolumePress::Kind::Mute || p.kind == VolumePress::Kind::Unmute;
                int r = 0;
                if (p.kind == VolumePress::Kind::Set) r = write_mixer_level(*alsa, t.card, mixer_choice, p.level);
                else if (on_off && by == MuteBy::Switch)
                    r = write_mixer_switch(*alsa, t.card, mixer_choice, p.kind == VolumePress::Kind::Unmute);
                else if (!on_off) return;
                if (r >= 0) {
                    note_mixer_press(resume, t.key, mixer_choice, p);
                    dirty = true;
                }
                if (on_off && by == MuteBy::Vinyl)
                    std::fprintf(stderr, "[vinyl] volume: %s '%s' %s by Vinyl (no switch, no mute value; level %d kept)\n",
                                 t.card.c_str(), mixer_choice.volume.c_str(), p.kind == VolumePress::Kind::Mute ? "muted" : "un-muted",
                                 shown.level);
                if (r < 0) std::fprintf(stderr, "[vinyl] volume: %s %s: %s\n", t.card.c_str(), mixer_choice.volume.c_str(), alsa_error_text(r).c_str());
                if (p.kind == VolumePress::Kind::Set && r >= 0) { /* what was written and what the control holds now */
                    long back = 0;
                    const int rr = alsa->mixer_get(t.card, mixer_choice.volume, back); /* a fresh read of the control */
                    std::fprintf(stderr, "[vinyl] volume: %s\n",
                                 volume_set_log_text(t.card + " '" + mixer_choice.volume + "'", mixer_choice.model, p.level,
                                                     mixer_raw_for_level(mixer_choice, p.level), rr, back)
                                     .c_str());
                } else if (r >= 0 && by == MuteBy::Switch)
                    std::fprintf(stderr, "[vinyl] volume: set %s '%s' %s\n", t.card.c_str(), mixer_choice.sw.c_str(),
                                 p.kind == VolumePress::Kind::Mute ? "off (muted)" : "on (un-muted)");
                read_output_mixer(mono_ms_now(), true);
                apply_gain();
            }
        }
        refresh(true);
    }

    /* ------------------------------------------------------------------------------------------ the Library (P2b) */

    /* the roots the library indexes now */
    std::vector<RootSpec> library_wanted() {
        if (demo) {
            std::vector<RootSpec> w;
            if (!demo_world.empty) w.push_back(RootSpec{demo_world.music, "Music"});
            if (!demo_drive_dir.empty()) w.push_back(RootSpec{demo_drive_dir, "SANDISK"});
            return w;
        }
        const char *hm = std::getenv("HOME"), *us = std::getenv("USER");
        return library_roots(hm ? hm : "", us ? us : "", read_mounts(), settings.get().library_folders);
    }

    LibHost lib_host() {
        LibHost h;
        h.playing_path = [this]() -> std::string {
            if (!snap.path.empty()) return snap.path;
            return queue.current_path() ? *queue.current_path() : std::string();
        };
        h.play = [this](const std::vector<std::string> &paths, int start) { play_paths(paths, start); };
        h.play_source = [this](const std::vector<std::string> &paths, int start, const std::string &source) { play_paths(paths, start, source); };
        h.ask_name = [this](const NameAsk &a) { view->ask_name(a); };
        h.ask_confirm = [this](const std::string &t, const std::string &b, const std::string &ok) { view->ask_confirm(t, b, ok); };
        h.open_queue = [this] { go(Page::Queue); }; /* Add to the queue: Done comes back here */
        h.choose_export = [this](const std::string &) { /* the Browser in choose mode: the folder the playlist goes to */
            choose_mode = true;
            browser_from_lib = true;
            open_folder(roots.size() == 1 ? roots.front().path : std::string(), "");
            go(Page::Browser);
        };
        h.play_next = [this](const std::vector<std::string> &p) { queue_insert(p, true); };
        h.add_to_queue = [this](const std::vector<std::string> &p) { queue_insert(p, false); };
        h.notice = [this](Notice::Kind k, const std::string &t) { notice(k, t); };
        h.show = [this](Page p) { go(p); };
        h.open_folders = [this] {
            browser_from_lib = true;
            choose_mode = false;
            if (drives_listed()) open_folder("", ""); /* the drives are on the list of roots */
            go(Page::Browser);
        };
        h.choose_folder = [this] {
            choose_mode = true;
            browser_from_lib = true;
            open_folder(roots.size() == 1 ? roots.front().path : std::string(), "");
            go(Page::Browser);
        };
        h.wanted = [this] { return library_wanted(); };
        h.folders = [this] { return settings.get().library_folders; };
        h.set_folders = [this](const std::vector<std::string> &f) {
            Settings st = settings.get();
            st.library_folders = f;
            apply_settings(st);
        };
        h.mini = [this] { return mini_view(); };
        h.scroll = [this](Page p, int rows) { view->scroll_page(p, rows); };
        h.scroll_y = [this] { return view ? view->lib_scroll_y(view->page()) : 0; };
        if (drives) {
            h.drives = [this] { return drives ? drives->list() : std::vector<DriveInfo>(); };
            h.drives_available = [this] { return drives && drives->available(); };
            h.mount = [this](const std::string &dev, const std::string &name) { request_mount(dev, name); };
            h.eject = [this](const std::string &dev, const std::string &name) {
                if (drives && drives->by_vinyl(dev)) drives->unmount_async(dev, name);
            };
        }
        h.search_open = [this](const std::string &t, bool edit) { view->search_open(t, edit); };
        h.search_edit = [this] { view->search_edit(); };
        h.search_end_edit = [this] { view->search_end_edit(); };
        h.show_sheet = [this](const SheetView &v) { view->show_sheet(v); };
        h.hide_sheet = [this] {
            if (view) view->hide_sheet();
        };
        h.sheet_select = [this](int i) { view->sheet_select(i); };
        return h;
    }

    /* the Library: its database opens now (a few ms); without SQLite or with a newer schema Browse stays the folder list */
    void make_library() {
        if (demo && !env_on("VINYL_DEMO_LIBRARY")) return;
        LibConfig lc;
        lc.demo = demo;
        lc.ram_mb = system_ram_mb();
        const char *hm = std::getenv("HOME");
        lc.home = demo ? demo_world.root : (hm ? hm : "");
        if (demo) {
            lc.db_path = demo_world.root + "/library.db";
            std::string mode = env_str("VINYL_DEMO_LIBRARY");
            if (mode == "1") mode = "scan";
            if (mode == "full" && env_str("VINYL_DEMO_DRIVES") == "vinyl") mode = "drives";
            if (demo_world.empty) mode = "empty";
            make_demo_library(demo_world, lc.db_path, mode);
            if (const std::string pl = env_str("VINYL_DEMO_PLAYLISTS"); !pl.empty()) make_demo_playlists(demo_world, pl == "1" ? std::string("list") : pl);
        } else {
            lc.db_path = library_db_path();
        }
        lib = std::make_unique<LibraryCtl>(lib_host(), lc);
        if (!lib->available()) {
            std::fprintf(stderr, "[vinyl] the Library is not available: %s (Browse stays the folder list)\n", lib->error().c_str());
            lib.reset();
            return;
        }
        std::fprintf(stderr, "[vinyl] the Library: %s\n", lc.db_path.c_str());
        if (demo) { /* the scenes: a scan shown as running, or paused for heat, without the thread */
            const std::string sc = env_str("VINYL_DEMO_SCAN");
            if (sc == "scanning" || sc == "paused") {
                ScanProgress p;
                p.state = sc == "paused" ? ScanProgress::State::Paused : ScanProgress::State::Scanning;
                p.files = sc == "paused" ? 735 : 388;
                lib->demo_progress(p, 1200);
            }
        }
    }

    /* Browse: the Library's hub, or the folder list without a Library */
    void open_browse() {
        if (!lib_on()) {
            go(Page::Browser);
            return;
        }
        browser_from_lib = false;
        lib->enter();
    }

    /* the Browser's choose mode: "Use folder" */
    void use_folder() {
        if (!choose_mode || !lib_on()) return;
        if (folder.empty()) {
            notice(Notice::Kind::Info, "Open a folder first.");
            return;
        }
        if (lib->exporting()) { /* an export: the playlist goes there; the Browser stays up when it could not be written */
            lib->export_to(folder);
            return;
        }
        choose_mode = false;
        lib->add_folder(folder);
    }

    /* a .m3u / .m3u8 row of the Browser held 1.5 s (or Ctrl+O): the Play / Import as a playlist / Add to the queue sheet (P2c) */
    void browser_hold(int i) {
        if (!lib_on() || choose_mode || i < 0 || i >= static_cast<int>(entries.size())) return;
        const Entry &e = entries[static_cast<size_t>(i)];
        if (e.kind != Entry::Kind::Playlist) return;
        br_sel = i;
        lib->open_import_sheet(e.path);
    }

    /* the Queue page's "Save as playlist" (the chip, the key P): the queue in its play order */
    void save_queue() {
        if (!lib_on() || queue.empty()) return;
        std::vector<std::string> paths;
        for (int pos = 0; pos < queue.size(); ++pos) paths.push_back(queue.at(pos));
        lib->save_queue(paths);
    }

    void lib_button(Page p, int i) {
        if (p == Page::Browser) use_folder();
        else if (lib) lib->button(p, i);
        refresh(true);
    }

    /* "Play": a new queue of these tracks, starting at `start`, then Now playing */
    void play_paths(const std::vector<std::string> &paths, int start, const std::string &source = "library") {
        if (paths.empty()) return;
        queue.set(paths, std::clamp(start, 0, static_cast<int>(paths.size()) - 1), source);
        ++qgen;
        ++queue_set_gen;
        jobs.want_tags(queue.items());
        play_pos(queue.current(), true);
        go(Page::Now);
    }

    /* "Play next" and "Add to queue": the tracks go into the queue, the playing one stays */
    void queue_insert(const std::vector<std::string> &paths, bool next) {
        if (paths.empty()) return;
        if (next) queue.insert_next(paths);
        else queue.append(paths);
        ++qgen;
        jobs.want_tags(paths);
        update_next();
        dirty = true;
        refresh(true);
    }

    /* Is the drive directory there: listed in the mounts and readable (a stale mount is not)? The scenes' drives are the fake
     * backend's. */
    bool media_present(const std::string &dir) {
        if (demo) {
            if (!drives) return true;
            for (const DriveInfo &d : drives->list())
                if (d.mounted && d.mount_dir == dir) return true;
            return false;
        }
        return drive_present_real(dir);
    }

    /* A drive went away (pulled, a stale mount that cannot be read, ejected, unmounted at exit): every queue entry that lies on it
     * is removed from the queue at once (owner ruling 2026-11-14, B50: "if media was removed vinyl should get back to default
     * music folder and on reopen go to the library directly"). The playing track among them: playback stops cleanly (no "file is
     * gone", no retry, no skipping through the dead entries), the next entry that is left is loaded paused (never started by
     * itself: the sound ended with the drive), the queue empty when none is; the red notice says so once. An empty queue leaves
     * Now playing (or the Queue) for the Library at the default music folder. true when something was removed. */
    bool drop_gone_media(bool at_start) {
        if (queue.empty()) return false;
        const std::vector<std::string> gone =
            gone_drive_dirs(queue.items(), [this](const std::string &d) { return media_present(d); }, drive_bases,
                            [this](const std::string &p) { return !demo && ::access(p.c_str(), F_OK) == 0; });
        if (gone.empty()) return false;
        const Queue::RemoveResult r = queue.remove_under(gone);
        std::fprintf(stderr, "[vinyl] media gone (%s): %d queue entr%s removed%s\n", gone.front().c_str(), r.removed, r.removed == 1 ? "y" : "ies",
                     r.current_removed ? ", the playing one among them" : "");
        ++qgen;
        ++queue_set_gen;
        dirty = true;
        if (r.current_removed) resume.position_ms = 0;
        if (at_start) return true;
        if (r.current_removed || queue.empty()) {
            engine->stop();
            long_path.clear();
            play_path.clear();
        }
        if (queue.empty()) {
            std::fprintf(stderr, "[vinyl] the queue is empty: back to the default music folder\n");
        } else if (r.current_removed) {
            engine->load(queue.current(), *queue.current_path(), 0, false); /* the next one that is left, paused */
            jobs.want_tags({*queue.current_path()});
        } else {
            engine_track_moved(); /* the playing track stays: only its place in the queue moved */
        }
        update_next();
        notice(Notice::Kind::Danger, r.current_removed ? "Drive removed. Paused." : "Drive removed. Its tracks left the queue.");
        if (queue.empty() && (view->page() == Page::Now || view->page() == Page::Queue)) go_default_folder();
        refresh(true);
        return true;
    }

    /* the Library at the default music folder: ~/Music when it is a root, else the first one (Esc: the hub) */
    void go_default_folder() {
        std::string def;
        const char *hm = std::getenv("HOME");
        const std::string music = hm ? std::string(hm) + "/Music" : std::string();
        for (const Root &r : roots)
            if (r.path == music) def = r.path;
        if (def.empty() && !roots.empty()) def = roots.front().path;
        browser_from_lib = lib_on();
        choose_mode = false;
        open_folder(def, "");
        go(Page::Browser);
        notice(Notice::Kind::Danger, "Drive removed. Paused."); /* go() took the first one away with the page: said again here, once */
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
        if (n == "ffmpeg") notice(Notice::Kind::Info, "The FFmpeg libraries could not be loaded. Reinstall Vinyl.");
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
        OutputVolumeModel model;
        const int step = output_step(nullptr, &model);
        if (t.kind == VolTarget::Kind::Sink) {
            line = "sink " + t.sink + " " + volume_log_text(v);
            if (step >= 0) {
                char b[64];
                std::snprintf(b, sizeof b, ", step %d/%d%s", step, model.n, step > 0 ? "" : " (silent)");
                line += b;
                if (step > 0) {
                    std::snprintf(b, sizeof b, ", %.2f dB", model.db_of(step));
                    line += b;
                }
            }
        }
        else if (t.kind == VolTarget::Kind::Mixer && software_volume(t))
            line = "software " + volume_log_text(v) + " for " + t.key + " (" + t.card + ": no playback mixer)";
        else if (t.kind == VolTarget::Kind::Mixer) {
            char db[64] = "";
            if (mixer_choice.has_db) std::snprintf(db, sizeof db, ", step %d/%d, %.2f dB", step, model.n, mixer_level.cdb / 100.0);
            else std::snprintf(db, sizeof db, ", step %d/%d", step, model.n);
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
        OutputVolumeModel vol_model;
        const int vol_step = output_step(&vol_muted, &vol_model);
        v.vol_level = vol_step < 0 ? -1 : vol_model.pct_of(vol_step);
        v.vol_muted = vol_muted;
        if (vol_step > 0) /* "Volume steps": Normal shows the percent, 1 / 0.5 / 0.1 dB the output's real level in dB */
            v.vol_value = volume_level_text(vol_model, vol_step, fine_db() > 0 ? VolumeUnit::Decibel : VolumeUnit::Percent);
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
        if (layout::shows_gapless(g) && settings.get().gapless) {
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
        /* P2a: the ReplayGain figure after Gapless where NP-480x480 and NP-800x480 draw it ("Gapless, RG -3.2 dB") */
        if (view->size_class() == layout::SizeClass::Square || view->size_class() == layout::SizeClass::Large) {
            const std::string rg = replaygain_chip_text(settings.get().replaygain, snap.rg);
            if (!rg.empty()) v.extra_chip = v.extra_chip.empty() ? rg : v.extra_chip + ", " + rg;
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
            v.queue_selected_by_user = pane_sel >= 0;
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
                r.value = e.extra;
                r.mono_value = false;
                r.mark = RowView::Mark::Chevron;
                break;
            case Entry::Kind::Heading:
                r.text = e.name;
                r.heading = true;
                r.mark = RowView::Mark::None;
                break;
            case Entry::Kind::Drive: /* the amber action dot: a tap mounts it */
                r.text = e.name;
                r.value = e.extra;
                r.mono_value = false;
                r.mark = RowView::Mark::Dot;
                r.amber = true;
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
        v.mini = mini_view();
        if (choose_mode) { /* Music folders > Add a folder: the strip asks for the folder, the mini player is away */
            v.choose = true;
            v.heading = (lib && lib->exporting() ? "EXPORT TO: " : "CHOOSE A FOLDER: ") +
                        (folder.empty() ? std::string("MUSIC") : breadcrumb(roots, folder));
            v.mini = MiniView();
            v.mini.shown = true;
            v.mini.title = folder.empty() ? std::string("Choose a folder") : folder.substr(folder.rfind('/') == std::string::npos ? 0 : folder.rfind('/') + 1);
            v.mini.sub = folder.empty() ? "Open a folder" : "Or press U";
        }
        return v;
    }

    /* the mini player's strip (the Browser's and the Library hub's): the track heard, its times and progress */
    MiniView mini_view() {
        MiniView m;
        const std::string *playing = queue.current_path();
        const std::string path = queue.empty() ? std::string() : !snap.path.empty() ? snap.path : (playing ? *playing : "");
        m.shown = !path.empty();
        if (m.shown) {
            TrackTags t;
            const bool known = jobs.tags(path, t);
            m.title = known && !t.title.empty() ? t.title : title_from_path(path);
            const uint64_t total = snap.total_ms() ? snap.total_ms() : (known ? t.duration_ms : 0);
            /* the artist (the view gives it the dots) and the times, whole, h:mm:ss for a track of an hour or more */
            m.sub = known ? t.artist : "";
            const bool hours = (total ? total : shown_pos_ms()) >= 3600000;
            m.time = layout::track_time_text(shown_pos_ms(), hours, true) + " / " +
                     (total ? layout::track_time_text(total, hours, true) : std::string("--:--"));
            m.progress = total ? static_cast<double>(shown_pos_ms()) / total : 0;
            m.playing = snap.state == PlayState::Playing;
        }
        return m;
    }

    ListView queue_view() {
        ListView v;
        v.queue_buttons = queue_buttons_enabled(queue.size());
        v.save_chip = lib_on() && !queue.empty(); /* P2c: "Save as playlist" */
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
        if (cdk::hidden() || knock.input_blocked()) return; /* hidden, or the screen off (Knock-knock): drawn when it is back */
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
        else if (is_lib_page(p) && lib) view->update_lib(p, lib->view());
        else if (is_menu_list(p)) view->update_menu(p, menu_view(p));
        else if (p == Page::About) {
            const menu::About a = about_now();
            if (a.output != about_shown) {
                about_shown = a.output;
                view->set_about(a);
            }
        }
    }

    MenuView menu_view(Page p) {
        MenuView v;
        const menu::State st = menu_state();
        const menu::Page mp = menu_page(p);
        v.rows = menu::rows_of(mp, st);
        v.selected = menu_sel.count(p) ? menu_sel[p] : menu::first_item(v.rows);
        v.heading = menu::page_heading(mp);
        v.hint = menu::page_hint(mp, st);
        v.hint_clock = p == Page::Sleep;
        v.value_bright = p == Page::Playback;
        v.gen = menu_gen;
        return v;
    }

    /* ------------------------------------------------------------------------------------------ the 100 ms tick */

    static void timer_cb(lv_timer_t *t) { static_cast<Impl *>(lv_timer_get_user_data(t))->tick(); }

    void tick() {
        const uint64_t now = mono_ms_now();
        if (lib) lib->tick(now);
        drive_results();
        if (pending_escape) { /* the search field's Esc, outside the key event that made it */
            pending_escape = false;
            escape();
        }
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
        knock_tick(); /* Knock-knock: `screen` in board.state's requests, the answers of the worker */
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
        /* the scenes: the next track starts by itself (a track change nobody asked for on this page) */
        if (demo_next_at && now >= demo_next_at) {
            demo_next_at = 0;
            if (queue.current() + 1 < queue.size()) play_pos(queue.current() + 1, snap.state == PlayState::Playing);
        }
        /* the scenes: a drive is pulled after a while */
        if (demo_pull_at && now >= demo_pull_at && demo_fake) {
            demo_pull_at = 0;
            demo_fake->devices.erase(std::remove_if(demo_fake->devices.begin(), demo_fake->devices.end(),
                                                    [](const DriveInfo &d) { return d.mounted; }),
                                     demo_fake->devices.end());
        }
        /* a drive the queue's files lie on went away: they leave the queue (B50) */
        if (!queue.empty() && (!demo || drives) && now - gone_checked >= 1000) {
            gone_checked = now;
            drop_gone_media(false);
        }
        /* the playing output's volume: the card's mixer without a server (the server's sinks come with the scans) */
        read_output_mixer(now, false);
        for (int n = vol_hold.poll(now); n > 0; --n) volume_step(vol_hold.dir());
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
            /* Knock-knock: hidden (Super, Alt+Tab, the three-finger hold) gives the light back (the launcher does too) */
            if (knock.input_blocked()) std::fprintf(stderr, "[vinyl] knock-knock: hidden, the screen comes back\n");
            apply_knock(knock.restore());
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
        /* the sleep timer: a minute mode at its end pauses (its fade is in apply_gain); silent until paused */
        if (sleep.expired(now)) {
            if (snap.state == PlayState::Playing) {
                engine->pause();
                sleep_silent = true;
            }
            sleep_ended();
        }
        if (sleep_silent && snap.state != PlayState::Playing) {
            sleep_silent = false;
            apply_gain();
        }
        /* a pause (by any path): state.json and positions.json hold it */
        if (last_state == PlayState::Playing && snap.state != PlayState::Playing) {
            dirty = true;
            pos_policy.event();
        }
        last_state = snap.state;
        /* the position in a long file (the Playback setting, On by default) */
        if (settings.get().remember_long && !snap.path.empty() && snap.state != PlayState::Idle && is_long_file(snap.total_ms())) {
            if (long_path != snap.path) {
                note_long_file(true);
                long_path = snap.path;
            }
            long_pos = snap.pos_ms();
            long_total = snap.total_ms();
            if (positions.note(long_path, long_pos, long_total)) {
                if (snap.state == PlayState::Playing) pos_policy.progress();
                else pos_policy.event();
            }
        } else if (!long_path.empty() && snap.path != long_path) {
            note_long_file(true);
            long_path.clear();
        }
        save_positions_now(false);
        /* save as it goes (the launcher may close a hidden app), the SD card in mind (report 060 finding 3): events at
         * once, the position while playing at most once a minute */
        if (snap.pos >= 0) {
            resume.position_ms = snap.pos_ms();
            if (snap.state == PlayState::Playing) state_policy.progress();
        }
        save(false);
        /* tags arrived: the rows show titles and durations */
        const uint64_t tg = jobs.gen();
        const bool tags_changed = tg != tags_seen;
        tags_seen = tg;
        if (!hidden && !knock.input_blocked()) { /* Knock-knock: nothing is drawn while the screen is off */
            view->tick(snap.state == PlayState::Playing);
            refresh(tags_changed);
        }
    }

    void on_engine(const EngineEvent &ev) {
        switch (ev.kind) {
        case EngineEvent::Kind::Started:
            play_path = ev.path;
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
            if (lib && !play_path.empty()) lib->note_played(play_path); /* the track before it played to its end */
            const int n = queue.next_after(ev.pos, false);
            if (n >= 0) jobs.want_tags({queue.at(n)});
            name_next(ev.pos);
            break;
        }
        case EngineEvent::Kind::QueueEnd:
            if (lib && !play_path.empty()) lib->note_played(play_path);
            if (sleep_armed) { /* the sleep timer's track mode: paused, on the next track (the next album's first) */
                const int n = queue.next_after(ev.pos, false);
                sleep_ended();
                if (n >= 0 && n != ev.pos) play_pos(n, false);
                update_next();
            }
            dirty = true;
            break;
        case EngineEvent::Kind::OpenFailed:
            if (drop_gone_media(false)) break; /* the file was on a drive that went away: no "file is gone" */
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

    /* state.json: an event at the next tick, the position while playing at most once a minute (WritePolicy), and at
     * exit */
    void save(bool force) {
        const uint64_t now = mono_ms_now();
        if (dirty) state_policy.event();
        dirty = false;
        if (!force && !state_policy.due(now)) return;
        resume_from_queue(queue, resume);
        resume.output = target.key();
        const int r = save_resume(resume_file, resume);
        if (r < 0 && force) std::fprintf(stderr, "[vinyl] cannot save %s: %s\n", resume_file.c_str(), std::strerror(-r));
        state_policy.wrote(now);
    }

    /* the long file left (a track change, a new track chosen, the exit): its place noted, the file written soon */
    void note_long_file(bool event) {
        if (long_path.empty() || !settings.get().remember_long) return;
        if (positions.note(long_path, long_pos, long_total)) {
            if (event) pos_policy.event();
            else pos_policy.progress();
        }
    }

    /* positions.json: on pause, a track change and exit, at most once a minute while a long file plays */
    void save_positions_now(bool at_exit) {
        const uint64_t now = mono_ms_now();
        if (at_exit ? !pos_policy.pending() : !pos_policy.due(now)) return;
        const int r = save_positions(posix_file_ops(), positions_file, positions);
        if (r < 0) std::fprintf(stderr, "[vinyl] cannot save %s: %s\n", positions_file.c_str(), std::strerror(-r));
        pos_policy.wrote(now);
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
