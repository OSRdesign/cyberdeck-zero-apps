/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: the bound app (see bound_app.hpp).
 */
#include "bound_app.hpp"

#include "binder.hpp"
#include "port.hpp"
#include "demo/demo_alsa.hpp"
#include "ui/cassette_ui.hpp"

#include "app.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace cassette {
namespace bind {

namespace {

std::string user_home()
{
    if (const char *h = std::getenv("HOME"); h && *h) return h;
    if (const passwd *pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return "/tmp";
}

struct BoundApp {
    std::unique_ptr<AlsaApi> api;
    std::unique_ptr<PulseApi> pulse; /* the sound server (Bluetooth input); outlives the controller */
    std::unique_ptr<Controller> controller;
    std::unique_ptr<Port> port;
    std::unique_ptr<Worker> worker;
    std::unique_ptr<Binder> binder;
    std::unique_ptr<ui::CassetteUi> ui;
    lv_timer_t *timer = nullptr;

    ~BoundApp()
    {
        if (timer) lv_timer_delete(timer);
        timer = nullptr;
        if (binder) binder->leave(); /* the take closed and kept, the journal updated */
        ui.reset();
        binder.reset();
        worker.reset();
        port.reset();
        controller.reset();
        pulse.reset();
        api.reset();
        std::fprintf(stderr, "[cassette] ended cleanly\n");
    }

    void poll()
    {
        if (cdk::hidden()) return;
        if (binder->tick(static_cast<int64_t>(lv_tick_get()), static_cast<int64_t>(std::time(nullptr))))
            ui->update(binder->vm());
        ui::Screen s;
        if (binder->take_nav(s) && ui->screen() != s) ui->go(s);
    }
};

} // namespace

bool bound_wanted()
{
    const char *v = std::getenv("CASSETTE_DEMO");
    return !v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "1") == 0;
}

std::shared_ptr<void> build_bound_app(lv_obj_t *screen)
{
    auto app = std::make_shared<BoundApp>();
    ControllerConfig cfg;
    if (demo::wanted()) {
        const demo::Script script = demo::script_from_env();
        std::string err;
        cfg.home = demo::demo_home(&err);
        if (cfg.home.empty()) {
            std::fprintf(stderr, "[cassette] demo: %s\n", err.c_str());
            cfg.home = "/tmp";
        }
        app->api = demo::make_demo_alsa(script);
        cfg.min_free_bytes = 16ull << 20; /* a demo home may be a small tmpfs */
        /* CASSETTE_DEMO_CODECS=none|flac|opus|both: which encoder libraries the Format picker finds (the scenes do not
         * depend on this machine's libraries); unset: the real ones */
        if (const char *cd = std::getenv("CASSETTE_DEMO_CODECS"); cd && *cd) {
            CodecAvailability ca;
            const std::string v = cd;
            ca.flac = v == "flac" || v == "both";
            ca.opus = v == "opus" || v == "both";
            if (!ca.flac) ca.flac_why = "demo: not installed";
            if (!ca.opus) ca.opus_why = "demo: not installed";
            cfg.codecs = [ca] { return ca; };
        }
        std::fprintf(stderr, "[cassette] demo mode: scripted sound system (cards %s), home %s\n", script.cards.c_str(),
                     cfg.home.c_str());
    } else {
        app->api = make_real_alsa();
        app->pulse = make_real_pulse(); /* libpulse is loaded at the first connect, only when a sound server runs */
        cfg.pulse = app->pulse.get();
        cfg.home = user_home();
    }
    cfg.api = app->api.get();
    app->controller = std::make_unique<Controller>(cfg);
    app->controller->start();
    app->port = make_controller_port(*app->controller, posix_file_ops());
    app->worker = std::make_unique<Worker>(true);
    app->binder = std::make_unique<Binder>(*app->port, *app->worker);
    app->ui = std::make_unique<ui::CassetteUi>(screen, app->binder->actions());
    app->binder->set_window_bars(app->ui->trim_bars_across()); /* one bar read per bar drawn, on the take's grid */
    const Snapshot s = app->controller->snapshot();
    std::fprintf(stderr, "[cassette] recordings in %s; sound input: %s\n", s.folder.c_str(),
                 s.card.present ? s.card.label.c_str() : "none (looking for one every second)");
    app->poll();
    app->ui->update(app->binder->vm());
    app->timer = lv_timer_create([](lv_timer_t *t) { static_cast<BoundApp *>(lv_timer_get_user_data(t))->poll(); }, 33,
                                 app.get());
    return app;
}

} // namespace bind
} // namespace cassette
