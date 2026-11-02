/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app: a view of the owner's stored dashboard (study 053 P2), drawn natively in the dashboard page's
 * scrolling area (see ha_app.hpp). LVGL thread only.
 *
 *   Layout    masonry as Home Assistant's: columns = clamp(floor(width_mm / 50), 1, 4) (a panel view: one), each card
 *             in order into the column that is the shortest so far (core/dash.hpp); the card as wide as its column;
 *             paddings and gaps from the layout service's space tokens; the page scrolls vertically.
 *   Cards     surface #1c1c1c (Home Assistant's dark card colour), small radius, no shadow, gradient or animation;
 *             titles in the toolkit's label colour. entities: an optional header (the title, and a switch for the
 *             header toggle), then 7 mm rows: the icon (its state colour), the name cut with dots, the state word at
 *             the right or a switch (light, switch, fan, input_boolean); sections in the lists' grey capitals,
 *             dividers as a hairline, other row types greyed with their name. sensor: the name and the icon, the
 *             value large with its unit, a line of its history (lv_chart, at most 100 points). weather-forecast: the
 *             condition's icon, the temperature, the condition, up to five days. Any other card: its title greyed and
 *             "Not supported on this screen".
 *   Touch     a row's icon or switch does its action (toggle; run; lock / unlock and garage-type covers after the
 *             question of 0.2.0), its name follows tap_action (the entity page by default); the header's switch
 *             turns the card's lights, switches, fans and input booleans all on or all off; a sensor card opens its
 *             entity page; a drag scrolls, a sideways swipe switches views.
 *   Keys      Up / Down from stop to stop (header toggles, entity rows, sensor cards), Left / Right to the previous /
 *             next card, Home / End; Enter acts, Ctrl+O opens the entity page; Up from the first stop to the tab bar.
 *   Refresh   only what changed is redrawn (a row's word, colour or switch; a chart's points); the cards are built
 *             again only when the view's shape changes.
 */
#include "ha_app.hpp"
#include "ha_ui.hpp"

#include "dash.hpp"
#include "mdi.hpp"

#include "cp0_font_service.hpp"

#include <algorithm>
#include <cstdio>

namespace haui {

namespace {

/* the shape of a view: what makes the cards' heights (a word or a colour does not) */
std::string signature(const std::vector<ha::DashCardView> &cards, bool panel)
{
    std::string s = panel ? "P" : "M";
    for (const ha::DashCardView &c : cards) {
        s += "|" + std::to_string(static_cast<int>(c.kind)) + (c.title.empty() ? "-" : "t") + (c.header_toggle ? "h" : "");
        for (const ha::DashRowView &r : c.rows) s += std::to_string(static_cast<int>(r.kind)) + (r.toggle ? "s" : "");
        s += "/" + std::to_string(c.spark.size()) + "/" + std::to_string(c.days.size()) +
             (c.condition.empty() && c.value.empty() ? "" : "c");
    }
    return s;
}

/* "history-graph" -> "History graph"; a custom card's type as written */
std::string type_title(const std::string &type)
{
    if (type.rfind("custom:", 0) == 0) return type;
    std::string s = type;
    for (char &c : s)
        if (c == '-' || c == '_') c = ' ';
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

std::string upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::intptr_t zone_code(int card, int row, int zone)
{
    return (static_cast<std::intptr_t>(card) * 65536 + (row + 2)) * 8 + zone;
}

} // namespace

bool HaApp::on_dash_tab() const
{
    const ha::Tab *t = current_tab();
    return screen_kind_ == Screen::Dashboard && t && t->kind == ha::Tab::Kind::View;
}

void HaApp::clear_dash()
{
    dcards_.clear();
    dash_rows_.clear();
    dstops_.clear();
    dstop_drawn_ = -1;
    dash_sig_.clear();
}

void HaApp::refresh_dash_view()
{
    if (!grid_) return;
    const ha::Tab *t = current_tab();
    if (!t) return;
    std::string note;
    if (view_.dash_view != t->view) note = view_.dash ? "Loading..." : "Not loaded yet";
    else if (view_.dash_cards.empty()) note = "Nothing in this view";
    if (!note.empty()) {
        if (!dcards_.empty()) {
            lv_obj_clean(grid_);
            grid_note_ = nullptr;
            clear_dash();
        }
        if (!grid_note_) {
            grid_note_ = plain_label(grid_, word_font_, kMuted);
            lv_obj_set_pos(grid_note_, margin_, margin_);
            lv_obj_set_width(grid_note_, layout_.w - 2 * margin_);
            lv_obj_remove_flag(grid_note_, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_label_set_text(grid_note_, note.c_str());
        return;
    }
    if (grid_note_ || signature(view_.dash_cards, view_.dash_panel) != dash_sig_ ||
        dcards_.size() != view_.dash_cards.size()) {
        build_dash_view();
        return;
    }
    for (std::size_t i = 0; i < dcards_.size(); ++i) {
        DashCardUi &c = dcards_[i];
        const ha::DashCardView &want = view_.dash_cards[i];
        draw_dash_card(c, want, false);
        for (int k = 0; k < c.rows && k < static_cast<int>(want.rows.size()); ++k)
            draw_dash_row(dash_rows_[static_cast<std::size_t>(c.first_row + k)], want.rows[static_cast<std::size_t>(k)],
                          false);
    }
}

void HaApp::build_dash_view()
{
    const int keep_y = lv_obj_get_scroll_y(grid_);
    lv_obj_clean(grid_);
    grid_note_ = nullptr;
    clear_dash();
    const std::vector<ha::DashCardView> &cards = view_.dash_cards;
    const int cols = view_.dash_panel ? 1 : ha::masonry_columns(layout_.w, cp0_ui_metrics_get()->ppmm_x100);
    const int col_w = (layout_.w - 2 * margin_ - (cols - 1) * gap_) / cols;
    const int radius = m_radius();
    dcards_.resize(cards.size());
    std::vector<int> heights;
    for (std::size_t i = 0; i < cards.size(); ++i) {
        DashCardUi &c = dcards_[i];
        const ha::DashCardView &v = cards[i];
        c.w = col_w;
        c.box = box(grid_);
        lv_obj_set_size(c.box, col_w, row_h_);
        lv_obj_set_style_radius(c.box, radius, 0);
        lv_obj_set_style_bg_opa(c.box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c.box, lv_color_hex(ha::rgb::kTile), 0);
        lv_obj_set_style_clip_corner(c.box, false, 0);
        int h = 0;
        switch (v.kind) {
        case ha::DashCard::Kind::Entities: h = build_entities_card(c, v, static_cast<int>(i)); break;
        case ha::DashCard::Kind::Sensor: h = build_sensor_card(c, v, static_cast<int>(i)); break;
        case ha::DashCard::Kind::Weather: h = build_weather_card(c, v); break;
        case ha::DashCard::Kind::Unsupported: h = build_unsupported_card(c, v); break;
        }
        c.h = h;
        lv_obj_set_height(c.box, h);
        heights.push_back(h);
    }
    std::vector<int> ys;
    const std::vector<int> col_of = ha::masonry_place(heights, cols, gap_, &ys);
    for (std::size_t i = 0; i < dcards_.size(); ++i) {
        DashCardUi &c = dcards_[i];
        c.col = col_of[i];
        c.x = margin_ + c.col * (col_w + gap_);
        c.y = gap_ + ys[i];
        lv_obj_set_pos(c.box, c.x, c.y);
    }
    /* the keyboard's stops, in the cards' order */
    for (std::size_t i = 0; i < dcards_.size(); ++i) {
        DashCardUi &c = dcards_[i];
        const ha::DashCardView &v = cards[i];
        if (c.hdr) dstops_.push_back(DashStop{static_cast<int>(i), -1, c.hdr});
        for (int k = 0; k < c.rows; ++k) {
            const DashRowUi &r = dash_rows_[static_cast<std::size_t>(c.first_row + k)];
            if (v.rows[static_cast<std::size_t>(k)].kind == ha::DashRow::Kind::Entity)
                dstops_.push_back(DashStop{static_cast<int>(i), c.first_row + k, r.box});
        }
        if (v.kind == ha::DashCard::Kind::Sensor) dstops_.push_back(DashStop{static_cast<int>(i), -2, c.box});
    }
    dash_sig_ = signature(cards, view_.dash_panel);
    lv_obj_update_layout(grid_); /* the labels' real widths before their texts (dots are decided then) */
    for (std::size_t i = 0; i < dcards_.size(); ++i) {
        DashCardUi &c = dcards_[i];
        draw_dash_card(c, cards[i], true);
        for (int k = 0; k < c.rows; ++k)
            draw_dash_row(dash_rows_[static_cast<std::size_t>(c.first_row + k)], cards[i].rows[static_cast<std::size_t>(k)],
                          true);
    }
    if (keep_y > 0) lv_obj_scroll_to_y(grid_, keep_y, LV_ANIM_OFF);
    dstop_ = std::clamp(dstop_, 0, std::max(0, static_cast<int>(dstops_.size()) - 1));
    style_dash_focus();
}

/* A touch zone: clickable, its card, row and zone in its user data. */
lv_obj_t *HaApp::dash_zone(lv_obj_t *parent, int x, int y, int w, int h, int card, int row, DashZone zone)
{
    lv_obj_t *z = box(parent);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(z, x, y);
    lv_obj_set_size(z, std::max(1, w), h);
    lv_obj_set_user_data(z, reinterpret_cast<void *>(zone_code(card, row, static_cast<int>(zone))));
    lv_obj_add_event_cb(z, dash_event_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(z, dash_event_cb, LV_EVENT_CLICKED, this);
    return z;
}

/* A switch that only shows the state Home Assistant reports (no optimistic flip, no animation): not clickable itself,
 * the zone around it acts. */
lv_obj_t *HaApp::dash_switch(lv_obj_t *parent, int zone_x, int zone_w, int row_h)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_size(sw, sw_w_, sw_h_);
    lv_obj_set_pos(sw, zone_x + (zone_w - sw_w_) / 2, (row_h - sw_h_) / 2);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kTrack), LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kTrack), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kAccent), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kKnob), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -std::max(2, sw_h_ / 7), LV_PART_KNOB);
    lv_obj_set_style_anim_duration(sw, 0, 0);
    return sw;
}

void HaApp::draw_switch(lv_obj_t *sw, bool on, bool grey)
{
    if (!sw) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_opa(sw, grey ? LV_OPA_40 : LV_OPA_COVER, 0);
}

int HaApp::build_entities_card(DashCardUi &c, const ha::DashCardView &v, int card)
{
    const int w = c.w;
    const int sw_zone = std::max(row_h_, sw_w_ + 2 * margin_);
    int y = 0;
    c.first_row = static_cast<int>(dash_rows_.size());
    if (v.header_toggle) {
        c.hdr = box(c.box);
        lv_obj_set_pos(c.hdr, 0, 0);
        lv_obj_set_size(c.hdr, w, row_h_);
        lv_obj_set_style_radius(c.hdr, m_radius(), 0);
        c.title = line_label(c.hdr, tab_bold_, CP0_UI_LIST_TEXT_RGB, std::max(1, w - margin_ - sw_zone));
        lv_obj_set_pos(c.title, margin_, (row_h_ - line_h(tab_bold_)) / 2);
        c.hdr_value = line_label(c.hdr, name_font_, ha::rgb::kSecondary, 0);
        lv_obj_add_flag(c.hdr_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *z = dash_zone(c.hdr, w - sw_zone, 0, sw_zone, row_h_, card, -1, DashZone::Header);
        c.hdr_sw = dash_switch(z, 0, sw_zone, row_h_);
        y = row_h_;
    } else if (!v.title.empty()) {
        c.title = line_label(c.box, tab_bold_, CP0_UI_LIST_TEXT_RGB, std::max(1, w - 2 * margin_));
        lv_obj_set_pos(c.title, margin_, cpad_);
        y = cpad_ + line_h(tab_bold_) + cpad_ / 2;
    } else {
        y = cpad_ / 2;
    }
    const int glyph_h = line_h(glyph_font_ ? glyph_font_ : name_font_);
    for (std::size_t k = 0; k < v.rows.size(); ++k) {
        const ha::DashRowView &rv = v.rows[k];
        DashRowUi r;
        r.card = card;
        const int row_index = static_cast<int>(dash_rows_.size());
        switch (rv.kind) {
        case ha::DashRow::Kind::Entity:
        case ha::DashRow::Kind::Other: {
            r.box = box(c.box);
            lv_obj_set_pos(r.box, 0, y);
            lv_obj_set_size(r.box, w, row_h_);
            lv_obj_set_style_radius(r.box, m_radius(), 0);
            lv_obj_set_style_bg_color(r.box, lv_color_hex(kHighlight), 0);
            lv_obj_set_style_bg_opa(r.box, LV_OPA_TRANSP, 0);
            /* the icon zone (the action), the name zone (tap_action: the entity page), the switch zone (the toggle): the
             * labels inside their zone, so that a tap on a label is a tap on its zone */
            const bool entity = rv.kind == ha::DashRow::Kind::Entity;
            const bool sw = entity && rv.toggle;
            const int name_w = (sw ? w - sw_zone : w) - row_h_;
            lv_obj_t *icon_parent = r.box, *name_parent = r.box;
            r.name_x = row_h_;
            if (entity) {
                icon_parent = dash_zone(r.box, 0, 0, row_h_, row_h_, card, row_index, DashZone::Icon);
                name_parent = dash_zone(r.box, row_h_, 0, name_w, row_h_, card, row_index, DashZone::Name);
                r.name_x = 0;
            }
            r.name_right = r.name_x + name_w - (sw ? 0 : margin_);
            r.glyph = lv_label_create(icon_parent);
            lv_obj_set_style_text_font(r.glyph, glyph_font_ ? glyph_font_ : name_font_, 0);
            lv_obj_remove_flag(r.glyph, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(r.glyph, 0, (row_h_ - glyph_h) / 2);
            lv_obj_set_width(r.glyph, row_h_);
            lv_obj_set_style_text_align(r.glyph, LV_TEXT_ALIGN_CENTER, 0);
            r.name = line_label(name_parent, name_font_, ha::rgb::kText, std::max(1, r.name_right - r.name_x));
            lv_obj_set_pos(r.name, r.name_x, (row_h_ - line_h(name_font_)) / 2);
            r.value = line_label(name_parent, name_font_, ha::rgb::kSecondary, 1);
            lv_label_set_text(r.value, "");
            lv_label_set_text(r.glyph, "");
            lv_obj_set_style_text_align(r.value, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(r.value, r.name_right - 1, (row_h_ - line_h(name_font_)) / 2);
            if (sw) {
                lv_obj_t *z = dash_zone(r.box, w - sw_zone, 0, sw_zone, row_h_, card, row_index, DashZone::Switch);
                r.sw = dash_switch(z, 0, sw_zone, row_h_);
            }
            y += row_h_;
            break;
        }
        case ha::DashRow::Kind::Section: {
            const int top = k ? cpad_ : 0;
            const int h = top + line_h(word_font_) + cpad_;
            r.box = box(c.box);
            lv_obj_set_pos(r.box, 0, y);
            lv_obj_set_size(r.box, w, h);
            if (k) {
                lv_obj_t *line = box(r.box);
                lv_obj_set_pos(line, margin_, top / 2);
                lv_obj_set_size(line, std::max(1, w - 2 * margin_), 1);
                lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(line, lv_color_hex(kTrack), 0);
            }
            r.name = line_label(r.box, word_font_, CP0_UI_LIST_SECTION_RGB, std::max(1, w - 2 * margin_));
            lv_obj_set_pos(r.name, margin_, top + cpad_ / 2);
            y += h;
            break;
        }
        case ha::DashRow::Kind::Divider: {
            const int h = cpad_ * 2 + 1;
            r.box = box(c.box);
            lv_obj_set_pos(r.box, 0, y);
            lv_obj_set_size(r.box, w, h);
            lv_obj_t *line = box(r.box);
            lv_obj_set_pos(line, margin_, cpad_);
            lv_obj_set_size(line, std::max(1, w - 2 * margin_), 1);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(line, lv_color_hex(kTrack), 0);
            y += h;
            break;
        }
        }
        dash_rows_.push_back(r);
    }
    c.rows = static_cast<int>(v.rows.size());
    y += cpad_ / 2;
    return std::max(y, line_h(tab_bold_) + 2 * cpad_);
}

int HaApp::build_sensor_card(DashCardUi &c, const ha::DashCardView &v, int card)
{
    const int w = c.w;
    const lv_font_t *gf = glyph_font_ ? glyph_font_ : name_font_;
    const int gw = line_h(gf);
    int y = cpad_;
    c.name = line_label(c.box, word_font_, ha::rgb::kSecondary, std::max(1, w - 2 * margin_ - gw - gap_));
    lv_obj_set_pos(c.name, margin_, y + (std::max(gw, line_h(word_font_)) - line_h(word_font_)) / 2);
    c.glyph = lv_label_create(c.box);
    lv_obj_set_style_text_font(c.glyph, gf, 0);
    lv_obj_set_pos(c.glyph, w - margin_ - gw, y);
    lv_obj_set_width(c.glyph, gw);
    lv_obj_set_style_text_align(c.glyph, LV_TEXT_ALIGN_CENTER, 0);
    y += std::max(gw, line_h(word_font_));
    c.value = lv_label_create(c.box);
    lv_obj_set_style_text_font(c.value, big_font_, 0);
    lv_obj_set_style_text_color(c.value, lv_color_hex(ha::rgb::kText), 0);
    lv_label_set_long_mode(c.value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(c.value, margin_, y);
    lv_obj_set_size(c.value, std::max(1, w - 2 * margin_), line_h(big_font_));
    c.unit = line_label(c.box, name_font_, ha::rgb::kSecondary, 0);
    lv_obj_set_y(c.unit, y + line_h(big_font_) - line_h(name_font_) - std::max(1, line_h(big_font_) / 10));
    y += line_h(big_font_);
    if (v.graph && !v.spark.empty()) {
        const int ch = row_h_;
        y += cpad_ / 2;
        c.chart = lv_chart_create(c.box);
        lv_obj_remove_style_all(c.chart);
        lv_obj_remove_flag(c.chart, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(c.chart, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(c.chart, cpad_, y);
        lv_obj_set_size(c.chart, std::max(2, w - 2 * cpad_), ch);
        lv_chart_set_type(c.chart, LV_CHART_TYPE_LINE);
        lv_chart_set_div_line_count(c.chart, 0, 0);
        lv_chart_set_point_count(c.chart, static_cast<uint32_t>(v.spark.size()));
        lv_chart_set_axis_range(c.chart, LV_CHART_AXIS_PRIMARY_Y, -30, 1030);
        lv_obj_set_style_line_width(c.chart, std::max(2, row_h_ / 30), LV_PART_ITEMS);
        lv_obj_set_style_width(c.chart, 0, LV_PART_INDICATOR);
        lv_obj_set_style_height(c.chart, 0, LV_PART_INDICATOR);
        c.series = lv_chart_add_series(c.chart, lv_color_hex(kAccent), LV_CHART_AXIS_PRIMARY_Y);
        y += ch;
    }
    y += cpad_;
    y = std::max(y, row_h_);
    /* the card itself is the touch zone (its labels inside it): the entity page */
    lv_obj_add_flag(c.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(c.box, reinterpret_cast<void *>(zone_code(card, -2, static_cast<int>(DashZone::Card))));
    lv_obj_add_event_cb(c.box, dash_event_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(c.box, dash_event_cb, LV_EVENT_CLICKED, this);
    return y;
}

int HaApp::build_weather_card(DashCardUi &c, const ha::DashCardView &v)
{
    const int w = c.w;
    int y = cpad_;
    c.name = line_label(c.box, word_font_, ha::rgb::kSecondary, std::max(1, w - 2 * margin_));
    lv_obj_set_pos(c.name, margin_, y);
    y += line_h(word_font_) + cpad_ / 2;
    if (!v.condition.empty() || !v.value.empty()) {
        const lv_font_t *hg = hero_glyph_font_ ? hero_glyph_font_ : big_font_;
        const int gh = line_h(hg);
        c.glyph = lv_label_create(c.box);
        lv_obj_set_style_text_font(c.glyph, hg, 0);
        lv_obj_set_pos(c.glyph, margin_, y);
        const int tx = margin_ + gh + margin_;
        c.value = lv_label_create(c.box);
        lv_obj_set_style_text_font(c.value, big_font_, 0);
        lv_obj_set_style_text_color(c.value, lv_color_hex(ha::rgb::kText), 0);
        lv_obj_set_pos(c.value, tx, y);
        c.unit = line_label(c.box, name_font_, ha::rgb::kSecondary, 0);
        lv_obj_set_y(c.unit, y + line_h(big_font_) - line_h(name_font_) - std::max(1, line_h(big_font_) / 10));
        c.cond = line_label(c.box, word_font_, ha::rgb::kSecondary, std::max(1, w - tx - margin_));
        lv_obj_set_pos(c.cond, tx, y + line_h(big_font_));
        y += std::max(gh, line_h(big_font_) + line_h(word_font_));
    }
    if (!v.days.empty()) {
        y += cpad_;
        const int inner = w - 2 * margin_;
        const int min_col = std::max(text_w("Wed", word_font_), text_w("-10\xC2\xB0", word_font_)) + gap_;
        const int n = std::clamp(inner / std::max(1, min_col), 1, std::min(5, static_cast<int>(v.days.size())));
        const int cw = inner / n;
        const lv_font_t *gf = glyph_font_ ? glyph_font_ : word_font_;
        const int lw = line_h(word_font_), lg = line_h(gf);
        for (int d = 0; d < n; ++d) {
            const int x = margin_ + d * cw;
            const int ys[4] = {y, y + lw, y + lw + lg, y + 2 * lw + lg};
            const lv_font_t *fonts[4] = {word_font_, gf, word_font_, word_font_};
            const std::uint32_t colours[4] = {ha::rgb::kSecondary, ha::rgb::kText, ha::rgb::kText, ha::rgb::kSecondary};
            for (int k = 0; k < 4; ++k) {
                lv_obj_t *l = line_label(c.box, fonts[k], colours[k], cw);
                lv_obj_set_pos(l, x, ys[k]);
                lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
                c.days.push_back(l);
            }
        }
        y += 3 * lw + lg;
    }
    y += cpad_;
    return y;
}

int HaApp::build_unsupported_card(DashCardUi &c, const ha::DashCardView &v)
{
    const int w = c.w;
    int y = cpad_;
    c.title = line_label(c.box, name_font_, ha::rgb::kSecondary, std::max(1, w - 2 * margin_));
    lv_obj_set_pos(c.title, margin_, y);
    y += line_h(name_font_);
    c.cond = line_label(c.box, word_font_, ha::rgb::kDisabled, std::max(1, w - 2 * margin_));
    lv_obj_set_pos(c.cond, margin_, y);
    y += line_h(word_font_) + cpad_;
    (void)v;
    return y;
}

/* Only what changed (or everything, forced). */
void HaApp::draw_dash_row(DashRowUi &r, const ha::DashRowView &want, bool force)
{
    const ha::DashRowView &old = r.shown;
    if (!force && r.drawn && want == old) return;
    switch (want.kind) {
    case ha::DashRow::Kind::Section:
        if (force || want.label != old.label) lv_label_set_text(r.name, upper(want.label).c_str());
        break;
    case ha::DashRow::Kind::Divider: break;
    case ha::DashRow::Kind::Other:
        if (force || want.label != old.label) {
            lv_label_set_text(r.name, want.label.c_str());
            lv_obj_set_style_text_color(r.name, lv_color_hex(ha::rgb::kDisabled), 0);
            lv_label_set_text(r.glyph, "");
        }
        break;
    case ha::DashRow::Kind::Entity: {
        const ha::EntityRow &e = want.e;
        const bool grey = (e.muted && !e.pending && !e.done) || !want.available;
        const bool dim = e.unavailable || !want.available;
        /* a toggleable row shows its switch, the word only while waiting ("...") or when there is no switch to show */
        const bool show_sw = r.sw && want.available && want.has_state && !e.unavailable;
        const std::string word = show_sw ? (e.pending ? std::string("...") : std::string()) : want.word;
        const int room = r.name_right - r.name_x;
        const int vw = word.empty() ? 0 : std::min(text_w(word, name_font_) + 2, room * 55 / 100);
        lv_obj_set_width(r.value, std::max(1, vw));
        lv_obj_set_x(r.value, r.name_right - std::max(1, vw));
        lv_label_set_text(r.value, word.c_str());
        lv_obj_set_width(r.name, std::max(1, room - (vw ? vw + gap_ : 0)));
        if (force || e.name != old.e.name) lv_label_set_text(r.name, e.name.c_str());
        if (force || e.glyph != old.e.glyph || (!e.glyph && e.name != old.e.name)) {
            const std::string g = e.glyph && glyph_font_ ? ha::utf8_of(e.glyph) : initial(e.name);
            lv_label_set_text(r.glyph, g.c_str());
        }
        std::uint32_t name_rgb = ha::rgb::kText, word_rgb = ha::rgb::kSecondary, glyph_rgb = e.glyph_rgb;
        if (dim) name_rgb = word_rgb = glyph_rgb = ha::rgb::kDisabled;
        else if (grey) {
            name_rgb = ha::rgb::kSecondary;
            word_rgb = glyph_rgb = ha::rgb::kDisabled;
        }
        lv_obj_set_style_text_color(r.name, lv_color_hex(name_rgb), 0);
        lv_obj_set_style_text_color(r.value, lv_color_hex(word_rgb), 0);
        lv_obj_set_style_text_color(r.glyph, lv_color_hex(glyph_rgb), 0);
        if (r.sw) {
            if (show_sw) lv_obj_remove_flag(r.sw, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(r.sw, LV_OBJ_FLAG_HIDDEN);
            draw_switch(r.sw, e.on, grey || e.pending);
        }
        break;
    }
    }
    r.shown = want;
    r.drawn = true;
}

void HaApp::draw_dash_card(DashCardUi &c, const ha::DashCardView &want, bool force)
{
    const ha::DashCardView &old = c.shown;
    switch (want.kind) {
    case ha::DashCard::Kind::Entities:
        if (c.title && (force || want.title != old.title)) lv_label_set_text(c.title, want.title.c_str());
        if (c.hdr_sw) {
            const bool grey = want.header_pending || view_.conn != ha::Conn::Connected;
            const int key = (want.header_on ? 2 : 0) + (grey ? 1 : 0);
            if (force || key != c.hdr_key) draw_switch(c.hdr_sw, want.header_on, grey);
            c.hdr_key = key;
        }
        break;
    case ha::DashCard::Kind::Sensor:
    case ha::DashCard::Kind::Weather: {
        const ha::EntityRow &e = want.e;
        const bool grey = e.muted && !e.pending;
        /* the colours only when the look changed: a style change repaints the object */
        const bool look = force || !c.drawn || grey != (old.e.muted && !old.e.pending) ||
                          e.unavailable != old.e.unavailable || e.glyph_rgb != old.e.glyph_rgb;
        if (force || e.name != old.e.name) lv_label_set_text(c.name, e.name.c_str());
        const std::uint32_t glyph = want.kind == ha::DashCard::Kind::Weather ? want.big_glyph : e.glyph;
        if (c.glyph && (force || glyph != (old.kind == ha::DashCard::Kind::Weather ? old.big_glyph : old.e.glyph))) {
            const std::string g = glyph && glyph_font_ ? ha::utf8_of(glyph) : initial(e.name);
            lv_label_set_text(c.glyph, g.c_str());
        }
        if (c.glyph && look) {
            std::uint32_t rgb = want.kind == ha::DashCard::Kind::Weather ? ha::rgb::kText : e.glyph_rgb;
            if (grey || e.unavailable) rgb = ha::rgb::kDisabled;
            lv_obj_set_style_text_color(c.glyph, lv_color_hex(rgb), 0);
        }
        if (c.value && (force || want.value != old.value || want.unit != old.unit)) {
            lv_label_set_text(c.value, want.value.c_str());
            lv_label_set_text(c.unit, want.unit.c_str());
            const int vx = lv_obj_get_x(c.value);
            const int room = c.w - margin_ - vx;
            const int uw = want.unit.empty() ? 0 : text_w(want.unit, name_font_) + 2;
            const int vw = std::min(text_w(want.value, big_font_) + 2, std::max(1, room - uw - gap_ / 2));
            lv_obj_set_size(c.value, std::max(1, vw), line_h(big_font_));
            lv_obj_set_width(c.unit, std::max(1, uw));
            lv_obj_set_x(c.unit, vx + vw + gap_ / 2);
        }
        if (c.value && look) lv_obj_set_style_text_color(c.value, lv_color_hex(grey ? ha::rgb::kSecondary : ha::rgb::kText), 0);
        if (c.cond && (force || want.condition != old.condition)) lv_label_set_text(c.cond, want.condition.c_str());
        if (c.chart && c.series && (force || want.spark != old.spark)) {
            const std::size_t n = std::min(want.spark.size(), static_cast<std::size_t>(lv_chart_get_point_count(c.chart)));
            for (std::size_t i = 0; i < n; ++i)
                lv_chart_set_series_value_by_id(c.chart, c.series, static_cast<uint32_t>(i),
                                                want.spark[i] < 0 ? LV_CHART_POINT_NONE : want.spark[i]);
            lv_chart_refresh(c.chart);
        }
        if (c.chart && look) {
            lv_chart_set_series_color(c.chart, c.series, lv_color_hex(grey ? ha::rgb::kDisabled : kAccent));
            lv_chart_refresh(c.chart);
        }
        if (!c.days.empty() && (force || !(want.days == old.days))) {
            for (std::size_t d = 0; d * 4 + 3 < c.days.size() && d < want.days.size(); ++d) {
                const ha::ForecastCol &f = want.days[d];
                lv_label_set_text(c.days[d * 4], f.day.c_str());
                const std::string g = f.glyph && glyph_font_ ? ha::utf8_of(f.glyph) : std::string("?");
                lv_label_set_text(c.days[d * 4 + 1], g.c_str());
                lv_label_set_text(c.days[d * 4 + 2], f.high.c_str());
                lv_label_set_text(c.days[d * 4 + 3], f.low.c_str());
            }
        }
        break;
    }
    case ha::DashCard::Kind::Unsupported:
        if (force || want.title != old.title || want.type != old.type) {
            lv_label_set_text(c.title, (want.title.empty() ? type_title(want.type) : want.title).c_str());
            lv_label_set_text(c.cond, "Not supported on this screen");
        }
        break;
    }
    c.shown = want;
    c.drawn = true;
}

void HaApp::dash_event_cb(lv_event_t *e)
{
    auto *self = static_cast<HaApp *>(lv_event_get_user_data(e));
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    lv_point_t p{};
    if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->press_at_ = p;
        return;
    }
    const int dx = p.x - self->press_at_.x, dy = p.y - self->press_at_.y;
    const std::intptr_t code = reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj));
    const int zone = static_cast<int>(code % 8);
    const int row = static_cast<int>((code / 8) % 65536) - 2;
    const int card = static_cast<int>(code / 8 / 65536);
    self->post([self, card, row, zone, dx, dy] {
        if (self->swipe(dx, dy)) return;
        self->focus_visible_ = false; /* a tap: the focus goes (touch-only boards show none) */
        self->style_focus();
        self->dash_tap(card, row, static_cast<DashZone>(zone));
    });
}

void HaApp::dash_tap(int card, int row, DashZone zone)
{
    if (card < 0 || card >= static_cast<int>(dcards_.size())) return;
    const DashCardUi &c = dcards_[static_cast<std::size_t>(card)];
    if (zone == DashZone::Header) {
        dash_header(card);
        return;
    }
    if (zone == DashZone::Card) {
        if (!c.shown.e.id.empty()) open_entity(c.shown.e.id);
        return;
    }
    if (row < 0 || row >= static_cast<int>(dash_rows_.size())) return;
    const ha::DashRowView r = dash_rows_[static_cast<std::size_t>(row)].shown;
    if (r.kind != ha::DashRow::Kind::Entity) return;
    switch (zone) {
    case DashZone::Icon:
    case DashZone::Switch: act_on_dash_row(r); return;
    case DashZone::Name:
        switch (r.tap) {
        case ha::TapAction::None: return;
        case ha::TapAction::Toggle: act_on_dash_row(r); return;
        case ha::TapAction::Default:
        case ha::TapAction::MoreInfo: open_entity(r.e.id); return;
        }
        return;
    default: return;
    }
}

/* A row's action: a switch toggles, a scene or script runs, a lock and a garage-type cover ask first (0.2.0's
 * question), anything else opens its entity page; an unavailable or unknown entity opens its page. */
void HaApp::act_on_dash_row(const ha::DashRowView &r)
{
    if (r.kind != ha::DashRow::Kind::Entity || r.e.id.empty()) return;
    if (!r.available || r.e.unavailable) {
        open_entity(r.e.id);
        return;
    }
    if (r.toggle) {
        backend_->send(ha::Command::of(ha::Command::Kind::Toggle, r.e.id));
        refresh(true);
        return;
    }
    act_on_tile(r.e);
}

/* The header toggle: all on when none is on, else all off (lights, switches, fans, input booleans; never a lock or a
 * cover). */
void HaApp::dash_header(int card)
{
    if (card < 0 || card >= static_cast<int>(dcards_.size())) return;
    const ha::DashCardView &v = dcards_[static_cast<std::size_t>(card)].shown;
    if (!v.header_toggle || v.toggle_ids.empty()) return;
    backend_->send(ha::Command::set_group(v.toggle_ids, !v.header_on));
    refresh(true);
}

void HaApp::dash_key(ha::NavKey k)
{
    const int n = static_cast<int>(dstops_.size());
    if (n == 0) {
        if (k == ha::NavKey::Up) {
            zone_ = Zone::Bar;
            style_focus();
        }
        return;
    }
    dstop_ = std::clamp(dstop_, 0, n - 1);
    const DashStop &s = dstops_[static_cast<std::size_t>(dstop_)];
    switch (k) {
    case ha::NavKey::Up:
        if (dstop_ == 0) zone_ = Zone::Bar;
        else --dstop_;
        break;
    case ha::NavKey::Down:
        if (dstop_ + 1 >= n) return;
        ++dstop_;
        break;
    case ha::NavKey::Left: {
        int i = dstop_;
        while (i > 0 && dstops_[static_cast<std::size_t>(i)].card == s.card) --i;
        if (dstops_[static_cast<std::size_t>(i)].card == s.card) return;
        const int prev_card = dstops_[static_cast<std::size_t>(i)].card;
        while (i > 0 && dstops_[static_cast<std::size_t>(i - 1)].card == prev_card) --i;
        dstop_ = i;
        break;
    }
    case ha::NavKey::Right: {
        int i = dstop_;
        while (i < n && dstops_[static_cast<std::size_t>(i)].card == s.card) ++i;
        if (i >= n) return;
        dstop_ = i;
        break;
    }
    case ha::NavKey::Home: dstop_ = 0; break;
    case ha::NavKey::End: dstop_ = n - 1; break;
    case ha::NavKey::Enter:
        if (s.row == -1) dash_header(s.card);
        else if (s.row == -2) dash_tap(s.card, -2, DashZone::Card);
        else act_on_dash_row(dash_rows_[static_cast<std::size_t>(s.row)].shown);
        return;
    case ha::NavKey::Details:
        if (s.row == -2) dash_tap(s.card, -2, DashZone::Card);
        else if (s.row >= 0) {
            const ha::DashRowView &r = dash_rows_[static_cast<std::size_t>(s.row)].shown;
            if (!r.e.id.empty()) open_entity(r.e.id);
        }
        return;
    default: return;
    }
    style_focus();
    focus_tile_into_view();
}

void HaApp::style_dash_focus()
{
    if (dstop_drawn_ >= 0 && dstop_drawn_ < static_cast<int>(dstops_.size())) {
        const DashStop &s = dstops_[static_cast<std::size_t>(dstop_drawn_)];
        if (s.row == -2) lv_obj_set_style_bg_color(s.obj, lv_color_hex(ha::rgb::kTile), 0);
        else lv_obj_set_style_bg_opa(s.obj, LV_OPA_TRANSP, 0);
        set_focus_ring(s.obj, false);
    }
    dstop_drawn_ = -1;
    if (!focus_visible_ || zone_ != Zone::Grid || !on_dash_tab() || dstops_.empty()) return;
    dstop_ = std::clamp(dstop_, 0, static_cast<int>(dstops_.size()) - 1);
    const DashStop &s = dstops_[static_cast<std::size_t>(dstop_)];
    lv_obj_set_style_bg_color(s.obj, lv_color_hex(kHighlight), 0);
    lv_obj_set_style_bg_opa(s.obj, LV_OPA_COVER, 0);
    set_focus_ring(s.obj, true);
    dstop_drawn_ = dstop_;
}

} // namespace haui
