/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field: the game screen, input, the clock, the first-dig worker, the win and loss sequences, the result panel and
 * the test fixtures (see minefield_app.hpp). The list pages are minefield_pages.cpp.
 */
#include "minefield_app.hpp"

#include "palette.hpp"

#include "app.hpp"
#include "fsutil.hpp"
#include "json.hpp"
#include "keyboard_input.h"
#include "solver.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <thread>

#include <unistd.h>

using namespace minefield;

namespace mfui {

namespace {

ButtonCtx g_button_ctx[4];

bool env_on(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

/* The store: the real folders, or (scenes) folders of the harness's output that a group of scenes shares. */
Store make_store()
{
    const char *group = std::getenv("MINEFIELD_TEST_DATA");
    if (!group || !*group) return Store();
    const char *state = std::getenv("APPLAUNCH_STATE_DIR");
    const char *canvas = std::getenv("APPLAUNCH_CANVAS");
    const std::string dir = std::string(state ? state : "/tmp") + "/../../../../_mdata-" + group + "/" + (canvas ? canvas : "x");
    const std::string data = dir + "/data", config = dir + "/config";
    make_dirs(data);
    make_dirs(config);
    if (env_on("MINEFIELD_TEST_RESET")) {
        for (const char *f : {"game.json", "game.json.bad", "stats.json", "stats.json.bad"}) remove_file(data + "/" + f);
        for (const char *f : {"settings.json", "settings.json.bad"}) remove_file(config + "/" + f);
    }
    return Store(data, config);
}

int text_w(const char *text, const lv_font_t *font)
{
    lv_point_t sz{};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

std::string mmss(std::uint32_t s)
{
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(b, sizeof b, "%u:%02u", s / 60, s % 60);
    return b;
}

std::uint64_t make_entropy()
{
    return static_cast<std::uint64_t>(lv_tick_get()) * 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(::getpid()) << 20) ^
           static_cast<std::uint64_t>(std::time(nullptr));
}

lv_obj_t *make_button(lv_obj_t *root, const char *text, const lv_font_t *font, int radius, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(pal::kBar), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(pal::kBarPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(pal::kBarBorder), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(pal::kText), 0);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

} // namespace

/* ------------------------------------------------------------------------------------------------ construction */

MineFieldApp::MineFieldApp(lv_obj_t *screen) : screen_(screen)
{
    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen_, layout_, cp0::ui::add_title, [this] { post([this] { back(); }); });
    cp0::ui::StatusStrip::add(page_->header());
    chrome_ = compute_chrome();

    Store store = make_store();
    if (const char *js = std::getenv("MINEFIELD_TEST_STATS")) { /* test only: the statistics file's text, written before the load */
        std::string text = js;
        std::replace(text.begin(), text.end(), '\'', '"'); /* the scene words cannot hold a double quote: ' stands for it */
        Stats st;
        if (Stats::from_json(text, st)) store.save_stats(st);
    }
    if (env_on("MINEFIELD_TEST_OLDSAVE")) { /* test only: a game in progress as version 1 of the app saved it (fixed 9x9, no preset) */
        GameConfig oc;
        oc.dims = Dims{9, 9, 10};
        oc.seed = 3;
        Game og(oc);
        og.reveal(4, 4);
        std::string text = og.save();
        const size_t v = text.find("\"version\": 2");
        if (v != std::string::npos) text.replace(v, 12, "\"version\": 1");
        const size_t pr = text.find("  \"preset\"");
        if (pr != std::string::npos) text.erase(pr, text.find('\n', pr) - pr + 1);
        make_dirs(store.data_dir());
        write_file_atomic(store.data_dir() + "/game.json", text);
    }
    session_ = std::make_unique<Session>(std::move(store), make_entropy());
    {
        const lv_area_t &vp = chrome_.viewport;
        std::array<Dims, kRecordedPresets> boards = derive_boards(lv_area_get_width(&vp), lv_area_get_height(&vp), chrome_.ppmm);
        if (const char *td = std::getenv("MINEFIELD_TEST_DIMS")) { /* test only: "9x9x10,16x16x40,30x16x99" instead of the screen's */
            std::string spec = td;
            std::replace(spec.begin(), spec.end(), ',', ' ');
            std::istringstream in(spec);
            for (int i = 0; i < kRecordedPresets; ++i) {
                std::string one;
                int w = 0, h = 0, m = 0;
                if (!(in >> one) || std::sscanf(one.c_str(), "%dx%dx%d", &w, &h, &m) != 3) break;
                boards[static_cast<size_t>(i)] = clamp_dims(w, h, m);
            }
        }
        session_->set_screen_boards(boards);
    }
    const bool resumed = session_->load();
    const bool dropped = session_->take_dropped_notice();
    probes_on_ = env_on("MINEFIELD_TEST_PROBES");
    if (const char *d = std::getenv("MINEFIELD_TEST_GENDELAY")) gen_delay_ms_ = std::atoi(d);

    if (!resumed) {
        bool changed = false;
        if (const char *tp = std::getenv("MINEFIELD_TEST_PREFS")) { /* "preset=expert,fair=1,anim=2,flag=1,timer=0" */
            Settings s = session_->settings();
            std::string spec = tp;
            std::replace(spec.begin(), spec.end(), ',', ' ');
            std::istringstream in(spec);
            std::string it;
            while (in >> it) {
                const size_t eq = it.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = it.substr(0, eq), v = it.substr(eq + 1);
                const int n = std::atoi(v.c_str());
                if (k == "preset") parse_preset(v, s.preset);
                else if (k == "fair") s.fair = n != 0;
                else if (k == "anim") s.animations = n == 0 ? Animations::On : (n == 1 ? Animations::Reduced : Animations::Off);
                else if (k == "flag") s.flag_mode = n != 0;
                else if (k == "timer") s.show_timer = n != 0;
            }
            session_->set_settings(s);
            changed = true;
        }
        const char *ts = std::getenv("MINEFIELD_TEST_SEED");
        if (ts || changed) session_->new_game(ts ? static_cast<std::uint64_t>(std::strtoull(ts, nullptr, 10)) : 0);
    }
    flag_mode_ = session_->settings().flag_mode;

    build_game_widgets();
    board_->bind(&session_->game());
    show_title();
    focus(board_->surface());
    lv_obj_add_event_cb(board_->surface(), surface_key_cb, LV_EVENT_KEY, this);
    if (LV_EVENT_KEYBOARD != 0) lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    last_tick_ = lv_tick_get();
    tick_timer_ = lv_timer_create(tick_cb, 250, this);

    if (!resumed) {
        if (const char *m = std::getenv("MINEFIELD_TEST_MAP")) load_test_map(m);
        if (const char *s = std::getenv("MINEFIELD_TEST_SETUP")) run_setup(s);
    }
    after_change(false);
    if (resumed) notice("Game resumed", 1800);
    else if (dropped) notice("Saved game dropped: screen changed", 3500);
}

MineFieldApp::~MineFieldApp()
{
    alive_ = false;
    lv_async_call_cancel(posted_cb, this);
    if (session_) session_->save_game();
    for (lv_timer_t *t : {tick_timer_, prep_timer_, anim_timer_, notice_timer_, hint_timer_})
        if (t) lv_timer_delete(t);
    if (LV_EVENT_KEYBOARD != 0 && lv_obj_is_valid(screen_))
        lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    dialog_.reset();
    list_.reset();
    board_.reset();
    page_.reset();
}

void MineFieldApp::posted_cb(void *data)
{
    auto *self = static_cast<MineFieldApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void MineFieldApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void MineFieldApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

/* ------------------------------------------------------------------------------------------------ widgets, layout */

void MineFieldApp::build_game_widgets()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    lv_obj_t *root = page_->root();
    const lv_font_t *body = cp0_ui_font(CP0_TEXT_BODY);
    const int radius = std::max(6, m->tok[CP0_TOK_RADIUS_M]);

    BoardEvents ev;
    ev.tap = [this](int cell) { on_tap(cell); };
    ev.long_press = [this](int cell) { on_long_press(cell); };
    ev.touched = [this] {
        if (keys_used_) {
            keys_used_ = false;
            board_->set_cursor(-1);
        }
    };
    board_ = std::make_unique<BoardView>(root, std::move(ev));

    /* the two readouts: plain cells, not buttons */
    for (lv_obj_t *&r : readout_) {
        r = lv_label_create(root);
        lv_obj_set_style_text_color(r, lv_color_hex(pal::kText), 0);
        lv_obj_set_style_text_font(r, body, 0);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(pal::kGround), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, radius, 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_CLIP);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }

    const char *names[4] = {LV_SYMBOL_REFRESH, "Dig", "Hint", "Menu"};
    for (int i = 0; i < 4; ++i) {
        btn_[i] = make_button(root, names[i], i == 0 ? cp0_ui_font(CP0_TEXT_SYMBOL) : body, radius, &btn_label_[i]);
        g_button_ctx[i] = {this, i};
        lv_obj_add_event_cb(btn_[i], button_cb, LV_EVENT_CLICKED, &g_button_ctx[i]);
    }

    banner_ = lv_obj_create(root);
    lv_obj_remove_style_all(banner_);
    lv_obj_set_style_bg_color(banner_, lv_color_hex(pal::kGround), 0);
    lv_obj_set_style_bg_opa(banner_, LV_OPA_90, 0);
    lv_obj_set_style_radius(banner_, 10, 0);
    lv_obj_set_style_pad_all(banner_, std::max(4, m->tok[CP0_TOK_SPACE_S]), 0);
    lv_obj_set_style_border_color(banner_, lv_color_hex(pal::kWarn), 0);
    lv_obj_set_style_border_width(banner_, 1, 0);
    lv_obj_set_style_border_opa(banner_, LV_OPA_60, 0);
    lv_obj_remove_flag(banner_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    banner_label_ = lv_label_create(banner_);
    lv_obj_set_style_text_font(banner_label_, body, 0);
    lv_obj_set_style_text_color(banner_label_, lv_color_hex(pal::kText), 0);
    lv_label_set_long_mode(banner_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_flow(banner_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(banner_, 2, 0);
    banner_extra_ = lv_label_create(banner_);
    lv_obj_set_style_text_font(banner_extra_, cp0_ui_font(CP0_TEXT_CAPTION), 0);
    lv_obj_set_style_text_color(banner_extra_, lv_color_hex(pal::kWarn), 0);
    lv_obj_add_flag(banner_extra_, LV_OBJ_FLAG_HIDDEN);
    apply_layout();
}

/* Where everything but the board goes.
 * Landscape has no strip: the page header, then the board (the whole rest of the canvas), and a dock of six items: the mines
 * left and the clock (plain readouts) and the buttons New game, Dig / Flag, Hint, Menu.
 *   Landscape  one column at the right edge, about 14 mm wide: the two readouts stacked on top, then the four buttons, each as
 *              tall as fits (at most 11 mm, at least 7 mm).
 *   Portrait   a strip over the board (mines left, the new-game button, the clock) and three buttons along the bottom. */
MineFieldApp::Chrome MineFieldApp::compute_chrome() const
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const double mm = m->ppmm_x100 / 100.0;
    const lv_font_t *body = cp0_ui_font(CP0_TEXT_BODY);
    const lv_font_t *cap = cp0_ui_font(CP0_TEXT_CAPTION);
    const int outer = std::max(2, std::max(4, m->tok[CP0_TOK_SPACE_M]) / 3);
    const int gap = outer;
    const int air = std::max(8, std::max(4, m->tok[CP0_TOK_SPACE_M])) / 2 + 1;
    const int w = layout_.w, h = layout_.h, by = layout_.body_y;
    const int target = m->tok[CP0_TOK_TARGET];
    const int mm7 = static_cast<int>(std::lround(7 * mm));
    Chrome c;
    c.side = w > h;
    int lw = 0;
    for (const char *t : {"Dig", "Flag", "Hint", "Menu"}) lw = std::max(lw, text_w(t, body));
    if (c.side) {
        /* about 14 mm wide (the labels need lw + air: the column grows only if they do not fit); the readouts are stacked, as
         * compact as the text allows, so the four buttons keep the height */
        int lw_cap = 0;
        for (const char *t : {"Dig", "Flag", "Hint", "Menu"}) lw_cap = std::max(lw_cap, text_w(t, cap));
        const int need_ro = text_w("Mines -99", cap) + 8;                /* the readouts, in the caption font */
        const int need_w = std::max(lw + 2 * air + 2, need_ro);          /* labels in the body font */
        const int need_cap = std::max(lw_cap + 2 * air + 2, need_ro);    /* labels in the caption font */
        const int dock_w = std::max(static_cast<int>(std::lround(14 * mm)), need_cap);
        c.label_need_px = need_w;
        const int x0 = w - dock_w - outer, y0 = by + outer;
        const int avail = h - outer - y0;
        const int ro_body = lv_font_get_line_height(body) + 6, ro_cap = lv_font_get_line_height(cap) + 6;
        const int bh_body = (avail - 2 * ro_body - 5 * gap) / 4;
        c.small_font = bh_body < static_cast<int>(std::lround(9 * mm)) || text_w("Mines -99", body) + 8 > dock_w; /* readouts in the caption font when the body font would cost button height */
        const int ro = c.small_font ? ro_cap : ro_body;
        int bh = (avail - 2 * ro - 5 * gap) / 4;
        bh = std::max(std::min(bh, static_cast<int>(std::lround(11 * mm))), mm7); /* never under 7 mm */
        int y = y0;
        c.readout[0] = {x0, y, dock_w, ro};
        y += ro + gap;
        c.readout[1] = {x0, y, dock_w, ro};
        y += ro + gap;
        for (int i = 0; i < 4; ++i) {
            c.btn[i] = {x0, y, dock_w, bh};
            y += bh + gap;
        }
        c.thickness_px = dock_w + outer;
        c.viewport = lv_area_t{outer, y0, x0 - outer - 1, h - outer - 1};
        c.button_h_px = bh;
    } else {
        /* portrait: the strip over the board (mines left, the new-game button in the middle, the clock) and the three-button
         * dock under it, as before the landscape dock was reworked */
        const int pad = std::max(4, m->tok[CP0_TOK_SPACE_M]);
        const int vp = std::max(2, pad / 3);
        const int status_h = target;
        const int sw = w - 2 * pad;
        const int new_w = std::max(target, static_cast<int>(std::lround(12 * mm)));
        c.strip = true;
        c.small_font = text_w("Mines 999", body) + text_w("Time 999", body) + new_w + 4 * pad > sw;
        const lv_font_t *hf = c.small_font ? cap : body;
        const int tw = text_w("Time 999", hf) + 2, mw = text_w("Mines 999", hf) + 2;
        c.readout[0] = {pad, by, mw, status_h};
        c.readout[1] = {pad + sw - tw, by, tw, status_h};
        c.btn[0] = {pad + (sw - new_w) / 2, by, new_w, status_h};
        const int bh = std::max(target, static_cast<int>(std::lround(11 * mm)));
        const int bw = (w - 4 * pad) / 3;
        for (int i = 1; i < 4; ++i) c.btn[i] = {pad + (i - 1) * (bw + pad), h - pad - bh, bw, bh};
        const int header_h = by + status_h;
        const int dock_h = bh + pad;
        c.thickness_px = dock_h;
        c.strip_px = status_h;
        c.button_h_px = bh;
        c.viewport = lv_area_t{vp, header_h + vp, w - vp - 1, header_h + vp + (h - header_h - dock_h - 2 * vp) - 1};
    }
    c.ppmm = mm;
    return c;
}

void MineFieldApp::apply_layout()
{
    const lv_font_t *body = cp0_ui_font(CP0_TEXT_BODY);
    const lv_font_t *cap = cp0_ui_font(CP0_TEXT_CAPTION);
    const int air = std::max(8, std::max(4, cp0_ui_metrics_get()->tok[CP0_TOK_SPACE_M])) / 2 + 1;
    chrome_ = compute_chrome();
    const Chrome &c = chrome_;
    side_dock_ = c.side;
    const lv_area_t &vp = c.viewport;
    board_->configure(vp, fit_board(lv_area_get_width(&vp), lv_area_get_height(&vp), c.ppmm, game().width(), game().height(),
                                    game().config().dims.mines));
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_pos(btn_[i], c.btn[i].x, c.btn[i].y);
        lv_obj_set_size(btn_[i], c.btn[i].w, c.btn[i].h);
        const int room = c.btn[i].w - 2 * air;
        const char *t = lv_label_get_text(btn_label_[i]);
        if (i > 0) lv_obj_set_style_text_font(btn_label_[i], text_w(t, body) > room ? cap : body, 0);
        lv_obj_center(btn_label_[i]);
    }
    for (int i = 0; i < 2; ++i) {
        const Rect &r = c.readout[i];
        const lv_font_t *f = c.small_font ? cap : body;
        const int lh = lv_font_get_line_height(f);
        lv_obj_set_style_text_font(readout_[i], f, 0);
        lv_obj_set_style_bg_opa(readout_[i], c.strip ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_text_align(readout_[i], c.strip ? (i == 0 ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT) : LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(readout_[i], r.x, r.y);
        lv_obj_set_size(readout_[i], r.w, r.h);
        lv_obj_set_style_pad_top(readout_[i], std::max(0, (r.h - lh) / 2), 0);
    }
    mines_shown_.clear();
    time_shown_.clear();
    banner_room_ = {vp.x1, vp.y1, lv_area_get_width(&vp), lv_area_get_height(&vp)};
    refresh_buttons();
    refresh_hud();
    raise_chrome();
    make_probes();
    dump_layout();
}

void MineFieldApp::raise_chrome()
{
    for (lv_obj_t *o : {readout_[0], readout_[1], btn_[0], btn_[1], btn_[2], btn_[3], banner_}) lv_obj_move_foreground(o);
}

void MineFieldApp::show_game_widgets(bool shown)
{
    for (lv_obj_t *o : {readout_[0], readout_[1], btn_[0], btn_[1], btn_[2], btn_[3]}) {
        if (shown) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!shown) lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    board_->set_hidden(!shown);
    if (probes_) {
        if (shown) lv_obj_remove_flag(probes_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(probes_, LV_OBJ_FLAG_HIDDEN);
    }
    if (shown) {
        refresh_hud();
        if (notice_timer_) lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    }
}

void MineFieldApp::refresh_hud()
{
    if (!readout_[0] || view_ != View::Game) return;
    const Game &g = game();
    const std::string mines = "Mines " + std::to_string(g.mines_left());
    const bool show_timer = session_->settings().show_timer;
    const std::string time = show_timer ? "Time " + std::to_string(g.display_seconds()) : "";
    if (mines != mines_shown_) {
        mines_shown_ = mines;
        lv_label_set_text(readout_[0], mines.c_str());
    }
    if (time != time_shown_) {
        time_shown_ = time;
        lv_label_set_text(readout_[1], time.c_str());
    }
}

void MineFieldApp::refresh_buttons()
{
    lv_label_set_text(btn_label_[1], flag_mode_ ? "Flag" : "Dig");
    lv_obj_set_style_bg_color(btn_[1], lv_color_hex(flag_mode_ ? pal::kAccent : pal::kBar), 0);
    lv_obj_set_style_text_color(btn_label_[1], lv_color_hex(flag_mode_ ? pal::kOnAccent : pal::kText), 0);
    lv_obj_center(btn_label_[1]);
    const bool done = game().finished();
    lv_obj_set_style_text_opa(btn_label_[2], done ? LV_OPA_40 : LV_OPA_COVER, 0);
}

void MineFieldApp::button_cb(lv_event_t *e)
{
    auto *ctx = static_cast<ButtonCtx *>(lv_event_get_user_data(e));
    ctx->app->post([app = ctx->app, i = ctx->index] {
        if (app->view_ != View::Game || app->dialog_) return;
        if (app->animating_) {
            app->skip_anim();
            return;
        }
        if (app->keys_used_) {
            app->keys_used_ = false;
            app->board_->set_cursor(-1);
        }
        switch (i) {
        case 0: app->request_new_game(); break;
        case 1: app->toggle_mode(); break;
        case 2: app->do_hint(); break;
        case 3:
            app->menu_sel_ = 0;
            app->open_view(View::Menu);
            break;
        default: break;
        }
    });
}

/* ------------------------------------------------------------------------------------------------ notices */

void MineFieldApp::notice_cb(lv_timer_t *t)
{
    auto *self = static_cast<MineFieldApp *>(lv_timer_get_user_data(t));
    self->notice_timer_ = nullptr; /* one shot: LVGL deletes it */
    lv_obj_add_flag(self->banner_, LV_OBJ_FLAG_HIDDEN);
}

/* A line drawn over the board's top edge (the bottom edge when it would cover `avoid`, the cell a hint points at); it takes
 * no room of its own. */
void MineFieldApp::notice(const std::string &text, int ms, const std::string &extra, int avoid)
{
    if (notice_timer_) {
        lv_timer_delete(notice_timer_);
        notice_timer_ = nullptr;
    }
    if (text.empty()) {
        lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(banner_label_, text.c_str());
    lv_label_set_text(banner_extra_, extra.c_str());
    if (extra.empty()) lv_obj_add_flag(banner_extra_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(banner_extra_, LV_OBJ_FLAG_HIDDEN);
    const lv_font_t *font = cp0_ui_font(CP0_TEXT_BODY);
    const int pad = lv_obj_get_style_pad_left(banner_, LV_PART_MAIN);
    const int max_w = std::max(80, banner_room_.w - 16);
    const int want = std::max(text_w(text.c_str(), font), text_w(extra.c_str(), cp0_ui_font(CP0_TEXT_CAPTION)));
    const int bw = std::min(max_w, want + 2 * pad + 6);
    lv_obj_set_width(banner_, bw);
    lv_obj_set_width(banner_label_, bw - 2 * pad);
    lv_obj_set_width(banner_extra_, bw - 2 * pad);
    lv_obj_set_height(banner_, LV_SIZE_CONTENT);
    lv_obj_update_layout(banner_);
    const int bh = lv_obj_get_height(banner_);
    const int x = banner_room_.x + (banner_room_.w - bw) / 2;
    int y = side_dock_ ? banner_room_.y + 4 : banner_room_.y + banner_room_.h - bh - 8;
    if (avoid >= 0 && avoid < board_cells()) {
        const lv_area_t a = board_->cell_area(avoid);
        const bool hits = !(a.x2 < x || a.x1 > x + bw - 1 || a.y2 < y || a.y1 > y + bh - 1);
        if (hits) y = side_dock_ ? banner_room_.y + banner_room_.h - bh - 4 : banner_room_.y + 4;
    }
    lv_obj_set_pos(banner_, x, y);
    if (view_ == View::Game) lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(banner_);
    if (ms > 0) {
        notice_timer_ = lv_timer_create(notice_cb, static_cast<uint32_t>(ms), this);
        lv_timer_set_repeat_count(notice_timer_, 1);
    }
}

void MineFieldApp::clear_notice()
{
    notice("", 0);
}

/* test only (MINEFIELD_TEST_DUMP): the geometry of this screen, appended to a file */
void MineFieldApp::dump_layout() const
{
    const char *dump = std::getenv("MINEFIELD_TEST_DUMP");
    if (!dump || !*dump) return;
    std::FILE *f = std::fopen(dump, "a");
    if (!f) return;
    const Chrome &c = chrome_;
    const int vw = lv_area_get_width(&c.viewport), vh = lv_area_get_height(&c.viewport);
    std::fprintf(f, "%dx%d ppmm=%.2f %s dock=%.1fmm (body-font labels need %.1fmm) buttons=%.1fmm viewport=%dx%d (%.1fx%.1fmm)\n", layout_.w, layout_.h, c.ppmm,
                 c.side ? "side(readouts stacked)" : "bottom(strip + 3 buttons)",
                 c.thickness_px / c.ppmm, c.label_need_px / c.ppmm, c.button_h_px / c.ppmm, vw, vh, vw / c.ppmm, vh / c.ppmm);
    for (int i = 0; i < kRecordedPresets; ++i) {
        const BoardPlan b = derive_board(vw, vh, c.ppmm, static_cast<Preset>(i));
        std::fprintf(f, "  %s %dx%d mines=%d cell=%dpx (%.1fmm) fits=%d\n", preset_name(static_cast<Preset>(i)), b.dims.w, b.dims.h,
                     b.dims.mines, b.cell_px, b.cell_mm, b.fits ? 1 : 0);
    }
    std::fclose(f);
}

/* ------------------------------------------------------------------------------------------------ the clock */

void MineFieldApp::tick_cb(lv_timer_t *t)
{
    auto *self = static_cast<MineFieldApp *>(lv_timer_get_user_data(t));
    const std::uint32_t now = lv_tick_get();
    std::uint32_t dt = lv_tick_elaps(self->last_tick_);
    self->last_tick_ = now;
    static bool was_hidden = false;
    const bool hidden = cdk::hidden();
    if (hidden && !was_hidden) self->session_->save_game();
    was_hidden = hidden;
    /* a frozen app (SIGSTOP while hidden) wakes up with a huge gap: it was not played */
    if (dt > 1000) dt = 0;
    if (hidden || self->view_ != View::Game || self->dialog_ || self->preparing_) return;
    self->session_->tick(dt);
    self->last_save_ms_ += dt;
    if (self->last_save_ms_ >= 20000) {
        self->last_save_ms_ = 0;
        if (self->session_->in_progress()) self->session_->save_game();
    }
    self->refresh_hud();
}

/* ------------------------------------------------------------------------------------------------ the game */

bool MineFieldApp::can_act() const
{
    return alive_ && view_ == View::Game && !dialog_ && !preparing_ && !animating_ && !game().finished();
}

void MineFieldApp::after_change(bool animate)
{
    board_->sync();
    refresh_hud();
    refresh_buttons();
    make_probes();
    if (game().finished() && !finish_handled_) on_finish(animate);
}

void MineFieldApp::clear_hint()
{
    if (hint_timer_) {
        lv_timer_delete(hint_timer_);
        hint_timer_ = nullptr;
    }
    hint_cell_ = -1;
    if (board_) board_->set_hint(-1, false);
}

void MineFieldApp::hint_cb(lv_timer_t *t)
{
    auto *self = static_cast<MineFieldApp *>(lv_timer_get_user_data(t));
    self->hint_timer_ = nullptr;
    self->hint_cell_ = -1;
    self->board_->set_hint(-1, false);
}

void MineFieldApp::dig(int cell, bool announce)
{
    const int x = cell % game().width(), y = cell / game().width();
    if (game().board().state(cell) == CellState::Flagged) {
        if (announce) notice("Flagged: press F to take the flag off", 1500);
        return;
    }
    if (game().status() == Game::Status::Ready && !supplied_) {
        start_prepare(cell);
        return;
    }
    clear_hint();
    session_->reveal(x, y);
    supplied_ = false;
    after_change(true);
}

void MineFieldApp::flag(int cell)
{
    clear_hint();
    if (session_->toggle_flag(cell % game().width(), cell / game().width())) after_change(false);
}

void MineFieldApp::chord(int cell)
{
    clear_hint();
    const int w = game().width();
    const Game::ChordOutcome o = session_->chord(cell % w, cell / w);
    if (o == Game::ChordOutcome::FlagMismatch) {
        int nb[8];
        const int n = game().board().neighbours(cell, nb);
        std::vector<int> cells;
        for (int k = 0; k < n; ++k)
            if (game().board().state(nb[k]) != CellState::Revealed) cells.push_back(nb[k]);
        board_->flash(cells, 700);
        notice("Flags do not match", 2000);
    } else if (o == Game::ChordOutcome::Opened || o == Game::ChordOutcome::Exploded || o == Game::ChordOutcome::Won) {
        after_change(true);
    }
}

void MineFieldApp::on_tap(int cell)
{
    if (animating_) {
        skip_anim();
        return;
    }
    if (!can_act() || cell < 0 || cell >= board_cells()) return;
    const CellState st = game().board().state(cell);
    if (st == CellState::Revealed) {
        chord(cell);
        return;
    }
    if (flag_mode_) flag(cell);
    else if (st == CellState::Hidden) dig(cell, false); /* a tap on a flag in Dig mode does nothing */
}

void MineFieldApp::on_long_press(int cell)
{
    if (!can_act() || cell < 0 || cell >= board_cells()) return;
    const CellState st = game().board().state(cell);
    if (st == CellState::Revealed) return;
    board_->pulse(cell, 240);
    if (!flag_mode_) flag(cell);
    else if (st == CellState::Hidden) dig(cell, false);
}

void MineFieldApp::toggle_mode()
{
    flag_mode_ = !flag_mode_;
    refresh_buttons();
}

/* ---- the first dig lays the mines on a worker thread */

void MineFieldApp::start_prepare(int cell)
{
    cancel_prepare();
    preparing_ = true;
    board_->set_input_enabled(false);
    notice("Preparing board...", 0);
    auto st = std::make_shared<Prep>();
    st->cfg = game().config();
    st->cell = cell;
    st->delay_ms = gen_delay_ms_;
    std::thread([st] {
        if (st->delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(st->delay_ms));
        st->result = Game::prepare(st->cfg, st->cell);
        st->done.store(true, std::memory_order_release);
    }).detach();
    prep_ = st;
    prep_timer_ = lv_timer_create(prep_cb, 40, this);
}

void MineFieldApp::prep_cb(lv_timer_t *t)
{
    auto *self = static_cast<MineFieldApp *>(lv_timer_get_user_data(t));
    if (self->prep_ && self->prep_->done.load(std::memory_order_acquire)) self->finish_prepare();
}

void MineFieldApp::finish_prepare()
{
    std::shared_ptr<Prep> st = prep_;
    cancel_prepare();
    if (!st) return;
    clear_notice();
    board_->set_input_enabled(true);
    session_->game().supply(st->result);
    supplied_ = true; /* dig() must not prepare again */
    last_tick_ = lv_tick_get();
    dig(st->cell, false);
}

void MineFieldApp::cancel_prepare()
{
    if (prep_timer_) {
        lv_timer_delete(prep_timer_);
        prep_timer_ = nullptr;
    }
    prep_.reset(); /* the thread keeps its own reference and ends by itself */
    if (preparing_) {
        preparing_ = false;
        if (board_) board_->set_input_enabled(true);
        clear_notice();
    }
}

/* ---- hint */

void MineFieldApp::do_hint()
{
    if (!can_act()) return;
    clear_hint();
    const Game::Hint h = session_->hint();
    switch (h.kind) {
    case Game::HintKind::Unavailable: notice("Dig a cell first", 1800); return;
    case Game::HintKind::None: notice("Nothing certain here", 2200); return;
    default: break;
    }
    hint_cell_ = h.cell;
    board_->set_hint(h.cell, session_->settings().animations == Animations::On);
    hint_timer_ = lv_timer_create(hint_cb, 5000, this);
    lv_timer_set_repeat_count(hint_timer_, 1);
    notice(h.kind == Game::HintKind::Safe ? "Safe: dig here" : "Mine here: flag it", 2500, "+10 s", h.cell);
    refresh_hud();
    make_probes();
}

/* ---- new game, same board */

void MineFieldApp::request_new_game()
{
    if (preparing_ || animating_) {
        if (animating_) skip_anim();
        return;
    }
    if (session_->in_progress()) {
        confirm("Start a new game?", "The game in progress counts as a lost game.", "New game", [this] { start_new_game(); });
    } else {
        start_new_game();
    }
}

void MineFieldApp::start_new_game()
{
    cancel_prepare();
    if (animating_) skip_anim();
    clear_hint();
    clear_notice();
    close_dialog();
    if (view_ != View::Game) close_view();
    session_->new_game();
    supplied_ = false;
    finish_handled_ = result_shown_ = false;
    loss_order_.clear();
    board_->bind(&session_->game());
    apply_layout();
    keys_used_ = false;
    last_tick_ = lv_tick_get();
    after_change(false);
}

void MineFieldApp::replay_same()
{
    const Game &old = game();
    const GameConfig cfg = old.config();
    const int first = old.first_click();
    Game::Prepared p;
    if (first >= 0) {
        p.valid = true;
        p.first = first;
        p.gen.mines = old.board().mine_map();
        p.gen.seed_used = old.seed_used();
        p.gen.attempts = old.attempts();
        p.gen.fair_requested = cfg.fair;
        p.gen.fair_failed = old.fair_failed();
    }
    cancel_prepare();
    clear_hint();
    clear_notice();
    close_dialog();
    if (view_ != View::Game) close_view();
    session_->replay(cfg);
    finish_handled_ = result_shown_ = false;
    loss_order_.clear();
    board_->bind(&session_->game());
    apply_layout();
    keys_used_ = false;
    last_tick_ = lv_tick_get();
    if (first >= 0) {
        session_->game().supply(p);
        supplied_ = true;
        after_change(false);
        dig(first, false);
    } else {
        supplied_ = false;
        after_change(false);
    }
}

/* ---- the end of a game */

void MineFieldApp::on_finish(bool animate)
{
    finish_handled_ = true;
    clear_hint();
    const Animations a = session_->settings().animations;
    const bool won = game().status() == Game::Status::Won;
    if (animate && a == Animations::On && !dialog_) {
        animating_ = true;
        anim_tick_ = 0;
        board_->set_input_enabled(true);
        if (won) {
            anim_kind_ = 1;
            board_->set_sweep(0.0);
        } else {
            anim_kind_ = 2;
            loss_order_ = game().mine_reveal_order();
            board_->set_mine_reveal(loss_order_, 0);
        }
        if (anim_kind_ == 2 && loss_order_.empty()) {
            animating_ = false;
            board_->reveal_all_mines(true);
            post([this] { show_result(); });
            return;
        }
        anim_timer_ = lv_timer_create(anim_cb, 40, this);
        return;
    }
    if (!won) board_->reveal_all_mines(true);
    post([this] { show_result(); });
}

void MineFieldApp::anim_cb(lv_timer_t *t)
{
    static_cast<MineFieldApp *>(lv_timer_get_user_data(t))->anim_step();
}

void MineFieldApp::anim_step()
{
    if (!animating_) return;
    ++anim_tick_;
    bool end = false;
    if (anim_kind_ == 1) {
        constexpr int kSteps = 22;
        board_->set_sweep(std::min(1.0, anim_tick_ / static_cast<double>(kSteps)));
        end = anim_tick_ >= kSteps;
    } else {
        const int n = static_cast<int>(loss_order_.size());
        const int per = std::max(1, (n + 29) / 30);
        board_->set_mine_reveal(loss_order_, anim_tick_ * per);
        end = anim_tick_ * per >= n;
    }
    if (end) skip_anim();
}

void MineFieldApp::skip_anim()
{
    if (!animating_) return;
    animating_ = false;
    if (anim_timer_) {
        lv_timer_delete(anim_timer_);
        anim_timer_ = nullptr;
    }
    if (anim_kind_ == 1) board_->set_sweep(-1);
    else board_->reveal_all_mines(true);
    anim_kind_ = 0;
    post([this] { show_result(); });
}

void MineFieldApp::show_result()
{
    if (!alive_ || dialog_ || result_shown_ || !game().finished() || view_ != View::Game) return;
    result_shown_ = true;
    const Game &g = game();
    const bool won = g.status() == Game::Status::Won;
    const GameRecord rec = g.record();
    const Preset preset = g.config().preset;
    std::vector<std::string> lines{"Time " + mmss(g.time_ms() / 1000)};
    if (won) {
        if (g.assisted()) {
            lines.push_back("Hints used: no best time");
        } else {
            const std::vector<BestEntry> &best = session_->stats().get(preset).best;
            int rank = -1;
            for (int i = static_cast<int>(best.size()) - 1; i >= 0; --i)
                if (best[static_cast<size_t>(i)] == BestEntry{rec.time_ms, g.config().seed, g.config().fair, rec.dims.w, rec.dims.h, rec.dims.mines}) {
                    rank = i;
                    break;
                }
            if (rank == 0) lines.push_back("New best!");
            else if (rank > 0) lines.push_back("Rank " + std::to_string(rank + 1) + " in the best 5");
            else lines.push_back("Not in the best 5");
        }
    } else {
        lines.push_back("Cells opened: " + std::to_string(g.board().revealed_safe()) + " of " +
                        std::to_string(g.board().size() - g.board().mines()));
    }
    cp0::ui::DialogSpec spec;
    spec.title = won ? "You won!" : "Mine hit";
    spec.page = page_.get();
    spec.buttons.push_back({"Close", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    if (won) {
        spec.buttons.push_back({layout_.w < 560 ? "Replay" : "Same board", cp0::ui::ButtonRole::Normal, [this] {
                                    post([this] {
                                        close_dialog();
                                        replay_same();
                                    });
                                }});
        spec.buttons.push_back({"New game", cp0::ui::ButtonRole::Primary, [this] {
                                    post([this] {
                                        close_dialog();
                                        start_new_game();
                                    });
                                }});
    } else {
        spec.buttons.push_back({"New game", cp0::ui::ButtonRole::Normal, [this] {
                                    post([this] {
                                        close_dialog();
                                        start_new_game();
                                    });
                                }});
        spec.buttons.push_back({"Try again", cp0::ui::ButtonRole::Primary, [this] {
                                    post([this] {
                                        close_dialog();
                                        replay_same();
                                    });
                                }});
    }
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    for (const std::string &l : lines) dialog_->add_text(l);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

void MineFieldApp::close_dialog()
{
    if (!dialog_) return;
    dialog_.reset();
    if (view_ == View::Game) {
        focus(board_->surface());
        last_tick_ = lv_tick_get();
    } else if (list_) {
        list_->set_hidden(false);
        focus(list_->focus_object());
    }
}

void MineFieldApp::confirm(const std::string &title, const std::string &text, const std::string &ok, std::function<void()> on_ok,
                           const std::string &cancel)
{
    if (dialog_) return;
    cp0::ui::DialogSpec spec;
    spec.title = title;
    spec.page = page_.get();
    spec.buttons.push_back({cancel, cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    spec.buttons.push_back({ok, cp0::ui::ButtonRole::Primary, [this, on_ok] {
                                post([this, on_ok] {
                                    close_dialog();
                                    on_ok();
                                });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    dialog_->add_text(text);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

/* ------------------------------------------------------------------------------------------------ keys */

void MineFieldApp::surface_key_cb(lv_event_t *e)
{
    static_cast<MineFieldApp *>(lv_event_get_user_data(e))->on_key(lv_event_get_key(e));
}

void MineFieldApp::keyboard_cb(lv_event_t *e)
{
    auto *self = static_cast<MineFieldApp *>(lv_event_get_user_data(e));
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(e));
    if (item) self->on_keyboard(*item);
}

void MineFieldApp::use_keys()
{
    if (keys_used_) return;
    keys_used_ = true;
    if (cursor_ < 0 || cursor_ >= board_cells()) cursor_ = board_->center_cell();
    show_cursor();
}

void MineFieldApp::show_cursor()
{
    board_->set_cursor(keys_used_ ? cursor_ : -1);
}

void MineFieldApp::cursor_move(int dx, int dy)
{
    const int w = game().width(), h = game().height();
    int x = cursor_ % w + dx, y = cursor_ / w + dy;
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    cursor_ = y * w + x;
    show_cursor();
    make_probes();
}

void MineFieldApp::cursor_activate()
{
    if (cursor_ < 0 || !can_act()) return;
    const CellState st = game().board().state(cursor_);
    if (st == CellState::Revealed) chord(cursor_);
    else dig(cursor_, true);
}

void MineFieldApp::on_key(uint32_t key)
{
    if (view_ != View::Game || dialog_) return;
    if (animating_) {
        skip_anim();
        return;
    }
    if (key == LV_KEY_ESC) {
        post([this] { back(); });
        return;
    }
    if (preparing_ || game().finished()) return;
    switch (key) {
    case LV_KEY_LEFT: use_keys(); cursor_move(-1, 0); break;
    case LV_KEY_RIGHT: use_keys(); cursor_move(1, 0); break;
    case LV_KEY_UP: use_keys(); cursor_move(0, -1); break;
    case LV_KEY_DOWN: use_keys(); cursor_move(0, 1); break;
    case LV_KEY_ENTER: use_keys(); cursor_activate(); break;
    default: break;
    }
}

void MineFieldApp::on_keyboard(const struct key_item &item)
{
    if (item.key_state != 1 || view_ != View::Game || dialog_) return;
    if (animating_) {
        skip_anim();
        return;
    }
    if ((item.mods & KBD_MOD_CTRL) && (item.key_code == 24 /* KEY_O */ || item.utf8[0] == 'o' || item.utf8[0] == 0x0F)) {
        post([this] { menu_sel_ = 0; open_view(View::Menu); });
        return;
    }
    if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO)) return;
    const char c = item.utf8[0];
    if (item.utf8[1] != 0) return;
    const char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (lower == 'n') {
        post([this] { request_new_game(); });
        return;
    }
    if (lower == 'm') {
        post([this] { menu_sel_ = 0; open_view(View::Menu); });
        return;
    }
    if (preparing_) return;
    switch (lower) {
    case 't': toggle_mode(); return;
    default: break;
    }
    if (game().finished()) return;
    switch (lower) {
    case ' ': use_keys(); cursor_activate(); break;
    case 'f':
        use_keys();
        if (game().board().state(cursor_) != CellState::Revealed && can_act()) flag(cursor_);
        break;
    case 'c':
        use_keys();
        if (can_act() && game().board().state(cursor_) == CellState::Revealed) chord(cursor_);
        break;
    case 'h': do_hint(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ test fixtures */

/* test only: "w h mines first mine_map cells" (the saved-game format's fields) becomes the game in progress */
void MineFieldApp::load_test_map(const std::string &spec)
{
    std::istringstream in(spec);
    int w = 0, h = 0, mines = 0, first = 0;
    std::string map, cells;
    in >> w >> h >> mines >> first >> map >> cells;
    std::string j = "{\"version\":1,\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) + ",\"mines\":" +
                    std::to_string(mines) + ",\"seed\":\"1\",\"seed_used\":\"1\",\"fair\":false,\"fair_failed\":false,\"attempts\":1," +
                    "\"first_click\":" + std::to_string(first) + ",\"clock_ms\":0,\"penalty_ms\":0,\"hints\":0," + "\"mine_map\":\"" + map +
                    "\",\"cells\":\"" + cells + "\"}";
    Game g;
    std::string why;
    if (Game::load(j, g, &why)) {
        session_->game() = g;
    } else {
        std::fprintf(stderr, "MINEFIELD_TEST_MAP: %s\n", why.c_str());
    }
}

/* test only: ops separated by ';': dig X,Y | flag X,Y | tick MS | solve | safeleft | winall | lose | flagmines N | wrongflags N | hint */
void MineFieldApp::run_setup(const std::string &ops)
{
    std::istringstream in(ops);
    std::string op;
    while (std::getline(in, op, ';')) {
        std::istringstream o(op);
        std::string cmd;
        o >> cmd;
        int x = 0, y = 0;
        char comma = 0;
        Game &g = session_->game();
        const int w = g.width();
        if (cmd == "dig") {
            o >> x >> comma >> y;
            session_->reveal(x, y);
        } else if (cmd == "flag") {
            o >> x >> comma >> y;
            session_->toggle_flag(x, y);
        } else if (cmd == "tick") {
            int ms = 0;
            o >> ms;
            session_->tick(static_cast<std::uint32_t>(ms));
        } else if (cmd == "solve") {
            for (int guard = 0; guard < 2000 && g.status() == Game::Status::Playing; ++guard) {
                const int c = find_safe_cell(g.view());
                if (c < 0) break;
                session_->reveal(c % w, c / w);
            }
        } else if (cmd == "safeleft" || cmd == "winall") {
            const int keep = cmd == "safeleft" ? 1 : 0;
            for (int i = 0; i < g.board().size() && g.board().safe_left() > keep && !g.finished(); ++i)
                if (!g.board().is_mine(i) && g.board().state(i) == CellState::Hidden) session_->reveal(i % w, i / w);
        } else if (cmd == "lose") {
            for (int i = 0; i < g.board().size(); ++i)
                if (g.board().is_mine(i) && g.board().state(i) == CellState::Hidden) {
                    session_->reveal(i % w, i / w);
                    break;
                }
        } else if (cmd == "hint") {
            session_->hint();
        } else if (cmd == "wrongflags") {
            int n = 0;
            o >> n;
            for (int i = g.board().size() - 1; i >= 0 && n > 0; --i)
                if (!g.board().is_mine(i) && g.board().state(i) == CellState::Hidden) {
                    session_->toggle_flag(i % w, i / w);
                    --n;
                }
        } else if (cmd == "flagmines") {
            int n = 0;
            o >> n;
            for (int i = 0; i < g.board().size() && n > 0; ++i)
                if (g.board().is_mine(i) && g.board().state(i) == CellState::Hidden) {
                    session_->toggle_flag(i % w, i / w);
                    --n;
                }
        }
    }
    if (game().finished()) finish_handled_ = false;
}

/* test only (MINEFIELD_TEST_PROBES): invisible labels a scene taps by their text: every visible cell "@cX,Y", the
 * first hidden safe cell "@safe", a hidden mine "@mine", an opened number "@num", a flag "@flag", the new-game button
 * "@new"; "@cellN.N" (the cell size in mm) when the board lies inside its area, else "@OVERFLOW". */
void MineFieldApp::make_probes()
{
    if (!probes_on_ || !board_) return;
    if (!probes_) {
        probes_ = lv_obj_create(page_->root());
        lv_obj_remove_style_all(probes_);
        lv_obj_set_pos(probes_, 0, 0);
        lv_obj_set_size(probes_, layout_.w, layout_.h);
        lv_obj_remove_flag(probes_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
    lv_obj_clean(probes_);
    if (view_ != View::Game) return;
    const lv_area_t vp = board_->viewport_area();
    auto add = [&](const std::string &name, lv_area_t a, bool clip = true) {
        if (clip) {
            a.x1 = std::max(a.x1, vp.x1);
            a.y1 = std::max(a.y1, vp.y1);
            a.x2 = std::min(a.x2, vp.x2);
            a.y2 = std::min(a.y2, vp.y2);
        }
        if (a.x2 - a.x1 < 5 || a.y2 - a.y1 < 5) return;
        lv_obj_t *l = lv_label_create(probes_);
        lv_label_set_text(l, name.c_str());
        lv_obj_set_style_text_opa(l, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(l, a.x1, a.y1);
        lv_obj_set_size(l, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    };
    const Board &b = game().board();
    const int w = b.width();
    bool have_safe = false, have_mine = false, have_num = false, have_flag = false;
    for (int i = 0; i < b.size(); ++i) {
        const lv_area_t a = board_->cell_area(i);
        add("@c" + std::to_string(i % w) + "," + std::to_string(i / w), a);
        const CellState st = b.state(i);
        if (!have_safe && st == CellState::Hidden && b.placed() && !b.is_mine(i)) have_safe = true, add("@safe", a);
        if (!have_mine && st == CellState::Hidden && b.is_mine(i)) have_mine = true, add("@mine", a);
        if (!have_num && st == CellState::Revealed && b.adjacent(i) > 0) have_num = true, add("@num", a);
        if (!have_flag && st == CellState::Flagged) have_flag = true, add("@flag", a);
    }
    const lv_area_t &nb = chrome_.btn[0].w ? lv_area_t{chrome_.btn[0].x, chrome_.btn[0].y, chrome_.btn[0].x + chrome_.btn[0].w - 1,
                                                    chrome_.btn[0].y + chrome_.btn[0].h - 1}
                                           : lv_area_t{0, 0, 0, 0};
    add("@new", nb, false);
    /* the board lies entirely inside its viewport: no pan, no scroll, ever */
    const BoardPlan &pl = board_->plan();
    const bool inside = pl.valid && pl.fits && pl.origin_x >= 0 && pl.origin_y >= 0 &&
                        pl.origin_x + pl.board_w <= lv_area_get_width(&vp) && pl.origin_y + pl.board_h <= lv_area_get_height(&vp);
    char mm[48];
    std::snprintf(mm, sizeof mm, "@cell%.1f", pl.cell_mm);
    lv_obj_t *cm = lv_label_create(probes_);
    lv_label_set_text(cm, inside ? mm : "@OVERFLOW");
    lv_obj_set_style_text_opa(cm, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(cm, vp.x1, vp.y1);
}

} // namespace mfui
