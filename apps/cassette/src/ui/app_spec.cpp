/*
 * SPDX-License-Identifier: MIT
 *
 * What Cassette gives the shared runners (_common/app.hpp): its id, title, minimum screen (X-MinSize 480x320), minimum
 * portrait canvas (X-Portrait 320x480) and UI.
 *
 * Until the application controller (task T6a, src/core/app) is bound, the UI runs on the synthetic demonstration
 * model (ui/demo.hpp; CASSETTE_DEMO picks its state, the scenes use it). The binding replaces DemoDriver by the
 * controller: its snapshot() fills the ViewModel, its commands are the UiActions (view_model.hpp).
 */
#include "app.hpp"
#include "cassette_ui.hpp"
#include "demo.hpp"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

struct CassetteApp {
    std::unique_ptr<cassette::ui::DemoDriver> demo;
    std::unique_ptr<cassette::ui::CassetteUi> ui;
    lv_timer_t *live = nullptr;
    std::uint32_t started = 0;
    ~CassetteApp()
    {
        if (live) lv_timer_delete(live);
    }
};

} // namespace

namespace cdk {

const AppSpec &app_spec()
{
    static const AppSpec spec = [] {
        AppSpec s;
        s.id = "cassette";
        s.title = "Cassette";
        s.min_w = cassette::ui::kMinW;
        s.min_h = cassette::ui::kMinH;
        s.portrait_w = cassette::ui::kPortraitW;
        s.portrait_h = cassette::ui::kPortraitH;
        s.build = [](lv_obj_t *screen) -> std::shared_ptr<void> {
            auto app = std::make_shared<CassetteApp>();
            const char *scene = std::getenv("CASSETTE_DEMO");
            app->demo = std::make_unique<cassette::ui::DemoDriver>(cassette::ui::demo_model(scene && *scene ? scene : "idle"));
            app->ui = std::make_unique<cassette::ui::CassetteUi>(screen, app->demo->actions());
            cassette::ui::CassetteUi *ui = app->ui.get();
            app->demo->changed = [ui](const cassette::ui::ViewModel &vm) { ui->update(vm); };
            ui->update(app->demo->model());
            if (const char *live = std::getenv("CASSETTE_DEMO_LIVE"); live && *live && std::string(live) != "0") {
                app->started = lv_tick_get();
                app->live = lv_timer_create(
                    [](lv_timer_t *t) {
                        auto *a = static_cast<CassetteApp *>(lv_timer_get_user_data(t));
                        a->demo->tick_live(lv_tick_elaps(a->started));
                    },
                    33, app.get());
            }
            if (const char *page = std::getenv("CASSETTE_DEMO_PAGE")) {
                const std::string p = page;
                if (p == "settings") ui->go(cassette::ui::Screen::Settings);
                else if (p == "list") ui->go(cassette::ui::Screen::List);
                else if (p == "trim") ui->go(cassette::ui::Screen::Trim);
            }
            return app;
        };
        return s;
    }();
    return spec;
}

} // namespace cdk
