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
#include <cstdio>

namespace cassette {
namespace ui {

namespace {

/* `text` with its trailing `tail` (when it ends with it) in the strip's red, as LVGL recolor markup. */
std::string red_tail(const std::string &text, const std::string &tail)
{
    if (tail.empty() || text.size() < tail.size() || text.compare(text.size() - tail.size(), tail.size(), tail) != 0) return text;
    char b[16];
    std::snprintf(b, sizeof b, "#%06X ", static_cast<unsigned>(col::kRecWord));
    return text.substr(0, text.size() - tail.size()) + b + tail + "#";
}

const Icon kKeyIcons[5] = {Icon::Rew, Icon::Play, Icon::Fwd, Icon::Stop, Icon::Rec};
const char *const kKeyNames[5] = {"REW", "PLAY", "FWD", "STOP", "REC"};

lv_obj_t *peak_lamp(lv_obj_t *parent, const Fit &fit, double x, double y, double d = 12)
{
    lv_obj_t *o = box(parent, fit.x(x), fit.y(y), fit.px(d), fit.px(d), col::kPeakOff, LV_RADIUS_CIRCLE);
    border(o, 1, col::kPeakEdge);
    return o;
}

/* The MON lamp: 70 % of the PEAK lamp (8.4 board px), its centre on design y `cy`; drawn dark until set lit. */
lv_obj_t *mon_lamp(lv_obj_t *parent, const Fit &fit, double x, double cy, double dia = 8.4)
{
    const int d = std::max(5, fit.px(dia));
    lv_obj_t *o = box(parent, fit.x(x), fit.y(cy) - d / 2, d, d, col::kMonOff, LV_RADIUS_CIRCLE);
    border(o, 1, col::kPeakEdge);
    lv_obj_set_style_shadow_color(o, lv_color_hex(col::kMonOn), 0);
    return o;
}

/* The square layout's design numbers (the 720x720 mockup; Fit scales them by width / 720). */
constexpr double kSqMargin = 24;             /* the content's left and right edge: 12 inside the body's 12 */
constexpr std::uint32_t kSqStripEdge = 0xA89F83;

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
    square_ = frame.square;
    info_box_ = frame.portrait || frame.square;
    const double s = frame.scale(), t = frame.target;
    btn_h_ = std::max(44 * s, t);
    const std::vector<Band> bl = {{40, 54, 0, 0, true},     {54, 176, 5 * s + btn_h_ + 73 * s, 0, false},
                                  {206, 250, t, 0, false},   {252, 302, t, 0, false}, {302, 320, 0, 0, true}};
    const std::vector<Band> bp = {{40, 54, 0, 0, true},   {54, 214, 0, 0, true},   {294, 338, t, 0, false},
                                  {352, 396, t, 0, false}, {402, 458, t, 0, false}, {458, 480, 0, 0, true}};
    if (square_) {
        fit_ = square_fit(frame.w, frame.h, frame.body_y);
        const int bx = fit_.x(12), by = fit_.y(72);
        lv_obj_t *body = box(root_, bx, by, fit_.x(708) - bx, fit_.sh - fit_.px(12) - by, col::kBody, fit_.px(12));
        border(body, 1, col::kBodyEdge);
    } else {
        fit_ = frame.fit({179, 204, 251}, {217, 291, 345, 399}, bl, bp);
        if (fit_.mins_dropped) btn_h_ = 44 * fit_.s;
        deck_body(root_, fit_);
    }
    CassetteWindow::Layout lay;
    if (square_) {
        /* the window 498 x 260 under the panel's top, the cassette 2.295x (459 x 239) in it; the countdown's ring and the
         * texts at 1.7x the boards' */
        box(root_, fit_.x(102), fit_.y(150), fit_.w(102, 10), fit_.h(150, 22), col::kHinge, fit_.px(2));
        box(root_, fit_.x(102), fit_.y(250), fit_.w(102, 10), fit_.h(250, 22), col::kHinge, fit_.px(2));
        g_ = sq_geometry(frame.target / fit_.s);
        const double ts = std::min(2.295, (g_.win_h - 14) / 104); /* the cassette, 7 px margin at least */
        const double rd = std::min(160.0, g_.win_h - 100);       /* the countdown's ring */
        lay = {{111, 84, 498, g_.win_h}, 111 + (498 - 200 * ts) / 2, 84 + (g_.win_h - 104 * ts) / 2, ts, rd,
               84 + g_.win_h / 2 + 2 - rd / 2, 112 * rd / 160, 150, 94, 84 + g_.win_h - 36, {477, 94, 120, 36}, 1.7};
    } else if (frame.portrait) {
        box(root_, fit_.x(11), fit_.y(84), fit_.w(11, 8), fit_.h(84, 18), col::kHinge, fit_.px(2));
        box(root_, fit_.x(11), fit_.y(166), fit_.w(11, 8), fit_.h(166, 18), col::kHinge, fit_.px(2));
        lay = {{16, 54, 288, 160}, 20, 61, 1.4, 104, 84, 80, 96, 64, 194, {226, 62, 72, 20}};
    } else {
        box(root_, fit_.x(11), fit_.y(72), fit_.w(11, 8), fit_.h(72, 16), col::kHinge, fit_.px(2));
        box(root_, fit_.x(11), fit_.y(142), fit_.w(11, 8), fit_.h(142, 16), col::kHinge, fit_.px(2));
        lay = {{16, 54, 234, 122}, 33, 63, 1.0, 84, 75, 60, 87, 64, 156, {176, 60, 68, 20}};
    }
    window_ = std::make_unique<CassetteWindow>(root_, fit_, lay, anim, [this] {
        if (act_.countdown_cancel) act_.countdown_cancel();
    });
    /* the strip text and the counter keep what they showed until the loaded cassette has landed */
    window_->set_landed_callback([this] {
        if (last_rec_) update_deck(*last_rec_);
    });
    if (square_) build_square();
    else if (frame.portrait) build_portrait();
    else build_landscape();

    for (int i = 0; i < 5; ++i) {
        const DRect r = square_ ? DRect{kSqMargin + 136.8 * i, g_.key_y, 124.8, g_.key_h}
                        : frame.portrait ? DRect{16.0 + 59 * i, 402, 52, 56} : DRect{16.0 + 91 * i, 252, 84, 50};
        const KeyType t = square_ ? KeyType{20, 2, 6, 1.45} : frame.portrait ? KeyType{10, 1, 4} : KeyType{12, 1.5, 3};
        keys_[i] = std::make_unique<PianoKey>(root_, fit_, r, t, [this, i](KeyEdge e) {
            if (window_->loading()) return; /* the cassette is going in: touches are ignored (Esc is not one) */
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
    if (square_) {
        ns.x = fit_.x(kSqMargin);
        ns.y = fit_.y(g_.row_y);
        ns.w = fit_.w(kSqMargin, 672);
        ns.h_min = ns.h_max = fit_.h(g_.row_y, g_.row_h);
        ns.px = fit_.px(28);
        ns.min_px = std::max(9, fit_.px(18));
        ns.ls = ls_screen(1, fit_.s);
        ns.pad = fit_.px(16);
        ns.radius = fit_.px(8);
        notice_ = std::make_unique<NoticeBox>(root_, ns, [this] {
            if (act_.notice_dismiss) act_.notice_dismiss();
        });
        notice_covers_ = {counter_->obj(), list_btn_->obj(), gear_btn_->obj(), box_title_, box_detail_};
    } else if (frame.portrait) {
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
    list_btn_ = std::make_unique<IconButton>(root_, mid, DRect{366, 59, 40, 44}, Icon::List, [this] { if (!window_->loading()) nav_.go(Screen::List); });
    gear_btn_ = std::make_unique<IconButton>(root_, mid, DRect{410, 59, 40, 44}, Icon::Gear, [this] { if (!window_->loading()) nav_.go(Screen::Settings); });
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
    /* MON: bottom left, under IN and the -40 label, on the TARGET caption's row (left of -40 is too narrow for it) */
    mon_lamp_ = mon_lamp(root_, lo, 268, 164.5);
    mon_label_ = fit_label(root_, lo, Face::Bold, 9, col::kScale, 280, 159, 0, 11, LV_TEXT_ALIGN_LEFT, 0.8);
    lv_label_set_text(mon_label_, "MON");

    /* the info strip: the text left, the lamps right */
    box(root_, fit_.x(16), fit_.y(182), fit_.w(16, 448), fit_.h(182, 20), col::kStrip, fit_.px(5));
    strip_x_ = fit_.x(26);
    strip_w_ = fit_.w(26, 428);
    strip_y_ = fit_.y(182);
    strip_h_ = fit_.h(182, 20);
    strip_text_ = fit_label(root_, fit_, Face::Bold, 14, col::kInk, 26, 182, 300, 20, LV_TEXT_ALIGN_LEFT, 0.5);
    lv_label_set_recolor(strip_text_, true); /* the lost-audio part is red (red_tail) */
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
                                      [this](int d) { if (act_.card_gain && !window_->loading()) act_.card_gain(d); });
    soft_ = std::make_unique<Stepper>(root_, fit_, DRect{244, 206, 220, 44}, "SOFT", 17, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.soft_gain && !window_->loading()) act_.soft_gain(d); });
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
    /* PEAK at the bottom left, where MON was; MON, LIM and LOW CUT are the list at the right edge (build_meter_lamps) */
    const PeakGeo pk = portrait_peak();
    peak_ = peak_lamp(root_, fit_, pk.lamp.x, pk.lamp.y, pk.lamp.w);
    lv_obj_t *peak = fit_label(root_, fit_, Face::Bold, 10, col::kScale, pk.label.x, pk.label.y, 0, pk.label.h, LV_TEXT_ALIGN_LEFT, 1);
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
    build_meter_lamps(portrait_lamp_spec(), 0.5, col::kScale);

    counter_ = std::make_unique<Counter>(root_, fit_, 16, 294, 88, 44, 15, 30, 8, 24, 6);
    list_btn_ = std::make_unique<IconButton>(root_, fit_, DRect{108, 294, 44, 44}, Icon::List, [this] { if (!window_->loading()) nav_.go(Screen::List); });
    gear_btn_ = std::make_unique<IconButton>(root_, fit_, DRect{156, 294, 44, 44}, Icon::Gear, [this] { if (!window_->loading()) nav_.go(Screen::Settings); });
    /* the info box: the codec over the time left; the state lamp at the right end of the time line, so the codec line has
     * the whole box width (the time line ends before the lamp) */
    box(root_, fit_.x(204), fit_.y(294), fit_.w(204, 100), fit_.h(294, 44), col::kStrip, fit_.px(6));
    box_title_ = fit_label(root_, fit_, Face::Bold, 15, col::kInk, 212, 299, 84, 18, LV_TEXT_ALIGN_LEFT, 0);
    box_detail_ = fit_label(root_, fit_, Face::Medium, 13, col::kInkSoft, 212, 317, 69, 16);
    lv_label_set_recolor(box_detail_, true); /* the lost-audio part is red (red_tail) */
    state_lamp_ = std::make_unique<Lamp>(root_, fit_.x(285), fit_.y(319.5), fit_.px(11), col::kLampRed, fit_.px(6));

    card_ = std::make_unique<Stepper>(root_, fit_, DRect{16, 352, 142, 44}, "CARD", 14, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.card_gain && !window_->loading()) act_.card_gain(d); });
    soft_ = std::make_unique<Stepper>(root_, fit_, DRect{162, 352, 142, 44}, "SOFT", 14, Icon::Minus, Icon::Plus,
                                      [this](int d) { if (act_.soft_gain && !window_->loading()) act_.soft_gain(d); });
}

/* The square layout: one column, every number the 720x720 mockup's (design px, scaled by width / 720). */
void RecorderPage::build_square()
{
    const double my = g_.meter_y - 440; /* the panel against the mockup (the window may have shrunk) */
    const double sy = my + kSqStereoGrow; /* the content's rows as drawn with two channels; one channel moves them (square_stereo) */
    /* what moves with the channel count: down for the rows that sit at the panel's top, up for those at its bottom */
    auto moved = [this](lv_obj_t *o, double mono_dy) {
        sq_moved_.push_back({o, lv_obj_get_style_y(o, LV_PART_MAIN), mono_dy});
        return o;
    };
    const double half = kSqStereoGrow / 2;

    /* row under the window: the counter, the two buttons, the info box */
    counter_ = std::make_unique<Counter>(root_, fit_, kSqMargin, g_.row_y, 200, g_.row_h, 30, 44, 14, 39, 8);
    list_btn_ = std::make_unique<IconButton>(root_, fit_, DRect{236, g_.row_y, 84, g_.row_h}, Icon::List,
                                             [this] { if (!window_->loading()) nav_.go(Screen::List); }, 1.6);
    gear_btn_ = std::make_unique<IconButton>(root_, fit_, DRect{332, g_.row_y, 84, g_.row_h}, Icon::Gear,
                                             [this] { if (!window_->loading()) nav_.go(Screen::Settings); }, 1.6);
    lv_obj_t *strip = box(root_, fit_.x(428), fit_.y(g_.row_y), fit_.w(428, 268), fit_.h(g_.row_y, g_.row_h), col::kStrip, fit_.px(8));
    border(strip, 1, kSqStripEdge);
    title_px_ = 30;
    detail_px_ = 24;
    detail_min_ = 16;
    box_title_ = fit_label(root_, fit_, Face::Bold, 30, col::kInk, 444, g_.row_y + 8 + (g_.row_h - 72) / 2, 236, 30, LV_TEXT_ALIGN_LEFT, 0);
    box_detail_ = fit_label(root_, fit_, Face::Medium, 24, col::kInkSoft, 444, g_.row_y + 44 + (g_.row_h - 72) / 2, 210, 24);
    lv_label_set_recolor(box_detail_, true); /* the lost-audio part is red (red_tail) */
    state_lamp_ = std::make_unique<Lamp>(root_, fit_.x(662), fit_.y(g_.row_y + 47 + (g_.row_h - 72) / 2), fit_.px(18), col::kLampRed, fit_.px(6));

    /* the meter panel across the full width: IN / L R, the bars, the scale, TARGET, PEAK, MON */
    sq_panel_ = framed_panel(root_, fit_, {kSqMargin, g_.meter_y, 672, g_.meter_h}, col::kPanel);
    in_label_ = fit_label(root_, fit_, Face::Bold, 28, col::kInLabel, 45, g_.bar_y, 0, 28, LV_TEXT_ALIGN_LEFT, 1);
    lv_label_set_text(in_label_, "IN");
    moved(in_label_, half);
    ch_l_ = fit_label(root_, fit_, Face::Bold, 16, col::kInLabel, 48, g_.bar_y, 0, 17);
    lv_label_set_text(ch_l_, "L");
    ch_r_ = fit_label(root_, fit_, Face::Bold, 16, col::kInLabel, 48, g_.bar_y + 21, 0, 17);
    lv_label_set_text(ch_r_, "R");
    LedStyle ls;
    ls.over = 6;
    ls.tick_w = 3;
    ls.radius = 3;
    bar_ = std::make_unique<LedBar>(root_, fit_, 112, g_.bar_y, 30, kSqStereoBarH, 4, ls);
    /* PEAK at the bottom left (lamp over label, under IN); MON, LIM and LOW CUT are the list at the right edge. Neither moves
     * with the channel count (the list and PEAK are clear of both states' rows). */
    const PeakGeo pk = square_peak(g_);
    peak_ = peak_lamp(root_, fit_, pk.lamp.x, pk.lamp.y, pk.lamp.w);
    lv_obj_t *peak = fit_label(root_, fit_, Face::Bold, 18, col::kScale, pk.label.x, pk.label.y, pk.label.w, pk.label.h,
                               LV_TEXT_ALIGN_CENTER, 1);
    lv_label_set_text(peak, "PEAK");
    const double sx[4] = {107, 345, 464, 583};
    const char *const st[4] = {"-40", "-20", "-10", "0"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = fit_label(root_, fit_, Face::Medium, 22, col::kInLabel, sx[i], sy + 482, 40, 22, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(l, st[i]);
        moved(l, -half);
    }
    lv_obj_t *br = box(root_, fit_.x(388), fit_.y(sy + 510), fit_.w(388, 143), fit_.h(sy + 510, 6), 0, 0, LV_OPA_TRANSP);
    border(br, fit_.px(2), col::kAmber);
    lv_obj_set_style_border_side(br, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
    moved(br, -half);
    lv_obj_t *tg = fit_label(root_, fit_, Face::Bold, 16, col::kAmber, 541, sy + 505, 0, 16, LV_TEXT_ALIGN_LEFT, 2);
    lv_label_set_text(tg, "TARGET");
    moved(tg, -half);
    build_meter_lamps(square_lamp_spec(g_), 1, col::kInLabel);

    StepperLook look;
    look.btn_w = std::max(60.0, 41.0 / fit_.s); /* at least the toolkit's 40 px wide: more design px under scale 0.68 */
    look.block_h = 58;
    look.cap_px = 20;
    look.cap_ls = 2;
    look.cap_top = 8;
    look.cap_line = 20;
    look.val_top = 27; /* the mono face sits low in its line: the digits end a few px above the frame */
    look.val_line = 30;
    look.icon = 1.6;
    card_ = std::make_unique<Stepper>(root_, fit_, DRect{kSqMargin, g_.step_y, 328, g_.step_h}, "CARD", 28, Icon::Minus,
                                      Icon::Plus, [this](int d) { if (act_.card_gain && !window_->loading()) act_.card_gain(d); },
                                      look);
    soft_ = std::make_unique<Stepper>(root_, fit_, DRect{368, g_.step_y, 328, g_.step_h}, "SOFT", 28, Icon::Minus,
                                      Icon::Plus, [this](int d) { if (act_.soft_gain && !window_->loading()) act_.soft_gain(d); },
                                      look);
    sq_stereo_ = true; /* drawn for two channels; the default (one) is applied now */
    square_stereo(false);
}

/* The list at the meter panel's right edge (portrait and square: their info box has no room for LIM and LOW CUT, landscape
 * has them in the strip): MON, LIM, LOW CUT, one lamp and one label per row, the dots at one x, the labels at one x, equal
 * size and pitch. The places are the pure geometry of meter_lamps.hpp; the label texts are measured here, so "LOW CUT"
 * becomes "CUT" only where the list is too narrow for it. The three lamps are drawn alike: one diameter, the same 1 px
 * rim, the same green and glow when on, the same neutral grey without glow and the same dimmed label when off
 * (list_lamp_level, from update_deck). LIM and LOW CUT are Lamp objects (landscape's type), MON a plain box. */
void RecorderPage::build_meter_lamps(const MeterLampSpec &spec, double ls, std::uint32_t label_rgb)
{
    const int lpx = fit_.px(spec.px);
    auto width = [&](const std::string &t, double) {
        return text_width(font(Face::Bold, lpx), t.c_str(), ls_screen(ls, fit_.s)) / fit_.s;
    };
    const MeterLampGeo g = meter_lamps_column(spec, width);
    const int d = fit_.px(spec.lamp_d);
    lamp_glow_ = fit_.px(spec.lamp_d * 0.5);
    mon_glow_ = spec.lamp_d * 0.5;
    mon_lamp_ = mon_lamp(root_, fit_, g.lamp[0].x, g.lamp[0].y + spec.lamp_d / 2, spec.lamp_d);
    lv_obj_set_pos(mon_lamp_, fit_.x(g.lamp[0].x), fit_.y(g.lamp[0].y)); /* the same rounding as the two Lamps */
    lv_obj_set_size(mon_lamp_, d, d);
    lim_ = std::make_unique<Lamp>(root_, fit_.x(g.lamp[1].x), fit_.y(g.lamp[1].y), d, col::kLampGreen, lamp_glow_);
    cut_ = std::make_unique<Lamp>(root_, fit_.x(g.lamp[2].x), fit_.y(g.lamp[2].y), d, col::kLampGreen, lamp_glow_);
    for (lv_obj_t *o : {lim_->obj(), cut_->obj()}) border(o, 1, col::kPeakEdge); /* MON's rim (mon_lamp) */
    for (int i = 0; i < kLampRows; ++i) {
        lv_obj_t *l = fit_label(root_, fit_, Face::Bold, spec.px, label_rgb, g.label[i].x, g.label[i].y, 0, g.label[i].h,
                                LV_TEXT_ALIGN_LEFT, ls);
        lv_label_set_text(l, g.text(i));
        (i == 0 ? mon_label_ : i == 1 ? lim_label_ : cut_label_) = l;
    }
}

/* The square layout's meter panel is always as tall as two channels need (kSqStereoGrow more than one), so nothing under
 * it ever moves. With one channel the single bar row (24 px) and the rows around it sit in the middle of the panel: they
 * move by half the growth (the top and bottom paddings stay equal); with two the bars take two rows (38 px). Only the
 * panel's own contents change place here. */
void RecorderPage::square_stereo(bool stereo)
{
    sq_stereo_ = stereo;
    const double bd = stereo ? 0 : kSqStereoGrow / 2;
    bar_->set_height(fit_, g_.bar_y + bd, stereo ? kSqStereoBarH : kSqBarH);
    for (const Moved &m : sq_moved_) {
        const double d = stereo ? 0 : m.mono_dy;
        lv_obj_set_y(m.obj, m.y + fit_.y(g_.meter_y + d) - fit_.y(g_.meter_y));
    }
}

void RecorderPage::place_steppers(bool card)
{
    if (!first_ && card == card_shown_) return;
    card_shown_ = card;
    card_->set_shown(card);
    if (square_) {
        const double y = g_.step_y;
        card_->place(fit_, DRect{kSqMargin, y, 328, g_.step_h}, 28);
        soft_->place(fit_, card ? DRect{368, y, 328, g_.step_h} : DRect{kSqMargin, y, 672, g_.step_h}, 28);
    } else if (frame_.portrait) soft_->place(fit_, card ? DRect{162, 352, 142, 44} : DRect{16, 352, 288, 44}, card ? 14 : 16);
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

/* The info box's first line, bold, one line: the largest size (ui_format.hpp largest_fit_px) at which it fits the label's
 * width, down to the font ladder's smallest (6 px); centred on the line's place whatever the size. */
void RecorderPage::set_box_codec(const std::string &text)
{
    if (text == box_codec_ && box_codec_px_ > 0) return;
    const int w = lv_obj_get_style_width(box_title_, LV_PART_MAIN);
    const int px = largest_fit_px(fit_.px(title_px_), 6, w,
                                  [&](int p) { return text_width(font(Face::Bold, p), text.c_str(), 0); });
    const lv_font_t *f = font(Face::Bold, px);
    if (box_title_cy_ < 0)
        box_title_cy_ = lv_obj_get_style_y(box_title_, LV_PART_MAIN) + lv_font_get_line_height(lv_obj_get_style_text_font(box_title_, LV_PART_MAIN)) / 2;
    lv_obj_set_style_text_font(box_title_, f, 0);
    lv_obj_set_style_text_letter_space(box_title_, 0, 0);
    lv_obj_set_y(box_title_, box_title_cy_ - lv_font_get_line_height(f) / 2);
    set_text(box_title_, text);
    box_codec_ = text;
    box_codec_px_ = px;
}

void RecorderPage::update(const RecorderVm &rec, const std::string &notice)
{
    last_rec_ = &rec;
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
    /* a take is being loaded: the strip text and the counter wait for the landing of the cassette (the landed callback
     * calls this again); everything else follows the model at once */
    const bool hold = window_->before_landing();
    if (!hold) counter_->set_ms(rec.counter_ms);
    const bool stereo = rec.meter.channels >= 2;
    if (square_ && stereo != sq_stereo_) square_stereo(stereo);
    bar_->update(rec.meter);
    show(in_label_, !stereo);
    show(ch_l_, stereo);
    show(ch_r_, stereo);
    const bool lit = rec.meter.ch[0].peak_lamp || (stereo && rec.meter.ch[1].peak_lamp);
    if (first_ || lit != peak_lit_) { /* a style change redraws: only when the lamp changes */
        peak_lit_ = lit;
        lv_obj_set_style_bg_color(peak_, lv_color_hex(lit ? col::kPeakOn : col::kPeakOff), 0);
        lv_obj_set_style_shadow_width(peak_, lit ? fit_.px(6) : 0, 0);
        lv_obj_set_style_shadow_color(peak_, lv_color_hex(col::kPeakOn), 0);
    }
    /* MON: only with a playback side; lit while the monitor runs; touched only when it changes */
    if (first_ || rec.monitor_shown != mon_shown_) {
        mon_shown_ = rec.monitor_shown;
        show(mon_lamp_, mon_shown_);
        show(mon_label_, mon_shown_);
    }
    const bool mon_lit = rec.monitor_shown && rec.monitor_active;
    if (info_box_ && (first_ || mon_lit != mon_lit_)) {
        mon_lit_ = mon_lit;
        list_lamp_level(mon_lamp_, mon_label_, mon_lit);
    } else if (first_ || mon_lit != mon_lit_) {
        mon_lit_ = mon_lit;
        lv_obj_set_style_bg_color(mon_lamp_, lv_color_hex(mon_lit ? col::kMonOn : col::kMonOff), 0);
        lv_obj_set_style_shadow_width(mon_lamp_, mon_lit ? fit_.px(mon_glow_) : 0, 0);
        lv_obj_set_style_text_opa(mon_label_, mon_lit ? LV_OPA_COVER : LV_OPA_50, 0);
    }

    const std::string state = info_state(rec);
    if (notice_on_ || hold) {
        /* the strip (landscape) or the counter row (portrait) is under the notice: nothing of it is touched; while a take
         * is loaded the strip keeps its text until the landing */
    } else if (!info_box_) {
        if (first_ || state != state_shown_) {
            state_shown_ = state;
            place_strip_right(state);
        }
        if (first_ || rec.info.limiter != limiter_) lamp_level(*lim_, rec.info.limiter, fit_.px(5));
        if (first_ || rec.info.low_cut != low_cut_) lamp_level(*cut_, rec.info.low_cut, fit_.px(5));
        limiter_ = rec.info.limiter;
        low_cut_ = rec.info.low_cut;
        /* the largest size that fits one line, down to the smallest the strip allows (the REW / FWD text at 640x480 needs the
         * smaller size; a text that fits keeps the size it had) */
        set_fitted(strip_text_, Face::Bold, fit_.px(14), ls_screen(0.5, fit_.s), std::max(8, fit_.px(9)),
                   red_tail(info_line(rec), info_lost(rec)));
    } else {
        set_box_codec(info_codec(rec));
        set_fitted(box_detail_, Face::Medium, fit_.px(detail_px_), 0, std::max(9, fit_.px(detail_min_)),
                   red_tail(info_detail(rec), info_lost(rec)));
        state_lamp_->set(!state.empty(), state == "PLAY" ? col::kLampGreen : col::kLampRed);
    }

    if (info_box_) { /* the meter panel's lamps (not under the notice: it covers the counter row only) */
        if (first_ || rec.info.limiter != limiter_) list_lamp_level(lim_->obj(), lim_label_, rec.info.limiter);
        if (first_ || rec.info.low_cut != low_cut_) list_lamp_level(cut_->obj(), cut_label_, rec.info.low_cut);
        limiter_ = rec.info.limiter;
        low_cut_ = rec.info.low_cut;
    }

    place_steppers(rec.card.shown);
    const bool decimal = !frame_.portrait;
    card_->set_value(fmt_db(rec.card.db, decimal));
    card_->set_enabled(rec.card.can_down, rec.card.can_up);
    soft_->set_value(fmt_db(rec.soft.db, decimal));
    soft_->set_enabled(rec.soft.can_down, rec.soft.can_up);

    /* the PLAY/PAUSE key shows the current state (the table in ui_format.hpp, play_key_look): PLAY latched while a take
     * plays or scans, PAUSE latched while paused, PAUSE unlatched while recording, PLAY unlatched otherwise; the latch is
     * the model's (KeyState::latched) */
    const bool pause = play_key_look(rec.transport).pause_glyph;
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

/* LIM / LOW CUT lit (the function is on in Settings) or dim: full opacity with its glow, or opacity 46 and no glow. */
void RecorderPage::lamp_level(Lamp &l, bool on, int glow)
{
    lv_obj_set_style_opa(l.obj(), static_cast<lv_opa_t>(on ? 255 : 46), 0);
    lv_obj_set_style_shadow_width(l.obj(), on ? (glow > 0 ? glow : lamp_glow_) : 0, 0);
}

/* MON, LIM or LOW CUT of the meter panel's list: on, the lamp green with its glow and the label at full opacity; off, the
 * neutral grey dot without glow and the label at 55 %. The dot keeps its size and rim in both states. */
void RecorderPage::list_lamp_level(lv_obj_t *dot, lv_obj_t *label, bool on)
{
    lv_obj_set_style_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(on ? col::kLampGreen : col::kListLampOff), 0);
    lv_obj_set_style_shadow_color(dot, lv_color_hex(col::kLampGreen), 0);
    lv_obj_set_style_shadow_width(dot, on ? lamp_glow_ : 0, 0);
    lv_obj_set_style_text_opa(label, static_cast<lv_opa_t>(on ? 255 : col::kListLabelOffOpa), 0);
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
