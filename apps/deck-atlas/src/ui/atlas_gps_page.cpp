/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the GPS status page (see core/gps_page.hpp for what it says). Opened from the map (the chip's tap, the S key) or
 * from Settings > GPS receiver > GPS status; Esc goes back to where it came from. One scrolling page of cards: the receiver
 * (the dish chip beside the state in words, the port, the speed, the rate, the last data, bad checksums), the position, the
 * DOPs, and the satellites (counts and legend, a sky plot, the signal bars). The sky plot and the bars are drawn with LVGL
 * primitives in draw events (like the chip); the page redraws a part only when its data changed (GpsPageLive), about every
 * 250 ms at most. GPS Off: one sentence, nothing else, no port.
 *
 * Reads only: the status the reader already keeps (gps_status_); the page never talks to the port.
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>

namespace atlasui {

namespace {

constexpr std::uint32_t kGrid = 0x3A3A3A;       /* gridlines, the rings */
constexpr std::uint32_t kGridFaint = 0x2A2A2A;
constexpr std::uint32_t kDotInk = 0x101010;     /* the letter on a filled dot, the fill of a hollow one */
constexpr atlas::Constellation kSystems[6] = {atlas::Constellation::Gps,    atlas::Constellation::Glonass,
                                              atlas::Constellation::Galileo, atlas::Constellation::Beidou,
                                              atlas::Constellation::Qzss,    atlas::Constellation::Sbas};

lv_color_t rgb(std::uint32_t c) { return lv_color_hex(c); }

int text_w(const lv_font_t *font, const std::string &s)
{
    if (s.empty()) return 0;
    lv_point_t p{0, 0};
    lv_text_get_size(&p, s.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<int>(p.x);
}

void text_at(lv_layer_t *layer, const lv_font_t *font, const std::string &s, int x, int y, std::uint32_t c)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s.c_str();
    d.text_local = 1;
    d.font = font;
    d.color = rgb(c);
    const lv_area_t a = {x, y, x + text_w(font, s) + 1, y + lv_font_get_line_height(font) - 1};
    lv_draw_label(layer, &d, &a);
}

void box(lv_layer_t *layer, const lv_area_t &a, std::uint32_t fill, lv_opa_t fill_opa, std::uint32_t border, lv_opa_t border_opa,
         int border_w, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = rgb(fill);
    d.bg_opa = fill_opa;
    d.border_color = rgb(border);
    d.border_opa = border_opa;
    d.border_width = border_w;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void hline(lv_layer_t *layer, int x0, int x1, int y, std::uint32_t c)
{
    box(layer, lv_area_t{x0, y, x1, y}, c, LV_OPA_COVER, c, LV_OPA_TRANSP, 0, 0);
}

void stroke(lv_layer_t *layer, double x0, double y0, double x1, double y1, std::uint32_t c, int width)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = static_cast<lv_value_precise_t>(std::lround(x0));
    d.p1.y = static_cast<lv_value_precise_t>(std::lround(y0));
    d.p2.x = static_cast<lv_value_precise_t>(std::lround(x1));
    d.p2.y = static_cast<lv_value_precise_t>(std::lround(y1));
    d.color = rgb(c);
    d.opa = LV_OPA_COVER;
    d.width = width;
    lv_draw_line(layer, &d);
}

lv_area_t square_at(double cx, double cy, int r)
{
    const int x = static_cast<int>(std::lround(cx)), y = static_cast<int>(std::lround(cy));
    return lv_area_t{x - r, y - r, x + r, y + r};
}

const char *system_name_short(atlas::Constellation c)
{
    switch (c) {
    case atlas::Constellation::Gps: return "GPS";
    case atlas::Constellation::Glonass: return "GLONASS";
    case atlas::Constellation::Galileo: return "Galileo";
    case atlas::Constellation::Beidou: return "BeiDou";
    case atlas::Constellation::Qzss: return "QZSS";
    case atlas::Constellation::Sbas: return "SBAS";
    default: return "?";
    }
}

} // namespace

/* ------------------------------------------------------------------------------------------------ opening */

void AtlasApp::open_gps_status(atlas::StatusOrigin from)
{
    if (dialog_) return;
    if (!status_nav_.open_from(from, settings_.gps)) return;
    if (from == atlas::StatusOrigin::Settings && list_) settings_sel_ = list_->selected();
    if (tab_ == Tab::Map && view_dirty_) {
        save_settings();
        view_dirty_ = false;
    }
    clear_page_objects();
    sub_ = Sub::GpsStatus;
    show_tabs(false);
    if (from == atlas::StatusOrigin::Map && map_box_) lv_obj_add_flag(map_box_, LV_OBJ_FLAG_HIDDEN);
    set_title();
    build_gps_page();
    gps_live_.reset();
    update_gps_page(true);
    log_line("gps: status page opened from %s (%s)", from == atlas::StatusOrigin::Map ? "the map" : "Settings",
             gp_text_.headline.c_str());
}

/* ------------------------------------------------------------------------------------------------ building */

lv_obj_t *AtlasApp::gp_card(lv_obj_t *parent, const char *title, int width)
{
    lv_obj_t *c = lv_obj_create(parent);
    plain(c);
    lv_obj_set_width(c, width);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, rgb(pal::kPanel), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_pad_all(c, space_s_, 0);
    lv_obj_set_style_pad_row(c, std::max(space_s_ / 2, 2), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    if (title && *title) {
        lv_obj_t *t = make_label(c, caption_px_, pal::kMuted, false);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_text(t, title);
    }
    return c;
}

/* name at the left (caption, grey), value at the right (body, white; wraps when it is long) */
void AtlasApp::gp_row(lv_obj_t *card, const char *name, std::string atlas::GpsPageText::*field)
{
    lv_obj_t *row = lv_obj_create(card);
    plain(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, space_s_, 0);
    lv_obj_t *n = make_label(row, caption_px_, pal::kMuted, false);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(n, LV_SIZE_CONTENT);
    lv_label_set_text(n, name);
    lv_obj_t *v = make_label(row, body_px_, pal::kText, true);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    gp_.fields.emplace_back(v, field);
}

/* a small stacked cell: name over value (the DOP card) */
void AtlasApp::gp_cell(lv_obj_t *parent, int width, const char *name, std::string atlas::GpsPageText::*field)
{
    lv_obj_t *c = lv_obj_create(parent);
    plain(c);
    lv_obj_set_size(c, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *n = make_label(c, caption_px_, pal::kMuted, false);
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_text(n, name);
    lv_obj_t *v = make_label(c, body_px_, pal::kText, false);
    lv_obj_set_width(v, lv_pct(100));
    gp_.fields.emplace_back(v, field);
}

void AtlasApp::build_gps_page()
{
    gp_ = GpsPageUi();
    page_box_ = lv_obj_create(page_->root());
    plain(page_box_);
    lv_obj_add_flag(page_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page_box_, LV_OBJ_FLAG_CLICKABLE); /* a touch on a card drags the page */
    lv_obj_set_scroll_dir(page_box_, LV_DIR_VER);
    lv_obj_set_pos(page_box_, 0, body_y_);
    lv_obj_set_size(page_box_, w_, h_ - body_y_);
    lv_obj_set_flex_flow(page_box_, LV_FLEX_FLOW_COLUMN);
    const int padh = std::max(space_m_, layout_.roller_x);
    lv_obj_set_style_pad_hor(page_box_, padh, 0);
    lv_obj_set_style_pad_ver(page_box_, space_m_, 0);
    lv_obj_set_style_pad_row(page_box_, space_s_, 0);
    lv_obj_add_event_cb(
        page_box_,
        [](lv_event_t *e) {
            auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
            lv_obj_t *box = self->page_box_;
            const uint32_t k = lv_event_get_key(e);
            const int step = std::max(self->row_ * 2, 40);
            if (k == LV_KEY_ESC) self->post([self] { self->back(); });
            else if (k == LV_KEY_DOWN)
                lv_obj_scroll_by(box, 0, -std::min<int>(step, std::max<int>(lv_obj_get_scroll_bottom(box), 0)), LV_ANIM_OFF);
            else if (k == LV_KEY_UP)
                lv_obj_scroll_by(box, 0, std::min<int>(step, std::max<int>(lv_obj_get_scroll_top(box), 0)), LV_ANIM_OFF);
            else if (k == LV_KEY_HOME) lv_obj_scroll_to_y(box, 0, LV_ANIM_OFF);
            else if (k == LV_KEY_END) lv_obj_scroll_to_y(box, LV_COORD_MAX, LV_ANIM_OFF);
        },
        LV_EVENT_KEY, this);

    const int cw = w_ - 2 * padh;               /* the width of the cards' column */
    const bool wide = cw >= 520;                /* 640x480: Receiver and Position side by side, the sky beside the bars */
    const lv_font_t *cap = cp0_ui_font_px(caption_px_);
    gp_.lh = lv_font_get_line_height(cap);
    gp_.label_w = text_w(cap, "000") + 2;
    gp_.axis_w = text_w(cap, "50") + 6;
    gp_.built_off = !settings_.gps;

    /* ---- the receiver: the dish chip beside the state in words */
    const int half = (cw - space_s_) / 2;
    lv_obj_t *top = page_box_;
    if (wide && !gp_.built_off) {
        top = lv_obj_create(page_box_);
        plain(top);
        lv_obj_set_size(top, cw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(top, space_s_, 0);
    }
    const int card_w = wide && !gp_.built_off ? half : cw;
    lv_obj_t *rc = gp_card(top, gp_.built_off ? "GPS" : "RECEIVER", card_w);
    lv_obj_t *head = lv_obj_create(rc);
    plain(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, space_s_, 0);
    if (!gp_.built_off) {
        const int ch = body_px_ * 5 / 2, cwid = ch * 9 / 4;   /* the map chip's proportions, a little larger than there */
        gp_.chip = lv_obj_create(head);
        plain(gp_.chip);
        lv_obj_set_size(gp_.chip, cwid, ch);
        lv_obj_add_event_cb(gp_.chip, gp_chip_cb, LV_EVENT_DRAW_MAIN, this);
    }
    lv_obj_t *col = lv_obj_create(head);
    plain(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_min_width(col, std::max(10 * body_px_, 120), 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    gp_.headline = make_label(col, body_px_, pal::kText, true);
    lv_obj_set_width(gp_.headline, lv_pct(100));
    gp_.detail = make_label(col, caption_px_, pal::kMuted, true);
    lv_obj_set_width(gp_.detail, lv_pct(100));

    if (!gp_.built_off) {
        gp_row(rc, "Device", &atlas::GpsPageText::device);
        gp_row(rc, "Port", &atlas::GpsPageText::port);
        gp_row(rc, "Speed", &atlas::GpsPageText::speed);
        gp_row(rc, "Sentences", &atlas::GpsPageText::rate);
        gp_row(rc, "Last data", &atlas::GpsPageText::last_data);
        gp_row(rc, "Bad checksums", &atlas::GpsPageText::bad_checksums);
        gp_row(rc, "Chosen", &atlas::GpsPageText::chosen);

        /* ---- the position */
        lv_obj_t *pc = gp_card(top, "POSITION", card_w);
        gp_row(pc, "Latitude", &atlas::GpsPageText::lat);
        gp_row(pc, "Longitude", &atlas::GpsPageText::lon);
        gp_row(pc, "Altitude (MSL)", &atlas::GpsPageText::altitude);
        gp_row(pc, "Accuracy", &atlas::GpsPageText::accuracy);
        gp_row(pc, "Speed", &atlas::GpsPageText::speed_over_ground);
        gp_row(pc, "Course", &atlas::GpsPageText::course);
        gp_row(pc, "Date (UTC)", &atlas::GpsPageText::utc_date);
        gp_row(pc, "Time (UTC)", &atlas::GpsPageText::utc_time);

        /* ---- DOP */
        lv_obj_t *dc = gp_card(page_box_, "DILUTION OF PRECISION", cw);
        lv_obj_t *cells = lv_obj_create(dc);
        plain(cells);
        lv_obj_set_size(cells, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cells, LV_FLEX_FLOW_ROW_WRAP);
        const int cell_w = (cw - 2 * space_s_) / 5;
        gp_cell(cells, cell_w, "HDOP", &atlas::GpsPageText::hdop);
        gp_cell(cells, cell_w, "VDOP", &atlas::GpsPageText::vdop);
        gp_cell(cells, cell_w, "PDOP", &atlas::GpsPageText::pdop);
        gp_cell(cells, cell_w, "Fix", &atlas::GpsPageText::mode);
        gp_cell(cells, cell_w, "Used", &atlas::GpsPageText::used);

        /* ---- the satellites */
        lv_obj_t *sc = gp_card(page_box_, "SATELLITES", cw);
        const int inner = cw - 2 * space_s_;
        lv_obj_t *counts = lv_obj_create(sc);
        plain(counts);
        lv_obj_set_size(counts, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(counts, LV_FLEX_FLOW_ROW_WRAP);
        gp_cell(counts, inner / 2, "In view", &atlas::GpsPageText::in_view);
        gp_cell(counts, inner / 2, "In the fix", &atlas::GpsPageText::in_fix);
        lv_obj_t *legend = lv_obj_create(sc);
        plain(legend);
        lv_obj_set_size(legend, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(legend, space_m_, 0);
        lv_obj_set_style_pad_row(legend, 2, 0);
        for (int i = 0; i < 6; ++i) {
            gp_.legend[i] = make_label(legend, caption_px_, atlas::constellation_rgb(kSystems[i]), false);
            lv_label_set_long_mode(gp_.legend[i], LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_width(gp_.legend[i], LV_SIZE_CONTENT);
        }

        const int gap = space_s_;
        const int side = wide ? std::min(inner * 2 / 5, 300) : std::min(inner, 340);
        const int bars_w = wide ? inner - side - gap : inner;
        gp_.bars_w = bars_w;
        lv_obj_t *body = lv_obj_create(sc);
        plain(body);
        lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(body, gap, 0);
        lv_obj_set_style_pad_row(body, space_s_, 0);

        lv_obj_t *skyc = lv_obj_create(body);
        plain(skyc);
        lv_obj_set_size(skyc, side, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(skyc, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *st = make_label(skyc, caption_px_, pal::kMuted, false);
        lv_label_set_text(st, "SKY (30 and 60 degree rings, N up)");
        lv_label_set_long_mode(st, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(st, lv_pct(100));
        gp_.sky = lv_obj_create(skyc);
        plain(gp_.sky);
        lv_obj_set_size(gp_.sky, side, side);
        lv_obj_add_event_cb(gp_.sky, gp_sky_cb, LV_EVENT_DRAW_MAIN, this);

        lv_obj_t *barc = lv_obj_create(body);
        plain(barc);
        lv_obj_set_size(barc, bars_w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(barc, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *bt = make_label(barc, caption_px_, pal::kMuted, false);
        lv_label_set_text(bt, "SIGNAL (dB-Hz)");
        lv_obj_set_width(bt, lv_pct(100));
        gp_.bars = lv_obj_create(barc);
        plain(gp_.bars);
        lv_obj_set_width(gp_.bars, bars_w);
        lv_obj_add_event_cb(gp_.bars, gp_bars_cb, LV_EVENT_DRAW_MAIN, this);
        gp_.plot_h = std::clamp(h_ / 7, 48, 84);
        gp_.row_h = gp_.plot_h + 2 * gp_.lh + gp_.lh / 2 + 6;
        lv_obj_set_height(gp_.bars, gp_.row_h);

        lv_obj_t *key = make_label(sc, caption_px_, pal::kMuted, true);
        lv_obj_set_width(key, lv_pct(100));
        lv_label_set_text(key, "Filled: used in the fix. Ring or outline: in view only. Short stub: not tracked yet. "
                               "The letter is the system.");
    }

    lv_obj_t *foot = make_label(page_box_, caption_px_, pal::kMuted, true);
    lv_obj_set_width(foot, lv_pct(100));
    lv_label_set_text(foot, "The app never transmits to the receiver. Your position never leaves this device.");
    focus(page_box_);
}

/* ------------------------------------------------------------------------------------------------ live update */

void AtlasApp::apply_gps_text(const atlas::GpsPageText &t)
{
    auto set = [](lv_obj_t *l, const std::string &s) {
        if (l && s != lv_label_get_text(l)) lv_label_set_text(l, s.c_str());
    };
    set(gp_.headline, t.headline);
    if (gp_.detail) {
        set(gp_.detail, t.detail);
        if (t.detail.empty()) lv_obj_add_flag(gp_.detail, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(gp_.detail, LV_OBJ_FLAG_HIDDEN);
    }
    for (const auto &f : gp_.fields) set(f.first, t.*(f.second));
    if (gp_.chip) lv_obj_invalidate(gp_.chip);
    for (int i = 0; i < 6 && gp_.legend[i]; ++i) {
        int n = 0;
        for (const atlas::SkySat &s : gp_sats_) n += s.sys == kSystems[i];
        const std::string text = std::string(1, atlas::constellation_letter(kSystems[i])) + " " +
                                 system_name_short(kSystems[i]) + " " + std::to_string(n);
        set(gp_.legend[i], text);
        lv_obj_set_style_text_color(gp_.legend[i], rgb(n ? atlas::constellation_rgb(kSystems[i]) : pal::kMuted), 0);
    }
}

void AtlasApp::layout_gps_bars()
{
    if (!gp_.bars) return;
    gp_.layout = atlas::bar_layout(static_cast<int>(gp_sats_.size()), gp_.bars_w, gp_.axis_w, gp_.label_w);
    const int rows = std::max(1, gp_.layout.rows);
    const int h = rows * gp_.row_h;
    if (lv_obj_get_height(gp_.bars) != h) lv_obj_set_height(gp_.bars, h);
}

void AtlasApp::update_gps_page(bool force)
{
    if (sub_ != Sub::GpsStatus) return;
    const std::int64_t now = mono_ms();
    if (!force && now - gp_at_ < 250) return;
    gp_at_ = now;
    const atlas::GpsPageText t = atlas::gps_page_text(gps_status_, settings_.units == atlas::Units::Mi, now);
    if (!force && t.off != gp_.built_off) { /* GPS was switched while the page was open: build the other page */
        if (page_box_) lv_obj_delete(page_box_);
        page_box_ = nullptr;
        build_gps_page();
        gps_live_.reset();
    }
    const atlas::GpsPageChange ch = gps_live_.update(t, gps_status_.sats);
    if (!ch.any()) return;
    gp_text_ = t;
    if (ch.sky || ch.bars) gp_sats_ = gps_status_.sats;
    if (ch.bars) layout_gps_bars();
    if (ch.text) apply_gps_text(t);
    if (ch.sky && gp_.sky) lv_obj_invalidate(gp_.sky);
    if (ch.bars && gp_.bars) lv_obj_invalidate(gp_.bars);
}

/* ------------------------------------------------------------------------------------------------ drawing */

void AtlasApp::gp_chip_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    lv_area_t a;
    lv_obj_get_coords(self->gp_.chip, &a);
    if (self->gp_text_.chip.level == atlas::ChipLevel::Hidden) return;
    draw_chip(lv_event_get_layer(e), a, self->gp_text_.chip, false, cp0_ui_metrics_get()->tok[CP0_TOK_RADIUS_S],
              atlas::map_metrics(self->overlay_input()).pad);
}

void AtlasApp::gp_sky_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    lv_area_t a;
    lv_obj_get_coords(self->gp_.sky, &a);
    self->draw_gps_sky(lv_event_get_layer(e), a);
}

void AtlasApp::gp_bars_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    lv_area_t a;
    lv_obj_get_coords(self->gp_.bars, &a);
    self->draw_gps_bars(lv_event_get_layer(e), a);
}

/* The sky: the horizon, the 30 and 60 degree rings, the N-S and E-W lines and the four letters; a dot per satellite (filled =
 * used in the fix, a ring = in view only) in its system's colour with the system's letter inside, the PRN beside the dot
 * where there is room. */
void AtlasApp::draw_gps_sky(lv_layer_t *layer, const lv_area_t &o)
{
    const lv_font_t *cap = cp0_ui_font_px(caption_px_);
    const int lh = lv_font_get_line_height(cap);
    const int w = o.x2 - o.x1 + 1, h = o.y2 - o.y1 + 1;
    atlas::SkyGeom g = atlas::sky_geom(w, h, lh + 3);
    for (int e : {0, 30, 60}) {
        const int r = static_cast<int>(std::lround(atlas::sky_ring_radius(g, e)));
        if (r < 1) continue;
        box(layer, square_at(o.x1 + g.cx, o.y1 + g.cy, r), 0, LV_OPA_TRANSP, e == 0 ? 0x6A6A6A : kGrid, LV_OPA_COVER, e == 0 ? 2 : 1,
            LV_RADIUS_CIRCLE);
    }
    const double R = g.r;
    stroke(layer, o.x1 + g.cx, o.y1 + g.cy - R, o.x1 + g.cx, o.y1 + g.cy + R, kGridFaint, 1);
    stroke(layer, o.x1 + g.cx - R, o.y1 + g.cy, o.x1 + g.cx + R, o.y1 + g.cy, kGridFaint, 1);
    const struct {
        const char *s;
        double dx, dy;
    } cards[4] = {{"N", 0, -1}, {"E", 1, 0}, {"S", 0, 1}, {"W", -1, 0}};
    for (const auto &c : cards) {
        const int tw = text_w(cap, c.s);
        const double cx = o.x1 + g.cx + c.dx * (R + lh / 2.0 + 2), cy = o.y1 + g.cy + c.dy * (R + lh / 2.0 + 2);
        text_at(layer, cap, c.s, static_cast<int>(std::lround(cx)) - tw / 2, static_cast<int>(std::lround(cy)) - lh / 2,
                c.s[0] == 'N' ? pal::kText : pal::kMuted);
    }
    const int dr = std::max(8, caption_px_ * 2 / 3);
    const int label_w = text_w(cap, "000");
    const std::vector<bool> room = atlas::sky_labels_fit(gp_sats_, g, dr, label_w, lh, w, h);
    for (int pass = 0; pass < 2; ++pass) { /* the dots in view first, the used ones over them */
        for (std::size_t i = 0; i < gp_sats_.size(); ++i) {
            const atlas::SkySat &s = gp_sats_[i];
            if (!atlas::sky_placeable(s) || s.used != (pass == 1)) continue;
            const atlas::SkyPoint p = atlas::sky_point(g, s.elev, s.azim);
            const double x = o.x1 + p.x, y = o.y1 + p.y;
            const std::uint32_t c = atlas::constellation_rgb(s.sys);
            const std::string letter(1, atlas::constellation_letter(s.sys));
            if (s.used) box(layer, square_at(x, y, dr), c, LV_OPA_COVER, c, LV_OPA_COVER, 0, LV_RADIUS_CIRCLE);
            else box(layer, square_at(x, y, dr), kDotInk, LV_OPA_COVER, c, LV_OPA_COVER, 2, LV_RADIUS_CIRCLE);
            text_at(layer, cap, letter, static_cast<int>(std::lround(x)) - text_w(cap, letter) / 2,
                    static_cast<int>(std::lround(y)) - lh / 2, s.used ? kDotInk : c);
        }
    }
    for (std::size_t i = 0; i < gp_sats_.size(); ++i) {
        if (!room[i]) continue;
        const atlas::SkyPoint p = atlas::sky_point(g, gp_sats_[i].elev, gp_sats_[i].azim);
        text_at(layer, cap, std::to_string(gp_sats_[i].prn), static_cast<int>(std::lround(o.x1 + p.x)) + dr + 1,
                static_cast<int>(std::lround(o.y1 + p.y)) - lh / 2, pal::kText);
    }
}

/* The signal bars: one bar per satellite in view (constellation, then PRN), height = SNR on a 0-50 dB-Hz scale (lines at 25 and
 * 50), solid when used in the fix, outlined when only in view, a short stub when not tracked; the system's letter and the PRN
 * under it. More satellites than fit in a row wrap into further rows, each with its own scale. */
void AtlasApp::draw_gps_bars(lv_layer_t *layer, const lv_area_t &o)
{
    const lv_font_t *cap = cp0_ui_font_px(caption_px_);
    const int lh = gp_.lh;
    if (gp_sats_.empty()) {
        const std::string s = gps_status_.receiver_alive ? "No satellites in view" : "No satellite data";
        text_at(layer, cap, s, o.x1 + gp_.axis_w, o.y1 + gp_.row_h / 2 - lh / 2, pal::kMuted);
        return;
    }
    const atlas::BarLayout &L = gp_.layout;
    for (int r = 0; r < L.rows; ++r) {
        const int base = o.y1 + r * gp_.row_h + lh / 2 + gp_.plot_h;
        const int right = o.x1 + L.axis_w + L.per_row * L.cell_w - 1;
        for (int v : {25, 50}) {
            const int y = base - atlas::bar_height(v, gp_.plot_h);
            hline(layer, o.x1 + L.axis_w - 3, right, y, kGridFaint);
            const std::string s = std::to_string(v);
            text_at(layer, cap, s, o.x1 + L.axis_w - 5 - text_w(cap, s), y - lh / 2, pal::kMuted);
        }
        hline(layer, o.x1 + L.axis_w - 3, right, base, 0x777777);
    }
    for (std::size_t i = 0; i < gp_sats_.size() && i < L.cells.size(); ++i) {
        const atlas::SkySat &s = gp_sats_[i];
        const atlas::BarCell &c = L.cells[i];
        const int base = o.y1 + c.row * gp_.row_h + lh / 2 + gp_.plot_h;
        const std::uint32_t col = atlas::constellation_rgb(s.sys);
        const int bh = atlas::bar_height(s.snr, gp_.plot_h);
        const int x0 = o.x1 + c.bar_x, x1 = x0 + c.bar_w - 1;
        if (bh <= 0) { /* in view, not tracked: a short outlined stub */
            box(layer, lv_area_t{x0, base - 4, x1, base - 1}, col, LV_OPA_TRANSP, col, LV_OPA_COVER, 1, 0);
        } else if (s.used) {
            box(layer, lv_area_t{x0, base - bh, x1, base - 1}, col, LV_OPA_COVER, col, LV_OPA_COVER, 0, 0);
        } else {
            box(layer, lv_area_t{x0, base - bh, x1, base - 1}, col, LV_OPA_30, col, LV_OPA_COVER, 2, 0);
        }
        const std::string letter(1, atlas::constellation_letter(s.sys)), prn = std::to_string(s.prn);
        const int cx = o.x1 + c.x + c.w / 2;
        text_at(layer, cap, letter, cx - text_w(cap, letter) / 2, base + 3, col);
        text_at(layer, cap, prn, cx - text_w(cap, prn) / 2, base + 3 + lh, s.used ? pal::kText : pal::kMuted);
    }
}

} // namespace atlasui
