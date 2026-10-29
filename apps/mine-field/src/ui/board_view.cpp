/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field: the board widget (see board_view.hpp).
 */
#include "board_view.hpp"

#include "palette.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace mfui {

using namespace minefield;

namespace {

lv_color_t rgb(std::uint32_t v)
{
    return lv_color_hex(v);
}

const char *const kDigits[9] = {"", "1", "2", "3", "4", "5", "6", "7", "8"};

int even_clamp(double v, int lo, int hi)
{
    int p = static_cast<int>(std::lround(v)) & ~1;
    return std::min(hi, std::max(lo, p));
}

lv_area_t intersect(const lv_area_t &a, const lv_area_t &b)
{
    lv_area_t r{std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
    return r;
}

bool empty(const lv_area_t &a)
{
    return a.x2 < a.x1 || a.y2 < a.y1;
}

void rect(lv_layer_t *layer, const lv_area_t &a, std::uint32_t color, int radius = 0, int opa = LV_OPA_COVER)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = rgb(color);
    d.bg_opa = static_cast<lv_opa_t>(opa);
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void frame(lv_layer_t *layer, const lv_area_t &a, std::uint32_t color, int width, int radius = 0,
           lv_border_side_t sides = LV_BORDER_SIDE_FULL, int opa = LV_OPA_COVER)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = rgb(color);
    d.border_opa = static_cast<lv_opa_t>(opa);
    d.border_width = width;
    d.border_side = sides;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void line(lv_layer_t *layer, double x1, double y1, double x2, double y2, std::uint32_t color, int width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = static_cast<lv_value_precise_t>(x1);
    d.p1.y = static_cast<lv_value_precise_t>(y1);
    d.p2.x = static_cast<lv_value_precise_t>(x2);
    d.p2.y = static_cast<lv_value_precise_t>(y2);
    d.color = rgb(color);
    d.width = width;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

std::uint32_t now_ms()
{
    return lv_tick_get();
}

} // namespace

BoardView::BoardView(lv_obj_t *parent, BoardEvents events) : ev_(std::move(events))
{
    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_add_flag(obj_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    lv_obj_remove_flag(obj_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_event_cb(obj_, event_cb, LV_EVENT_ALL, this);
}

BoardView::~BoardView()
{
    if (anim_) lv_timer_delete(anim_);
    if (obj_ && lv_obj_is_valid(obj_)) {
        lv_obj_remove_event_cb_with_user_data(obj_, event_cb, this);
        lv_obj_delete(obj_);
    }
}

void BoardView::set_hidden(bool hidden)
{
    if (hidden) lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj_, LV_OBJ_FLAG_HIDDEN);
}

void BoardView::bind(const Game *game)
{
    game_ = game;
    drawn_.clear();
    mine_shown_.clear();
    mines_all_ = false;
    cursor_ = hint_ = pulse_cell_ = -1;
    hint_animate_ = false;
    flash_cells_.clear();
    sweep_ = -1;
    arm_anim();
    invalidate_all();
}

/* ------------------------------------------------------------------------------------------------ geometry */

void BoardView::configure(const lv_area_t &viewport, const BoardPlan &plan)
{
    viewport_ = viewport;
    plan_ = plan;
    cols_ = plan.dims.w;
    rows_ = plan.dims.h;
    lv_obj_set_pos(obj_, viewport.x1, viewport.y1);
    lv_obj_set_size(obj_, std::max(1, lv_area_get_width(&viewport)), std::max(1, lv_area_get_height(&viewport)));
    drawn_.clear();
    mine_shown_.clear();
    invalidate_all();
}

lv_area_t BoardView::cell_area(int cell) const
{
    const int s = plan_.cell_px;
    const int x = viewport_.x1 + plan_.origin_x + (cell % std::max(1, cols_)) * s;
    const int y = viewport_.y1 + plan_.origin_y + (cell / std::max(1, cols_)) * s;
    return lv_area_t{x, y, x + s - 1, y + s - 1};
}

int BoardView::center_cell() const
{
    return (rows_ / 2) * cols_ + cols_ / 2;
}

/* ------------------------------------------------------------------------------------------------ invalidation */

std::uint8_t BoardView::code_of(int i) const
{
    const Board &b = game_->board();
    const bool fin = game_->finished();
    const int st = static_cast<int>(b.state(i));
    const int adj = st == static_cast<int>(CellState::Revealed) ? b.adjacent(i) : 0;
    return static_cast<std::uint8_t>(st | (adj << 2) | ((fin && b.is_mine(i)) ? 0x40 : 0) | (fin ? 0x80 : 0));
}

void BoardView::invalidate_cell(int cell)
{
    if (cell < 0 || !plan_.valid || cell >= cols_ * rows_) return;
    lv_area_t a = intersect(cell_area(cell), viewport_area());
    if (empty(a)) return;
    /* ring overlays draw a pixel or two outside the tile: invalidate with a little air */
    a.x1 -= 2;
    a.y1 -= 2;
    a.x2 += 2;
    a.y2 += 2;
    lv_obj_invalidate_area(obj_, &a);
}

void BoardView::invalidate_all()
{
    lv_obj_invalidate(obj_);
}

void BoardView::sync()
{
    if (!game_ || !plan_.valid) return;
    const int n = game_->board().size();
    if (static_cast<int>(drawn_.size()) != n) {
        drawn_.assign(static_cast<std::size_t>(n), 0xFF);
        if (static_cast<int>(mine_shown_.size()) != n) mine_shown_.assign(static_cast<std::size_t>(n), 0);
    }
    for (int i = 0; i < n; ++i) {
        const std::uint8_t c = code_of(i);
        if (c == drawn_[static_cast<std::size_t>(i)]) continue;
        drawn_[static_cast<std::size_t>(i)] = c;
        invalidate_cell(i);
    }
}

void BoardView::set_cursor(int cell)
{
    if (cell == cursor_) return;
    const int old = cursor_;
    cursor_ = cell;
    invalidate_cell(old);
    invalidate_cell(cell);
}

void BoardView::set_hint(int cell, bool animate)
{
    const int old = hint_;
    hint_ = cell;
    hint_animate_ = animate && cell >= 0;
    hint_big_ = false;
    last_hint_toggle_ = now_ms();
    invalidate_cell(old);
    invalidate_cell(cell);
    arm_anim();
}

void BoardView::flash(const std::vector<int> &cells, int ms)
{
    const std::vector<int> old = flash_cells_;
    flash_cells_ = cells;
    flash_until_ = now_ms() + static_cast<std::uint32_t>(ms);
    for (int c : old) invalidate_cell(c);
    for (int c : cells) invalidate_cell(c);
    arm_anim();
}

void BoardView::pulse(int cell, int ms)
{
    const int old = pulse_cell_;
    pulse_cell_ = cell;
    pulse_until_ = now_ms() + static_cast<std::uint32_t>(ms);
    invalidate_cell(old);
    invalidate_cell(cell);
    arm_anim();
}

void BoardView::set_sweep(double position)
{
    sweep_ = position;
    invalidate_all();
}

void BoardView::set_mine_reveal(const std::vector<int> &order, int shown)
{
    if (!game_) return;
    const int n = game_->board().size();
    if (static_cast<int>(mine_shown_.size()) != n) mine_shown_.assign(static_cast<std::size_t>(n), 0);
    for (int k = 0; k < static_cast<int>(order.size()); ++k) {
        const int c = order[static_cast<std::size_t>(k)];
        const char want = k < shown ? 1 : 0;
        if (c >= 0 && c < n && mine_shown_[static_cast<std::size_t>(c)] != want) {
            mine_shown_[static_cast<std::size_t>(c)] = want;
            invalidate_cell(c);
        }
    }
}

void BoardView::reveal_all_mines(bool on)
{
    mines_all_ = on;
    invalidate_all();
}

void BoardView::clear_loss()
{
    mines_all_ = false;
    std::fill(mine_shown_.begin(), mine_shown_.end(), 0);
}

void BoardView::arm_anim()
{
    const bool need = hint_animate_ || !flash_cells_.empty() || pulse_cell_ >= 0;
    if (need && !anim_) anim_ = lv_timer_create(anim_cb, 50, this);
    if (!need && anim_) {
        lv_timer_delete(anim_);
        anim_ = nullptr;
    }
}

void BoardView::anim_cb(lv_timer_t *t)
{
    static_cast<BoardView *>(lv_timer_get_user_data(t))->tick();
}

void BoardView::tick()
{
    const std::uint32_t now = now_ms();
    if (!flash_cells_.empty() && static_cast<std::int32_t>(now - flash_until_) >= 0) {
        const std::vector<int> old = flash_cells_;
        flash_cells_.clear();
        for (int c : old) invalidate_cell(c);
    }
    if (pulse_cell_ >= 0 && static_cast<std::int32_t>(now - pulse_until_) >= 0) {
        const int old = pulse_cell_;
        pulse_cell_ = -1;
        invalidate_cell(old);
    }
    if (hint_animate_ && hint_ >= 0 && now - last_hint_toggle_ >= 320) {
        last_hint_toggle_ = now;
        hint_big_ = !hint_big_;
        invalidate_cell(hint_);
    }
    arm_anim();
}

/* ------------------------------------------------------------------------------------------------ drawing */

void BoardView::event_cb(lv_event_t *e)
{
    auto *self = static_cast<BoardView *>(lv_event_get_user_data(e));
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

void BoardView::draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t v = viewport_;
    rect(layer, v, pal::kGround);
    if (!game_ || !plan_.valid || cols_ <= 0) return;
    const lv_area_t clip = intersect(layer->_clip_area, v);
    if (empty(clip)) return;
    const int s = plan_.cell_px;
    const int x0 = std::max(0, (clip.x1 - v.x1 - plan_.origin_x) / s);
    const int x1 = std::min(cols_ - 1, (clip.x2 - v.x1 - plan_.origin_x) / s);
    const int y0 = std::max(0, (clip.y1 - v.y1 - plan_.origin_y) / s);
    const int y1 = std::min(rows_ - 1, (clip.y2 - v.y1 - plan_.origin_y) / s);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const int i = y * cols_ + x;
            draw_cell(layer, i, cell_area(i));
        }
}

void BoardView::draw_flag(lv_layer_t *layer, const lv_area_t &t, bool cross)
{
    const double s = lv_area_get_width(&t);
    const double x = t.x1, y = t.y1;
    const int pw = std::max(2, static_cast<int>(s / 11));
    /* base, pole, cloth: a triangle pointing right from the top of the pole */
    const int by = static_cast<int>(y + s * 0.76);
    const lv_area_t base{static_cast<int>(x + s * 0.28), by, static_cast<int>(x + s * 0.72), by + std::max(2, static_cast<int>(s / 10)) - 1};
    rect(layer, base, pal::kPole, 1);
    const int px = static_cast<int>(x + s * 0.47);
    const lv_area_t pole{px, static_cast<int>(y + s * 0.20), px + pw - 1, static_cast<int>(y + s * 0.78)};
    rect(layer, pole, pal::kPole);
    lv_draw_triangle_dsc_t td;
    lv_draw_triangle_dsc_init(&td);
    td.color = rgb(pal::kFlag);
    td.opa = LV_OPA_COVER;
    td.p[0].x = px + pw - 1;
    td.p[0].y = static_cast<lv_value_precise_t>(y + s * 0.20);
    td.p[1].x = static_cast<lv_value_precise_t>(x + s * 0.80);
    td.p[1].y = static_cast<lv_value_precise_t>(y + s * 0.34);
    td.p[2].x = px + pw - 1;
    td.p[2].y = static_cast<lv_value_precise_t>(y + s * 0.48);
    lv_draw_triangle(layer, &td);
    if (cross) {
        const int w = std::max(2, static_cast<int>(s / 12));
        line(layer, x + s * 0.18, y + s * 0.18, x + s * 0.82, y + s * 0.82, pal::kBad, w + 2);
        line(layer, x + s * 0.82, y + s * 0.18, x + s * 0.18, y + s * 0.82, pal::kBad, w + 2);
        line(layer, x + s * 0.18, y + s * 0.18, x + s * 0.82, y + s * 0.82, pal::kCross, w);
        line(layer, x + s * 0.82, y + s * 0.18, x + s * 0.18, y + s * 0.82, pal::kCross, w);
    }
}

void BoardView::draw_mine(lv_layer_t *layer, const lv_area_t &t)
{
    const double s = lv_area_get_width(&t);
    const double cx = t.x1 + s / 2.0, cy = t.y1 + s / 2.0;
    const double r = s * 0.22, spike = s * 0.37;
    const int w = std::max(2, static_cast<int>(s / 12));
    for (int k = 0; k < 4; ++k) { /* eight spikes: the axes and the diagonals */
        const double a = k * 3.14159265358979 / 4.0;
        line(layer, cx - spike * std::cos(a), cy - spike * std::sin(a), cx + spike * std::cos(a), cy + spike * std::sin(a),
             pal::kMineBody, w);
    }
    const lv_area_t body{static_cast<int>(cx - r), static_cast<int>(cy - r), static_cast<int>(cx + r), static_cast<int>(cy + r)};
    rect(layer, body, pal::kMineBody, LV_RADIUS_CIRCLE);
    const int g = std::max(2, static_cast<int>(r * 0.55));
    const int gx = static_cast<int>(cx - r * 0.55), gy = static_cast<int>(cy - r * 0.55);
    rect(layer, lv_area_t{gx, gy, gx + g - 1, gy + g - 1}, pal::kMineGlint, LV_RADIUS_CIRCLE);
}

void BoardView::draw_cell(lv_layer_t *layer, int i, const lv_area_t &ca)
{
    const Board &b = game_->board();
    const int s = lv_area_get_width(&ca);
    const int in = s >= 48 ? 2 : 1;
    const lv_area_t t{ca.x1 + in, ca.y1 + in, ca.x2 - in, ca.y2 - in};
    const CellState st = b.state(i);
    const bool fin = game_->finished();
    const bool lost = game_->status() == Game::Status::Lost;
    const bool mine = fin && b.is_mine(i);
    const int radius = std::max(0, std::min(4, s / 14));
    const int bw = std::max(2, s / 18);

    auto tile = [&] {
        rect(layer, t, pal::kTile, radius);
        frame(layer, t, pal::kTileHi, bw, radius, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT));
        frame(layer, t, pal::kTileLo, bw, radius, static_cast<lv_border_side_t>(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT));
    };
    auto open = [&](std::uint32_t color) {
        rect(layer, t, color);
        if (color == pal::kOpen) frame(layer, t, pal::kOpenLine, 1);
    };

    if (st == CellState::Revealed) {
        if (mine) {
            open(pal::kExploded);
            draw_mine(layer, t);
        } else {
            open(pal::kOpen);
            const int n = b.adjacent(i);
            if (n > 0) {
                lv_draw_label_dsc_t ld;
                lv_draw_label_dsc_init(&ld);
                ld.font = cp0_ui_font_px(even_clamp(s * 0.62, 12, 48));
                ld.color = rgb(pal::kDigit[n]);
                ld.text = kDigits[n];
                ld.align = LV_TEXT_ALIGN_CENTER;
                const int lh = lv_font_get_line_height(ld.font);
                const lv_area_t ta{t.x1, t.y1 + (lv_area_get_height(&t) - lh) / 2, t.x2, t.y1 + (lv_area_get_height(&t) + lh) / 2};
                lv_draw_label(layer, &ld, &ta);
            }
        }
    } else if (st == CellState::Flagged) {
        tile();
        draw_flag(layer, t, lost && !mine);
    } else if (lost && mine &&
               (mines_all_ || (i < static_cast<int>(mine_shown_.size()) && mine_shown_[static_cast<std::size_t>(i)]))) {
        open(pal::kOpen);
        draw_mine(layer, t);
    } else {
        tile();
    }

    /* overlays */
    if (!flash_cells_.empty() && std::find(flash_cells_.begin(), flash_cells_.end(), i) != flash_cells_.end())
        rect(layer, t, pal::kFlash, radius, LV_OPA_50);
    if (sweep_ >= 0) {
        const double span = cols_ + rows_ + 6.0;
        const double pos = sweep_ * span - 3.0;
        const double d = std::fabs((i % cols_) + (i / cols_) - pos);
        if (d < 3.0) rect(layer, t, pal::kSweep, radius, static_cast<int>(110 * (1.0 - d / 3.0)));
    }
    if (i == pulse_cell_) frame(layer, t, pal::kText, std::max(2, s / 12), radius);
    if (i == hint_) frame(layer, ca, pal::kHint, std::max(3, s / (hint_big_ ? 7 : 11)), radius);
    if (i == cursor_) frame(layer, ca, pal::kCursor, std::max(3, s / 11), radius);
}

/* ------------------------------------------------------------------------------------------------ pointer */

int BoardView::cell_at(int sx, int sy) const
{
    return hit_cell(sx - viewport_.x1, sy - viewport_.y1, plan_.origin_x, plan_.origin_y, plan_.cell_px, cols_, rows_);
}

void BoardView::on_pointer(lv_event_code_t code)
{
    lv_indev_t *indev = lv_indev_active();
    /* a keypad's Enter also presses the focused object: only a pointer or a finger counts here */
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER || !input_ || !plan_.valid) {
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) pressed_ = false;
        return;
    }
    lv_point_t p{0, 0};
    lv_indev_get_point(indev, &p);
    switch (code) {
    case LV_EVENT_PRESSED:
        pressed_ = true;
        moved_ = fired_ = false;
        press_x_ = p.x;
        press_y_ = p.y;
        press_cell_ = cell_at(p.x, p.y);
        press_ms_ = now_ms();
        if (ev_.touched) ev_.touched();
        break;
    case LV_EVENT_PRESSING:
        if (!pressed_ || moved_) break;
        if ((p.x - press_x_) * (p.x - press_x_) + (p.y - press_y_) * (p.y - press_y_) >= kMoveThresholdPx * kMoveThresholdPx) {
            moved_ = true; /* a drag is neither a tap nor a long press */
        } else if (!fired_ && press_cell_ >= 0 && now_ms() - press_ms_ >= static_cast<std::uint32_t>(kLongPressMs)) {
            fired_ = true;
            if (ev_.long_press) ev_.long_press(press_cell_);
        }
        break;
    case LV_EVENT_RELEASED:
        if (pressed_ && !moved_ && !fired_ && press_cell_ >= 0 && ev_.tap) ev_.tap(press_cell_);
        pressed_ = false;
        break;
    case LV_EVENT_PRESS_LOST: pressed_ = false; break;
    default: break;
    }
}

} // namespace mfui
