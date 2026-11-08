/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the trim and split page (boards CassetteTrim-480x320 / -320x480, note n7).
 *
 * The waveform window is ONE custom-drawn object: the overview strip of the whole take (amber = kept, grey = cut, cream
 * IN / OUT ticks, green split lines, the white frame of the part shown below), the main bars of the window around the
 * fixed red marker (bars outside IN..OUT grey), the IN / OUT handles (a chip at the edge when outside the window), the
 * numbered split flags (the selected one solid white with a white-framed number), the marker with its triangle and
 * the time ruler. A sideways drag scrubs (UiActions::trim_scrub), a tap on a flag selects it (trim_select_split);
 * flags and handles are never dragged (note n7).
 */
#include "pages.hpp"

#include "paint.hpp"
#include "theme.hpp"
#include "ui_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cassette {
namespace ui {

namespace {

const char *kRemove = "REMOVE", *kSplit = "SPLIT";

} // namespace

TrimPage::TrimPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav)
    : PageBase(parent, frame), act_(actions), nav_(nav)
{
    const bool p = frame.portrait;
    fit_ = frame.fit({145}, {200}); /* the main waveform takes what a larger screen leaves */
    deck_body(root_, fit_);

    /* the strip: sticker, TAKE, the in / out / length / splits text at the right */
    const double sy = p ? 54 : 52, sw = p ? 288 : 448;
    box(root_, fit_.x(16), fit_.y(sy), fit_.w(16, sw), fit_.h(sy, 28), col::kStrip, fit_.px(5));
    sticker_ = make_sticker(root_, fit_, 44, 20, 4, 15, 4, 3 + 1.5, fit_.px(2));
    lv_obj_set_pos(sticker_, fit_.x(24), fit_.y(sy + 4));
    take_ = fit_label(root_, fit_, Face::Bold, p ? 14 : 15, col::kInk, 76, sy + 6, 80, 16, LV_TEXT_ALIGN_LEFT, 0.4);
    info_ = fit_label(root_, fit_, Face::Medium, p ? 13 : 14, col::kInkSoft, 150, sy + 6, sw - 150 + 16 - 8, 16, LV_TEXT_ALIGN_RIGHT);

    /* the waveform window and its parts */
    framed_panel(root_, fit_, p ? DRect{16, 90, 288, 210} : DRect{16, 86, 448, 108}, col::kWindow);
    if (p) {
        over_x_ = fit_.x(24), over_y_ = fit_.y(96), over_w_ = fit_.w(24, 272), over_h_ = fit_.h(96, 18);
        main_x_ = fit_.x(24), main_y_ = fit_.y(122), main_w_ = fit_.w(24, 272), main_h_ = fit_.h(122, 150);
        ruler_y_ = fit_.y(279);
    } else {
        over_x_ = fit_.x(20), over_y_ = fit_.y(90), over_w_ = fit_.w(20, 440), over_h_ = fit_.h(90, 18);
        main_x_ = fit_.x(20), main_y_ = fit_.y(114), main_w_ = fit_.w(20, 440), main_h_ = fit_.h(114, 62);
        ruler_y_ = fit_.y(179);
    }
    pitch_ = 4 * fit_.s;
    bar_w_ = 3 * fit_.s;
    const DRect wr = p ? DRect{16, 90, 288, 210} : DRect{16, 86, 448, 108};
    wave_ = lv_obj_create(root_);
    plain(wave_);
    lv_obj_add_flag(wave_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(wave_, fit_.x(wr.x), fit_.y(wr.y));
    lv_obj_set_size(wave_, fit_.w(wr.x, wr.w), fit_.h(wr.y, wr.h));
    lv_obj_add_event_cb(wave_, wave_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(wave_, wave_event_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(wave_, wave_event_cb, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(wave_, wave_event_cb, LV_EVENT_RELEASED, this);

    /* ZOOM and POSITION */
    const double ky = p ? 306 : 198;
    const DRect zr = p ? DRect{16, ky, 142, 44} : DRect{16, ky, 220, 44};
    const DRect pr = p ? DRect{162, ky, 142, 44} : DRect{244, ky, 220, 44};
    zoom_ = std::make_unique<Stepper>(root_, fit_, zr, "ZOOM", p ? 15 : 17, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.trim_zoom) act_.trim_zoom(d); });
    pos_ = std::make_unique<Stepper>(root_, fit_, pr, "POSITION", p ? 15 : 17, Icon::Left, Icon::Right,
                                     [this](int d) { nudge(d, false); });
    if (p) {
        box(root_, fit_.x(16), fit_.y(356), fit_.w(16, 288), fit_.h(356, 26), col::kStrip, fit_.px(5));
        bottom_left_ = fit_label(root_, fit_, Face::Bold, 13, col::kInk, 24, 356, 150, 26, LV_TEXT_ALIGN_LEFT, 0.5);
        bottom_right_ = fit_label(root_, fit_, Face::Bold, 13, col::kInk, 170, 356, 126, 26, LV_TEXT_ALIGN_RIGHT, 0.5);
    }

    /* the keys */
    static const Icon icons[5] = {Icon::SetIn, Icon::SetOut, Icon::Scissors, Icon::Play, Icon::Check};
    static const char *const names[5] = {"SET IN", "SET OUT", "SPLIT", "PLAY", "SAVE"};
    for (int i = 0; i < 5; ++i) {
        const DRect r = p ? DRect{16.0 + 59 * i, 392, 52, 64} : DRect{16.0 + 91 * i, 252, 84, 50};
        const KeyType t = p ? KeyType{10, 1, 5} : KeyType{12, 1.5, 3};
        keys_[i] = std::make_unique<PianoKey>(root_, fit_, r, t, [this, i](KeyEdge e) {
            if (e != KeyEdge::Click) return;
            switch (i) {
            case 0: if (act_.trim_set_in) act_.trim_set_in(); break;
            case 1: if (act_.trim_set_out) act_.trim_set_out(); break;
            case 2:
                if (vm_.selected_split >= 0) {
                    if (act_.trim_remove_split) act_.trim_remove_split(vm_.selected_split);
                } else if (act_.trim_split) {
                    act_.trim_split();
                }
                break;
            case 3: if (act_.trim_play) act_.trim_play(); break;
            case 4: if (act_.trim_save) act_.trim_save(); break;
            default: break;
            }
        });
        keys_[i]->set(icons[i], names[i], i == 4 ? KeyLook::Green : KeyLook::Charcoal, true, false);
    }
}

void TrimPage::update(const TrimVm &t)
{
    if (have_ && t == vm_) return; /* nothing changed */
    have_ = true;
    vm_ = t;
    const bool p = frame_.portrait;
    set_sticker(sticker_, t.sticker);
    set_text(take_, t.take);
    lv_obj_update_layout(take_);
    {
        const int tx = lv_obj_get_style_x(take_, LV_PART_MAIN) + text_width(lv_obj_get_style_text_font(take_, LV_PART_MAIN), t.take.c_str(),
                                                         lv_obj_get_style_text_letter_space(take_, LV_PART_MAIN));
        const int right = fit_.x(16 + (p ? 288 : 448) - 8);
        lv_obj_set_x(info_, tx + fit_.px(8));
        lv_obj_set_width(info_, std::max(10, right - tx - fit_.px(8)));
        set_fitted(info_, Face::Medium, fit_.px(p ? 13 : 14), 0, std::max(9, fit_.px(10)),
                   p ? trim_strip_narrow_top(t) : trim_strip_wide(t));
    }
    zoom_->set_value(fmt_zoom(t.span_ms));
    pos_->set_value(p ? fmt_tenths_short(t.marker_ms) : fmt_tenths(t.marker_ms));
    if (p) {
        set_fitted(bottom_left_, Face::Bold, fit_.px(13), ls_screen(0.5, fit_.s), std::max(9, fit_.px(10)), trim_strip_narrow_left(t));
        set_fitted(bottom_right_, Face::Bold, fit_.px(13), ls_screen(0.5, fit_.s), std::max(9, fit_.px(10)), trim_strip_narrow_right(t));
    }
    const bool sel = t.selected_split >= 0 && t.selected_split < static_cast<int>(t.splits.size());
    keys_[2]->set(sel ? Icon::Remove : Icon::Scissors, sel ? kRemove : kSplit, sel ? KeyLook::Red : KeyLook::Charcoal, true, false);
    keys_[3]->set(Icon::Play, "PLAY", KeyLook::Charcoal, true, t.playing);
    main_bars_ = resample_max(t.window_peaks, bar_count(main_w_ + (pitch_ - bar_w_), pitch_));
    over_bars_ = resample_max(t.overview_peaks, bar_count(over_w_ + (pitch_ - bar_w_), pitch_));
    lv_obj_invalidate(wave_);
}

void TrimPage::nudge(int dir, bool shift)
{
    if (act_.trim_nudge) act_.trim_nudge(dir * nudge_step_ms(shift));
}

void TrimPage::wave_draw_cb(lv_event_t *e)
{
    auto *self = static_cast<TrimPage *>(lv_event_get_user_data(e));
    self->draw_wave(lv_event_get_layer(e));
}

void TrimPage::draw_wave(lv_layer_t *layer)
{
    const TrimVm &t = vm_;
    const double s = fit_.s;
    auto px = [s](double v) { return std::max(1, static_cast<int>(std::lround(v * s))); };
    const bool p = frame_.portrait;
    const std::int64_t len = std::max<std::int64_t>(1, t.length_ms);

    /* ---- overview: the whole take */
    const int ovn = static_cast<int>(over_bars_.size());
    const int max_ov = over_h_ - px(2);
    for (int i = 0; i < ovn; ++i) {
        const double mid_t = (i + 0.5) * len / std::max(1, ovn);
        const bool kept = mid_t >= t.in_ms && mid_t <= t.out_ms;
        const int h = std::max(px(3), static_cast<int>(px(3) + over_bars_[static_cast<std::size_t>(i)] * (max_ov - px(3))));
        const int x = over_x_ + static_cast<int>(std::lround(i * pitch_));
        fill(layer, area(x, over_y_ + (over_h_ - h) / 2, std::max(1, static_cast<int>(std::lround(bar_w_))), h),
             kept ? col::kAmber : col::kWaveCut, 0, static_cast<lv_opa_t>(kept ? 255 : 140));
    }
    const int ow = std::max(1, static_cast<int>(std::lround(ovn * pitch_ - (pitch_ - bar_w_))));
    auto ox = [&](std::int64_t tt) { return static_cast<int>(std::lround(overview_x(tt, len, over_x_, ow))); };
    fill(layer, area(ox(t.in_ms), over_y_, px(2), over_h_), col::kChip, 0);
    fill(layer, area(ox(t.out_ms) - px(2), over_y_, px(2), over_h_), col::kChip, 0);
    for (std::int64_t sp : t.splits) fill(layer, area(ox(sp), over_y_, px(2), over_h_), col::kFlag, 0);
    {
        const std::int64_t a = t.marker_ms - t.span_ms / 2, b = a + t.span_ms;
        const int x1 = ox(a) - px(2), x2 = std::max(ox(b) + px(2), x1 + px(8));
        frame(layer, area(x1, over_y_ - px(2), x2 - x1, over_h_ + px(4)), col::kWhite, px(2), px(3));
    }

    /* ---- main bars: the window around the marker */
    const int mn = static_cast<int>(main_bars_.size());
    const int mw = std::max(1, static_cast<int>(std::lround(mn * pitch_ - (pitch_ - bar_w_))));
    const WaveWindow win = wave_window(t.marker_ms, t.span_ms, main_x_, mw);
    const int max_m = main_h_ - px(2);
    for (int i = 0; i < mn; ++i) {
        const std::int64_t mid_t = win.t_of(main_x_ + (i + 0.5) * pitch_);
        if (mid_t < 0 || mid_t > len) continue;
        const bool kept = mid_t >= t.in_ms && mid_t <= t.out_ms;
        const int h = std::max(px(3), static_cast<int>(px(3) + main_bars_[static_cast<std::size_t>(i)] * (max_m - px(3))));
        const int x = main_x_ + static_cast<int>(std::lround(i * pitch_));
        fill(layer, area(x, main_y_ + (main_h_ - h) / 2, std::max(1, static_cast<int>(std::lround(bar_w_))), h),
             kept ? col::kAmber : col::kWaveCut, 0, static_cast<lv_opa_t>(kept ? 255 : 140));
    }

    const lv_font_t *chip_f = font(Face::Bold, px(11));
    const int chip_h = px(16), chip_pad = px(6), tri = px(6);
    const int top = main_y_ + px(2), bottom = main_y_ + main_h_ - px(2);
    auto chip = [&](const std::string &label, int x, bool arrow_left, bool arrow_right, bool align_right) {
        const int tw = text_width(chip_f, label.c_str(), 0);
        const int w = tw + 2 * chip_pad + (arrow_left || arrow_right ? tri + px(3) : 0);
        const int cx = align_right ? x - w : x;
        fill(layer, area(cx, top, w, chip_h), col::kChip, px(3));
        int tx = cx + chip_pad;
        const int cy = top + chip_h / 2;
        if (arrow_left) {
            triangle(layer, tx + tri, cy - tri / 2 - 1, tx, cy, tx + tri, cy + tri / 2 + 1, col::kChipText);
            tx += tri + px(3);
        }
        const int lh = lv_font_get_line_height(chip_f);
        text(layer, chip_f, label.c_str(), area(tx, cy - lh / 2, tw + 2, lh), col::kChipText);
        if (arrow_right) {
            const int ax = tx + tw + px(3);
            triangle(layer, ax, cy - tri / 2 - 1, ax + tri, cy, ax, cy + tri / 2 + 1, col::kChipText);
        }
    };
    auto handle = [&](std::int64_t at, const char *name) {
        const std::string tm = p ? fmt_short(at) : fmt_tenths(at);
        switch (wave_edge(win, at)) {
        case WaveEdge::Left: chip(std::string(name) + " " + tm, main_x_ + px(4), true, false, false); break;
        case WaveEdge::Right: chip(std::string(name) + " " + tm, main_x_ + mw, false, true, true); break;
        case WaveEdge::Inside: {
            const int x = static_cast<int>(std::lround(win.x_of(at)));
            fill(layer, area(x - px(1), top, px(2), bottom - top), col::kChip, 0);
            chip(name, x, false, false, std::string(name) == "OUT");
            break;
        }
        }
    };
    handle(t.in_ms, "IN");
    handle(t.out_ms, "OUT");

    /* split flags */
    for (std::size_t i = 0; i < t.splits.size(); ++i) {
        if (wave_edge(win, t.splits[i]) != WaveEdge::Inside) continue;
        const int x = static_cast<int>(std::lround(win.x_of(t.splits[i])));
        const bool sel = static_cast<int>(i) == t.selected_split;
        const std::string num = std::to_string(i + 1);
        if (sel) {
            fill(layer, area(x, top, px(3), bottom - top), col::kWhite, 0);
            const int w = px(22), h = px(18);
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.radius = px(4);
            d.bg_color = lv_color_hex(col::kFlag);
            d.border_color = lv_color_hex(col::kWhite);
            d.border_width = px(2);
            const lv_area_t a = area(x - px(10), bottom - h, w, h);
            lv_draw_rect(layer, &d, &a);
            const lv_font_t *f = font(Face::Bold, px(12));
            const int lh = lv_font_get_line_height(f);
            text(layer, f, num.c_str(), area(a.x1, a.y1 + (h - lh) / 2, w, lh), col::kFlagText, LV_TEXT_ALIGN_CENTER);
        } else {
            line(layer, x + px(1), top, x + px(1), bottom, px(2), col::kFlag, LV_OPA_COVER, false, px(4), px(4));
            const int w = px(18), h = px(16);
            const lv_area_t a = area(x - px(8), bottom - h, w, h);
            fill(layer, a, col::kFlag, px(3));
            const lv_font_t *f = font(Face::Bold, px(11));
            const int lh = lv_font_get_line_height(f);
            text(layer, f, num.c_str(), area(a.x1, a.y1 + (h - lh) / 2, w, lh), col::kFlagText, LV_TEXT_ALIGN_CENTER);
        }
    }

    /* the marker: fixed in the middle */
    {
        const int mx = main_x_ + mw / 2;
        fill(layer, area(mx, main_y_, px(2), main_h_), col::kMarker, 0);
        triangle(layer, mx - px(4), main_y_ - px(2), mx + px(6), main_y_ - px(2), mx + px(1), main_y_ + px(5), col::kMarker);
    }

    /* the time ruler */
    const lv_font_t *rf = font(Face::Medium, px(9));
    const int rlh = lv_font_get_line_height(rf);
    const std::vector<RulerTick> ticks = ruler_ticks(win, 5);
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const int x = static_cast<int>(std::lround(ticks[i].x)) - (i + 1 == ticks.size() ? 0 : 0);
        fill(layer, area(std::min(x, main_x_ + mw - 1), ruler_y_, 1, px(4)), col::kWaveCut, 0);
        if (ticks[i].t < 0 || ticks[i].t > len) continue;
        const std::string lab = fmt_ruler(ticks[i].t);
        const int w = px(28);
        const int ty = ruler_y_ + px(5) + (px(10) - rlh) / 2;
        if (i == 0) text(layer, rf, lab.c_str(), area(x, ty, w * 2, rlh), col::kRuler, LV_TEXT_ALIGN_LEFT);
        else if (i + 1 == ticks.size()) text(layer, rf, lab.c_str(), area(x - 2 * w, ty, 2 * w, rlh), col::kRuler, LV_TEXT_ALIGN_RIGHT);
        else text(layer, rf, lab.c_str(), area(x - w, ty, 2 * w, rlh), col::kRuler, LV_TEXT_ALIGN_CENTER);
    }
}

void TrimPage::wave_event_cb(lv_event_t *e)
{
    auto *self = static_cast<TrimPage *>(lv_event_get_user_data(e));
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    const int mn = static_cast<int>(self->main_bars_.size());
    const int mw = std::max(1, static_cast<int>(std::lround(mn * self->pitch_ - (self->pitch_ - self->bar_w_))));
    const WaveWindow win = wave_window(self->press_marker_, self->vm_.span_ms, self->main_x_, mw);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        self->press_ = pt;
        self->press_marker_ = self->vm_.marker_ms;
        self->dragging_ = false;
        break;
    case LV_EVENT_PRESSING: {
        const int dx = pt.x - self->press_.x;
        if (!self->dragging_ && std::abs(dx) >= self->fit_.px(8)) self->dragging_ = true;
        if (self->dragging_ && self->act_.trim_scrub)
            self->act_.trim_scrub(scrub_marker(win, self->press_marker_, dx, self->vm_.length_ms));
        break;
    }
    case LV_EVENT_RELEASED:
        if (!self->dragging_) {
            /* a tap: the flag under it (a 44 px wide target around each flag line) */
            const WaveWindow now = wave_window(self->vm_.marker_ms, self->vm_.span_ms, self->main_x_, mw);
            const int f = flag_at(now, self->vm_.splits, pt.x, self->fit_.px(22));
            if (f >= 0 && self->act_.trim_select_split) self->act_.trim_select_split(f);
        }
        self->dragging_ = false;
        break;
    default: break;
    }
}

} // namespace ui
} // namespace cassette
