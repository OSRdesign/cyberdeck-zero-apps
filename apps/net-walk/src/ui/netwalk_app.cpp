/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk: the screen, the drawing, the input and the test fixtures (see netwalk_app.hpp).
 */
#include "netwalk_app.hpp"

#include "app.hpp"
#include "fsutil.hpp"
#include "keyboard_input.h"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

using namespace netwalk;

namespace nwui {

namespace {

constexpr int kRowH = 14;
constexpr int kDlgBtnH = 14;
constexpr int kWinTitleH = 11;
constexpr int kLongTurnMs = 350; /* a press this long turns counter-clockwise */
constexpr int kLockMs = 600;     /* a hold this long toggles the padlock */
constexpr int kMovePx = 8;       /* a finger that moved this far (screen pixels) is a drag, not a press */
constexpr int kFlashMs = 110;
constexpr int kFlashSteps = 6;

bool env_on(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

/* The scores store: the real folder, or (scenes) a folder of the harness's output that one size of a scene group owns. */
ScoreStore make_store()
{
    const char *group = std::getenv("NETWALK_TEST_DATA");
    if (!group || !*group) return ScoreStore();
    const char *state = std::getenv("APPLAUNCH_STATE_DIR");
    const char *canvas = std::getenv("APPLAUNCH_CANVAS");
    const std::string dir = std::string(state ? state : "/tmp") + "/../../../../_nwdata-" + group + "/" + (canvas ? canvas : "x");
    make_dirs(dir);
    if (env_on("NETWALK_TEST_RESET")) remove_file(dir + "/scores.json");
    return ScoreStore(dir);
}

std::uint32_t now_ms()
{
    return lv_tick_get();
}

Rect grow_clamped(const Rect &r, const Rect &limit)
{
    const int x0 = std::max(r.x - 1, limit.x), y0 = std::max(r.y - 1, limit.y);
    const int x1 = std::min(r.x + r.w + 1, limit.x + limit.w), y1 = std::min(r.y + r.h + 1, limit.y + limit.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace

/* ------------------------------------------------------------------------------------------------ construction */

NetWalkApp::NetWalkApp(lv_obj_t *screen) : screen_(screen), store_(make_store())
{
    /* the launcher's page header, as in every app: Esc button, title, status strip */
    cp0_ui_roller_page(cp0_ui_metrics_get(), cp0::ui::status_strip_left(), &page_layout_);
    page_ = std::make_unique<cp0::ui::Page>(screen_, page_layout_, cp0::ui::add_title, [this] { post([this] { back(); }); });
    cp0::ui::StatusStrip::add(page_->header());
    page_->set_title({"Net Walk"});
    sw_ = page_layout_.w;
    sh_ = page_layout_.h - page_layout_.body_y;
    /* the body under the header is the surface: it draws everything (letterbox included) and takes the pointer and key
     * events; it starts below the header, so nothing interactive reaches under it */
    surface_ = lv_obj_create(page_->root());
    lv_obj_remove_style_all(surface_);
    lv_obj_set_pos(surface_, 0, page_layout_.body_y);
    lv_obj_set_size(surface_, sw_, sh_);
    lv_obj_add_flag(surface_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    lv_obj_remove_flag(surface_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_event_cb(surface_, event_cb, LV_EVENT_ALL, this);
    lv_obj_add_event_cb(surface_, key_cb, LV_EVENT_KEY, this);
    if (LV_EVENT_KEYBOARD != 0) lv_obj_add_event_cb(screen_, keyboard_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), this);
    if (lv_group_t *g = cdk::input_group()) {
        lv_group_add_obj(g, surface_);
        lv_group_focus_obj(surface_);
    }

    scores_ = store_.load();
    level_ = scores_.last();
    if (const char *l = std::getenv("NETWALK_TEST_LEVEL")) {
        bool found = false;
        const Level lv = level_from_key(l, &found);
        if (found) level_ = lv;
    }
    probes_on_ = env_on("NETWALK_TEST_PROBES");
    new_game(level_);
    last_tick_ = lv_tick_get();
    tick_timer_ = lv_timer_create(tick_cb, 250, this);
    if (const char *s = std::getenv("NETWALK_TEST_SETUP")) run_setup(s);
    relayout();
}

NetWalkApp::~NetWalkApp()
{
    alive_ = false;
    lv_async_call_cancel(posted_cb, this);
    for (lv_timer_t *t : {tick_timer_, flash_timer_})
        if (t) lv_timer_delete(t);
    if (LV_EVENT_KEYBOARD != 0 && lv_obj_is_valid(screen_))
        lv_obj_remove_event_cb_with_user_data(screen_, keyboard_cb, this);
    if (surface_ && lv_obj_is_valid(surface_)) {
        lv_obj_remove_event_cb_with_user_data(surface_, event_cb, this);
        lv_obj_remove_event_cb_with_user_data(surface_, key_cb, this);
    }
    if (probes_ && lv_obj_is_valid(probes_)) lv_obj_delete(probes_);
    page_.reset();
}

void NetWalkApp::posted_cb(void *data)
{
    auto *self = static_cast<NetWalkApp *>(data);
    self->post_armed_ = false;
    std::vector<std::function<void()>> fns;
    fns.swap(self->posted_);
    for (auto &fn : fns) fn();
}

void NetWalkApp::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
    if (!post_armed_) post_armed_ = lv_async_call(posted_cb, this) == LV_RESULT_OK;
}

/* ------------------------------------------------------------------------------------------------ layout and items */

void NetWalkApp::invalidate()
{
    if (surface_) lv_obj_invalidate(surface_);
    make_probes();
}

std::string NetWalkApp::new_label() const
{
    return layout_.short_new ? "New" : "New game";
}

void NetWalkApp::relayout()
{
    layout_ = compute_layout(sw_, sh_, game_.board().w());
    rebuild_items();
    invalidate();
}

void NetWalkApp::rebuild_items()
{
    items_.clear();
    dlg_lines_.clear();
    dlg_title_.clear();
    dlg_ = Rect();
    const int lw = layout_.lw;
    auto centered = [&](int w, int h) {
        w = std::min(w, lw - 2 * kMargin);
        return Rect{(lw - w) / 2, (layout_.lh - h) / 2, w, h};
    };
    switch (mode_) {
    case Mode::Play:
        items_.push_back({layout_.button[0], kBtnNew, new_label(), Aff::Button});
        items_.push_back({layout_.button[1], kBtnSize, "Size", Aff::Button});
        items_.push_back({layout_.button[2], kBtnScores, "Scores", Aff::Button});
        break;
    case Mode::Size: {
        dlg_title_ = "Size";
        const int h = 18 + 4 * kRowH + 4 + kDlgBtnH + 6;
        dlg_ = centered(14 + text_width("Expert 9x9 wrap") + 4 + 14 + 8, h);
        for (int i = 0; i < kLevelCount; ++i) {
            const LevelInfo &li = level_info(static_cast<Level>(i));
            items_.push_back({{dlg_.x + 4, dlg_.y + 18 + i * kRowH, dlg_.w - 8, kRowH},
                              static_cast<Id>(kSizeEasy + i), std::string(li.name) + " " + li.dims, Aff::Action});
        }
        const int bw = text_width("Cancel") + 10;
        items_.push_back({{dlg_.x + (dlg_.w - bw) / 2, dlg_.y + 18 + 4 * kRowH + 4, bw, kDlgBtnH}, kCancel, "Cancel", Aff::Button});
        break;
    }
    case Mode::Scores: {
        dlg_title_ = "Scores";
        const int h = 18 + 12 + 4 * 12 + 4 + kDlgBtnH + 6;
        dlg_ = centered(6 + text_width("Expert") + 8 + text_width("Moves") + 8 + text_width("00:00") + 6, h);
        const int bw = text_width("OK") + 18;
        items_.push_back({{dlg_.x + (dlg_.w - bw) / 2, dlg_.y + 18 + 12 + 4 * 12 + 4, bw, kDlgBtnH}, kOk, "OK", Aff::Button});
        break;
    }
    case Mode::Flash: break;
    case Mode::Win: {
        dlg_title_ = "Net Walk";
        dlg_lines_.push_back("Network up!");
        dlg_lines_.push_back(std::to_string(win_moves_) + (win_moves_ == 1 ? " move, " : " moves, ") + format_time(win_seconds_));
        if (win_record_.new_moves) dlg_lines_.push_back("New best moves!");
        if (win_record_.new_time) dlg_lines_.push_back("New best time!");
        int w = 104;
        for (const std::string &s : dlg_lines_) w = std::max(w, text_width(s) + 16);
        const int n = static_cast<int>(dlg_lines_.size());
        dlg_ = centered(w, 18 + n * kLineH + 6 + kDlgBtnH + 6);
        const int bw = text_width("OK") + 18;
        items_.push_back({{dlg_.x + (dlg_.w - bw) / 2, dlg_.y + 18 + n * kLineH + 6, bw, kDlgBtnH}, kOk, "OK", Aff::Button});
        break;
    }
    }
}

NetWalkApp::Item *NetWalkApp::item_at(int lx, int ly)
{
    for (Item &it : items_)
        if (it.r.contains(lx, ly)) return &it;
    return nullptr;
}

const NetWalkApp::Item *NetWalkApp::find_item(Id id) const
{
    for (const Item &it : items_)
        if (it.id == id) return &it;
    return nullptr;
}

/* test only: invisible labels the headless harness can find and tap (names in tests/scenes) */
void NetWalkApp::make_probes()
{
    if (!probes_on_ || !surface_) return;
    if (!probes_) {
        probes_ = lv_obj_create(page_->root());
        lv_obj_remove_style_all(probes_);
        lv_obj_set_pos(probes_, 0, page_layout_.body_y);
        lv_obj_set_size(probes_, sw_, sh_);
        lv_obj_remove_flag(probes_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
    lv_obj_clean(probes_);
    const int s = layout_.scale;
    auto add = [&](const std::string &name, const Rect &r) {
        lv_obj_t *l = lv_label_create(probes_);
        lv_label_set_text(l, name.c_str());
        lv_obj_set_style_text_opa(l, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(l, layout_.ox + r.x * s, layout_.oy + r.y * s);
        lv_obj_set_size(l, std::max(6, r.w * s), std::max(6, r.h * s));
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    };
    static const char *const modes[] = {"play", "size", "scores", "flash", "win"};
    const Rect corner{0, 0, 2, 2};
    add("@moves=" + std::to_string(game_.moves()), corner);
    add(std::string("@won=") + (game_.won() ? "1" : "0"), corner);
    add(std::string("@mode=") + modes[static_cast<int>(mode_)], corner);
    add("@powered=" + std::to_string(game_.board().powered_count()), corner);
    int locks = 0;
    for (int i = 0; i < game_.board().size(); ++i) locks += game_.board().locked(i) ? 1 : 0;
    add("@locks=" + std::to_string(locks), corner);
    int monitors = 0, lit = 0; /* the computers, and those whose screen is lime (the same array draw_tile reads) */
    for (int i = 0; i < game_.board().size(); ++i)
        if (i != game_.board().server() && game_.board().kind(i) == Kind::End) {
            ++monitors;
            lit += game_.powered()[static_cast<std::size_t>(i)] != 0 ? 1 : 0;
        }
    add("@monitors=" + std::to_string(monitors), corner);
    add("@lit=" + std::to_string(lit), corner);
    add(std::string("@alllit=") + (monitors > 0 && lit == monitors ? "1" : "0"), corner);
    add("@level=" + std::string(level_info(level_).key), corner);
    add("@scale=" + std::to_string(layout_.scale), corner);
    add("@body=" + std::to_string(sw_) + "x" + std::to_string(sh_), corner);
    add("@hdr=" + std::to_string(page_layout_.body_y), corner);
    add("@tile=" + std::to_string(layout_.tile), corner);
    add(std::string("@layout=") + (layout_.portrait ? "portrait" : "landscape"), corner);
    if (mode_ == Mode::Play) {
        const Board &b = game_.board();
        for (int i = 0; i < b.size(); ++i) {
            const Rect r = layout_.tile_rect(i);
            add("@t" + std::to_string(i % b.w()) + "," + std::to_string(i / b.w()), Rect{r.x + 1, r.y + 1, r.w - 2, r.h - 2});
        }
    }
    for (const Item &it : items_) {
        const char *name = nullptr;
        switch (it.id) {
        case kBtnNew: name = "@new"; break;
        case kBtnSize: name = "@size"; break;
        case kBtnScores: name = "@scores"; break;
        case kSizeEasy: name = "@s-easy"; break;
        case kSizeNormal: name = "@s-normal"; break;
        case kSizeHard: name = "@s-hard"; break;
        case kSizeExpert: name = "@s-expert"; break;
        case kOk: name = "@ok"; break;
        case kCancel: name = "@cancel"; break;
        default: break;
        }
        if (name) add(name, Rect{it.r.x + 1, it.r.y + 1, it.r.w - 2, it.r.h - 2});
    }
}

/* ------------------------------------------------------------------------------------------------ drawing */

void NetWalkApp::event_cb(lv_event_t *e)
{
    auto *self = static_cast<NetWalkApp *>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    switch (code) {
    case LV_EVENT_DRAW_MAIN: self->draw(e); break;
    case LV_EVENT_PRESSED:
    case LV_EVENT_PRESSING:
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: self->on_pointer(code); break;
    default: break;
    }
}

void NetWalkApp::draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t obj;
    lv_obj_get_coords(surface_, &obj);
    Painter p;
    p.layer = layer;
    p.scale = layout_.scale;
    p.ox = obj.x1 + layout_.ox;
    p.oy = obj.y1 + layout_.oy;
    /* the grey letterbox, then the logical canvas */
    {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = Painter::color(pal::kLetterbox);
        d.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &d, &obj);
    }
    if (!layout_.ok && layout_.lw == 0) return;
    p.rect(0, 0, layout_.lw, layout_.lh, pal::kFace);
    draw_board(p);
    draw_panel(p);
    if (mode_ == Mode::Size || mode_ == Mode::Scores || mode_ == Mode::Win) draw_modal(p);
}

void NetWalkApp::draw_board(const Painter &p) const
{
    const Rect &f = layout_.frame;
    const Board &b = game_.board();
    /* the frame: sunken; an Expert (wrap) board's frame is dashed navy: its edges meet */
    p.bevel(f.x, f.y, f.w, f.h, pal::kBoard, false);
    p.bevel(f.x + 1, f.y + 1, f.w - 2, f.h - 2, pal::kBoard, false);
    if (b.wrap()) {
        for (int k = 0; k < f.w; ++k) {
            const std::uint32_t c = (k / 3) % 2 ? pal::kWhite : pal::kNavy;
            p.rect(f.x + k, f.y, 1, 1, c);
            p.rect(f.x + k, f.y + f.h - 1, 1, 1, c);
        }
        for (int k = 0; k < f.h; ++k) {
            const std::uint32_t c = (k / 3) % 2 ? pal::kWhite : pal::kNavy;
            p.rect(f.x, f.y + k, 1, 1, c);
            p.rect(f.x + f.w - 1, f.y + k, 1, 1, c);
        }
    }
    for (int i = 0; i < b.size(); ++i) draw_tile(p, i);
}

void NetWalkApp::draw_tile(const Painter &p, int index) const
{
    const Board &b = game_.board();
    const Rect t = layout_.tile_rect(index);
    const int T = t.w;
    const std::uint8_t mask = b.mask(index);
    const bool lit = game_.powered()[static_cast<std::size_t>(index)] != 0;
    const bool locked = b.locked(index);

    /* the tile: a pale ground with a light and a shaded edge */
    p.rect(t, locked ? pal::kTileLocked : pal::kBoard);
    p.rect(t.x, t.y, T, 1, pal::kTileHi);
    p.rect(t.x, t.y, 1, T, pal::kTileHi);
    p.rect(t.x, t.y + T - 1, T, 1, pal::kTileLo);
    p.rect(t.x + T - 1, t.y, 1, T, pal::kTileLo);

    /* the cables: a dark edge round a fill, the arms from the middle to the tile's edge */
    const int cw = T >= 22 ? 5 : 3;
    const int cx0 = t.x + (T - cw) / 2, cy0 = t.y + (T - cw) / 2;
    Rect arm[4];
    int n = 0;
    if (mask & kN) arm[n++] = {cx0, t.y, cw, cy0 - t.y + cw};
    if (mask & kE) arm[n++] = {cx0, cy0, t.x + T - cx0, cw};
    if (mask & kS) arm[n++] = {cx0, cy0, cw, t.y + T - cy0};
    if (mask & kW) arm[n++] = {t.x, cy0, cx0 - t.x + cw, cw};
    const std::uint32_t edge = lit ? pal::kPowerEdge : pal::kCableEdge;
    const std::uint32_t fill = lit ? (flash_on_ ? pal::kFlash : pal::kPower) : pal::kCable;
    for (int i = 0; i < n; ++i) {
        const Rect g = grow_clamped(arm[i], t);
        p.rect(g, edge);
    }
    for (int i = 0; i < n; ++i) p.rect(arm[i], fill);

    const Kind kind = b.kind(index);
    if (index == b.server()) draw_device(p, t, SpriteKind::Server, true);
    else if (kind == Kind::End) draw_device(p, t, SpriteKind::Monitor, lit);

    if (locked) { /* the padlock, top right */
        const int lx = t.x + T - 6, ly = t.y + 1;
        p.rect(lx + 1, ly, 3, 1, pal::kLockEdge);
        p.rect(lx + 1, ly + 1, 1, 2, pal::kLockEdge);
        p.rect(lx + 3, ly + 1, 1, 2, pal::kLockEdge);
        p.rect(lx, ly + 2, 5, 4, pal::kLockEdge);
        p.rect(lx + 1, ly + 3, 3, 2, pal::kLockBody);
    }
    if (keys_used_ && index == cursor_ && mode_ == Mode::Play) {
        p.outline(t.x, t.y, T, T, pal::kNavy);
        p.outline(t.x + 1, t.y + 1, T - 2, T - 2, pal::kWhite);
    }
}

/* a device in the middle of its tile, always upright, painted over the cables (they run behind it); one rectangle per run of a
 * colour, whole logical pixels (times the magnification the tile has room for) */
void NetWalkApp::draw_device(const Painter &p, const Rect &t, SpriteKind kind, bool lit) const
{
    const Sprite &sp = sprite(kind, use_small_sprite(t.w));
    const int k = sprite_factor(sp, t.w);
    const int x0 = t.x + (t.w - sp.w * k) / 2, y0 = t.y + (t.h - sp.h * k) / 2;
    for (int r = 0; r < sp.h; ++r) {
        int c = 0;
        while (c < sp.w) {
            const char ch = sp.rows[r][c];
            const long rgb = sprite_color(kind, ch, lit);
            int c1 = c;
            while (c1 + 1 < sp.w && sp.rows[r][c1 + 1] == ch) ++c1;
            if (rgb >= 0) p.rect(x0 + c * k, y0 + r * k, (c1 - c + 1) * k, k, static_cast<std::uint32_t>(rgb));
            c = c1 + 1;
        }
    }
}

void NetWalkApp::draw_button(const Painter &p, const Rect &r, const std::string &label, bool pressed, bool tri, bool dflt) const
{
    p.rect(r.x, r.y, r.w, r.h, pal::kBlack);
    p.bevel(r.x + 1, r.y + 1, r.w - 2, r.h - 2, pal::kFace, !pressed);
    if (dflt) p.outline(r.x - 1, r.y - 1, r.w + 2, r.h + 2, pal::kBlack);
    const int tw = text_width(label) + (tri ? 8 : 0);
    const int tx = r.x + (r.w - tw) / 2 + (pressed ? 1 : 0);
    const int ty = r.y + (r.h - 7) / 2 + (pressed ? 1 : 0);
    p.text(label, tx, ty, pal::kBlack);
    if (tri) {
        const int ax = tx + text_width(label) + 3;
        const int ay = ty + 2;
        p.rect(ax, ay, 5, 1, pal::kBlack);
        p.rect(ax + 1, ay + 1, 3, 1, pal::kBlack);
        p.rect(ax + 2, ay + 2, 1, 1, pal::kBlack);
    }
}

void NetWalkApp::draw_panel(const Painter &p) const
{
    const Level lv = level_;
    const LevelInfo &li = level_info(lv);
    const Best &best = scores_.best(lv);
    const std::string caption = li.wrap ? std::string(li.name) + " wrap" : li.name;
    const std::string moves = std::to_string(game_.moves());
    const std::string time = format_time(game_.elapsed_seconds());
    const std::string bestv = best.moves > 0 ? std::to_string(best.moves) : "--";
    auto stat = [&](const Rect &cell, const char *label, const std::string &value) {
        p.text(label, cell.x + 1, cell.y + 2, pal::kBlack);
        p.text_right(value, cell.x + cell.w - 1, cell.y + 2, pal::kBlack);
    };
    p.text(caption, layout_.line[0].x + 1, layout_.line[0].y + 2, pal::kNavy);
    stat(layout_.line[1], "Moves", moves);
    stat(layout_.line[2], "Time", time);
    stat(layout_.line[3], "Best", bestv);
    const bool play = mode_ == Mode::Play;
    const char *labels[3] = {nullptr, "Size", "Scores"};
    for (int i = 0; i < 3; ++i) {
        const Id id = static_cast<Id>(kBtnNew + i);
        const bool pressed = play && pressed_ && !moved_ && press_item_ == id;
        draw_button(p, layout_.button[i], i == 0 ? new_label() : labels[i], pressed, i == 1, false);
    }
}

void NetWalkApp::draw_marker(const Painter &p, const Item &it, bool selected) const
{
    const int cy = it.r.y + it.r.h / 2;
    const int x = it.r.x + it.r.w - 11;
    if (it.aff == Aff::Action) { /* the action dot: white and ringed when idle */
        static const char *const ring[7] = {"..###..", ".#####.", "#######", "#######", "#######", ".#####.", "..###.."};
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 7; ++c) {
                if (ring[r][c] != '#') continue;
                const bool edge = r == 0 || r == 6 || c == 0 || c == 6 || (r == 1 && (c == 1 || c == 5)) ||
                                  (r == 5 && (c == 1 || c == 5));
                p.rect(x + c, cy - 3 + r, 1, 1, edge ? pal::kLo : pal::kWhite);
            }
    }
}

void NetWalkApp::draw_window(const Painter &p, const std::string &title) const
{
    p.outline(dlg_.x, dlg_.y, dlg_.w, dlg_.h, pal::kBlack);
    p.bevel(dlg_.x + 1, dlg_.y + 1, dlg_.w - 2, dlg_.h - 2, pal::kFace, true);
    if (!title.empty()) {
        p.rect(dlg_.x + 3, dlg_.y + 3, dlg_.w - 6, kWinTitleH, pal::kNavy);
        p.text(title, dlg_.x + 6, dlg_.y + 5, pal::kWhite);
    }
}

void NetWalkApp::draw_modal(const Painter &p) const
{
    draw_window(p, dlg_title_);
    if (mode_ == Mode::Size) {
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const Item &it = items_[i];
            if (it.id == kCancel) {
                draw_button(p, it.r, it.label, pressed_ && !moved_ && press_item_ == it.id, false, false);
                continue;
            }
            const bool sel = keys_used_ ? static_cast<int>(i) == sel_ : (pressed_ && press_item_ == it.id && !moved_);
            if (sel) p.rect(it.r, pal::kNavy);
            const int ty = it.r.y + (it.r.h - 7) / 2;
            if (static_cast<int>(it.id) - kSizeEasy == static_cast<int>(level_)) { /* a check on the current size */
                const std::uint32_t c = sel ? pal::kWhite : pal::kNavy;
                p.rect(it.r.x + 6, ty + 4, 1, 1, c);
                p.rect(it.r.x + 3, ty + 3, 1, 1, c);
                p.rect(it.r.x + 4, ty + 4, 1, 1, c);
                p.rect(it.r.x + 5, ty + 5, 1, 1, c);
                p.rect(it.r.x + 7, ty + 3, 1, 1, c);
                p.rect(it.r.x + 8, ty + 2, 1, 1, c);
                p.rect(it.r.x + 9, ty + 1, 1, 1, c);
                p.rect(it.r.x + 6, ty + 5, 1, 1, c);
            }
            p.text(it.label, it.r.x + 14, ty, sel ? pal::kWhite : pal::kBlack);
            draw_marker(p, it, sel);
        }
        return;
    }
    if (mode_ == Mode::Scores) {
        const int x0 = dlg_.x + 6, x1 = dlg_.x + dlg_.w - 6;
        const int col_time = x1, col_moves = x1 - text_width("00:00") - 8;
        int y = dlg_.y + 18;
        p.text_right("Moves", col_moves, y + 1, pal::kMuted);
        p.text_right("Time", col_time, y + 1, pal::kMuted);
        y += 12;
        for (int i = 0; i < kLevelCount; ++i, y += 12) { /* info rows: no marker, grey values */
            const Best &b = scores_.best(static_cast<Level>(i));
            p.text(level_info(static_cast<Level>(i)).name, x0, y + 1, pal::kBlack);
            p.text_right(b.moves > 0 ? std::to_string(b.moves) : "--", col_moves, y + 1, pal::kMuted);
            p.text_right(b.seconds > 0 ? format_time(b.seconds) : "--:--", col_time, y + 1, pal::kMuted);
        }
        draw_button(p, items_[0].r, items_[0].label, pressed_ && !moved_ && press_item_ == kOk, false, true);
        return;
    }
    if (mode_ == Mode::Win) {
        int y = dlg_.y + 18;
        for (const std::string &s : dlg_lines_) {
            p.text_center(s, dlg_.x + dlg_.w / 2, y + 2, pal::kBlack);
            y += kLineH;
        }
        draw_button(p, items_[0].r, items_[0].label, pressed_ && !moved_ && press_item_ == kOk, false, true);
    }
}

/* ------------------------------------------------------------------------------------------------ the game */

void NetWalkApp::new_game(Level level, bool pick_seed)
{
    std::uint64_t seed = game_.seed();
    if (pick_seed) {
        const char *ts = std::getenv("NETWALK_TEST_SEED");
        static bool test_seed_used = false;
        if (ts && !test_seed_used) {
            test_seed_used = true;
            seed = static_cast<std::uint64_t>(std::strtoull(ts, nullptr, 10));
        } else {
            seed = (static_cast<std::uint64_t>(std::time(nullptr)) << 20) ^ (static_cast<std::uint64_t>(lv_tick_get()) * 2654435761u) ^
                   (seed * 6364136223846793005ull + 1442695040888963407ull);
        }
    }
    level_ = level;
    game_.start(level, seed);
    if (scores_.last() != level) {
        scores_.set_last(level);
        store_.save(scores_);
    }
    mode_ = Mode::Play;
    flash_on_ = false;
    cursor_ = game_.board().server();
    if (flash_timer_) {
        lv_timer_delete(flash_timer_);
        flash_timer_ = nullptr;
    }
    shown_seconds_ = -1;
    if (surface_) relayout();
}

void NetWalkApp::turn_tile(int i, bool cw)
{
    if (mode_ != Mode::Play) return;
    if (game_.turn(i, cw)) after_move();
    else invalidate();
}

void NetWalkApp::lock_tile(int i)
{
    if (mode_ != Mode::Play) return;
    game_.toggle_lock(i);
    invalidate();
}

void NetWalkApp::after_move()
{
    if (game_.won()) {
        win_moves_ = game_.moves();
        win_seconds_ = std::max(1, game_.elapsed_seconds());
        win_record_ = scores_.record(level_, win_moves_, win_seconds_);
        store_.save(scores_);
        mode_ = Mode::Flash;
        flash_n_ = 0;
        flash_on_ = true;
        if (flash_timer_) lv_timer_delete(flash_timer_);
        flash_timer_ = lv_timer_create(flash_cb, kFlashMs, this);
        rebuild_items();
    }
    invalidate();
}

void NetWalkApp::finish_flash()
{
    if (flash_timer_) {
        lv_timer_delete(flash_timer_);
        flash_timer_ = nullptr;
    }
    flash_on_ = false;
    mode_ = Mode::Win;
    sel_ = 0;
    rebuild_items();
    invalidate();
}

void NetWalkApp::flash_cb(lv_timer_t *t)
{
    auto *self = static_cast<NetWalkApp *>(lv_timer_get_user_data(t));
    if (++self->flash_n_ >= kFlashSteps) {
        self->finish_flash();
        return;
    }
    self->flash_on_ = !self->flash_on_;
    self->invalidate();
}

void NetWalkApp::tick_cb(lv_timer_t *t)
{
    auto *self = static_cast<NetWalkApp *>(lv_timer_get_user_data(t));
    std::uint32_t dt = lv_tick_elaps(self->last_tick_);
    self->last_tick_ = lv_tick_get();
    if (dt > 1000) dt = 0; /* a frozen app (SIGSTOP while hidden) wakes up with a big gap: it was not played */
    if (cdk::hidden()) return;
    if (self->mode_ == Mode::Play) self->game_.tick(dt);
    const int secs = self->game_.elapsed_seconds();
    if (secs != self->shown_seconds_) {
        self->shown_seconds_ = secs;
        self->invalidate();
    }
}

void NetWalkApp::open_mode(Mode m)
{
    mode_ = m;
    pressed_ = false;
    if (m == Mode::Size) sel_ = static_cast<int>(level_);
    else sel_ = 0;
    rebuild_items();
    invalidate();
}

void NetWalkApp::close_modal()
{
    if (mode_ == Mode::Play) return;
    if (mode_ == Mode::Flash) {
        finish_flash();
        return;
    }
    mode_ = Mode::Play;
    pressed_ = false;
    rebuild_items();
    invalidate();
}

void NetWalkApp::quit()
{
    cdk::request_quit(0);
}

/* the header's Esc button and the Esc key: close the open window, else end the app */
void NetWalkApp::back()
{
    if (!alive_) return;
    if (mode_ == Mode::Play) quit();
    else close_modal();
}

void NetWalkApp::activate(Id id)
{
    switch (id) {
    case kBtnNew: new_game(level_); break;
    case kBtnSize: open_mode(Mode::Size); break;
    case kBtnScores: open_mode(Mode::Scores); break;
    case kSizeEasy:
    case kSizeNormal:
    case kSizeHard:
    case kSizeExpert: new_game(static_cast<Level>(id - kSizeEasy)); break;
    case kOk:
        if (mode_ == Mode::Win) new_game(level_); /* "Network up!" OK: the next board of the same size */
        else close_modal();
        break;
    case kCancel: close_modal(); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ pointer */

void NetWalkApp::on_pointer(lv_event_code_t code)
{
    lv_indev_t *indev = lv_indev_active();
    /* a keypad's Enter also presses the focused object: only a pointer or a finger counts here */
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) pressed_ = false;
        return;
    }
    lv_point_t pt{0, 0};
    lv_indev_get_point(indev, &pt);
    lv_area_t obj;
    lv_obj_get_coords(surface_, &obj);
    const int px = pt.x - obj.x1, py = pt.y - obj.y1;
    int lx = 0, ly = 0;
    const bool inside = layout_.to_logical(px, py, &lx, &ly);
    switch (code) {
    case LV_EVENT_PRESSED:
        pressed_ = true;
        moved_ = fired_ = false;
        press_lx_ = px;
        press_ly_ = py;
        press_ms_ = now_ms();
        press_tile_ = -1;
        press_item_ = kNone;
        keys_used_ = false;
        if (mode_ == Mode::Flash) break;
        if (inside) {
            if (Item *it = item_at(lx, ly)) press_item_ = it->id;
            else if (mode_ == Mode::Play) press_tile_ = layout_.tile_at(px, py);
        }
        invalidate();
        break;
    case LV_EVENT_PRESSING:
        if (!pressed_ || moved_) break;
        if ((px - press_lx_) * (px - press_lx_) + (py - press_ly_) * (py - press_ly_) >= kMovePx * kMovePx) {
            moved_ = true;
            invalidate();
        } else if (!fired_ && press_tile_ >= 0 && now_ms() - press_ms_ >= static_cast<std::uint32_t>(kLockMs)) {
            fired_ = true;
            lock_tile(press_tile_);
        }
        break;
    case LV_EVENT_RELEASED: {
        const bool was = pressed_;
        pressed_ = false;
        if (!was) break;
        if (mode_ == Mode::Flash) {
            close_modal();
            break;
        }
        if (!moved_ && !fired_) {
            if (press_tile_ >= 0) {
                turn_tile(press_tile_, now_ms() - press_ms_ < static_cast<std::uint32_t>(kLongTurnMs));
            } else if (press_item_ != kNone) {
                Item *it = inside ? item_at(lx, ly) : nullptr;
                if (it && it->id == press_item_) {
                    const Id id = press_item_;
                    post([this, id] { activate(id); });
                }
            }
        }
        press_item_ = kNone;
        invalidate();
        break;
    }
    case LV_EVENT_PRESS_LOST:
        pressed_ = false;
        press_item_ = kNone;
        invalidate();
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ keys */

void NetWalkApp::key_cb(lv_event_t *e)
{
    static_cast<NetWalkApp *>(lv_event_get_user_data(e))->on_key(lv_event_get_key(e));
}

void NetWalkApp::keyboard_cb(lv_event_t *e)
{
    auto *self = static_cast<NetWalkApp *>(lv_event_get_user_data(e));
    const auto *item = static_cast<const struct key_item *>(lv_event_get_param(e));
    if (item) self->on_keyboard(*item);
}

void NetWalkApp::use_keys()
{
    if (keys_used_) return;
    keys_used_ = true;
    if (cursor_ < 0 || cursor_ >= game_.board().size()) cursor_ = game_.board().server();
    invalidate();
}

void NetWalkApp::cursor_move(int dx, int dy)
{
    const Board &b = game_.board();
    int x = cursor_ % b.w() + dx, y = cursor_ / b.w() + dy;
    x = std::clamp(x, 0, b.w() - 1);
    y = std::clamp(y, 0, b.h() - 1);
    cursor_ = y * b.w() + x;
    invalidate();
}

void NetWalkApp::list_move(int d)
{
    const int n = static_cast<int>(items_.size()) - (mode_ == Mode::Size ? 1 : 0); /* the Cancel button is not a row */
    if (n <= 0) return;
    keys_used_ = true;
    sel_ = (sel_ + d + n) % n;
    invalidate();
}

void NetWalkApp::on_key(uint32_t key)
{
    if (mode_ == Mode::Flash) {
        post([this] { close_modal(); });
        return;
    }
    if (key == LV_KEY_ESC) {
        post([this] { back(); });
        return;
    }
    if (mode_ == Mode::Size) {
        switch (key) {
        case LV_KEY_UP: list_move(-1); break;
        case LV_KEY_DOWN: list_move(1); break;
        case LV_KEY_ENTER:
            if (sel_ >= 0 && sel_ < static_cast<int>(items_.size())) {
                const Id id = items_[static_cast<std::size_t>(sel_)].id;
                post([this, id] { activate(id); });
            }
            break;
        default: break;
        }
        return;
    }
    if (mode_ == Mode::Scores || mode_ == Mode::Win) {
        if (key == LV_KEY_ENTER) post([this] { activate(kOk); });
        return;
    }
    switch (key) {
    case LV_KEY_LEFT: use_keys(); cursor_move(-1, 0); break;
    case LV_KEY_RIGHT: use_keys(); cursor_move(1, 0); break;
    case LV_KEY_UP: use_keys(); cursor_move(0, -1); break;
    case LV_KEY_DOWN: use_keys(); cursor_move(0, 1); break;
    case LV_KEY_ENTER: use_keys(); turn_tile(cursor_, true); break;
    default: break;
    }
}

void NetWalkApp::on_keyboard(const struct key_item &item)
{
    if (item.key_state != 1) return;
    if (item.mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO)) return;
    if (item.utf8[1] != 0) return;
    const char c = item.utf8[0];
    const char lower = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (mode_ == Mode::Scores || mode_ == Mode::Win) {
        if (c == ' ') post([this] { activate(kOk); });
        return;
    }
    if (mode_ == Mode::Size) {
        if (c == ' ') {
            if (sel_ >= 0 && sel_ < static_cast<int>(items_.size())) {
                const Id id = items_[static_cast<std::size_t>(sel_)].id;
                post([this, id] { activate(id); });
            }
        }
        return;
    }
    if (mode_ != Mode::Play) return;
    switch (lower) {
    case ' ': use_keys(); turn_tile(cursor_, true); break;
    case 'c': use_keys(); turn_tile(cursor_, false); break;
    case 'l': use_keys(); lock_tile(cursor_); break;
    case 'n': post([this] { new_game(level_); }); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ test fixtures */

/* test only: NETWALK_TEST_SETUP="power:N almost lock:0,5 moves:N time:S" turns the fresh board into a game in progress */
void NetWalkApp::run_setup(const std::string &ops)
{
    Board &b = game_.board_for_test();
    std::istringstream in(ops);
    std::string op;
    int moves = -1, secs = -1;
    while (in >> op) {
        const std::size_t colon = op.find(':');
        const std::string name = op.substr(0, colon);
        const std::string arg = colon == std::string::npos ? "" : op.substr(colon + 1);
        if (name == "power") { /* the first N tiles of the solved tree, nearest the server first, are put right */
            const int want = std::atoi(arg.c_str());
            std::vector<int> order{b.server()};
            std::vector<char> seen(static_cast<std::size_t>(b.size()), 0);
            seen[static_cast<std::size_t>(b.server())] = 1;
            for (std::size_t k = 0; k < order.size(); ++k)
                for (std::uint8_t d : {kN, kE, kS, kW}) {
                    const int c = order[k];
                    if (!(b.solved_mask(c) & d)) continue;
                    const int nb = b.neighbor(c, d);
                    if (nb >= 0 && !seen[static_cast<std::size_t>(nb)]) {
                        seen[static_cast<std::size_t>(nb)] = 1;
                        order.push_back(nb);
                    }
                }
            for (int k = 0; k < want && k < static_cast<int>(order.size()); ++k) b.solve_tile(order[static_cast<std::size_t>(k)]);
        } else if (name == "almost") { /* everything right but tile 0, one turn away */
            for (int i = 0; i < b.size(); ++i) b.solve_tile(i);
            b.rotate(0, false);
            cursor_ = 0;
        } else if (name == "lock") {
            std::istringstream ls(arg);
            std::string tok;
            while (std::getline(ls, tok, ',')) b.set_locked(std::atoi(tok.c_str()), true);
        } else if (name == "moves") {
            moves = std::atoi(arg.c_str());
        } else if (name == "time") {
            secs = std::atoi(arg.c_str());
        } else if (name == "keys") {
            keys_used_ = true;
        }
    }
    if (moves >= 0 || secs >= 0) game_.restore(moves >= 0 ? moves : game_.moves(), secs >= 0 ? static_cast<std::uint32_t>(secs) * 1000u : game_.elapsed_ms());
    else game_.restore(game_.moves(), game_.elapsed_ms());
}

} // namespace nwui
