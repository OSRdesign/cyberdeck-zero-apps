/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike Solitaire: the table, input, the clock, the notices and the helper jobs (see klondike_app.hpp). The list and
 * text views and the dialogs are klondike_pages.cpp.
 */
#include "klondike_app.hpp"

#include "app.hpp"
#include "fsutil.hpp"
#include "hint.hpp"
#include "keyboard_input.h"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

using namespace klondike;

namespace kui {

namespace {

struct ButtonCtx {
    KlondikeApp *app;
    int index;
};
ButtonCtx g_button_ctx[kDockButtons];

const char *kButtonText[kDockButtons] = {"Undo", "Redo", "Hint", "Menu"};

std::string exe_path()
{
    char buf[1024];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "klondike-solitaire";
    buf[n] = 0;
    return buf;
}

bool env_on(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

/* The store: the real folders, or (scenes) folders of the harness's output that a group of scenes shares. */
Store make_store()
{
    const char *group = std::getenv("KLONDIKE_TEST_DATA");
    if (!group || !*group) return Store();
    const char *state = std::getenv("APPLAUNCH_STATE_DIR");
    const char *canvas = std::getenv("APPLAUNCH_CANVAS");
    const std::string dir = std::string(state ? state : "/tmp") + "/../../../../_kdata-" + group + "/" + (canvas ? canvas : "x");
    const std::string data = dir + "/data", config = dir + "/config";
    make_dirs(data);
    make_dirs(config);
    if (env_on("KLONDIKE_TEST_RESET")) {
        for (const char *f : {"game.json", "game.json.bad", "stats.json", "stats.json.bad"}) remove_file(data + "/" + f);
        for (const char *f : {"settings.json", "settings.json.bad", "ui.json", "ui.json.bad"}) remove_file(config + "/" + f);
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

} // namespace

/* ------------------------------------------------------------------------------------------------ construction */

KlondikeApp::KlondikeApp(lv_obj_t *screen) : screen_(screen)
{
    exe_ = exe_path();
    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen_, layout_, cp0::ui::add_title, [this] { post([this] { back(); }); });
    cp0::ui::StatusStrip::add(page_->header());

    session_ = std::make_unique<Session>(make_store());
    const bool resumed = session_->load();
    if (!resumed) {
        if (const char *r = std::getenv("KLONDIKE_TEST_RULES")) {
            Rules rules;
            Rules::parse(r, rules);
            session_->set_rules(rules);
        }
        if (const char *bt = std::getenv("KLONDIKE_TEST_BOARD")) {
            Board b;
            Game g;
            if (Board::from_text(bt, b) && g.new_game(b, session_->rules(), 1)) {
                g.set_labels(1, false);
                session_->game() = g;
            }
        } else if (const char *d = std::getenv("KLONDIKE_TEST_DEAL")) {
            const std::uint32_t n = static_cast<std::uint32_t>(std::strtoul(d, nullptr, 10));
            session_->begin_deal(n, n, false);
        } else if (std::getenv("KLONDIKE_TEST_RULES")) {
            session_->begin_deal(session_->game().deal_number(), session_->game().user_number(), false);
        }
    }
    if (const char *tp = std::getenv("KLONDIKE_TEST_PREFS")) { /* "left=1,four=1,felt=2,back=1,anim=2" for the scenes */
        UiPrefs &p = session_->prefs();
        for (std::string it : [&] { std::vector<std::string> v; std::string cur; for (const char *c = tp;; ++c) { if (*c == ',' || !*c) { v.push_back(cur); cur.clear(); if (!*c) break; } else cur += *c; } return v; }()) {
            const size_t eq = it.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = it.substr(0, eq);
            const int val = std::atoi(it.c_str() + eq + 1);
            if (k == "left") p.left_handed = val != 0;
            else if (k == "four") p.four_colour = val != 0;
            else if (k == "felt") p.felt = std::min(2, std::max(0, val));
            else if (k == "back") p.back = std::min(1, std::max(0, val));
            else if (k == "anim") p.anim = std::min(2, std::max(0, val));
            else if (k == "winnable") p.winnable = val != 0;
        }
    }
    stat_draw_ = game().rules().draw;
    stat_passes_ = game().rules().passes;
    stat_scoring_ = static_cast<int>(game().rules().scoring);

    win_ = make_win_animation();
    build_game_widgets();
    show_title();
    focus(board_->surface());
    lv_obj_add_event_cb(board_->surface(), surface_key_cb, LV_EVENT_KEY, this);
    if (LV_EVENT_KEYBOARD != 0) lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    last_tick_ = lv_tick_get();
    tick_timer_ = lv_timer_create(tick_cb, 250, this);
    after_change(false);
    if (const char *z = std::getenv("KLONDIKE_TEST_ZFUZZ")) { /* test only: "seed,steps" random play to check the stacking */
        unsigned seed = 1, steps = 300;
        std::sscanf(z, "%u,%u", &seed, &steps);
        zfuzz_rng_ = seed * 2654435761u + 12345u;
        zfuzz_left_ = static_cast<int>(steps);
        zfuzz_timer_ = lv_timer_create(zfuzz_cb, 23, this);
    }
    if (session_->in_progress() || resumed) notice(resumed ? "Game resumed" : "", resumed ? 1800 : 0);
}

KlondikeApp::~KlondikeApp()
{
    alive_ = false;
    lv_async_call_cancel(posted_cb, this);
    if (session_) session_->save_game();
    cancel_jobs();
    stop_autoplay();
    for (lv_timer_t *t : {tick_timer_, banner_timer_, hint_timer_, deadend_timer_, zfuzz_timer_})
        if (t) lv_timer_delete(t);
    if (LV_EVENT_KEYBOARD != 0 && lv_obj_is_valid(screen_))
        lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    if (win_) win_->stop();
    field_.reset();
    dialog_.reset();
    list_.reset();
    text_.reset();
    board_.reset();
    page_.reset();
}

void KlondikeApp::posted_cb(void *data)
{
    auto *self = static_cast<KlondikeApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void KlondikeApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

void KlondikeApp::focus(lv_obj_t *obj)
{
    lv_group_t *group = cdk::input_group();
    if (!group || !obj) return;
    if (lv_obj_get_group(obj) != group) lv_group_add_obj(group, obj);
    lv_group_focus_obj(obj);
}

BoardStyle KlondikeApp::board_style() const
{
    const UiPrefs &p = session_->prefs();
    BoardStyle s;
    s.four_colour = p.four_colour;
    s.felt = p.felt;
    s.back = p.back;
    s.anim = p.anim;
    return s;
}

Layout KlondikeApp::compute_layout() const
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    LayoutIn in;
    in.w = layout_.w;
    in.h = layout_.h;
    in.body_y = layout_.body_y;
    in.target = m->tok[CP0_TOK_TARGET];
    in.pad = std::max(4, m->tok[CP0_TOK_SPACE_M]);
    in.hud_h = hud_h_;
    for (const char *t : {"Undo", "Redo", "Hint", "Auto", "Menu"}) in.label_w = std::max(in.label_w, text_w(t, cp0_ui_font(CP0_TEXT_BODY)));
    in.ppmm_x100 = m->ppmm_x100;
    in.left_handed = session_->prefs().left_handed;
    in.draw3 = game().rules().draw == 3;
    return klondike::compute_layout(in);
}

void KlondikeApp::build_game_widgets()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    lv_obj_t *root = page_->root();
    const lv_font_t *body = cp0_ui_font(CP0_TEXT_BODY);
    hud_h_ = lv_font_get_line_height(body) + std::max(4, m->tok[CP0_TOK_SPACE_S]);
    lay_ = compute_layout();

    BoardEvents ev;
    ev.can_move = [this](int from, int to, int count) {
        Move mv;
        mv.from = static_cast<std::uint8_t>(from);
        mv.to = static_cast<std::uint8_t>(to);
        mv.count = static_cast<std::uint8_t>(count);
        return game().can_apply(mv);
    };
    ev.move = [this](const Move &mv) { user_move(mv); };
    ev.tap = [this](Hit h) { on_tap(h); };
    ev.double_tap = [this](Hit h) { on_double_tap(h); };
    ev.drag_started = [this] {
        clear_selection();
        clear_hint();
        keys_used_ = false;
        board_->hide_cursor();
    };
    board_ = std::make_unique<BoardView>(root, lay_, std::move(ev), board_style());

    hud_ = lv_obj_create(root);
    lv_obj_remove_style_all(hud_);
    lv_obj_remove_flag(hud_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_flex_flow(hud_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hud_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (lv_obj_t *&item : hud_item_) {
        item = lv_label_create(hud_);
        lv_obj_set_style_text_color(item, lv_color_white(), 0);
        lv_obj_set_style_text_font(item, body, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);
    }

    dock_ = lv_obj_create(root);
    lv_obj_remove_style_all(dock_);
    lv_obj_set_style_bg_color(dock_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dock_, LV_OPA_30, 0);
    lv_obj_remove_flag(dock_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    for (int i = 0; i < kDockButtons; ++i) {
        lv_obj_t *b = lv_obj_create(root);
        lv_obj_remove_style_all(b);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2B2B2B), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x555555), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, lv_color_hex(0x666666), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, std::max(6, m->tok[CP0_TOK_RADIUS_M]), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(b, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE));
        g_button_ctx[i] = {this, i};
        lv_obj_add_event_cb(b, button_cb, LV_EVENT_CLICKED, &g_button_ctx[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, kButtonText[i]);
        lv_obj_set_style_text_font(l, body, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
        btn_[i] = b;
        btn_label_[i] = l;
    }

    banner_ = lv_obj_create(root);
    lv_obj_remove_style_all(banner_);
    lv_obj_set_style_bg_color(banner_, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(banner_, LV_OPA_90, 0);
    lv_obj_set_style_radius(banner_, 10, 0);
    lv_obj_set_style_pad_all(banner_, std::max(6, m->tok[CP0_TOK_SPACE_M]), 0);
    lv_obj_set_style_border_color(banner_, lv_color_hex(0xFFD84D), 0);
    lv_obj_set_style_border_width(banner_, 1, 0);
    lv_obj_set_style_border_opa(banner_, LV_OPA_60, 0);
    lv_obj_remove_flag(banner_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    banner_label_ = lv_label_create(banner_);
    lv_obj_set_style_text_font(banner_label_, cp0_ui_font(CP0_TEXT_CAPTION), 0);
    lv_obj_set_style_text_color(banner_label_, lv_color_white(), 0);
    lv_label_set_long_mode(banner_label_, LV_LABEL_LONG_WRAP);
    apply_layout();
}

void KlondikeApp::apply_layout()
{
    lv_obj_set_pos(hud_, lay_.hud.x, lay_.hud.y);
    lv_obj_set_size(hud_, lay_.hud.w, lay_.hud.h);
    lv_obj_set_style_pad_left(hud_, std::max(4, lay_.card_w / 12), 0);
    lv_obj_set_style_pad_column(hud_, std::max(8, lay_.card_w / 4), 0);
    lv_obj_set_pos(dock_, lay_.dock.x, lay_.dock.y);
    lv_obj_set_size(dock_, lay_.dock.w, lay_.dock.h);
    for (int i = 0; i < kDockButtons; ++i) {
        lv_obj_set_pos(btn_[i], lay_.button[i].x, lay_.button[i].y);
        lv_obj_set_size(btn_[i], lay_.button[i].w, lay_.button[i].h);
        lv_obj_center(btn_label_[i]);
    }
    fit_button_fonts();
    raise_chrome();
    hud_shown_.clear();
}

void KlondikeApp::show_game_widgets(bool shown)
{
    for (lv_obj_t *o : {hud_, dock_, btn_[0], btn_[1], btn_[2], btn_[3]}) {
        if (shown) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!shown) lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    board_->set_hidden(!shown);
    if (shown) {
        refresh_hud();
        if (banner_timer_ || dead_banner_) lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    }
}

void KlondikeApp::stop_win()
{
    if (win_) win_->stop();
    set_dock_backing(false);
}

/* while the cascade plays, the cards pass under the side dock: it gets the felt's colour for a backing */
void KlondikeApp::set_dock_backing(bool on)
{
    if (!dock_) return;
    lv_obj_set_style_bg_color(dock_, on ? lv_obj_get_style_bg_color(board_->felt(), LV_PART_MAIN) : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dock_, on ? LV_OPA_COVER : LV_OPA_30, 0);
}

void KlondikeApp::raise_chrome()
{
    for (lv_obj_t *o : {dock_, btn_[0], btn_[1], btn_[2], btn_[3], hud_, banner_}) lv_obj_move_foreground(o);
}

void KlondikeApp::apply_prefs_change(bool relayout)
{
    session_->save_prefs();
    board_->set_style(board_style());
    if (relayout) {
        if (win_ && win_->running()) win_->skip(); /* the cascade's trail is sized for the old layout */
        lay_ = compute_layout();
        board_->set_layout(lay_);
        apply_layout();
    }
}

/* ------------------------------------------------------------------------------------------------ the clock */

void KlondikeApp::tick_cb(lv_timer_t *t)
{
    auto *self = static_cast<KlondikeApp *>(lv_timer_get_user_data(t));
    const std::uint32_t now = lv_tick_get();
    std::uint32_t dt = lv_tick_elaps(self->last_tick_);
    self->last_tick_ = now;
    const bool hidden = cdk::hidden();
    if (hidden && !self->was_hidden_) self->session_->save_game();
    self->was_hidden_ = hidden;
    /* a frozen app (SIGSTOP while hidden) wakes up with a huge gap: it was not played */
    if (dt > 1000) dt = 0;
    if (hidden || self->view_ != View::Game || self->dialog_ || self->won_shown_) return;
    self->session_->tick(dt);
    self->last_save_ms_ += dt;
    if (self->last_save_ms_ >= 20000) {
        self->last_save_ms_ = 0;
        if (self->session_->in_progress()) self->session_->save_game();
    }
    self->refresh_hud();
}

/* ------------------------------------------------------------------------------------------------ HUD, dock */

std::vector<std::string> KlondikeApp::hud_items(bool with_deal) const
{
    const Game &g = game();
    const Rules &r = g.rules();
    std::vector<std::string> v;
    if (r.scoring != Scoring::None) v.push_back("Score " + std::to_string(g.score()));
    if (r.scoring == Scoring::Vegas) v.push_back("Bank " + std::to_string(session_->vegas_bank_now()));
    if (r.scoring == Scoring::Standard && r.timed) v.push_back("Time " + mmss(g.elapsed_s()));
    v.push_back("Moves " + std::to_string(g.moves_made()));
    if (with_deal) v.push_back(std::string(g.winnable() ? "Winnable " : "Deal ") + std::to_string(g.user_number()));
    return v;
}

void KlondikeApp::refresh_hud()
{
    if (!hud_ || view_ != View::Game) return;
    const int pad = std::max(4, lay_.card_w / 12);
    const int gap = std::max(8, lay_.card_w / 4);
    const int max_w = lay_.hud.w - 2 * pad;
    const lv_font_t *fonts[2] = {cp0_ui_font(CP0_TEXT_BODY), cp0_ui_font(CP0_TEXT_CAPTION)};
    std::vector<std::string> items;
    const lv_font_t *font = fonts[1];
    bool done = false;
    for (int deal = 1; deal >= 0 && !done; --deal)
        for (const lv_font_t *f : fonts) {
            const std::vector<std::string> t = hud_items(deal != 0);
            int total = -gap;
            for (const std::string &s : t) {
                lv_point_t sz{};
                lv_text_get_size(&sz, s.c_str(), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                total += sz.x + gap;
            }
            if (total <= max_w) {
                items = t;
                font = f;
                done = true;
                break;
            }
        }
    if (!done) items = hud_items(false);
    std::string key;
    for (const std::string &s : items) key += s + "|";
    if (key == hud_shown_) return;
    hud_shown_ = key;
    for (int i = 0; i < 5; ++i) {
        if (i < static_cast<int>(items.size())) {
            lv_label_set_text(hud_item_[i], items[i].c_str());
            lv_obj_set_style_text_font(hud_item_[i], font, 0);
            lv_obj_remove_flag(hud_item_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hud_item_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    check_labels();
}

/* A label is drawn in the body font, or in the caption font when the button is too narrow for it (a very narrow canvas);
 * the layout already made the side dock wide enough for the body font. */
void KlondikeApp::fit_button_fonts()
{
    for (int i = 0; i < kDockButtons; ++i) {
        const int room = lay_.button[i].w - 2 * label_air(std::max(4, cp0_ui_metrics_get()->tok[CP0_TOK_SPACE_M]));
        const char *text = lv_label_get_text(btn_label_[i]);
        const lv_font_t *f = cp0_ui_font(CP0_TEXT_BODY);
        if (text_w(text, f) > room) f = cp0_ui_font(CP0_TEXT_CAPTION);
        lv_obj_set_style_text_font(btn_label_[i], f, 0);
        lv_obj_center(btn_label_[i]);
    }
    check_labels();
}

/* test only (KLONDIKE_TEST_ZCHECK): no dock label and no HUD line wider than what holds it; a violation stays as a LABELCLIP
 * label (scenes: expect notext "LABELCLIP") and goes to stderr. */
void KlondikeApp::check_labels()
{
    if (!env_on("KLONDIKE_TEST_ZCHECK") && !env_on("KLONDIKE_TEST_LABELS")) return;
    lv_obj_update_layout(screen_);
    std::string bad;
    for (int i = 0; i < kDockButtons; ++i)
        if (!lv_obj_has_flag(btn_[i], LV_OBJ_FLAG_HIDDEN) && lv_obj_get_width(btn_label_[i]) > lv_obj_get_width(btn_[i]) - 2)
            bad += std::string(lv_label_get_text(btn_label_[i])) + " (" + std::to_string(lv_obj_get_width(btn_label_[i])) + " > " + std::to_string(lv_obj_get_width(btn_[i])) + ") ";
    int hud_total = 0;
    for (lv_obj_t *it : hud_item_)
        if (!lv_obj_has_flag(it, LV_OBJ_FLAG_HIDDEN)) hud_total += lv_obj_get_width(it);
    if (hud_total > lay_.hud.w) bad += "HUD " + std::to_string(hud_total) + " > " + std::to_string(lay_.hud.w);
    if (bad.empty()) return;
    std::fprintf(stderr, "LABELCLIP %s\n", bad.c_str());
    if (clip_mark_) return;
    clip_mark_ = lv_label_create(screen_);
    lv_label_set_text(clip_mark_, "LABELCLIP");
    lv_obj_set_size(clip_mark_, 60, 12);
    lv_obj_set_style_text_opa(clip_mark_, LV_OPA_TRANSP, 0);
}

void KlondikeApp::refresh_buttons()
{
    const bool done = session_->finished();
    const bool dim[kDockButtons] = {!game().can_undo() || done, !game().can_redo() || done, done && !auto_available_, false};
    for (int i = 0; i < kDockButtons; ++i) lv_obj_set_style_text_opa(btn_label_[i], dim[i] ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_label_set_text(btn_label_[2], auto_available_ && !done ? "Auto" : "Hint");
    fit_button_fonts();
}

void KlondikeApp::button_cb(lv_event_t *e)
{
    auto *ctx = static_cast<ButtonCtx *>(lv_event_get_user_data(e));
    ctx->app->post([app = ctx->app, i = ctx->index] { app->button_action(i); });
}

void KlondikeApp::button_action(int index)
{
    if (view_ != View::Game || dialog_ || autoplay_) return;
    if (win_ && win_->running()) {
        win_->skip();
        return;
    }
    keys_used_ = false;
    board_->hide_cursor();
    switch (index) {
    case 0: do_undo(); break;
    case 1: do_redo(); break;
    case 2:
        if (auto_available_ && !session_->finished()) do_auto_complete();
        else do_hint();
        break;
    case 3: menu_sel_ = 0; open_view(View::Menu); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ notices */

void KlondikeApp::banner_timer_cb(lv_timer_t *t)
{
    auto *self = static_cast<KlondikeApp *>(lv_timer_get_user_data(t));
    self->banner_timer_ = nullptr; /* one shot: LVGL deletes it */
    if (!self->dead_banner_) lv_obj_add_flag(self->banner_, LV_OBJ_FLAG_HIDDEN);
}

void KlondikeApp::notice(const std::string &text, int ms)
{
    if (banner_timer_) {
        lv_timer_delete(banner_timer_);
        banner_timer_ = nullptr;
    }
    if (text.empty()) {
        if (!dead_banner_) lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(banner_label_, text.c_str());
    const lv_font_t *font = cp0_ui_font(CP0_TEXT_CAPTION);
    lv_point_t sz{};
    lv_text_get_size(&sz, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int pad = lv_obj_get_style_pad_left(banner_, LV_PART_MAIN);
    const int max_w = std::max(80, lay_.board.w - 16);
    const int bw = std::min(max_w, sz.x + 2 * pad + 4);
    lv_obj_set_width(banner_, bw);
    lv_obj_set_width(banner_label_, bw - 2 * pad);
    lv_obj_set_height(banner_, LV_SIZE_CONTENT);
    lv_obj_update_layout(banner_);
    const int bh = lv_obj_get_height(banner_);
    lv_obj_set_pos(banner_, lay_.board.x + (lay_.board.w - bw) / 2, lay_.board.bottom() - bh - 8);
    lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(banner_);
    if (ms > 0) {
        banner_timer_ = lv_timer_create(banner_timer_cb, static_cast<uint32_t>(ms), this);
        lv_timer_set_repeat_count(banner_timer_, 1);
    }
}

void KlondikeApp::clear_banner()
{
    dead_banner_ = false;
    if (banner_timer_) {
        lv_timer_delete(banner_timer_);
        banner_timer_ = nullptr;
    }
    lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------------------------------------ game flow */

void KlondikeApp::after_change(bool animate)
{
    ++version_;
    clear_hint();
    if (dead_banner_) clear_banner();
    board_->sync(game(), animate);
    cursor_fix();
    show_cursor();
    auto_available_ = false;
    if (!session_->finished() && !autoplay_) {
        std::vector<Move> plan;
        auto_available_ = auto_plan(plan);
    }
    refresh_hud();
    refresh_buttons();
    if (session_->finished() && !won_shown_) on_win();
    else if (!autoplay_) schedule_deadend();
}

void KlondikeApp::user_move(const Move &m)
{
    clear_selection();
    clear_hint();
    if (!session_->play(m)) {
        board_->sync(game(), true);
        return;
    }
    if (!session_->finished()) session_->auto_moves();
    after_change(true);
}

void KlondikeApp::draw_from_stock()
{
    const Move m = game().stock_move();
    if (m.count == 0) {
        notice("No more passes through the stock", 2200);
        return;
    }
    user_move(m);
}

void KlondikeApp::on_tap(const Hit &h)
{
    if (view_ != View::Game || dialog_ || autoplay_ || session_->finished()) return;
    keys_used_ = false;
    board_->hide_cursor();
    clear_hint();
    if (!h.valid()) {
        clear_selection();
        return;
    }
    if (h.pile == kStock) {
        clear_selection();
        draw_from_stock();
        return;
    }
    int count = 0;
    if (sel_.valid()) {
        if (h.pile == sel_.pile) {
            if (h == sel_ || !board_->pickable(h, &count)) clear_selection();
            else select(h);
            return;
        }
        int scount = 1;
        board_->pickable(sel_, &scount);
        Move m;
        m.from = static_cast<std::uint8_t>(sel_.pile);
        m.to = static_cast<std::uint8_t>(h.pile);
        m.count = static_cast<std::uint8_t>(scount);
        if (game().can_apply(m)) {
            user_move(m);
            return;
        }
        if (board_->pickable(h, &count)) select(h);
        else notice("That card can not go there", 1500);
        return;
    }
    if (board_->pickable(h, &count)) select(h);
}

void KlondikeApp::on_double_tap(const Hit &h)
{
    if (view_ != View::Game || dialog_ || autoplay_ || session_->finished()) return;
    keys_used_ = false;
    board_->hide_cursor();
    const Board &b = game().board();
    int count = 0;
    if (!board_->pickable(h, &count) || count != 1) {
        on_tap(h);
        return;
    }
    const Card c = b.card(h.pile, h.idx);
    const int f = is_foundation(h.pile) ? -1 : foundation_for(b, c);
    if (f < 0) {
        notice("Not ready for a foundation", 1200);
        return;
    }
    Move m;
    m.from = static_cast<std::uint8_t>(h.pile);
    m.to = static_cast<std::uint8_t>(f);
    m.count = 1;
    user_move(m);
}

void KlondikeApp::select(const Hit &h)
{
    sel_ = h;
    board_->show_selection(h.pile, h.idx);
    if (keys_used_) show_cursor();
}

void KlondikeApp::clear_selection()
{
    sel_ = Hit{};
    board_->hide_selection();
}

/* test only (KLONDIKE_TEST_ZFUZZ=seed,steps): a random player on the real handlers, one step every 23 ms so that many steps
 * land while the last move still flies. BoardView checks the stacking after each of them (a violation is a ZFAIL label). */
void KlondikeApp::zfuzz_cb(lv_timer_t *t)
{
    static_cast<KlondikeApp *>(lv_timer_get_user_data(t))->zfuzz_step();
}

void KlondikeApp::zfuzz_step()
{
    if (zfuzz_left_ <= 0) {
        if (zfuzz_timer_) lv_timer_pause(zfuzz_timer_);
        std::fprintf(stderr, "zfuzz done\n");
        lv_obj_t *done = lv_label_create(screen_); /* the scene waits for this label: the whole run was played */
        lv_label_set_text(done, "ZFUZZ DONE");
        lv_obj_set_size(done, 60, 12);
        lv_obj_set_style_text_opa(done, LV_OPA_TRANSP, 0);
        return;
    }
    if (view_ != View::Game || dialog_ || autoplay_) return;
    auto rnd = [this](unsigned n) {
        zfuzz_rng_ ^= zfuzz_rng_ << 13;
        zfuzz_rng_ ^= zfuzz_rng_ >> 17;
        zfuzz_rng_ ^= zfuzz_rng_ << 5;
        return (zfuzz_rng_ >> 8) % n;
    };
    --zfuzz_left_;
    if (win_ && win_->running()) win_->skip();
    if (session_->finished() || zfuzz_left_ % 90 == 0) { /* a fresh deal */
        stop_win();
        won_shown_ = false;
        board_->set_input_enabled(true);
        clear_selection();
        const std::uint32_t d = 1 + rnd(5000);
        session_->begin_deal(d, d, false);
        after_change(false);
        return;
    }
    const Board &b = game().board();
    std::vector<Move> mv;
    for (const Move &m : game().legal_moves())
        if (m.count > 0 && m.from != kStock && m.from < kPiles && m.to != m.from && m.from != kStock && !(m.from == kWaste && m.to == kStock)) mv.push_back(m);
    auto pick = [&]() -> const Move * { return mv.empty() ? nullptr : &mv[rnd(static_cast<unsigned>(mv.size()))]; };
    auto from_hit = [&](const Move &m) { return Hit{m.from, b.size(m.from) - m.count}; };
    auto to_hit = [&](const Move &m) { return Hit{m.to, is_tableau(m.to) && b.size(m.to) > 0 ? b.size(m.to) - 1 : -1}; };
    switch (rnd(16)) {
    case 0: case 1: case 2: case 3:
        if (const Move *m = pick()) board_->test_drag(m->from, b.size(m->from) - m->count, m->to);
        break;
    case 4: { /* a drop on nothing: the cards fly back */
        const int c = static_cast<int>(rnd(7));
        if (b.ntab[c] > 0) board_->test_drag(tableau_pile(c), b.ntab[c] - 1 - static_cast<int>(rnd(static_cast<unsigned>(b.ntab[c] - b.ndown[c] > 0 ? b.ntab[c] - b.ndown[c] : 1))), -1);
        break;
    }
    case 5: case 6:
        if (const Move *m = pick()) {
            on_tap(from_hit(*m));
            on_tap(to_hit(*m));
        }
        break;
    case 7: case 8: {
        const int p = rnd(3) == 0 ? static_cast<int>(kWaste) : tableau_pile(static_cast<int>(rnd(7)));
        if (b.size(p) > 0) {
            on_double_tap(Hit{p, b.size(p) - 1});
        }
        break;
    }
    case 9: do_undo(); break;
    case 10: do_redo(); break;
    case 11: for (unsigned k = 1 + rnd(5); k > 0; --k) do_undo(); break;
    case 12: do_hint(); break;
    case 13: case 14: on_tap(Hit{kStock, -1}); break;
    default:
        if (rnd(3) == 0) { /* resume from the save */
            session_->save_game();
            session_->load();
            after_change(false);
        } else if (rnd(2) == 0) { /* restart the deal */
            session_->restart();
            after_change(false);
        } else { /* the other hand */
            session_->prefs().left_handed = !session_->prefs().left_handed;
            apply_prefs_change(true);
            after_change(false);
        }
        break;
    }
}

void KlondikeApp::do_undo()
{
    if (session_->finished()) return;
    clear_selection();
    if (!session_->undo()) {
        notice("Nothing to undo", 1200);
        return;
    }
    after_change(true);
}

void KlondikeApp::do_redo()
{
    if (session_->finished()) return;
    clear_selection();
    if (!session_->redo()) {
        notice("Nothing to redo", 1200);
        return;
    }
    after_change(true);
}

void KlondikeApp::hint_timer_cb(lv_timer_t *t)
{
    auto *self = static_cast<KlondikeApp *>(lv_timer_get_user_data(t));
    self->hint_timer_ = nullptr;
    self->board_->hide_hint();
}

void KlondikeApp::clear_hint()
{
    if (hint_timer_) {
        lv_timer_delete(hint_timer_);
        hint_timer_ = nullptr;
    }
    if (board_) board_->hide_hint();
}

void KlondikeApp::do_hint()
{
    if (session_->finished()) return;
    clear_selection();
    clear_hint();
    const Move m = klondike::hint(game());
    if (m.count == 0) {
        notice("No useful move: undo, or start a new game", 2800);
        return;
    }
    const Board &b = game().board();
    int src_idx = -1;
    if (is_tableau(m.from)) src_idx = b.ntab[m.from - kTableau0] - m.count;
    else if (m.from == kWaste) src_idx = b.tp - 1;
    else if (is_foundation(m.from)) src_idx = b.size(m.from) - 1;
    const bool stock_op = (m.from == kStock || m.to == kStock);
    if (stock_op) board_->show_hint(kStock, -1, m.to == kStock ? kStock : kWaste);
    else board_->show_hint(m.from, src_idx, m.to);
    hint_timer_ = lv_timer_create(hint_timer_cb, 3500, this);
    lv_timer_set_repeat_count(hint_timer_, 1);
}

/* all face up: the cards can be sent up by the rules (draws allowed, a recycle is not: it could cost points) */
bool KlondikeApp::auto_plan(std::vector<Move> &plan) const
{
    Board b = game().board();
    const Rules &r = game().rules();
    if (b.won()) return false;
    for (int c = 0; c < kColumns; ++c)
        if (b.ndown[c] > 0) return false;
    plan.clear();
    int idle = 0;
    for (int step = 0; step < 700; ++step) {
        if (b.won()) return true;
        Move found;
        if (b.waste_size() > 0) {
            const int f = foundation_for(b, b.top(kWaste));
            if (f >= 0) found = Move{static_cast<std::uint8_t>(kWaste), static_cast<std::uint8_t>(f), 1};
        }
        for (int c = 0; c < kColumns && found.count == 0; ++c) {
            if (b.ntab[c] == 0) continue;
            const int f = foundation_for(b, b.top(tableau_pile(c)));
            if (f >= 0) found = Move{static_cast<std::uint8_t>(tableau_pile(c)), static_cast<std::uint8_t>(f), 1};
        }
        if (found.count) {
            idle = 0;
        } else {
            found = stock_move(b, r);
            if (found.count == 0 || kind_of(found) == MoveKind::Recycle) return false;
            if (++idle > 60) return false;
        }
        if (!can_move(b, r, found)) return false;
        do_move(b, r, found);
        plan.push_back(found);
    }
    return false;
}

void KlondikeApp::autoplay_cb(lv_timer_t *t)
{
    static_cast<KlondikeApp *>(lv_timer_get_user_data(t))->autoplay_step();
}

void KlondikeApp::stop_autoplay()
{
    if (autoplay_) lv_timer_delete(autoplay_);
    autoplay_ = nullptr;
}

void KlondikeApp::do_auto_complete()
{
    clear_selection();
    clear_hint();
    if (!auto_plan(auto_plan_) || auto_plan_.empty()) return;
    auto_step_ = 0;
    const int ms = session_->prefs().anim == 2 ? 25 : (session_->prefs().anim == 1 ? 60 : 110);
    autoplay_ = lv_timer_create(autoplay_cb, static_cast<uint32_t>(ms), this);
}

void KlondikeApp::autoplay_step()
{
    if (auto_step_ >= auto_plan_.size() || view_ != View::Game) {
        stop_autoplay();
        after_change(true);
        return;
    }
    const Move m = auto_plan_[auto_step_];
    if (!session_->play(m, auto_step_ > 0)) {
        stop_autoplay();
        after_change(true);
        return;
    }
    ++auto_step_;
    board_->sync(game(), true);
    refresh_hud();
    if (session_->finished()) {
        stop_autoplay();
        after_change(false);
    }
}

void KlondikeApp::request_new_game()
{
    stop_autoplay();
    auto go = [this] { start_new_game(session_->random_deal(), session_->prefs().winnable); };
    if (session_->in_progress()) {
        confirm("Start a new game?", "The game in progress counts as a lost game.", "New game", go);
    } else {
        go();
    }
}

void KlondikeApp::start_new_game(std::uint32_t user, bool want_winnable)
{
    cancel_jobs();
    stop_win();
    won_shown_ = false;
    board_->set_input_enabled(true);
    clear_selection();
    clear_banner();
    if (view_ != View::Game) close_view();
    if (want_winnable) {
        search_winnable(user);
        return;
    }
    session_->begin_deal(user, user, false);
    lay_ = compute_layout();
    board_->set_layout(lay_);
    apply_layout();
    after_change(false);
}

void KlondikeApp::search_winnable(std::uint32_t user)
{
    notice("Looking for a winnable deal...", 0);
    dead_banner_ = false;
    winnable_user_ = user;
    cdk::async_run::Spec spec;
    spec.argv = {exe_, "--klondike-worker", "winnable", std::to_string(user), session_->rules().serialize()};
    spec.timeout_ms = 45000;
    spec.merge_stderr = false;
    auto lines = std::make_shared<std::string>();
    spec.on_line = [lines](const std::string &line) { *lines = line; };
    spec.on_done = [this, lines, user](const cdk::async_run::Result &r) {
        if (!alive_) return;
        winnable_job_ = 0;
        bool found = false;
        std::uint32_t deal = 0;
        const bool parsed = r.ok() && parse_worker_winnable(*lines, found, deal);
        lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
        if (r.cancelled) return;
        if (parsed && found) session_->begin_deal(deal, user, true);
        else session_->begin_deal(user, user, false);
        lay_ = compute_layout();
        board_->set_layout(lay_);
        apply_layout();
        after_change(false);
        if (!(parsed && found)) notice("No winnable deal found: playing deal " + std::to_string(user), 4000);
    };
    winnable_job_ = cdk::async_run::run(std::move(spec));
}

void KlondikeApp::request_restart()
{
    auto go = [this] {
        cancel_jobs();
        stop_win();
        won_shown_ = false;
        board_->set_input_enabled(true);
        clear_selection();
        clear_banner();
        session_->restart();
        lay_ = compute_layout();
        board_->set_layout(lay_);
        apply_layout();
        after_change(false);
    };
    if (session_->in_progress()) confirm("Restart this deal?", "The game in progress counts as a lost game.", "Restart", go);
    else go();
}

void KlondikeApp::on_win()
{
    won_shown_ = true;
    stop_autoplay();
    cancel_jobs();
    clear_selection();
    board_->hide_marks();
    board_->set_input_enabled(false);
    notice("", 0);
    win_->start(*board_, [this] { post([this] { show_win_dialog(); }); });
    set_dock_backing(true);
    raise_chrome(); /* the flying cards pass under the buttons */
}

void KlondikeApp::show_win_dialog()
{
    if (!alive_ || dialog_ || !won_shown_) return;
    const Game &g = game();
    cp0::ui::DialogSpec spec;
    spec.title = "You won!";
    spec.page = page_.get();
    spec.buttons.push_back({"Close", cp0::ui::ButtonRole::Cancel, [this] { post([this] { close_dialog(); }); }});
    spec.buttons.push_back({"Statistics", cp0::ui::ButtonRole::Normal, [this] {
                                post([this] {
                                    close_dialog();
                                    open_view(View::Stats);
                                });
                            }});
    spec.buttons.push_back({"New game", cp0::ui::ButtonRole::Primary, [this] {
                                post([this] {
                                    close_dialog();
                                    request_new_game();
                                });
                            }});
    dialog_ = std::make_unique<cp0::ui::Dialog>(page_->root(), layout_, std::move(spec));
    std::string text;
    if (g.rules().scoring != Scoring::None) text += "Score " + std::to_string(g.score()) + "\n";
    text += "Time " + mmss(g.elapsed_s()) + "\nMoves " + std::to_string(g.moves_made());
    if (g.rules().scoring == Scoring::Vegas) text += "\nBank " + std::to_string(session_->vegas_bank_now());
    dialog_->add_text(text);
    dialog_->relayout();
    focus(dialog_->focus_object());
}

/* ------------------------------------------------------------------------------------------------ helper jobs */

void KlondikeApp::cancel_jobs()
{
    if (deadend_job_) cdk::async_run::cancel(deadend_job_);
    if (winnable_job_) cdk::async_run::cancel(winnable_job_);
    deadend_job_ = winnable_job_ = 0;
    if (deadend_timer_) {
        lv_timer_delete(deadend_timer_);
        deadend_timer_ = nullptr;
    }
}

void KlondikeApp::deadend_timer_cb(lv_timer_t *t)
{
    auto *self = static_cast<KlondikeApp *>(lv_timer_get_user_data(t));
    self->deadend_timer_ = nullptr;
    self->run_deadend();
}

void KlondikeApp::schedule_deadend()
{
    if (deadend_timer_) {
        lv_timer_delete(deadend_timer_);
        deadend_timer_ = nullptr;
    }
    if (session_->finished() || winnable_job_) return;
    deadend_timer_ = lv_timer_create(deadend_timer_cb, 700, this);
    lv_timer_set_repeat_count(deadend_timer_, 1);
}

void KlondikeApp::run_deadend()
{
    if (session_->finished() || winnable_job_) return;
    if (deadend_job_) cdk::async_run::cancel(deadend_job_);
    std::string text = game().board().to_text();
    std::replace(text.begin(), text.end(), '\n', ';');
    cdk::async_run::Spec spec;
    spec.argv = {exe_, "--klondike-worker", "deadend", text, game().rules().serialize()};
    spec.timeout_ms = 10000;
    spec.merge_stderr = false;
    auto line = std::make_shared<std::string>();
    spec.on_line = [line](const std::string &l) { *line = l; };
    const std::uint32_t ver = version_;
    spec.on_done = [this, line, ver](const cdk::async_run::Result &r) {
        if (!alive_) return;
        deadend_job_ = 0;
        bool dead = false;
        if (!r.ok() || !parse_worker_deadend(*line, dead) || !dead || ver != version_ || session_->finished()) return;
        dead_banner_ = true;
        if (view_ == View::Game) notice("No more useful moves. Undo, or start a new game.", 0);
    };
    deadend_job_ = cdk::async_run::run(std::move(spec));
}

/* ------------------------------------------------------------------------------------------------ keys */

void KlondikeApp::surface_key_cb(lv_event_t *e)
{
    static_cast<KlondikeApp *>(lv_event_get_user_data(e))->on_key(lv_event_get_key(e));
}

void KlondikeApp::keyboard_cb(lv_event_t *e)
{
    auto *self = static_cast<KlondikeApp *>(lv_event_get_user_data(e));
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(e));
    if (item) self->on_keyboard(*item);
}

void KlondikeApp::use_keys()
{
    if (!keys_used_) {
        keys_used_ = true;
        if (!cur_.valid()) cur_ = Hit{kStock, -1};
    }
}

void KlondikeApp::on_key(uint32_t key)
{
    if (view_ != View::Game || dialog_) return;
    if (win_ && win_->running()) { /* any key ends the cascade (and does nothing else) */
        win_->skip();
        return;
    }
    if (key == LV_KEY_ESC) {
        post([this] { back(); });
        return;
    }
    if (autoplay_ || session_->finished()) return;
    switch (key) {
    case LV_KEY_LEFT: use_keys(); cursor_move(-1, 0); break;
    case LV_KEY_RIGHT: use_keys(); cursor_move(1, 0); break;
    case LV_KEY_UP: use_keys(); cursor_move(0, -1); break;
    case LV_KEY_DOWN: use_keys(); cursor_move(0, 1); break;
    case LV_KEY_ENTER: use_keys(); cursor_activate(); break;
    default: break;
    }
}

void KlondikeApp::on_keyboard(const struct key_item &item)
{
    if (item.key_state != 1 || view_ != View::Game || dialog_ || autoplay_) return;
    if (win_ && win_->running()) {
        win_->skip();
        return;
    }
    if ((item.mods & KBD_MOD_CTRL) && (item.key_code == 24 /* KEY_O */ || item.utf8[0] == 'o' || item.utf8[0] == 0x0F)) {
        post([this] { menu_sel_ = 0; open_view(View::Menu); });
        return;
    }
    if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO)) return;
    const char c = item.utf8[0];
    if (item.utf8[1] != 0) return;
    if (c == ' ') {
        if (session_->finished()) return;
        use_keys();
        cursor_activate();
        return;
    }
    const char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (lower == 'n') {
        post([this] { request_new_game(); });
        return;
    }
    if (lower == 'm') {
        post([this] { menu_sel_ = 0; open_view(View::Menu); });
        return;
    }
    if (session_->finished()) return;
    switch (lower) {
    case 'u': do_undo(); break;
    case 'r': do_redo(); break;
    case 'h': do_hint(); break;
    case 'a':
        if (auto_available_) do_auto_complete();
        break;
    default: break;
    }
}

void KlondikeApp::cursor_fix()
{
    if (!cur_.valid()) return;
    const Board &b = game().board();
    if (cur_.pile == kStock) cur_.idx = -1;
    else if (cur_.pile == kWaste) cur_.idx = b.tp > 0 ? b.tp - 1 : -1;
    else if (is_foundation(cur_.pile)) cur_.idx = b.size(cur_.pile) > 0 ? b.size(cur_.pile) - 1 : -1;
    else {
        const int c = cur_.pile - kTableau0;
        const int n = b.ntab[c], nd = b.ndown[c];
        if (n == 0) cur_.idx = -1;
        else cur_.idx = std::min(n - 1, std::max(nd, cur_.idx));
    }
}

void KlondikeApp::show_cursor()
{
    if (!keys_used_ || !cur_.valid()) {
        board_->hide_cursor();
        return;
    }
    Hit h = cur_;
    if (sel_.valid() && is_tableau(h.pile)) h.idx = std::max(-1, game().board().ntab[h.pile - kTableau0] - 1);
    board_->show_cursor(h, !sel_.valid());
}

void KlondikeApp::cursor_move(int dx, int dy)
{
    const Layout &l = lay_;
    int top_row[6] = {kStock, kWaste, foundation_pile(0), foundation_pile(1), foundation_pile(2), foundation_pile(3)};
    std::sort(top_row, top_row + 6, [&](int a, int b) { return l.pile_col[a] < l.pile_col[b]; });
    const bool in_top = !is_tableau(cur_.pile);
    if (dx != 0) {
        if (in_top) {
            int pos = 0;
            for (int i = 0; i < 6; ++i)
                if (top_row[i] == cur_.pile) pos = i;
            pos = (pos + dx + 6) % 6;
            cur_ = Hit{top_row[pos], -1};
        } else {
            int c = (cur_.pile - kTableau0 + dx + kColumns) % kColumns;
            cur_ = Hit{tableau_pile(c), 1000};
        }
    } else if (dy < 0) {
        if (!in_top) {
            const Board &b = game().board();
            const int c = cur_.pile - kTableau0;
            const int nd = b.ndown[c];
            if (b.ntab[c] > 0 && cur_.idx > nd) {
                --cur_.idx;
            } else {
                /* up into the top row: the pile at this column, else the nearest one */
                int best = top_row[0], bd = 99;
                for (int p : top_row) {
                    const int d = std::abs(l.pile_col[p] - c);
                    if (d < bd) {
                        bd = d;
                        best = p;
                    }
                }
                cur_ = Hit{best, -1};
            }
        }
    } else if (dy > 0) {
        if (in_top) {
            cur_ = Hit{tableau_pile(l.pile_col[cur_.pile]), 1000};
        } else {
            const int n = game().board().ntab[cur_.pile - kTableau0];
            if (cur_.idx < n - 1) ++cur_.idx;
        }
    }
    cursor_fix();
    show_cursor();
}

void KlondikeApp::cursor_activate()
{
    if (!cur_.valid()) return;
    clear_hint();
    const Board &b = game().board();
    if (!sel_.valid()) {
        if (cur_.pile == kStock) {
            draw_from_stock();
            return;
        }
        int count = 0;
        if (board_->pickable(cur_, &count)) select(cur_);
        else notice("Nothing to pick up here", 1000);
        return;
    }
    int scount = 1;
    board_->pickable(sel_, &scount);
    if (cur_.pile == sel_.pile) {
        /* Enter again on the card: up to its foundation when it can go, else put it down */
        const Card c = b.card(sel_.pile, sel_.idx);
        const int f = (scount == 1 && !is_foundation(sel_.pile)) ? foundation_for(b, c) : -1;
        if (f >= 0) {
            Move m{static_cast<std::uint8_t>(sel_.pile), static_cast<std::uint8_t>(f), 1};
            user_move(m);
        } else {
            clear_selection();
            show_cursor();
        }
        return;
    }
    Move m;
    m.from = static_cast<std::uint8_t>(sel_.pile);
    m.to = static_cast<std::uint8_t>(cur_.pile);
    m.count = static_cast<std::uint8_t>(scount);
    if (cur_.pile == kStock || cur_.pile == kWaste || !game().can_apply(m)) {
        notice("That card can not go there", 1500);
        return;
    }
    user_move(m);
}

} // namespace kui
