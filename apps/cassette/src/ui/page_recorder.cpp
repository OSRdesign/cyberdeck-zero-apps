/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the recorder page (boards Cassette-480x320 / -320x480, CassetteNoCard, CassetteCount, CassetteWind).
 * All coordinates below are the boards' (design px); Fit maps them to the screen.
 */
#include "pages.hpp"

#include "paint.hpp"
#include "theme.hpp"
#include "ui_format.hpp"

#include <algorithm>
#include <cmath>

namespace cassette {
namespace ui {

namespace {

const Icon kKeyIcons[5] = {Icon::Rew, Icon::Play, Icon::Fwd, Icon::Stop, Icon::Rec};
const char *const kKeyNames[5] = {"REW", "PLAY", "FWD", "STOP", "REC"};

/* the PLAY/PAUSE key shows what a press does: PLAY while nothing runs, PAUSE otherwise (boards, note n10) */
bool shows_pause(Transport t)
{
    return t != Transport::Idle;
}

lv_obj_t *peak_lamp(lv_obj_t *parent, const Fit &fit, double x, double y)
{
    lv_obj_t *o = box(parent, fit.x(x), fit.y(y), fit.px(12), fit.px(12), col::kPeakOff, LV_RADIUS_CIRCLE);
    border(o, 1, col::kPeakEdge);
    return o;
}

/* A plain linear Fit (no bands) with design y `dy` on screen y `sy`: a group of elements kept together at scale s */
Fit anchored(const Fit &f, double dy, double sy)
{
    Fit a = f;
    a.segs.clear();
    a.oy = sy - (dy - a.dbody) * a.s;
    return a;
}

} // namespace

RecorderPage::RecorderPage(lv_obj_t *parent, const Frame &frame, const UiActions &actions, const Nav &nav, AnimOptions anim)
    : PageBase(parent, frame), act_(actions), nav_(nav)
{
    /* The deck (640x480 at 11.3 px/mm) needs 79 px touch targets where the board scaled by 4/3 gives 59-67: the key
     * row, the steppers and (portrait) the counter row get the toolkit's TARGET; the window row (landscape) gets what
     * the meter panel needs with TARGET-high Recordings / Settings buttons; the margins give back what is missing. */
    const double s = frame.scale(), t = frame.target;
    btn_h_ = std::max(44 * s, t);
    const std::vector<Band> bl = {{40, 54, 0, 0, true},     {54, 176, 5 * s + btn_h_ + 73 * s, 0, false},
                                  {206, 250, t, 0, false},   {252, 302, t, 0, false}, {302, 320, 0, 0, true}};
    const std::vector<Band> bp = {{40, 54, 0, 0, true},   {54, 214, 0, 0, true},   {294, 338, t, 0, false},
                                  {352, 396, t, 0, false}, {402, 458, t, 0, false}, {458, 480, 0, 0, true}};
    fit_ = frame.fit({179, 204, 251}, {217, 291, 345, 399}, bl, bp);
    if (fit_.mins_dropped) btn_h_ = 44 * fit_.s;
    deck_body(root_, fit_);
    CassetteWindow::Layout lay;
    if (frame.portrait) {
        box(root_, fit_.x(11), fit_.y(84), fit_.w(11, 8), fit_.h(84, 18), col::kHinge, fit_.px(2));
        box(root_, fit_.x(11), fit_.y(166), fit_.w(11, 8), fit_.h(166, 18), col::kHinge, fit_.px(2));
        lay = {{16, 54, 288, 160}, 20, 61, 1.4, 104, 84, 80, 96, 64, 194, {226, 62, 72, 20}};
    } else {
        box(root_, fit_.x(11), fit_.y(72), fit_.w(11, 8), fit_.h(72, 16), col::kHinge, fit_.px(2));
        box(root_, fit_.x(11), fit_.y(142), fit_.w(11, 8), fit_.h(142, 16), col::kHinge, fit_.px(2));
        lay = {{16, 54, 234, 122}, 33, 63, 1.0, 84, 75, 60, 87, 64, 156, {176, 60, 68, 20}};
    }
    /* OPEN on the window's frame, under the cassette (the board draws the cassette over its top) */
    open_ = fit_label(root_, fit_, Face::Bold, 9, col::kFrame, frame.portrait ? 262 : 200, frame.portrait ? 198 : 161, 0, 10,
                      LV_TEXT_ALIGN_LEFT, 1.5);
    lv_label_set_text(open_, "OPEN");
    window_ = std::make_unique<CassetteWindow>(root_, fit_, lay, anim, [this] {
        if (act_.countdown_cancel) act_.countdown_cancel();
    });
    lv_obj_move_to_index(open_, lv_obj_get_index(open_) + 1); /* over the window's frame, under the cassette canvas */
    if (frame.portrait) build_portrait();
    else build_landscape();

    for (int i = 0; i < 5; ++i) {
        const DRect r = frame.portrait ? DRect{16.0 + 59 * i, 402, 52, 56} : DRect{16.0 + 91 * i, 252, 84, 50};
        const KeyType t = frame.portrait ? KeyType{10, 1, 4} : KeyType{12, 1.5, 3};
        keys_[i] = std::make_unique<PianoKey>(root_, fit_, r, t, [this, i](KeyEdge e) {
            if (act_.deck_key) act_.deck_key(static_cast<KeyId>(i), e);
        });
        keys_[i]->set(kKeyIcons[i], kKeyNames[i], i == 4 ? KeyLook::Red : KeyLook::Charcoal, true, false);
    }

    /* the notice, over everything of the strip (landscape) or of the counter row (portrait); last, so on top */
    NoticeSpec ns;
    ns.px = 14;
    ns.min_px = std::max(9, fit_.px(11));
    ns.ls = ls_screen(0.5, fit_.s);
    ns.pad = fit_.px(10);
    if (frame.portrait) {
        ns.x = fit_.x(16);
        ns.y = fit_.y(294);
        ns.w = fit_.w(16, 288);
        ns.h_min = ns.h_max = fit_.h(294, 44);
        ns.px = fit_.px(14);
        ns.radius = fit_.px(6);
        notice_ = std::make_unique<NoticeBox>(root_, ns, [this] {
            if (act_.notice_dismiss) act_.notice_dismiss();
        });
        notice_covers_ = {counter_->obj(), list_btn_->obj(), gear_btn_->obj(), box_title_, box_detail_};
    } else {
        ns.x = fit_.x(16);
        ns.y = fit_.y(182);
        ns.w = fit_.w(16, 448);
        ns.h_min = fit_.h(182, 20);
        ns.h_max = std::max(ns.h_min, fit_.y(204) - fit_.y(182));
        ns.px = fit_.px(14);
        notice_ = std::make_unique<NoticeBox>(root_, ns);
        notice_covers_ = {strip_text_, strip_lim_, strip_cut_, strip_state_};
    }
}

void RecorderPage::build_landscape()
{
    framed_panel(root_, fit_, {258, 54, 206, 122}, col::kPanel);
    /* inside the meter panel: the button row from the top (the counter and PEAK centred on it, as on the board), the
     * meter from the bottom, both at scale s; on the board both meet exactly */
    const int row_y = fit_.y(54) + fit_.px(5);
    const int bh = static_cast<int>(std::lround(btn_h_));
    const Fit mid = anchored(fit_, 81, row_y + bh / 2.0); /* 81: the board's button / counter centre line */
    const Fit lo = anchored(fit_, 176, fit_.y(176));
    counter_ = std::make_unique<Counter>(root_, mid, 268, 64, 66, 34, 12, 26, 7, 20, 5);
    peak_ = peak_lamp(root_, mid, 345, 66);
    lv_obj_t *peak = fit_label(root_, mid, Face::Bold, 9, col::kScale, 337, 82, 0, 10, LV_TEXT_ALIGN_LEFT, 0.8);
    lv_label_set_text(peak, "PEAK");
    list_btn_ = std::make_unique<IconButton>(root_, mid, DRect{366, 59, 40, 44}, Icon::List, [this] { nav_.go(Screen::List); });
    gear_btn_ = std::make_unique<IconButton>(root_, mid, DRect{410, 59, 40, 44}, Icon::Gear, [this] { nav_.go(Screen::Settings); });
    for (IconButton *b : {list_btn_.get(), gear_btn_.get()}) {
        lv_obj_set_y(b->obj(), row_y);
        lv_obj_set_height(b->obj(), bh);
    }
    in_label_ = fit_label(root_, lo, Face::Bold, 13, col::kInLabel, 268, 112, 0, 22, LV_TEXT_ALIGN_LEFT, 1);
    lv_label_set_text(in_label_, "IN");
    ch_l_ = fit_label(root_, lo, Face::Bold, 9, col::kInLabel, 271, 112, 0, 11);
    lv_label_set_text(ch_l_, "L");
    ch_r_ = fit_label(root_, lo, Face::Bold, 9, col::kInLabel, 271, 123, 0, 11);
    lv_label_set_text(ch_r_, "R");
    bar_ = std::make_unique<LedBar>(root_, lo, 286, 112, 9, 22, 2);
    lv_obj_t *br = box(root_, lo.x(374), lo.y(139), lo.w(374, 42), lo.h(139, 5), 0, 0, LV_OPA_TRANSP);
    border(br, lo.px(2), col::kAmber);
    lv_obj_set_style_border_side(br, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
    const double sx[4] = {2, 74, 112, 150};
    const char *const st[4] = {"-40", "-20", "-10", "0"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = fit_label(root_, lo, Face::Bold, 11, col::kScale, 286 + sx[i], 146, 0, 14);
        lv_label_set_text(l, st[i]);
    }
    lv_obj_t *tg = fit_label(root_, lo, Face::Bold, 9, col::kAmber, 372, 159, 46, 11, LV_TEXT_ALIGN_CENTER, 1.2);
    lv_label_set_text(tg, "TARGET");

    /* the info strip: the text left, the lamps right */
    box(root_, fit_.x(16), fit_.y(182), fit_.w(16, 448), fit_.h(182, 20), col::kStrip, fit_.px(5));
    strip_x_ = fit_.x(26);
    strip_w_ = fit_.w(26, 428);
    strip_y_ = fit_.y(182);
    strip_h_ = fit_.h(182, 20);
    strip_text_ = fit_label(root_, fit_, Face::Bold, 14, col::kInk, 26, 182, 300, 20, LV_TEXT_ALIGN_LEFT, 0.5);
    strip_lim_ = fit_label(root_, fit_, Face::Bold, 12, col::kInk, 300, 182, 0, 20, LV_TEXT_ALIGN_LEFT, 1);
    lv_label_set_text(strip_lim_, "LIM");
    strip_cut_ = fit_label(root_, fit_, Face::Bold, 12, col::kInk, 340, 182, 0, 20, LV_TEXT_ALIGN_LEFT, 1);
    lv_label_set_text(strip_cut_, "LOW CUT");
    strip_state_ = fit_label(root_, fit_, Face::Bold, 12, col::kRecWord, 420, 182, 0, 20, LV_TEXT_ALIGN_LEFT, 1);
    const int d9 = fit_.px(9), d11 = fit_.px(11), glow = fit_.px(5);
    lim_ = std::make_unique<Lamp>(root_, 0, 0, d9, col::kLampGreen, glow);
    cut_ = std::make_unique<Lamp>(root_, 0, 0, d9, col::kLampGreen, glow);
    state_lamp_ = std::make_unique<Lamp>(root_, 0, 0, d11, col::kLampRed, fit_.px(6));

    card_ = std::make_unique<Stepper>(root_, fit_, DRect{16, 206, 220, 44}, "CARD", 17, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.card_gain) act_.card_gain(d); });
    soft_ = std::make_unique<Stepper>(root_, fit_, DRect{244, 206, 220, 44}, "SOFT", 17, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.soft_gain) act_.soft_gain(d); });
}

void RecorderPage::build_portrait()
{
    framed_panel(root_, fit_, {16, 220, 288, 68}, col::kPanel);
    in_label_ = fit_label(root_, fit_, Face::Bold, 13, col::kInLabel, 26, 227, 0, 22, LV_TEXT_ALIGN_LEFT, 1);
    lv_label_set_text(in_label_, "IN");
    ch_l_ = fit_label(root_, fit_, Face::Bold, 9, col::kInLabel, 30, 227, 0, 10);
    lv_label_set_text(ch_l_, "L");
    ch_r_ = fit_label(root_, fit_, Face::Bold, 9, col::kInLabel, 30, 237, 0, 10);
    lv_label_set_text(ch_r_, "R");
    bar_ = std::make_unique<LedBar>(root_, fit_, 46, 227, 11, 20, 3);
    peak_ = peak_lamp(root_, fit_, 272, 229);
    lv_obj_t *peak = fit_label(root_, fit_, Face::Bold, 10, col::kScale, 264, 243, 0, 11, LV_TEXT_ALIGN_LEFT, 1);
    lv_label_set_text(peak, "PEAK");
    const double sx[4] = {4, 94, 138, 182};
    const char *const st[4] = {"-40", "-20", "-10", "0"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = fit_label(root_, fit_, Face::Bold, 10, col::kScale, 46 + sx[i], 251, 0, 12);
        lv_label_set_text(l, st[i]);
    }
    lv_obj_t *br = box(root_, fit_.x(158), fit_.y(264), fit_.w(158, 54), fit_.h(264, 5), 0, 0, LV_OPA_TRANSP);
    border(br, fit_.px(2), col::kAmber);
    lv_obj_set_style_border_side(br, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_t *tg = fit_label(root_, fit_, Face::Bold, 9, col::kAmber, 158, 270, 54, 11, LV_TEXT_ALIGN_CENTER, 1.2);
    lv_label_set_text(tg, "TARGET");

    counter_ = std::make_unique<Counter>(root_, fit_, 16, 294, 88, 44, 15, 30, 8, 24, 6);
    list_btn_ = std::make_unique<IconButton>(root_, fit_, DRect{108, 294, 44, 44}, Icon::List, [this] { nav_.go(Screen::List); });
    gear_btn_ = std::make_unique<IconButton>(root_, fit_, DRect{156, 294, 44, 44}, Icon::Gear, [this] { nav_.go(Screen::Settings); });
    /* the info box: take over time left, the state lamp at the top right */
    box(root_, fit_.x(204), fit_.y(294), fit_.w(204, 100), fit_.h(294, 44), col::kStrip, fit_.px(6));
    box_title_ = fit_label(root_, fit_, Face::Bold, 15, col::kInk, 212, 299, 66, 18, LV_TEXT_ALIGN_LEFT, 0.5);
    box_detail_ = fit_label(root_, fit_, Face::Medium, 13, col::kInkSoft, 212, 317, 84, 16);
    state_lamp_ = std::make_unique<Lamp>(root_, fit_.x(285), fit_.y(302), fit_.px(11), col::kLampRed, fit_.px(6));

    card_ = std::make_unique<Stepper>(root_, fit_, DRect{16, 352, 142, 44}, "CARD", 14, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.card_gain) act_.card_gain(d); });
    soft_ = std::make_unique<Stepper>(root_, fit_, DRect{162, 352, 142, 44}, "SOFT", 14, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.soft_gain) act_.soft_gain(d); });
}

void RecorderPage::place_steppers(bool card)
{
    if (!first_ && card == card_shown_) return;
    card_shown_ = card;
    card_->set_shown(card);
    if (frame_.portrait) soft_->place(fit_, card ? DRect{162, 352, 142, 44} : DRect{16, 352, 288, 44}, card ? 14 : 16);
    else soft_->place(fit_, card ? DRect{244, 206, 220, 44} : DRect{16, 206, 448, 44}, 17);
}

/* The strip's right group, right-aligned: LIM (lamp) LOW CUT (lamp) [REC|PLAY (lamp)], 6 px apart (the board). */
void RecorderPage::place_strip_right(const std::string &state)
{
    const int gap = fit_.px(6);
    const int right = strip_x_ + strip_w_;
    const int cy = strip_y_ + strip_h_ / 2;
    auto width = [](lv_obj_t *l) {
        return text_width(lv_obj_get_style_text_font(l, LV_PART_MAIN), lv_label_get_text(l),
                          lv_obj_get_style_text_letter_space(l, LV_PART_MAIN));
    };
    int x = right;
    auto put_lamp = [&](Lamp &lamp) {
        const int d = lv_obj_get_style_width(lamp.obj(), LV_PART_MAIN);
        x -= d;
        lv_obj_set_pos(lamp.obj(), x, cy - d / 2);
        x -= gap;
    };
    auto put_label = [&](lv_obj_t *l) {
        x -= width(l);
        lv_obj_set_x(l, x);
        x -= gap;
    };
    const bool has_state = !state.empty();
    show(strip_state_, has_state);
    if (has_state) {
        set_text(strip_state_, state);
        lv_obj_set_style_text_color(strip_state_, lv_color_hex(state == "PLAY" ? col::kPlayWord : col::kRecWord), 0);
        state_lamp_->set(true, state == "PLAY" ? col::kLampGreen : col::kLampRed);
        put_lamp(*state_lamp_);
        put_label(strip_state_);
    } else {
        state_lamp_->set(false, col::kLampRed);
    }
    put_lamp(*cut_);
    put_label(strip_cut_);
    put_lamp(*lim_);
    put_label(strip_lim_);
    /* the text takes what is left */
    lv_obj_set_width(strip_text_, std::max(10, x - strip_x_));
}

void RecorderPage::update(const RecorderVm &rec, const std::string &notice)
{
    const bool was_on = notice_on_;
    notice_on_ = notice_->set(notice);
    if (notice_on_ != was_on) {
        /* the state label (REC / PLAY) comes back with place_strip_right, when the strip is placed again */
        for (lv_obj_t *o : notice_covers_)
            if (notice_on_ || o != strip_state_) show(o, !notice_on_);
        state_shown_ = "?"; /* the strip's right group is placed again when the notice ends */
        first_ = true;
    }
    update_deck(rec);
}

void RecorderPage::update_deck(const RecorderVm &rec)
{
    window_->update(rec);
    counter_->set_ms(rec.counter_ms);
    bar_->update(rec.meter);
    const bool stereo = rec.meter.channels >= 2;
    show(in_label_, !stereo);
    show(ch_l_, stereo);
    show(ch_r_, stereo);
    show(open_, rec.transport != Transport::Countdown); /* under the countdown panel */
    const bool lit = rec.meter.ch[0].peak_lamp || (stereo && rec.meter.ch[1].peak_lamp);
    if (first_ || lit != peak_lit_) { /* a style change redraws: only when the lamp changes */
        peak_lit_ = lit;
        lv_obj_set_style_bg_color(peak_, lv_color_hex(lit ? col::kPeakOn : col::kPeakOff), 0);
        lv_obj_set_style_shadow_width(peak_, lit ? fit_.px(6) : 0, 0);
        lv_obj_set_style_shadow_color(peak_, lv_color_hex(col::kPeakOn), 0);
    }

    const std::string state = info_state(rec);
    if (notice_on_) {
        /* the strip (landscape) or the counter row (portrait) is under the notice: nothing of it is touched */
    } else if (!frame_.portrait) {
        if (first_ || state != state_shown_) {
            state_shown_ = state;
            place_strip_right(state);
        }
        auto lamp = [this](Lamp &l, bool on) {
            lv_obj_set_style_opa(l.obj(), static_cast<lv_opa_t>(on ? 255 : 46), 0);
            lv_obj_set_style_shadow_width(l.obj(), on ? fit_.px(5) : 0, 0);
        };
        if (first_ || rec.info.limiter != limiter_) lamp(*lim_, rec.info.limiter);
        if (first_ || rec.info.low_cut != low_cut_) lamp(*cut_, rec.info.low_cut);
        limiter_ = rec.info.limiter;
        low_cut_ = rec.info.low_cut;
        set_fitted(strip_text_, Face::Bold, fit_.px(14), ls_screen(0.5, fit_.s), std::max(9, fit_.px(11)), info_line(rec));
    } else {
        set_fitted(box_title_, Face::Bold, fit_.px(15), ls_screen(0.5, fit_.s), std::max(9, fit_.px(11)), info_title(rec));
        set_fitted(box_detail_, Face::Medium, fit_.px(13), 0, std::max(9, fit_.px(10)), info_detail(rec));
        state_lamp_->set(!state.empty(), state == "PLAY" ? col::kLampGreen : col::kLampRed);
    }

    place_steppers(rec.card.shown);
    const bool decimal = !frame_.portrait;
    card_->set_value(fmt_db(rec.card.db, decimal));
    card_->set_enabled(rec.card.can_down, rec.card.can_up);
    soft_->set_value(fmt_db(rec.soft.db, decimal));
    soft_->set_enabled(rec.soft.can_down, rec.soft.can_up);

    const bool pause = shows_pause(rec.transport);
    for (int i = 0; i < 5; ++i) {
        const KeyState &k = rec.keys[i];
        Icon ic = kKeyIcons[i];
        std::string name = kKeyNames[i];
        if (i == 1 && pause) {
            ic = Icon::Pause;
            name = "PAUSE";
        }
        keys_[i]->set(ic, name, i == 4 ? KeyLook::Red : KeyLook::Charcoal, k.enabled, k.latched);
    }
    first_ = false;
}

void RecorderPage::press_key(int key, bool down)
{
    if (key >= 0 && key < 5) keys_[key]->set_pressed(down);
}

void RecorderPage::set_shown_page(bool shown)
{
    set_shown(shown);
    window_->set_visible(shown);
}

} // namespace ui
} // namespace cassette
