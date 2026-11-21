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
#include <utility>

namespace vhs {
namespace ui {
namespace {

/* the canvas's notices, in its order (Notices-480x320): VHS_DEMO_NOTICES=1 and N walk them (the scenes) */
const Notice kDemoNotices[] = {
    {kNoticeUnplugged, false}, {kNoticeBusy, false},     {kNoticeNoAccess, false}, {kNoticePower, false},
    {kNoticeDiskFull, true},   {kNoticeNoSound, false},  {kNoticeDrops, false},
};

/* not on the canvas: provisional wording (brief 12.4 item 3) */
constexpr const char *kDeletePhoto = "Delete this photo?";
constexpr const char *kDeleteScene = "Delete this scene?";
constexpr const char *kDeleteKeys = "Y = Delete, Enter or Esc = Cancel";
constexpr const char *kRenameTitle = "Rename";
constexpr const char *kRenameLabel = "Name";

std::int64_t now_ms()
{
    return mono_us() / 1000;
}

} // namespace

VhsApp::VhsApp(lv_obj_t *screen) : screen_(screen)
{
    /* the headless harness sends stderr to a file (fully buffered) and ends the run without flushing it: whole lines
     * there, so a scene's log keeps its last lines (the fake camera only) */
    if (std::getenv("VHS_FAKE")) std::setvbuf(stderr, nullptr, _IOLBF, 0);
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
    page_ = std::make_unique<cp0::ui::Page>(screen_, layout_, cp0::ui::add_title, [this] { on_back(); });
    cp0::ui::StatusStrip::add(page_->header());
    page_->set_title({"VHS"});
    CaptureLayoutIn in;
    in.w = layout_.w;
    in.h = layout_.h;
    in.body_y = layout_.body_y;
    in.target = m->tok[CP0_TOK_TARGET];
    in.square = m->cls == CP0_UI_SQUARE;
    geo_ = capture_layout(in);
    rgeo_ = review_layout(in);
    ScenesLayoutIn sin;
    sin.base = in;
    sin.row = m->tok[CP0_TOK_ROW_H];
    sin.list_row = m->tok[CP0_TOK_LIST_ROW_H];
    sgeo_ = scenes_layout(sin);
    pgeo_ = playback_layout(in, m->tok[CP0_TOK_ROW_H]);

    ControllerConfig cc = controller_config(opt);
    cc.box_w = geo_.picture.w;
    cc.box_h = geo_.picture.h;
    cc.ui_tid = current_tid();
    ctl_ = std::make_unique<Controller>(cc);

    CaptureActions act;
    act.toggle = [this] { /* "Photo now" in its place while recording */
        if (ctl_->recording()) ctl_->photo_now(now_ms());
        else ctl_->toggle_capture_mode();
        on_status_timer();
    };
    act.shutter = [this] {
        ctl_->shutter(now_ms());
        on_status_timer();
    };
    act.scenes = [this] {
        if (!ctl_->recording()) open_scenes(); /* dimmed while recording */
    };
    act.settings = [this] {
        if (!ctl_->recording()) open_settings(); /* dimmed while recording */
    };
    act.cancel = [this] {
        ctl_->cancel_countdown();
        on_status_timer();
    };
    act.camera = [this] { /* 0.1.2: the camera chip: the next camera (refused while recording: a toast says why) */
        ctl_->next_camera();
        on_status_timer();
    };
    act.exposure = [this](int d) { /* 0.1.2: the Exposure row: a stop darker / brighter, 0 = Auto */
        if (d == 0) ctl_->exposure_set(0);
        else ctl_->exposure_step(d);
        on_status_timer();
    };
    capture_ = std::make_unique<CapturePage>(page_->root(), geo_, act);
    ScenesActions sa;
    sa.open = [this](int i) { scene_open(i); };
    sa.hold = [this](int i) { open_sheet(i); };
    sa.filter = [this](SceneFilter f) {
        ctl_->scenes_set_filter(f);
        on_status_timer();
    };
    scenes_ = std::make_unique<ScenesPage>(page_->root(), sgeo_, sa);
    sheet_ = std::make_unique<SceneSheet>(page_->root(), sgeo_, [this](SheetAction a) { sheet_act(a); },
                                          [this] { sheet_->close(); });
    copy_ = std::make_unique<CopyDialog>(page_->root(), sgeo_, in.target, [this] { ctl_->copy_cancel(); },
                                         [this](int i) { ctl_->copy_choose(i); }, [this] { ctl_->copy_dismiss(); });
    ReviewActions ra;
    ra.prev = [this] { ctl_->review_move(-1); };
    ra.next = [this] { ctl_->review_move(1); };
    ra.del = [this] { ask_delete(); };
    review_ = std::make_unique<ReviewPage>(page_->root(), rgeo_, ra);
    PlaybackActions pa;
    pa.toggle = [this] {
        ctl_->playback_toggle();
        on_status_timer();
    };
    pa.minus = [this] {
        ctl_->playback_volume(-1);
        on_status_timer();
    };
    pa.plus = [this] {
        ctl_->playback_volume(1);
        on_status_timer();
    };
    pa.seek = [this](std::int64_t ms) {
        ctl_->playback_seek_to(ms);
        on_status_timer();
    };
    playback_ = std::make_unique<PlaybackPage>(page_->root(), pgeo_, pa);
    settings_ = std::make_unique<SettingsPage>(page_->root(), geo_, sgeo_.toast, sgeo_.toast_px,
                                               [this](const std::string &id) { settings_activate(id); }, [this] { close_settings(); });

    std::fprintf(stderr,
                 "[vhs] page %dx%d (%s), target %d, picture box %dx%d, %s, %s, refresh %d ms, camera %s, TurboJPEG %s\n",
                 layout_.w, layout_.h, geo_.column ? "column" : (geo_.square ? "square row" : "portrait rows"), in.target,
                 geo_.picture.w, geo_.picture.h, pix_name(opt.fmt), opt.fastdct ? "fastdct" : "accurate DCT",
                 static_cast<int>(LV_DEF_REFR_PERIOD), opt.fake ? "fake" : (opt.device.empty() ? "discovery" : opt.device.c_str()),
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
    field_.reset();
    text_dlg_.reset();
    dlg_.reset();
    capture_.reset(); /* the images let go of the pictures first */
    scenes_.reset();
    sheet_.reset();
    copy_.reset();
    review_.reset();
    playback_.reset();
    settings_.reset();
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
    if (cdk::hidden()) return;
    if (view_ == View::Scenes) {
        scenes_->tick(now_ms()); /* a held row */
        return;
    }
    if (view_ == View::Playback) {
        if (auto pic = ctl_->playback_picture()) playback_->show_picture(*pic); /* the presenter's picture due now */
        return;
    }
    if (view_ != View::Capture || !ctl_->running()) return;
    capture_->tick(now_ms()); /* the REC dot while a scene starts */
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
    /* hidden by the launcher: the camera is released (a recording finished first); shown: it reopens (brief 13) */
    if (cdk::hidden()) {
        /* brief 13 step 4; the view's "playing" is refreshed by step(), which does not run while hidden: ask the
         * controller (a toggle here paused at the first tick and played again at the next) */
        if (view_ == View::Playback && ctl_->playback_pause()) std::fprintf(stderr, "[vhs] hidden: playback paused\n");
        if (ctl_->running()) {
            std::fprintf(stderr, "[vhs] hidden: camera released\n");
            ctl_->stop();
        }
        return;
    }
    if (view_ == View::Capture && !ctl_->running()) {
        std::fprintf(stderr, "[vhs] camera opened\n");
        ctl_->start();
    }
    const Snapshot s = ctl_->step(now_ms());
    if (view_ == View::Review) review_->update(ctl_->review(), ctl_->review_picture());
    else if (view_ == View::Scenes) {
        scenes_->update(ctl_->scenes(), s.notice);
        update_copy();
    }
    else if (view_ == View::Playback) playback_->update(ctl_->playback(), s.notice);
    else if (view_ == View::Settings) settings_->set_notice(s.notice);
    else if (view_ == View::Capture) capture_->update(s);
}

/* ------------------------------------------------------------------------------------------------ the scenes */

void VhsApp::open_scenes()
{
    if (view_ == View::Scenes) return;
    ctl_->stop(); /* the camera is open only while the Capture page shows */
    view_ = View::Scenes;
    capture_->show_page(false);
    scenes_->show(true);
    page_->set_title({"Scenes"});
    ctl_->scenes_open(sgeo_.thumb.w, sgeo_.thumb.h, sgeo_.portrait);
    on_status_timer();
    std::fprintf(stderr, "[vhs] scenes: opened (the camera released)\n");
}

void VhsApp::close_scenes()
{
    if (view_ != View::Scenes) return;
    sheet_->close();
    ctl_->scenes_close();
    scenes_->show(false);
    view_ = View::Capture;
    capture_->show_page(true);
    page_->set_title({"VHS"});
    ctl_->recount();
    on_status_timer(); /* the camera opens again */
    capture_->focus_first();
}

const SceneRow *VhsApp::scene_at(int index) const
{
    const ScenesView &v = ctl_->scenes();
    if (index < 0 || index >= static_cast<int>(v.rows.size())) return nullptr;
    return &v.rows[static_cast<std::size_t>(index)];
}

void VhsApp::scene_open(int index)
{
    const SceneRow *r = scene_at(index);
    if (!r) return;
    sheet_->close();
    if (r->e.kind == SceneKind::Photo) {
        /* on the next LVGL cycle: the Enter that opened it must not also press the review's focused button */
        pending_review_ = r->e.name;
        lv_async_call(
            [](void *p) {
                auto *self = static_cast<VhsApp *>(p);
                if (self->view_ == View::Scenes && !self->pending_review_.empty()) self->open_review(self->pending_review_);
                self->pending_review_.clear();
            },
            this);
    } else {
        /* on the next LVGL cycle (the Enter that opened it must not press the page's focused button) */
        pending_play_ = r->e.name;
        lv_async_call(
            [](void *p) {
                auto *self = static_cast<VhsApp *>(p);
                if (self->view_ == View::Scenes && !self->pending_play_.empty()) self->open_playback(self->pending_play_);
                self->pending_play_.clear();
            },
            this);
    }
}

void VhsApp::open_sheet(int index)
{
    const SceneRow *r = scene_at(index);
    if (!r) return;
    sheet_name_ = r->e.name;
    sheet_photo_ = r->e.kind == SceneKind::Photo;
    scenes_->select(index);
    const std::size_t dot = r->e.name.find_last_of('.');
    /* the Square class's header detail: the row's meta without its last part ("12:34 - 1.4 GB", not "... - MJPEG 720p") */
    std::string detail = r->meta;
    const std::size_t first = detail.find(" - ");
    if (first != std::string::npos) {
        const std::size_t second = detail.find(" - ", first + 3);
        if (second != std::string::npos) detail.resize(second);
    }
    sheet_->open(dot == std::string::npos ? r->e.name : r->e.name.substr(0, dot), sheet_photo_, r->e.locked, detail); /* canvas: the stem */
}

void VhsApp::sheet_act(SheetAction a)
{
    const int index = scenes_->selected();
    switch (a) {
    case SheetAction::Play: scene_open(index); break;
    case SheetAction::Rename: ask_rename(); break;
    case SheetAction::Lock:
        sheet_->close();
        if (const SceneRow *r = scene_at(index)) ctl_->scene_toggle_lock(r->e.name);
        break;
    case SheetAction::Copy:
        sheet_->close();
        if (const SceneRow *r = scene_at(index)) ctl_->copy_request(r->e.name);
        on_status_timer();
        break;
    case SheetAction::Notes: break; /* hidden until it is built (owner, G6: a P2 candidate) */
    case SheetAction::Delete: ask_delete(); break;
    }
}

void VhsApp::scenes_key(std::uint32_t code, char ch, bool ctrl)
{
    auto is = [code, ch](std::uint32_t k, char c) { return code == k || (code == 0 && ch == c); };
    if (copy_->shown()) { /* the copy dialog is modal: the drive list's keys, or its Cancel (Enter, as the Dialog) */
        if (copy_->picking()) {
            if (code == KEY_UP) copy_->move(-1);
            else if (code == KEY_DOWN) copy_->move(1);
            else if (code == KEY_ENTER || code == KEY_KPENTER) copy_->choose();
        } else if (code == KEY_ENTER || code == KEY_KPENTER) {
            ctl_->copy_cancel();
        }
        on_status_timer();
        return;
    }
    if (sheet_->shown()) {
        if (code == KEY_UP) sheet_->move(-1);
        else if (code == KEY_DOWN) sheet_->move(1);
        else if (code == KEY_ENTER || code == KEY_KPENTER) sheet_->choose();
        else if (is(KEY_N, 'n')) sheet_act(SheetAction::Rename);
        else if (is(KEY_L, 'l')) sheet_act(SheetAction::Lock);
        else if (is(KEY_U, 'u')) sheet_act(SheetAction::Copy);
        else if (code == KEY_DELETE) sheet_act(SheetAction::Delete);
        return;
    }
    const SceneRow *r = scene_at(scenes_->selected());
    if (ctrl && is(KEY_O, 'o')) {
        open_sheet(scenes_->selected());
    } else if (code == KEY_UP) {
        scenes_->move(-1);
    } else if (code == KEY_DOWN) {
        scenes_->move(1);
    } else if (code == KEY_ENTER || code == KEY_KPENTER) {
        scene_open(scenes_->selected());
    } else if (is(KEY_TAB, '\t')) {
        const ScenesView &v = ctl_->scenes();
        ctl_->scenes_set_filter(static_cast<SceneFilter>((static_cast<int>(v.filter) + 1) % 3));
    } else if (r && is(KEY_N, 'n')) {
        sheet_name_ = r->e.name;
        sheet_photo_ = r->e.kind == SceneKind::Photo;
        ask_rename();
    } else if (r && is(KEY_L, 'l')) {
        ctl_->scene_toggle_lock(r->e.name);
    } else if (r && is(KEY_U, 'u')) {
        ctl_->copy_request(r->e.name);
    } else if (r && code == KEY_DELETE) {
        sheet_name_ = r->e.name;
        sheet_photo_ = r->e.kind == SceneKind::Photo;
        ask_delete();
    }
    on_status_timer();
}

/* The copy dialog follows the core's steps (app/copy_flow.hpp): the drive list, the progress, nothing. */
void VhsApp::update_copy()
{
    const CopyView &v = ctl_->copy_view();
    if (v.stage == CopyView::Stage::Choosing) {
        if (!copy_->picking()) {
            std::vector<std::pair<std::string, std::string>> rows;
            for (const CopyDriveRow &r : v.drives) rows.emplace_back(r.label, r.value);
            copy_->show_drives(rows);
        }
    } else if (v.stage == CopyView::Stage::Copying) {
        copy_->show_progress(v.title, v.text, v.fraction, v.progress);
    } else if (copy_->shown()) {
        copy_->hide();
    }
}

/* ------------------------------------------------------------------------------------------------ settings */

namespace {

std::string split_text(const Settings &s)
{
    if (!s.split) return "Off";
    return s.split_mib % 1024 == 0 ? "at " + std::to_string(s.split_mib / 1024) + " GiB" : "at " + std::to_string(s.split_mib) + " MiB";
}

std::string timer_text(int s)
{
    return s > 0 ? std::to_string(s) + " s" : std::string("Off");
}

} // namespace

void VhsApp::open_settings()
{
    if (view_ != View::Capture || ctl_->recording()) return; /* brief 11: nothing changes while recording */
    ctl_->stop();                                            /* the camera is open only while the Capture page shows */
    view_ = View::Settings;
    capture_->show_page(false);
    ctl_->settings_open();
    ctl_->refresh_cameras(); /* 0.1.2: the Camera row's list */
    settings_->set_rows(settings_rows());
    settings_->show(true);
    page_->set_title({"Settings"});
    std::fprintf(stderr, "[vhs] settings: opened (the camera released)\n");
}

void VhsApp::close_settings()
{
    if (view_ != View::Settings) return;
    close_text();
    settings_->close_picker();
    settings_->show(false);
    ctl_->settings_close();
    view_ = View::Capture;
    capture_->show_page(true);
    page_->set_title({"VHS"});
    on_status_timer(); /* the camera opens again, with the new settings */
    capture_->focus_first();
}

std::vector<SettingsItem> VhsApp::settings_rows()
{
    const Settings &s = ctl_->settings();
    const Selection sel = ctl_->selection();
    const PhotoMode pm = s.have_photo_mode ? s.photo_mode : (sel.ok ? sel.mode : default_photo_mode(ctl_->soc()));
    const Size ps = s.photo_size.valid() ? s.photo_size : (sel.ok ? Size{sel.photo.w, sel.photo.h} : Size());
    const Size vs = s.video_size.valid() ? s.video_size : (sel.ok && sel.video.valid() ? Size{sel.video.w, sel.video.h} : Size{1280, 720});
    std::string out = "Automatic";
    for (const AudioOutputRow &r : ctl_->output_rows())
        if (r.chosen) out = r.label;
    /* 0.1.2: the camera in use (else the remembered one) and the sound input */
    std::string cam = "No camera";
    {
        /* the remembered camera (the one just chosen, while the camera is released), else the one in use */
        std::string l = ctl_->camera_label_of(s.camera_id);
        if (l.empty()) l = ctl_->camera_label_of(ctl_->current_camera_key());
        if (!l.empty()) cam = l;
        else if (!ctl_->cameras().empty()) cam = ctl_->cameras().front().label;
        else if (!s.camera_name.empty()) cam = s.camera_name + " (not plugged in)";
    }
    std::string snd = "Automatic";
    for (const SoundRow &r : ctl_->sound_rows())
        if (r.chosen) snd = r.label;
    return {
        {"camera", "Camera", cam},
        {"folder", "Folder", s.folder},
        {"prefix", "Name prefix", s.prefix},
        {"photo", "Photo", pm == PhotoMode::Light ? "Light" : "Instant"},
        {"psize", "Photo size", ps.valid() ? photo_size_text(ps) : std::string("Largest")},
        {"vsize", "Video size", size_text(vs) + ", MJPEG"},
        {"timer", "Self-timer", timer_text(s.self_timer_s)},
        {"split", "Split long scenes", split_text(s)},
        {"sound", "Sound input", snd},
        {"output", "Playback output", out, false, false, false},
    };
}

void VhsApp::settings_activate(const std::string &id)
{
    const Settings cur = ctl_->settings();
    const Selection sel = ctl_->selection();
    const std::size_t colon = id.find(':');
    if (colon == std::string::npos) { /* a main row: its pick list or the text box */
        std::vector<SettingsItem> c;
        std::string head;
        if (id == "folder") {
            head = "FOLDER";
            c.push_back({"folder:" + cur.folder, cur.folder, "", true});
            if (cur.folder != "~/VHS") c.push_back({"folder:~/VHS", "~/VHS", "", false}); /* Cassette's rule */
        } else if (id == "prefix") {
            ask_text("Name prefix", "Prefix", cur.prefix, [this](const std::string &t) {
                Settings s = ctl_->settings();
                /* the rename's reasons (Cassette's prefix rule), after "Not saved: " (provisional) */
                const char *why = t.empty() ? "the name is empty"
                                  : t.size() > 24 ? "up to 24 characters"
                                  : !valid_prefix(t) ? "use letters, digits, space, - and _" : nullptr;
                if (why) {
                    ctl_->push_notice(Notice{std::string("Not saved: ") + why + ".", false});
                    return;
                }
                s.prefix = t;
                ctl_->settings_apply(s);
                settings_->set_rows(settings_rows());
            });
            return;
        } else if (id == "photo") {
            head = "PHOTO";
            const bool light = (cur.have_photo_mode ? cur.photo_mode : sel.mode) == PhotoMode::Light;
            c.push_back({"photo:instant", "Instant", "", !light});
            c.push_back({"photo:light", "Light", "", light});
        } else if (id == "psize" || id == "vsize") {
            head = id == "psize" ? "PHOTO SIZE" : "VIDEO SIZE";
            const Size now = id == "psize" ? (cur.photo_size.valid() ? cur.photo_size : Size{sel.photo.w, sel.photo.h})
                                           : (cur.video_size.valid() ? cur.video_size : Size{sel.video.w, sel.video.h});
            /* 0.1.2: the Photo size lists every MJPEG size the camera offers (3264x2448 (8 MP) at 15 fps too); the Video size
             * the sizes it offers at 30 fps */
            for (const Size &z : (id == "psize" ? sel.photo_sizes : sel.sizes))
                c.push_back({id + ":" + size_text(z), id == "vsize" ? size_text(z) + ", MJPEG" : photo_size_text(z), "", z == now});
            if (c.empty() && now.valid())
                c.push_back({id + ":" + size_text(now), id == "vsize" ? size_text(now) + ", MJPEG" : photo_size_text(now), "", true});
        } else if (id == "timer") {
            head = "SELF-TIMER";
            for (int t : {0, 3, 5, 10}) c.push_back({"timer:" + std::to_string(t), timer_text(t), "", t == cur.self_timer_s});
        } else if (id == "split") {
            head = "SPLIT LONG SCENES";
            Settings on = cur;
            on.split = true;
            c.push_back({"split:on", split_text(on), "", cur.split});
            c.push_back({"split:off", "Off", "", !cur.split});
        } else if (id == "camera") {
            head = "CAMERA";
            std::string now = cur.camera_id;
            if (ctl_->camera_label_of(now).empty()) now = ctl_->current_camera_key();
            for (const CameraEntry &e : ctl_->cameras())
                c.push_back({"camera:" + e.key, e.label, e.port.empty() ? std::string() : "USB " + e.port, e.key == now});
        } else if (id == "sound") {
            head = "SOUND INPUT";
            for (const SoundRow &r : ctl_->sound_rows()) c.push_back({"sound:" + r.id, r.label, r.value, r.chosen});
        } else if (id == "output") {
            head = "PLAYBACK OUTPUT";
            for (const AudioOutputRow &r : ctl_->output_rows()) c.push_back({"out:" + r.key, r.label, r.value, r.chosen, r.dimmed});
            if (c.empty()) c.push_back({"out:auto", "Automatic", "", true});
        }
        if (!c.empty()) settings_->open_picker(head, c);
        return;
    }
    /* a choice: applied, and the list closes (Vinyl's pick list) */
    const std::string key = id.substr(0, colon), v = id.substr(colon + 1);
    if (key == "camera") { /* 0.1.2: remembered at once; the camera opens again when Settings closes */
        ctl_->select_camera(v);
        settings_->close_picker();
        settings_->set_rows(settings_rows());
        return;
    }
    Settings s = cur;
    if (key == "sound") s.sound_input = v == "auto" ? std::string() : v;
    if (key == "folder") s.folder = v;
    else if (key == "photo") {
        s.have_photo_mode = true;
        s.photo_mode = v == "light" ? PhotoMode::Light : PhotoMode::Instant;
    } else if (key == "psize") parse_size(v, s.photo_size);
    else if (key == "vsize") parse_size(v, s.video_size);
    else if (key == "timer") s.self_timer_s = std::atoi(v.c_str());
    else if (key == "split") s.split = v == "on";
    if (key == "out") ctl_->set_output(v);
    else ctl_->settings_apply(s);
    settings_->close_picker();
    settings_->set_rows(settings_rows());
}

/* ------------------------------------------------------------------------------------------------ playback */

void VhsApp::open_playback(const std::string &name)
{
    if (view_ != View::Scenes) return;
    sheet_->close();
    scenes_->show(false);
    view_ = View::Playback;
    playback_->show(true);
    page_->set_title({"Scene"});
    ctl_->playback_open(name, pgeo_.picture.w, pgeo_.picture.h);
    playback_->focus_first();
    on_status_timer();
    std::fprintf(stderr, "[vhs] playback: opened %s\n", name.c_str());
}

void VhsApp::close_playback()
{
    if (view_ != View::Playback) return;
    ctl_->playback_close();
    playback_->show(false);
    view_ = View::Scenes;
    scenes_->show(true);
    page_->set_title({"Scenes"});
    on_status_timer();
}

/* ------------------------------------------------------------------------------------------------ the review */

void VhsApp::open_review(const std::string &start)
{
    if (view_ == View::Review) return;
    ctl_->stop();
    review_from_scenes_ = view_ == View::Scenes;
    if (review_from_scenes_) scenes_->show(false);
    else capture_->show_page(false);
    view_ = View::Review;
    review_->show(true);
    page_->set_title({"Photo"});
    ctl_->review_open(rgeo_.picture.w, rgeo_.picture.h, start);
    review_->focus_first();
    std::fprintf(stderr, "[vhs] review: opened (the camera released)\n");
}

void VhsApp::close_review()
{
    if (view_ != View::Review) return;
    close_dialog();
    ctl_->review_close();
    review_->show(false);
    if (review_from_scenes_) {
        view_ = View::Scenes;
        scenes_->show(true);
        page_->set_title({"Scenes"});
        ctl_->scenes_open(sgeo_.thumb.w, sgeo_.thumb.h, sgeo_.portrait); /* a photo may have gone */
        on_status_timer();
        return;
    }
    view_ = View::Capture;
    capture_->show_page(true);
    page_->set_title({"VHS"});
    on_status_timer(); /* the camera opens again */
    capture_->focus_first();
}

/* ------------------------------------------------------------------------------------------------ dialogs */

void VhsApp::dialog_cancel_cb(void *p)
{
    static_cast<VhsApp *>(p)->dialog_choose(false);
}

void VhsApp::dialog_yes_cb(void *p)
{
    static_cast<VhsApp *>(p)->dialog_choose(true);
}

void VhsApp::ask_delete()
{
    if (dlg_ || text_dlg_) return;
    std::string name;
    bool photo = true;
    if (view_ == View::Review) {
        if (ctl_->review().count == 0) return;
        name = ctl_->review().name;
    } else if (view_ == View::Scenes) {
        name = sheet_name_;
        photo = sheet_photo_;
    } else {
        return;
    }
    sheet_->close();
    on_dialog_ = [this, name](bool yes) {
        if (!yes) return;
        if (view_ == View::Review) ctl_->review_delete();
        else ctl_->scene_delete(name);
    };
    /* the toolkit's dialog, as Vinyl's "Clear the queue?": Enter and Esc cancel, Y or the Delete button deletes */
    cp0::ui::DialogSpec spec;
    spec.title = photo ? kDeletePhoto : kDeleteScene;
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Primary, [this] { lv_async_call(dialog_cancel_cb, this); }});
    spec.buttons.push_back({"Delete", cp0::ui::ButtonRole::Destructive, [this] { lv_async_call(dialog_yes_cb, this); }});
    spec.page = page_.get();
    dlg_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    dlg_->add_text(kDeleteKeys);
    dlg_->relayout();
    if (lv_group_t *g = cdk::input_group()) {
        lv_group_add_obj(g, dlg_->focus_object());
        lv_group_focus_obj(dlg_->focus_object());
    }
}

void VhsApp::close_dialog()
{
    if (!dlg_) return;
    dlg_.reset();
    if (view_ == View::Review) review_->focus_first();
}

void VhsApp::dialog_choose(bool yes)
{
    if (!dlg_) return;
    auto cb = std::move(on_dialog_);
    on_dialog_ = nullptr;
    close_dialog();
    if (cb) cb(yes);
}

void VhsApp::ask_rename()
{
    if (dlg_ || text_dlg_ || sheet_name_.empty()) return;
    sheet_->close();
    SceneName n;
    const std::string name = sheet_name_;
    const std::string prefix = parse_scene_name(name, n) ? n.prefix : std::string();
    ask_text(kRenameTitle, kRenameLabel, prefix, [this, name](const std::string &text) { ctl_->scene_rename(name, text); });
    std::fprintf(stderr, "[vhs] scenes: rename %s\n", name.c_str());
}

void VhsApp::ask_text(const std::string &title, const std::string &label, const std::string &text,
                      std::function<void(const std::string &)> done)
{
    if (dlg_ || text_dlg_) return;
    on_text_ = std::move(done);
    /* Cassette's text box: the toolkit's Dialog with a TextField (the keyboard policy applies: osk_policy) */
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { close_text(); }});
    spec.buttons.push_back({"Save", cp0::ui::ButtonRole::Primary, [this] {
                                const std::string t = field_ ? field_->text() : std::string();
                                auto cb = on_text_;
                                close_text();
                                if (cb) cb(t);
                            }});
    text_dlg_ = std::make_unique<cp0::ui::Dialog>(page_->root(), page_->layout(), spec);
    cp0::ui::TextFieldSpec fs;
    fs.label = label;
    fs.text = text;
    fs.max_bytes = 24;
    cp0::ui::TextFieldEvents fe;
    fe.submit = [this](const std::string &t) {
        auto cb = on_text_;
        close_text();
        if (cb) cb(t);
    };
    fe.escape = [this] { close_text(); };
    fe.back = [this] { close_text(); };
    field_ = std::make_unique<cp0::ui::TextField>(text_dlg_->body(), text_dlg_->field_layout(), fs, fe);
    text_dlg_->relayout();
    if (lv_group_t *g = cdk::input_group()) {
        lv_group_add_obj(g, field_->focus_object());
        lv_group_focus_obj(field_->focus_object());
    }
    field_->begin_edit();
    /* the page goes while the box is up: nothing of it may lie under the toolkit's keyboard (Cassette) */
    if (view_ == View::Scenes) scenes_->show(false);
    if (view_ == View::Settings) settings_->show(false);
}

void VhsApp::close_text()
{
    if (!text_dlg_) return;
    /* never delete the box inside its own event: hand the objects to the next LVGL cycle (Cassette) */
    auto *pending = new std::pair<std::unique_ptr<cp0::ui::TextField>, std::unique_ptr<cp0::ui::Dialog>>(std::move(field_),
                                                                                                       std::move(text_dlg_));
    if (pending->second) lv_obj_add_flag(pending->second->root(), LV_OBJ_FLAG_HIDDEN);
    if (pending->first && pending->first->editing()) pending->first->end_edit();
    lv_async_call(
        [](void *p) { delete static_cast<std::pair<std::unique_ptr<cp0::ui::TextField>, std::unique_ptr<cp0::ui::Dialog>> *>(p); },
        pending);
    on_text_ = nullptr;
    if (view_ == View::Scenes) scenes_->show(true);
    if (view_ == View::Settings) settings_->show(true);
}

/* ------------------------------------------------------------------------------------------------ keys */

void VhsApp::on_back()
{
    if (text_dlg_) {
        close_text();
    } else if (dlg_) {
        dialog_choose(false);
    } else if (view_ == View::Review) {
        close_review();
    } else if (view_ == View::Settings) {
        if (settings_->in_picker()) settings_->close_picker(); /* Esc closes a pick list unchanged */
        else close_settings();
    } else if (view_ == View::Playback) {
        close_playback(); /* brief 10: Esc returns to Scenes and stops playback */
    } else if (view_ == View::Scenes) {
        if (copy_->shown() && copy_->picking()) ctl_->copy_dismiss(); /* the drive list closes, no copy */
        else if (copy_->shown()) ctl_->copy_cancel();                 /* Esc presses Cancel (the toolkit's Dialog) */
        else if (sheet_->shown()) sheet_->close();
        else close_scenes();
        if (view_ == View::Scenes) on_status_timer();
    } else if (ctl_->cancel_countdown()) {
        on_status_timer(); /* Esc during the countdown cancels it (D30, brief 12.4 item 1) */
    } else if (ctl_->recording()) {
        /* brief 12.4 item 1 (provisional): finish the scene, then quit (Cassette) */
        std::fprintf(stderr, "[vhs] Esc while recording: the scene is finished, then VHS quits\n");
        ctl_->finish_recording(RecStop::User);
        cdk::request_quit(0);
    } else {
        cdk::request_quit(0);
    }
}

void VhsApp::demo_next_notice()
{
    demo_index_ = (demo_index_ + 1) % static_cast<int>(sizeof kDemoNotices / sizeof kDemoNotices[0]);
    ctl_->dismiss_notice(now_ms());
    ctl_->push_notice(kDemoNotices[demo_index_]);
    on_status_timer();
}

void VhsApp::on_key(const key_item &item)
{
    if (item.key_state != KBD_KEY_PRESSED) return;
    if (text_dlg_) return; /* the text field takes its keys itself */
    const bool ctrl = (item.mods & KBD_MOD_CTRL) != 0;
    if (item.mods & (KBD_MOD_ALT | KBD_MOD_LOGO)) return;
    const std::uint32_t code = item.key_code;
    char ch = 0;
    if (code == 0 && item.utf8[0] && !item.utf8[1]) {
        ch = item.utf8[0];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    auto is = [code, ch](std::uint32_t k, char c) { return code == k || (code == 0 && ch == c); };
    if (dlg_) { /* the dialog takes Enter and Esc itself (its focus object); Y deletes */
        if (is(KEY_Y, 'y')) dialog_choose(true);
        else if (code == KEY_ESC) dialog_choose(false);
        return;
    }
    /* Settings: the toolkit's list takes every key itself (its focus object: Esc closes a pick list, then the page) */
    if (view_ == View::Settings) return;
    if (code == KEY_ESC) {
        on_back();
        return;
    }
    if (view_ == View::Scenes) {
        scenes_key(code, ch, ctrl);
        return;
    }
    if (view_ == View::Settings) return; /* the list takes its keys itself (its focus object) */
    if (view_ == View::Playback) {
        /* brief 10: Left / Right seek 5 s, Shift 30 s; Space pauses / plays (provisional) */
        const bool shift = (item.mods & KBD_MOD_SHIFT) != 0;
        if (code == KEY_LEFT || code == KEY_RIGHT) {
            const std::int64_t step = shift ? 30000 : 5000;
            ctl_->playback_seek_by(code == KEY_RIGHT ? step : -step);
        } else if (is(KEY_SPACE, ' ') || code == KEY_PLAYPAUSE) {
            ctl_->playback_toggle();
        }
        on_status_timer();
        return;
    }
    if (ctrl) return;
    lv_group_t *g = cdk::input_group();
    if (view_ == View::Review) {
        if (code == KEY_LEFT) ctl_->review_move(-1);
        else if (code == KEY_RIGHT) ctl_->review_move(1);
        else if (code == KEY_DELETE) ask_delete();
        else if (g && code == KEY_UP) lv_group_focus_prev(g);
        else if (g && code == KEY_DOWN) lv_group_focus_next(g);
        on_status_timer();
        return;
    }
    if (code == KEY_STOPCD || code == KEY_STOP) {
        if (ctl_->cancel_countdown()) on_status_timer();
    } else if (is(KEY_TAB, '\t')) {
        ctl_->toggle_capture_mode();
        on_status_timer();
    } else if (demo_notices_ && is(KEY_N, 'n')) {
        demo_next_notice();
    } else if (is(KEY_SPACE, ' ')) {
        ctl_->shutter(now_ms()); /* nothing during the countdown: a tap, Cancel, Esc or Stop cancels it (D30) */
        on_status_timer();
    } else if (is(KEY_R, 'r')) {
        ctl_->record(now_ms()); /* start (after the self-timer when set) or stop, in either mode (brief 12.3) */
        on_status_timer();
    } else if (ctl_->capture_mode() == CaptureMode::Photo && (code == KEY_MINUS || code == KEY_KPMINUS || (code == 0 && ch == '-'))) {
        ctl_->exposure_step(-1); /* 0.1.2: a stop darker (a camera that can do manual exposure) */
        on_status_timer();
    } else if (ctl_->capture_mode() == CaptureMode::Photo &&
               (code == KEY_EQUAL || code == KEY_KPPLUS || (code == 0 && (ch == '+' || ch == '=')))) {
        ctl_->exposure_step(1);
        on_status_timer();
    } else if (ctl_->capture_mode() == CaptureMode::Photo && (code == KEY_0 || code == KEY_KP0 || (code == 0 && ch == '0'))) {
        ctl_->exposure_set(0);
        on_status_timer();
    } else if (is(KEY_C, 'c')) {
        ctl_->next_camera(); /* 0.1.2: the next camera (with one camera it does nothing; recording: a toast says why) */
        on_status_timer();
    } else if (is(KEY_S, 's')) {
        open_settings(); /* brief 12.3 */
    } else if (g && (code == KEY_UP || code == KEY_LEFT)) {
        lv_group_focus_prev(g);
    } else if (g && (code == KEY_DOWN || code == KEY_RIGHT)) {
        lv_group_focus_next(g);
    }
}

} // namespace ui
} // namespace vhs
