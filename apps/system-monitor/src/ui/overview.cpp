/*
 * SPDX-License-Identifier: MIT
 *
 * System Monitor: the overview's tile grid (see overview.hpp). Every size from the layout service.
 */
#include "overview.hpp"

#include "panels.hpp"

#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace sm {
namespace {

constexpr std::uint32_t kTileBg = 0x1E1E1E;     /* the launcher home grid's tile ... */
constexpr std::uint32_t kTileFocusBg = 0x262626; /* ... selected ... */
constexpr std::uint32_t kTilePressed = 0x33414D; /* ... and pressed */
constexpr std::uint32_t kWhite = 0xFFFFFF;
/* PDA mode (a portrait screen): the cards stack in one column when it is at most this wide, else two columns (grid.hpp):
 * the 480 px wide portrait panels (53-56 mm) get one column, the 800 px wide tablet (90 mm) two. mm x 10. */
constexpr int kPortraitCardMaxMmX10 = 600;

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, std::uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_label_set_text(l, "");
    return l;
}

void set_text_if(lv_obj_t *label, const std::string &t)
{
    const char *now = lv_label_get_text(label);
    if (!now || t != now) lv_label_set_text(label, t.c_str());
}

const lv_font_t *medium(int px)
{
    if (const lv_font_t *f = cp0_fonts().get("Montserrat-Medium.ttf", static_cast<uint16_t>(px))) return f;
    return cp0_ui_font_px(px);
}

int even(int px) { return px - (px % 2); }

std::uint32_t value_rgb(Tone t, bool dim)
{
    if (dim) return kRgbLabel;
    switch (t) {
    case Tone::Good: return kRgbValue;
    case Tone::Warn: return kRgbWarn;
    case Tone::Error: return kRgbError;
    default: return kWhite;
    }
}

std::uint32_t frame_rgb(Tone t)
{
    switch (t) {
    case Tone::Good: return kRgbOk;
    case Tone::Warn: return kRgbWarn;
    case Tone::Error: return kRgbError;
    default: return kRgbBorder;
    }
}

/* the first variant that fits `room`, else the last one */
const std::string &fit(const std::vector<std::string> &variants, const lv_font_t *font, int room)
{
    static const std::string none;
    if (variants.empty()) return none;
    for (const std::string &v : variants)
        if (text_width(v.c_str(), font) <= room) return v;
    return variants.back();
}

/* the widest text a value takes, so the history line beside it keeps its place from second to second */
const char *reserve_of(Spark spark)
{
    switch (spark) {
    case Spark::Temp: return "88.8 \xC2\xB0" "C";
    case Spark::Cpu:
    case Spark::Mem:
    case Spark::Bar: return "100%";
    default: return "";
    }
}

} // namespace

OverviewGrid::OverviewGrid(lv_obj_t *parent, const cp0_ui_roller_page_t &layout, Events events, int focus)
    : layout_(layout), events_(std::move(events))
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    pad_ = cp0_ui_tok(CP0_TOK_SPACE_S);
    gap_ = cp0_ui_tok(CP0_TOK_SPACE_S);
    const int margin = cp0_ui_tok(CP0_TOK_SPACE_M);
    small_ = caption_font();
    label_ = medium(m ? m->text_px[CP0_TEXT_BODY] : 24);
    small_h_ = lv_font_get_line_height(small_);
    const int body_line = lv_font_get_line_height(label_);

    root_ = plain(parent);
    lv_obj_set_pos(root_, 0, layout.body_y);
    lv_obj_set_size(root_, layout.w, layout.body_h);
    lv_obj_add_event_cb(root_, key_cb, LV_EVENT_KEY, this);

    x0_ = margin;
    y0_ = pad_;
    const int w = layout.w - 2 * margin;
    const int h = layout.body_h - 2 * pad_;
    const int min_tile_h = 2 * pad_ + 2 * small_h_ + body_line;
    portrait_ = layout.h > layout.w;
    const int max_tile_w = portrait_ && m ? cp0_ui_mm_px(m->ppmm_x100, kPortraitCardMaxMmX10, 1) : 0;
    grid_ = grid_layout(w, h, gap_, cp0_ui_tok(CP0_TOK_ROW_H), kFootCount, kTileCount, min_tile_h, max_tile_w);

    for (int i = 0; i < kTileCount; ++i) {
        tiles_.push_back(make_box(grid_.tiles[static_cast<std::size_t>(i)], false));
        build_tile(tiles_.back(), i);
    }
    for (int i = 0; i < kFootCount; ++i) {
        footer_.push_back(make_box(grid_.footer[static_cast<std::size_t>(i)], true));
        Item &f = footer_.back();
        f.value = text(f.box, label_, kWhite);
        lv_obj_center(f.value);
    }
    shown_footer_.assign(kFootCount, std::vector<std::string>());
    set_focus(std::clamp(focus, 0, kTileCount + kFootCount - 1));
}

OverviewGrid::~OverviewGrid()
{
    if (root_) lv_obj_delete(root_);
}

OverviewGrid::Item OverviewGrid::make_box(const Rect &r, bool footer)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int border = m ? std::max(1, m->shell.tile_border) : 3;
    const int ring = m ? std::max(2, m->shell.tile_border_sel - 2) : 3;
    Item it;
    it.rect = r;
    it.box = lv_obj_create(root_);
    lv_obj_remove_style_all(it.box);
    lv_obj_remove_flag(it.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(it.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(it.box, x0_ + r.x, y0_ + r.y);
    lv_obj_set_size(it.box, r.w, r.h);
    lv_obj_set_style_radius(it.box, cp0_ui_tok(CP0_TOK_RADIUS_M), 0);
    lv_obj_set_style_bg_color(it.box, lv_color_hex(kTileBg), 0);
    lv_obj_set_style_bg_opa(it.box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(it.box, border, 0);
    lv_obj_set_style_border_color(it.box, lv_color_hex(kRgbBorder), 0);
    lv_obj_set_style_bg_color(it.box, lv_color_hex(kTilePressed), LV_STATE_PRESSED);
    // the focus ring: an outline in the gap around the box, so the Power tile's coloured frame stays visible
    lv_obj_set_style_outline_color(it.box, lv_color_hex(kRgbBlue), LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(it.box, ring, LV_STATE_CHECKED);
    lv_obj_set_style_outline_pad(it.box, std::max(1, gap_ / 2 - ring), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(it.box, lv_color_hex(kTileFocusBg), LV_STATE_CHECKED);
    lv_obj_add_event_cb(it.box, click_cb, LV_EVENT_CLICKED, this);
    (void)footer;
    return it;
}

void OverviewGrid::build_tile(Item &it, int id)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    it.title = text(it.box, small_, kRgbLabel);
    lv_obj_set_pos(it.title, pad_, pad_);
    it.hint = text(it.box, small_, kRgbLabel);
    it.detail = text(it.box, small_, kRgbText);
    lv_obj_set_pos(it.detail, pad_, it.rect.h - pad_ - small_h_);
    // the big value: the largest even size the middle has room for in height, and in width for about 13 characters
    // ("Under-voltage", "192.168.100.20"), 48 px at most, the BODY size at least
    const int mid_h = it.rect.h - 2 * pad_ - 2 * small_h_;
    const int body_px = m ? m->text_px[CP0_TEXT_BODY] : 24;
    it.value_px = std::max(body_px, even(std::min({CP0_UI_FONT_MAX_PX, mid_h * 4 / 5, (it.rect.w - 2 * pad_) / 8})));
    it.value_font = medium(it.value_px);
    it.value = text(it.box, it.value_font, kWhite);
    lv_obj_set_pos(it.value, pad_, pad_ + small_h_ + (mid_h - lv_font_get_line_height(it.value_font)) / 2);
    const TileText t = overview_tiles(nullptr)[static_cast<std::size_t>(id)];
    it.spark = t.spark;
    set_text_if(it.title, t.title);
}

/* The history line or the bar beside the value: from the widest value it takes to the tile's right edge, when that
 * leaves at least two value-heights of room. On a portrait screen a tile whose middle has room for three value lines
 * has it under the value instead, across the tile (the value at the top of the middle): the tall cards of 800x1280. */
void OverviewGrid::place_spark(Item &it)
{
    const int mid_h = it.rect.h - 2 * pad_ - 2 * small_h_;
    const int top = pad_ + small_h_;
    const int line = lv_font_get_line_height(it.value_font);
    const bool below = portrait_ && it.spark != Spark::None && mid_h >= 3 * line + gap_;
    if (portrait_) lv_obj_set_y(it.value, below ? top : top + (mid_h - line) / 2); /* else centred, as build_tile put it */
    int x = pad_, w = it.rect.w - 2 * pad_;
    bool room = below;
    if (!below) {
        const int value_w = std::max(text_width(lv_label_get_text(it.value), it.value_font),
                                     text_width(reserve_of(it.spark), it.value_font));
        x = pad_ + value_w + gap_ * 2;
        w = it.rect.w - pad_ - x;
        room = it.spark != Spark::None && w >= 2 * it.value_px;
    }
    const int new_x = room ? x : -1;
    if (new_x == it.spark_x && (it.chart || it.bar || !room)) return;
    it.spark_x = new_x;
    if (it.chart) lv_obj_delete(it.chart);
    if (it.bar) lv_obj_delete(it.bar);
    it.chart = it.bar = nullptr;
    it.series = nullptr;
    it.shown_points = 0;
    it.shown_bar = -1;
    it.shown_spark_rgb = -1;
    if (!room) return;
    // where it goes: beside the value, centred on the middle; or under the value, the rest of the middle
    const int area_y = below ? top + line + gap_ / 2 : top;
    const int area_h = below ? mid_h - line - gap_ : mid_h;
    if (it.spark == Spark::Bar) {
        const int bh = std::max(4, it.value_px / 3);
        it.bar = lv_bar_create(it.box);
        lv_obj_remove_flag(it.bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(it.bar, w, bh);
        lv_obj_set_pos(it.bar, x, area_y + (area_h - bh) / 2);
        lv_bar_set_range(it.bar, 0, 100);
        lv_obj_set_style_radius(it.bar, bh / 2, LV_PART_MAIN);
        lv_obj_set_style_radius(it.bar, bh / 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(it.bar, lv_color_hex(kRgbTrack), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(it.bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(it.bar, lv_color_hex(kRgbValue), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(it.bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_anim_duration(it.bar, 0, 0);
        return;
    }
    // the line beside the value, at most twice its height, centred on it; under the value: all the room there
    const int ch = below ? area_h : std::max(8, std::min(mid_h - pad_ / 2, 2 * line));
    it.chart = lv_chart_create(it.box);
    lv_obj_remove_flag(it.chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(it.chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(it.chart, w, ch);
    lv_obj_set_pos(it.chart, x, area_y + (area_h - ch) / 2);
    lv_obj_set_style_bg_opa(it.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(it.chart, 0, 0);
    lv_obj_set_style_pad_all(it.chart, 2, 0);
    lv_obj_set_style_line_width(it.chart, std::max(2, pad_ / 4), LV_PART_ITEMS);
    lv_obj_set_style_width(it.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(it.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(it.chart, 0, 0);
    lv_chart_set_type(it.chart, LV_CHART_TYPE_LINE);
    // one point every few pixels (each segment is a draw task): the 120 samples binned into a few dozen points
    std::size_t points = std::clamp<std::size_t>(static_cast<std::size_t>(w / std::max(3, pad_ / 2)), 12, kHistory);
    // portrait: a whole number of samples per point, so that the 120 samples reach across the line (91 points of 2
    // samples would leave the oldest third empty)
    if (portrait_) points = kHistory / ((kHistory + points - 1) / points);
    lv_chart_set_point_count(it.chart, static_cast<uint32_t>(points));
    lv_chart_set_div_line_count(it.chart, 0, 0);
    it.data.assign(points, LV_CHART_POINT_NONE);
    it.bins.assign(points, 0.0f);
    it.series = lv_chart_add_series(it.chart, lv_color_hex(kRgbValue), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_y_array(it.chart, it.series, it.data.data());
}

/* One point more since the last time (or a new scale): the line is redrawn; otherwise nothing is touched. */
void OverviewGrid::update_chart(Item &it, const Ring &ring, std::int32_t lo, std::int32_t hi, double scale)
{
    if (!it.chart) return;
    const float last = ring.size() ? ring.last() : -1e30f;
    if (ring.size() == it.shown_points && last == it.shown_last && lo == it.shown_lo && hi == it.shown_hi) return;
    it.shown_points = ring.size();
    it.shown_last = last;
    if (lo != it.shown_lo || hi != it.shown_hi) {
        it.shown_lo = lo;
        it.shown_hi = hi;
        lv_chart_set_axis_range(it.chart, LV_CHART_AXIS_PRIMARY_Y, lo, std::max(hi, lo + 1));
    }
    bin_history(ring, kHistory, it.bins);
    bool changed = false;
    for (std::size_t k = 0; k < it.data.size(); ++k) {
        const float b = it.bins[k];
        const std::int32_t v = std::isnan(b) ? LV_CHART_POINT_NONE : static_cast<std::int32_t>(std::lround(b * scale));
        if (v != it.data[k]) {
            it.data[k] = v;
            changed = true;
        }
    }
    if (changed) lv_chart_refresh(it.chart); /* no pixel moved: nothing redrawn */
}

void OverviewGrid::update_tile(Item &it, const TileText &t, const Snapshot *s)
{
    const int room = it.rect.w - 2 * pad_;
    // the value: the first variant that fits at the tile's size; a smaller size only when none does
    if (t.value != it.shown_value) {
        it.shown_value = t.value;
        const std::string *v = &fit(t.value, it.value_font, room);
        if (text_width(v->c_str(), it.value_font) > room) {
            const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
            const int floor_px = m ? m->text_px[CP0_TEXT_CAPTION] : 18;
            for (int px = it.value_px - 2; px >= floor_px; px -= 2) {
                const lv_font_t *f = medium(px);
                if (text_width(v->c_str(), f) <= room) {
                    lv_obj_set_style_text_font(it.value, f, 0);
                    break;
                }
            }
        } else {
            lv_obj_set_style_text_font(it.value, it.value_font, 0);
        }
        set_text_if(it.value, *v);
        place_spark(it);
    }
    const int vrgb = static_cast<int>(value_rgb(t.value_tone, t.value_dim));
    if (vrgb != it.shown_value_rgb) {
        it.shown_value_rgb = vrgb;
        lv_obj_set_style_text_color(it.value, lv_color_hex(static_cast<std::uint32_t>(vrgb)), 0);
    }
    // the history line in the value's state colour (green while all is well)
    const int srgb = static_cast<int>(t.value_tone == Tone::Warn || t.value_tone == Tone::Error
                                          ? value_rgb(t.value_tone, false)
                                          : kRgbValue);
    if (it.series && srgb != it.shown_spark_rgb) {
        it.shown_spark_rgb = srgb;
        lv_chart_set_series_color(it.chart, it.series, lv_color_hex(static_cast<std::uint32_t>(srgb)));
    }
    if (t.detail != it.shown_detail) {
        it.shown_detail = t.detail;
        set_text_if(it.detail, fit(t.detail, small_, room));
    }
    const int drgb = static_cast<int>(t.detail_tone == Tone::Normal ? kRgbText : value_rgb(t.detail_tone, false));
    if (drgb != it.shown_detail_rgb) {
        it.shown_detail_rgb = drgb;
        lv_obj_set_style_text_color(it.detail, lv_color_hex(static_cast<std::uint32_t>(drgb)), 0);
    }
    if (t.hint != it.shown_hint) { /* at the right of the title, only when it has room */
        it.shown_hint = t.hint;
        const int title_w = text_width(lv_label_get_text(it.title), small_);
        const int hint_w = text_width(t.hint.c_str(), small_);
        const bool show = !t.hint.empty() && title_w + gap_ * 2 + hint_w <= room;
        set_text_if(it.hint, show ? t.hint : std::string());
        lv_obj_set_pos(it.hint, it.rect.w - pad_ - (show ? hint_w : 0), pad_);
    }
    if (t.frame) {
        const int f = static_cast<int>(t.frame_tone);
        if (f != it.shown_frame) {
            it.shown_frame = f;
            const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
            const std::uint32_t rgb = frame_rgb(t.frame_tone);
            lv_obj_set_style_border_color(it.box, lv_color_hex(rgb), 0);
            lv_obj_set_style_border_width(it.box, m ? std::max(2, m->shell.tile_border_sel) : 5, 0);
            const bool alarm = t.frame_tone == Tone::Warn || t.frame_tone == Tone::Error;
            /* a warning or an error also tints the ground (a fifth of the state colour) */
            lv_obj_set_style_bg_color(it.box, alarm ? lv_color_mix(lv_color_hex(rgb), lv_color_hex(kTileBg), LV_OPA_20)
                                                    : lv_color_hex(kTileBg), 0);
            lv_obj_set_style_bg_color(it.box, alarm ? lv_color_mix(lv_color_hex(rgb), lv_color_hex(kTileFocusBg), LV_OPA_20)
                                                    : lv_color_hex(kTileFocusBg), LV_STATE_CHECKED);
        }
    }
    if (!s) return;
    switch (it.spark) {
    case Spark::Cpu: update_chart(it, s->hist.cpu, 0, 1000, 10.0); break;
    case Spark::Mem: update_chart(it, s->hist.mem, 0, 1000, 10.0); break;
    case Spark::Temp: {
        int lo = 20, hi = 90;
        temp_range(s->hist.temp, lo, hi);
        update_chart(it, s->hist.temp, lo * 10, hi * 10, 10.0);
        break;
    }
    case Spark::Rx: {
        const double top = rate_ceiling(s->hist.rx.max());
        update_chart(it, s->hist.rx, 0, static_cast<std::int32_t>(std::min(top, 2.0e9)), 1.0);
        break;
    }
    case Spark::Bar:
        if (it.bar) {
            const int v = static_cast<int>(std::lround(std::clamp(t.bar_pct, 0.0, 100.0)));
            if (v != it.shown_bar) {
                it.shown_bar = v;
                lv_bar_set_value(it.bar, v, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(it.bar, lv_color_hex(t.bar_tone == Tone::Normal ? kRgbValue : value_rgb(t.bar_tone, false)),
                                          LV_PART_INDICATOR);
            }
        }
        break;
    default: break;
    }
}

void OverviewGrid::update(const Snapshot *s, int period_s)
{
    const std::vector<TileText> tiles = overview_tiles(s);
    for (std::size_t i = 0; i < tiles_.size() && i < tiles.size(); ++i) {
        if (tiles[i].spark != tiles_[i].spark) { /* e.g. no thermal zone: no history line */
            tiles_[i].spark = tiles[i].spark;
            tiles_[i].shown_value.clear();
        }
        update_tile(tiles_[i], tiles[i], s);
    }
    const std::vector<std::vector<std::string>> foot = overview_footer(s, period_s);
    for (std::size_t i = 0; i < footer_.size() && i < foot.size(); ++i) {
        if (foot[i] == shown_footer_[i]) continue;
        shown_footer_[i] = foot[i];
        set_text_if(footer_[i].value, fit(foot[i], label_, footer_[i].rect.w - 2 * pad_));
        lv_obj_center(footer_[i].value);
    }
}

std::vector<Rect> OverviewGrid::rects() const
{
    std::vector<Rect> r = grid_.tiles;
    r.insert(r.end(), grid_.footer.begin(), grid_.footer.end());
    return r;
}

void OverviewGrid::set_focus(int index)
{
    const int n = static_cast<int>(tiles_.size() + footer_.size());
    if (index < 0 || index >= n) return;
    const auto box = [this](int i) { return i < static_cast<int>(tiles_.size()) ? tiles_[static_cast<std::size_t>(i)].box
                                                                                 : footer_[static_cast<std::size_t>(i) - tiles_.size()].box; };
    if (focus_ >= 0 && focus_ < n) lv_obj_remove_state(box(focus_), LV_STATE_CHECKED);
    focus_ = index;
    lv_obj_add_state(box(focus_), LV_STATE_CHECKED);
}

void OverviewGrid::on_key(std::uint32_t key)
{
    const int n = static_cast<int>(tiles_.size() + footer_.size());
    int next = -1;
    switch (key) {
    case LV_KEY_LEFT: next = next_focus(rects(), focus_, Dir::Left); break;
    case LV_KEY_RIGHT: next = next_focus(rects(), focus_, Dir::Right); break;
    case LV_KEY_UP: next = next_focus(rects(), focus_, Dir::Up); break;
    case LV_KEY_DOWN: next = next_focus(rects(), focus_, Dir::Down); break;
    case LV_KEY_NEXT: next = (focus_ + 1) % n; break;
    case LV_KEY_PREV: next = (focus_ + n - 1) % n; break;
    case LV_KEY_ENTER:
        if (events_.open) events_.open(focus_);
        return;
    case LV_KEY_ESC:
        if (events_.back) events_.back();
        return;
    default: return;
    }
    if (next >= 0) set_focus(next);
}

void OverviewGrid::key_cb(lv_event_t *event)
{
    auto *self = static_cast<OverviewGrid *>(lv_event_get_user_data(event));
    if (self && lv_event_get_code(event) == LV_EVENT_KEY) self->on_key(lv_event_get_key(event));
}

void OverviewGrid::click_cb(lv_event_t *event)
{
    auto *self = static_cast<OverviewGrid *>(lv_event_get_user_data(event));
    if (!self) return;
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    int index = -1;
    for (std::size_t i = 0; i < self->tiles_.size(); ++i)
        if (self->tiles_[i].box == target) index = static_cast<int>(i);
    for (std::size_t i = 0; i < self->footer_.size(); ++i)
        if (self->footer_[i].box == target) index = static_cast<int>(self->tiles_.size() + i);
    if (index < 0) return;
    self->set_focus(index);
    if (self->events_.open) self->events_.open(index); /* one tap opens: a tile is a large target */
}

} // namespace sm
