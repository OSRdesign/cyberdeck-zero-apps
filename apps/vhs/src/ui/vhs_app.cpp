/*
 * SPDX-License-Identifier: MIT
 *
 * VHS UI: the app (see vhs_app.hpp).
 */
#include "vhs_app.hpp"

#include "app.hpp"
#include "core/live/bench.hpp"
#include "core/live/clock.hpp"

#include "cp0_ui_metrics_lvgl.h"
#include "keyboard_input.h"

#include <linux/input-event-codes.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace vhs {
namespace ui {
namespace {

/* the canvas's notices, in its order (Notices-480x320): VHS_DEMO_NOTICES=1 and N walk them (the scenes) */
const Notice kDemoNotices[] = {
    {kNoticeUnplugged, false}, {kNoticeBusy, false},     {kNoticeNoAccess, false}, {kNoticePower, false},
    {kNoticeDiskFull, true},   {kNoticeNoSound, false},  {kNoticeDrops, false},
};

} // namespace

VhsApp::VhsApp(lv_obj_t *screen) : screen_(screen)
{
    display_ = lv_obj_get_display(screen);
    std::string warnings;
    Options opt = options_from_env(&warnings);
    if (!warnings.empty()) std::fprintf(stderr, "[vhs] %s\n", warnings.c_str());
    if (!opt.fmt_set) {
        /* the display's own format: RGB565 on the 16-bit panels, XRGB8888 where the display is 32-bit */
        const lv_color_format_t cf = lv_display_get_color_format(display_);
        opt.fmt = (cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_ARGB8888) ? PixFmt::XRGB8888 : PixFmt::RGB565;
    }
    demo_notices_ = std::getenv("VHS_DEMO_NOTICES") && std::string(std::getenv("VHS_DEMO_NOTICES")) == "1";

    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen_, layout_, cp0::ui::add_title, [] { cdk::request_quit(0); });
    cp0::ui::StatusStrip::add(page_->header());
    page_->set_title({"VHS"});
    CaptureLayoutIn in;
    in.w = layout_.w;
    in.h = layout_.h;
    in.body_y = layout_.body_y;
    in.target = m->tok[CP0_TOK_TARGET];
    geo_ = capture_layout(in);

    ControllerConfig cc;
    cc.fake = opt.fake;
    cc.fake_opt = opt.fake_opt;
    cc.fake_script = opt.fake_script;
    cc.force_undervolt = opt.fake_undervolt;
    cc.device = opt.device;
    cc.dev_root = opt.dev_root;
    cc.sys_root = opt.sys_root;
    cc.persist = !opt.fake;
    cc.fmt = opt.fmt;
    cc.box_w = geo_.picture.w;
    cc.box_h = geo_.picture.h;
    cc.fastdct = opt.fastdct;
    cc.ui_tid = current_tid();
    cc.log_every = opt.stats ? 1 : 10;
    cc.mode = opt.mode;
    cc.denom = opt.denom;
    cc.soc_override = opt.soc;
    ctl_ = std::make_unique<Controller>(cc);

    CaptureActions act;
    act.toggle = [this] { ctl_->toggle_capture_mode(); on_status_timer(); };
    act.shutter = [] { std::fprintf(stderr, "[vhs] shutter: photo comes in M2\n"); };
    act.scenes = [] { std::fprintf(stderr, "[vhs] scenes: the list comes in M4\n"); };
    act.settings = [] { std::fprintf(stderr, "[vhs] settings: the page comes in M6\n"); };
    capture_ = std::make_unique<CapturePage>(page_->root(), geo_, act);

    std::fprintf(stderr, "[vhs] page %dx%d, target %d, picture box %dx%d, %s, %s, refresh %d ms, camera %s, TurboJPEG %s\n",
                 layout_.w, layout_.h, in.target, geo_.picture.w, geo_.picture.h, pix_name(opt.fmt),
                 opt.fastdct ? "fastdct" : "accurate DCT", static_cast<int>(LV_DEF_REFR_PERIOD),
                 opt.fake ? "fake" : (opt.device.empty() ? "discovery" : opt.device.c_str()),
                 TurboJpeg::get().available() ? TurboJpeg::get().path().c_str() : TurboJpeg::get().error().c_str());
    ctl_->start();
    const char *demo = std::getenv("VHS_DEMO_NOTICE");
    if (demo && demo[0]) {
        const char *red = std::getenv("VHS_DEMO_NOTICE_RED");
        ctl_->push_notice(Notice{demo, red && red[0] == '1'});
    }

    if (LV_EVENT_KEYBOARD != 0)
        lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    lv_display_add_event_cb(display_, display_cb, LV_EVENT_ALL, this);
    frame_timer_ = lv_timer_create(frame_timer_cb, LV_DEF_REFR_PERIOD, this);
    status_timer_ = lv_timer_create(status_timer_cb, 250, this);
    on_status_timer();
}

VhsApp::~VhsApp()
{
    if (frame_timer_) lv_timer_delete(frame_timer_);
    if (status_timer_) lv_timer_delete(status_timer_);
    if (display_) lv_display_remove_event_cb_with_user_data(display_, display_cb, this);
    if (screen_ && lv_obj_is_valid(screen_) && LV_EVENT_KEYBOARD != 0)
        lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    capture_.reset(); /* the image lets go of the picture first */
    page_.reset();
    if (ctl_) ctl_->stop();
    ctl_.reset();
}

void VhsApp::frame_timer_cb(lv_timer_t *t)
{
    static_cast<VhsApp *>(lv_timer_get_user_data(t))->on_frame_timer();
}

void VhsApp::status_timer_cb(lv_timer_t *t)
{
    static_cast<VhsApp *>(lv_timer_get_user_data(t))->on_status_timer();
}

void VhsApp::keyboard_cb(lv_event_t *e)
{
    auto *self = static_cast<VhsApp *>(lv_event_get_user_data(e));
    const auto *item = static_cast<const key_item *>(lv_event_get_param(e));
    if (item) self->on_key(*item);
}

void VhsApp::display_cb(lv_event_t *e)
{
    static_cast<VhsApp *>(lv_event_get_user_data(e))->on_display_event(e);
}

void VhsApp::on_display_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        refr_start_us_ = mono_us();
    } else if (code == LV_EVENT_REFR_READY && handoff_pending_ && refr_start_us_ >= handoff_us_) {
        /* the first redraw that started after the handoff carried the picture */
        const std::int64_t now = mono_us();
        handoff_pending_ = false;
        ctl_->pipeline().note_refresh(static_cast<double>(now - handoff_us_) / 1000.0,
                                      static_cast<double>(now - refr_start_us_) / 1000.0,
                                      static_cast<double>(now - handoff_ts_us_) / 1000.0);
    }
}

void VhsApp::on_frame_timer()
{
    if (cdk::hidden() || !ctl_->running()) return;
    Pipeline &pipe = ctl_->pipeline();
    if (!pipe.pictures().take()) return;
    const Picture &p = pipe.pictures().front();
    capture_->show_picture(p);
    pipe.note_shown(p);
    handoff_us_ = mono_us();
    handoff_ts_us_ = p.ts_us;
    handoff_pending_ = true;
}

void VhsApp::on_status_timer()
{
    /* hidden by the launcher: the camera is released; shown: it reopens (brief 3.3 rule 5) */
    if (cdk::hidden()) {
        if (ctl_->running()) {
            std::fprintf(stderr, "[vhs] hidden: camera released\n");
            ctl_->stop();
        }
        return;
    }
    if (!ctl_->running()) {
        std::fprintf(stderr, "[vhs] shown: camera reopened\n");
        ctl_->start();
    }
    capture_->update(ctl_->step(mono_us() / 1000));
}

void VhsApp::demo_next_notice()
{
    demo_index_ = (demo_index_ + 1) % static_cast<int>(sizeof kDemoNotices / sizeof kDemoNotices[0]);
    ctl_->dismiss_notice(mono_us() / 1000);
    ctl_->push_notice(kDemoNotices[demo_index_]);
    on_status_timer();
}

void VhsApp::on_key(const key_item &item)
{
    if (item.key_state != KBD_KEY_PRESSED) return;
    if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO)) return;
    const std::uint32_t code = item.key_code;
    char ch = 0;
    if (code == 0 && item.utf8[0] && !item.utf8[1]) {
        ch = item.utf8[0];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    auto is = [code, ch](std::uint32_t k, char c) { return code == k || (code == 0 && ch == c); };
    lv_group_t *g = cdk::input_group();
    if (code == KEY_ESC) {
        cdk::request_quit(0);
    } else if (is(KEY_TAB, '\t')) {
        ctl_->toggle_capture_mode();
        on_status_timer();
    } else if (demo_notices_ && is(KEY_N, 'n')) {
        demo_next_notice();
    } else if (is(KEY_SPACE, ' ') || is(KEY_R, 'r') || is(KEY_S, 's')) {
        std::fprintf(stderr, "[vhs] key %s: comes with a later milestone\n", code == KEY_SPACE ? "Space" : code == KEY_R ? "R" : "S");
    } else if (g && (code == KEY_UP || code == KEY_LEFT)) {
        lv_group_focus_prev(g);
    } else if (g && (code == KEY_DOWN || code == KEY_RIGHT)) {
        lv_group_focus_next(g);
    }
}

} // namespace ui
} // namespace vhs
