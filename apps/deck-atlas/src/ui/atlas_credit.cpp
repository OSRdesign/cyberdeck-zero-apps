/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the bottom row of the map view (see core/attribution.hpp): the scale bar at the left and the map credit at
 * the right, flush with the bottom edge, in the smallest font the toolkit has. The engine's own scale bar and credit are
 * moved out of sight: the widget is taller than the view by `reserve_` pixels and the map's parent clips it, so the
 * engine's bottom rows lie below the edge. The widget's centre is then lower than the view's middle by half of that;
 * visible_center() / set_visible_center() convert, and every place that cares where the middle of the view is uses them.
 */
#include "atlas_app.hpp"
#include "atlas_util.hpp"

#include "geo.hpp"

#include "cp0_map_widget.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>

namespace atlasui {

namespace {

const lv_font_t *small_font() { return cp0_ui_font_px(CP0_UI_FONT_MIN_PX); }

int text_width(const lv_font_t *font, const std::string &s)
{
    if (s.empty()) return 0;
    lv_point_t p{0, 0};
    lv_text_get_size(&p, s.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<int>(p.x);
}

void fill_area(lv_layer_t *layer, const lv_area_t &a, std::uint32_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(c);
    d.bg_opa = opa;
    d.radius = 0;
    lv_draw_rect(layer, &d, &a);
}

void text_at(lv_layer_t *layer, const lv_font_t *font, const std::string &s, int x, int y)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s.c_str();
    d.text_local = 1;
    d.font = font;
    d.color = lv_color_hex(pal::kText);
    const lv_area_t a = {x, y, x + text_width(font, s), y + lv_font_get_line_height(font) - 1};
    lv_draw_label(layer, &d, &a);
}

} // namespace

void AtlasApp::visible_center(double &lat, double &lon) const
{
    if (!map_) return;
    double clat = 0, clon = 0;
    cp0_map_get_center(map_, &clat, &clon);
    atlas::shift_down_px(clat, clon, cp0_map_get_zoom(map_), cp0_map_get_scale(map_), -reserve_ / 2.0, lat, lon);
}

void AtlasApp::set_visible_center(double lat, double lon)
{
    if (!map_) return;
    double clat = 0, clon = 0;
    atlas::shift_down_px(lat, lon, cp0_map_get_zoom(map_), cp0_map_get_scale(map_), reserve_ / 2.0, clat, clon);
    cp0_map_set_center(map_, clat, clon);
}

void AtlasApp::apply_reserve(int reserve)
{
    if (!map_ || (reserve == reserve_ && map_h_ == view_h_ + reserve)) return;
    double lat = 0, lon = 0;
    visible_center(lat, lon);
    reserve_ = reserve;
    map_h_ = view_h_ + reserve;
    lv_obj_set_size(map_, w_, map_h_);
    set_visible_center(lat, lon);
}

void AtlasApp::build_credit()
{
    credit_ = lv_obj_create(map_box_);
    plain(credit_);
    lv_obj_set_pos(credit_, 0, view_h_ - 1);
    lv_obj_set_size(credit_, w_, 1);
    lv_obj_add_event_cb(credit_, credit_cb, LV_EVENT_DRAW_MAIN, this);
}

void AtlasApp::credit_cb(lv_event_t *e)
{
    auto *self = static_cast<AtlasApp *>(lv_event_get_user_data(e));
    lv_area_t a;
    lv_obj_get_coords(self->credit_, &a);
    self->draw_credit(lv_event_get_layer(e), a);
}

/* The credit as the engine would draw it (the same text), then the bottom row for the new size, credit and reserve. */
void AtlasApp::update_attribution()
{
    if (!credit_ || !map_) return;
    const std::string text = engine_attribution();
    const std::string key = text + "|" + std::to_string(w_) + "x" + std::to_string(view_h_);
    if (key == attr_key_ && !credit_pending_) return;
    credit_text_ = text;

    /* the engine's rows below the view's edge, for this credit */
    atlas::AttrInput in;
    in.w = w_;
    in.h = view_h_;
    in.target = target_;
    in.space = space_m_;
    const lv_font_t *cap = cp0_ui_font(CP0_TEXT_CAPTION);
    in.text_h = lv_font_get_line_height(cap);
    apply_reserve(atlas::engine_reserve(in, text_width(cap, text)));

    /* our row: the smallest font */
    const lv_font_t *font = small_font();
    const atlas::MapMetrics mm = atlas::map_metrics(overlay_input());
    atlas::CreditInput ci;
    ci.w = w_;
    ci.h = view_h_;
    ci.text_h = lv_font_get_line_height(font);
    ci.pad = mm.pad;
    const atlas::MeasureFn measure = [font](const std::string &s) { return text_width(font, s); };
    credit_plan_ = atlas::plan_credit(ci, text, measure);
    /* the engine's zoom buttons are centred in the taller widget, so on a short view they come down to the credit's rows:
     * then the credit stops left of their column (grows a line) instead of running under the lower button */
    credit_pending_ = true;
    if (map_box_ && !credit_plan_.credit.empty()) {
        lv_obj_update_layout(map_box_);
        lv_area_t box, mapa, zo;
        lv_obj_get_coords(map_box_, &box);
        lv_obj_get_coords(map_, &mapa);
        if (cp0_map_control_area(map_, CP0_MAP_CONTROL_ZOOM_OUT, &zo) == CP0_MAP_OK && zo.y2 > zo.y1) {
            const int th = zo.y2 - zo.y1 + 1;
            const int plate_bottom = zo.y1 + th / 2 - mm.plate / 2 + mm.plate - box.y1; /* in the view's coordinates */
            const int col_x = mapa.x2 + 1 - mm.margin - mm.plate - box.x1;
            credit_pending_ = false;
            if (credit_plan_.credit.y <= plate_bottom + 1 && credit_plan_.credit.x + credit_plan_.credit.w > col_x - mm.margin / 2) {
                ci.right = col_x - std::max(mm.margin / 2, 1);
                credit_plan_ = atlas::plan_credit(ci, text, measure);
            }
        }
    } else {
        credit_pending_ = false;
    }
    if (!credit_pending_) attr_key_ = key;
    credit_top_ = credit_plan_.credit.empty() ? -1 : credit_plan_.credit.y;
    credit_h_ = std::max(credit_plan_.credit.h, credit_plan_.scale.h);
    credit_y_ = view_h_ - credit_h_;
    lv_obj_set_pos(credit_, 0, credit_y_);
    lv_obj_set_size(credit_, w_, credit_h_);
    lv_obj_invalidate(credit_);
    gps_layout(); /* the follow button keeps clear of the credit at the bottom right */
}

void AtlasApp::draw_credit(lv_layer_t *layer, const lv_area_t &o)
{
    if (!map_) return;
    const atlas::CreditLayout &p = credit_plan_;
    const lv_font_t *font = small_font();
    const int th = lv_font_get_line_height(font);
    auto area = [&](const atlas::Rect &r) {
        return lv_area_t{o.x1 + r.x, o.y1 + r.y - credit_y_, o.x1 + r.x + r.w - 1, o.y1 + r.y - credit_y_ + r.h - 1};
    };

    /* the scale bar at the left: a bar with two ticks, its length as a label to the right of it */
    double clat = 0, clon = 0;
    cp0_map_get_center(map_, &clat, &clon);
    const double mpp = cp0_map_metres_per_px(clat, cp0_map_get_zoom(map_), cp0_map_get_scale(map_));
    cp0_map_scalebar_t sb;
    const cp0_map_units_t units = settings_.units == atlas::Units::Mi ? CP0_MAP_UNITS_IMPERIAL : CP0_MAP_UNITS_METRIC;
    if (mpp > 0 && cp0_map_scalebar(mpp, p.sb_bar_max, units, &sb) == CP0_MAP_OK) {
        const int lw = text_width(font, sb.label);
        const atlas::Rect r = atlas::scale_plate(p, sb.px, lw);
        const lv_area_t a = area(r);
        fill_area(layer, a, pal::kBar, LV_OPA_80);
        const int thick = std::max(th / 4, 2), tick = std::max(th * 2 / 3, 3);
        const int bx = a.x1 + p.hp, my = a.y1 + (r.h - thick) / 2;
        fill_area(layer, lv_area_t{bx, my, bx + sb.px - 1, my + thick - 1}, pal::kText, LV_OPA_COVER);
        const int ty = a.y1 + (r.h - tick) / 2;
        fill_area(layer, lv_area_t{bx, ty, bx + 1, ty + tick - 1}, pal::kText, LV_OPA_COVER);
        fill_area(layer, lv_area_t{bx + sb.px - 2, ty, bx + sb.px - 1, ty + tick - 1}, pal::kText, LV_OPA_COVER);
        text_at(layer, font, sb.label, bx + sb.px + p.hp, o.y1 + p.text_y - credit_y_);
    }

    /* the credit at the right: one plate, its last line on the scale bar's baseline */
    if (!p.credit.empty()) {
        fill_area(layer, area(p.credit), pal::kBar, LV_OPA_80);
        for (std::size_t i = 0; i < p.lines.size(); ++i)
            text_at(layer, font, p.lines[i], o.x1 + p.credit.x + p.hp,
                    o.y1 + p.credit.y - credit_y_ + p.vp + static_cast<int>(i) * th);
    }
}

} // namespace atlasui
