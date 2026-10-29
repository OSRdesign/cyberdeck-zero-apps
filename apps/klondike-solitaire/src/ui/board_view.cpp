/*
 * SPDX-License-Identifier: MIT
 */
#include "board_view.hpp"

#include "app.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace klondike;

namespace kui {

namespace {
int screen_w() { return cdk::screen_w(); }
int screen_h() { return cdk::screen_h(); }
} // namespace

namespace {

const char *rank_text(int rank)
{
    static const char *t[14] = {"", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
    return t[rank < 1 ? 0 : (rank > 13 ? 13 : rank)];
}

int even_clamp(double v, int lo, int hi)
{
    int n = static_cast<int>(std::lround(v));
    n &= ~1;
    return std::min(hi, std::max(lo, n));
}

lv_draw_buf_t *make_a8(const art::Mask &m, bool flip_v)
{
    lv_draw_buf_t *b = lv_draw_buf_create(static_cast<uint32_t>(m.w), static_cast<uint32_t>(m.h), LV_COLOR_FORMAT_A8, 0);
    if (!b) return nullptr;
    const uint32_t stride = b->header.stride;
    for (int y = 0; y < m.h; ++y) {
        const int sy = flip_v ? m.h - 1 - y : y;
        std::memcpy(static_cast<uint8_t *>(b->data) + static_cast<size_t>(y) * stride, &m.a[static_cast<size_t>(sy) * m.w], static_cast<size_t>(m.w));
    }
    return b;
}

lv_draw_buf_t *make_argb(const std::vector<std::uint32_t> &px, int w, int h)
{
    lv_draw_buf_t *b = lv_draw_buf_create(static_cast<uint32_t>(w), static_cast<uint32_t>(h), LV_COLOR_FORMAT_ARGB8888, 0);
    if (!b) return nullptr;
    const uint32_t stride = b->header.stride;
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t *>(b->data) + static_cast<size_t>(y) * stride, &px[static_cast<size_t>(y) * w], static_cast<size_t>(w) * 4);
    return b;
}

void draw_mask(lv_layer_t *layer, const lv_draw_buf_t *buf, int x, int y, lv_color_t color, lv_opa_t opa = LV_OPA_COVER)
{
    if (!buf) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = buf;
    d.recolor = color;
    d.recolor_opa = LV_OPA_COVER;
    d.opa = opa;
    const lv_area_t a = {x, y, x + static_cast<int>(buf->header.w) - 1, y + static_cast<int>(buf->header.h) - 1};
    lv_draw_image(layer, &d, &a);
}

bool env_set(const char *name)
{
    const char *v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

} // namespace

struct BoardView::Art {
    lv_draw_buf_t *idx_suit[4] = {};
    lv_draw_buf_t *pip_suit[4] = {};
    lv_draw_buf_t *pip_flip[4] = {};
    lv_draw_buf_t *big_suit[4] = {};
    lv_draw_buf_t *mid_suit[4] = {};
    lv_draw_buf_t *emblem[3] = {};
    lv_draw_buf_t *back = nullptr;
    const lv_font_t *font = nullptr;
    int rank_w[14] = {};
    int text_dy = 0, ink_h = 0;  /* the rank's label is moved by text_dy so that the glyphs start kIndexMargin under the top edge; ink_h = their height */
    int rank_h = 0, idx_size = 0, pip_size = 0, big_size = 0, mid_size = 0, ew = 0, eh = 0;
    int pad = 2, band = 0;
    std::vector<lv_draw_buf_t *> owned;

    lv_draw_buf_t *own(lv_draw_buf_t *b)
    {
        if (b) owned.push_back(b);
        return b;
    }
    ~Art()
    {
        for (lv_draw_buf_t *b : owned) lv_draw_buf_destroy(b);
    }
};

/* ------------------------------------------------------------------------------------------------ construction */

BoardView::BoardView(lv_obj_t *page_root, const Layout &layout, BoardEvents events, const BoardStyle &style)
    : layout_(layout), events_(std::move(events)), style_(style), parent_(page_root)
{
    test_probes_ = env_set("KLONDIKE_TEST_PROBES");
    test_zcheck_ = env_set("KLONDIKE_TEST_ZCHECK") || env_set("KLONDIKE_TEST_ZFUZZ");
    const int top = layout_.hud.y;
    felt_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(felt_);
    lv_obj_set_pos(felt_, 0, top);
    lv_obj_set_size(felt_, screen_w(), screen_h() - top);
    lv_obj_set_style_bg_opa(felt_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(felt_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(felt_, LV_OBJ_FLAG_SCROLLABLE);
    apply_felt();

    surface_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(surface_);
    lv_obj_set_pos(surface_, 0, top);
    lv_obj_set_size(surface_, screen_w(), screen_h() - top);
    lv_obj_add_flag(surface_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    lv_obj_remove_flag(surface_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_event_cb(surface_, surface_event_cb, LV_EVENT_ALL, this);

    for (int p = 0; p < kPiles; ++p) {
        if (p == kPiles) break;
        lv_obj_t *s = lv_obj_create(parent_);
        lv_obj_remove_style_all(s);
        lv_obj_remove_flag(s, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        slot_ctx_[p].view = this;
        slot_ctx_[p].id = p;
        lv_obj_add_event_cb(s, slot_event_cb, LV_EVENT_ALL, &slot_ctx_[p]);
        slots_[p] = s;
    }
    for (int id = 0; id < kCards; ++id) {
        lv_obj_t *c = lv_obj_create(parent_);
        lv_obj_remove_style_all(c);
        lv_obj_remove_flag(c, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        cards_[id].obj = c;
        cards_[id].ctx.view = this;
        cards_[id].ctx.id = id;
        lv_obj_add_event_cb(c, card_event_cb, LV_EVENT_ALL, &cards_[id].ctx);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
    auto make_frame = [&]() {
        lv_obj_t *f = lv_obj_create(parent_);
        lv_obj_remove_style_all(f);
        lv_obj_remove_flag(f, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(f, LV_OPA_COVER, 0);
        lv_obj_add_flag(f, LV_OBJ_FLAG_HIDDEN);
        return f;
    };
    frame_select_ = make_frame();
    frame_hint_src_ = make_frame();
    frame_hint_dst_ = make_frame();
    frame_cursor_ = make_frame();
    if (test_probes_) {
        probes_ = lv_obj_create(parent_);
        lv_obj_remove_style_all(probes_);
        lv_obj_set_pos(probes_, 0, 0);
        lv_obj_set_size(probes_, screen_w(), screen_h());
        lv_obj_remove_flag(probes_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
    set_layout(layout_);
}

BoardView::~BoardView()
{
    lv_anim_delete(this, anim_exec_cb);
    /* the objects are children of the page root: deleted with it; the callbacks must not run after this */
    for (CardView &c : cards_)
        if (c.obj && lv_obj_is_valid(c.obj)) lv_obj_remove_event_cb(c.obj, card_event_cb);
    for (lv_obj_t *s : slots_)
        if (s && lv_obj_is_valid(s)) lv_obj_remove_event_cb(s, slot_event_cb);
    if (surface_ && lv_obj_is_valid(surface_)) lv_obj_remove_event_cb(surface_, surface_event_cb);
    for (lv_obj_t *o : {felt_, surface_, probes_, frame_cursor_, frame_select_, frame_hint_src_, frame_hint_dst_})
        if (o && lv_obj_is_valid(o)) lv_obj_delete(o);
    for (lv_obj_t *s : slots_)
        if (s && lv_obj_is_valid(s)) lv_obj_delete(s);
    for (CardView &c : cards_)
        if (c.obj && lv_obj_is_valid(c.obj)) lv_obj_delete(c.obj);
}

void BoardView::apply_felt()
{
    lv_obj_set_style_bg_color(felt_, lv_color_hex(art::felt_rgb(style_.felt)), 0);
    lv_obj_invalidate(felt_);
}

void BoardView::set_style(const BoardStyle &style)
{
    const bool back_changed = style.back != style_.back;
    style_ = style;
    apply_felt();
    if (back_changed) build_art();
    for (CardView &c : cards_) lv_obj_invalidate(c.obj);
    for (lv_obj_t *s : slots_) lv_obj_invalidate(s);
}

void BoardView::set_layout(const Layout &layout)
{
    layout_ = layout;
    const int top = layout_.hud.y;
    lv_obj_set_pos(felt_, 0, top);
    lv_obj_set_size(felt_, screen_w(), screen_h() - top);
    lv_obj_set_pos(surface_, 0, top);
    lv_obj_set_size(surface_, screen_w(), screen_h() - top);
    build_art();
    place_slots();
    for (CardView &c : cards_) lv_obj_set_size(c.obj, layout_.card_w, layout_.card_h);
    const int bw = std::max(3, layout_.card_w / 18);
    for (lv_obj_t *f : {frame_cursor_, frame_select_, frame_hint_src_, frame_hint_dst_}) {
        lv_obj_set_style_border_width(f, bw, 0);
        lv_obj_set_style_radius(f, layout_.radius + 2, 0);
    }
    if (game_) sync(*game_, false);
}

void BoardView::place_slots()
{
    for (int p = 0; p < kPiles; ++p) {
        const Rect r = slot_rect(p);
        lv_obj_set_pos(slots_[p], r.x, r.y);
        lv_obj_set_size(slots_[p], r.w, r.h);
        lv_obj_invalidate(slots_[p]);
    }
}

void BoardView::set_hidden(bool hidden)
{
    for (lv_obj_t *o : {felt_, surface_}) {
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t *s : slots_) {
        if (hidden) lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s, LV_OBJ_FLAG_HIDDEN);
    }
    if (hidden) {
        for (CardView &c : cards_) {
            c.visible = false;
            lv_obj_add_flag(c.obj, LV_OBJ_FLAG_HIDDEN);
        }
        hide_marks();
    } else if (game_) {
        sync(*game_, false);
    }
}

/* ------------------------------------------------------------------------------------------------ art */

void BoardView::build_art()
{
    if (layout_.card_w <= 0) return;
    const int w = layout_.card_w, h = layout_.card_h;
    if (art_ && card_w_made_ == w && back_made_ == style_.back) return;
    auto art = std::make_unique<Art>();
    Art &a = *art;
    /* the index must be whole in the strip a covered card shows (layout.index_h): the font of index_font_px, the glyphs
     * measured, the label shifted up by the empty space over them; a taller font than the layout assumed is shrunk */
    int rank_px = index_font_px(h);
    for (;;) {
        a.font = cp0_ui_font_px(rank_px);
        a.rank_h = lv_font_get_line_height(a.font);
        lv_font_glyph_dsc_t g{};
        if (!lv_font_get_glyph_dsc(a.font, &g, 'K', 0)) {
            a.ink_h = std::max(6, a.rank_h * 3 / 4);
            a.text_dy = -(a.rank_h - a.ink_h) / 2 + kIndexMargin;
        } else {
            const int asc = a.rank_h - a.font->base_line;
            a.ink_h = g.box_h;
            a.text_dy = kIndexMargin - (asc - (static_cast<int>(g.box_h) + g.ofs_y));
        }
        if (kIndexMargin + a.ink_h + 1 <= layout_.index_h || rank_px <= 8) break;
        rank_px -= 2;
    }
    for (int r = 1; r <= 13; ++r) {
        lv_point_t sz{};
        lv_text_get_size(&sz, rank_text(r), a.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        a.rank_w[r] = sz.x;
    }
    a.pad = std::max(2, w / 16);
    a.idx_size = std::max(6, a.ink_h);
    a.band = a.pad + a.rank_h + std::max(1, h / 50);
    const int area_w = w - 2 * std::max(3, w / 9);
    const int area_h = h - a.band - std::max(3, h / 22);
    a.pip_size = std::max(6, std::min(static_cast<int>(area_w * 0.32), static_cast<int>(area_h / 4.5)));
    a.big_size = std::max(8, std::min(static_cast<int>(area_w * 0.78), static_cast<int>(area_h * 0.62)));
    a.mid_size = std::max(8, std::min(static_cast<int>(area_w * 0.52), static_cast<int>(area_h * 0.34)));
    a.ew = std::max(8, static_cast<int>(area_w * 0.72));
    a.eh = std::max(8, static_cast<int>(area_h * 0.32));
    for (int s = 0; s < 4; ++s) {
        a.idx_suit[s] = a.own(make_a8(art::render_suit(s, a.idx_size), false));
        const art::Mask pip = art::render_suit(s, a.pip_size);
        a.pip_suit[s] = a.own(make_a8(pip, false));
        a.pip_flip[s] = a.own(make_a8(pip, true));
        a.big_suit[s] = a.own(make_a8(art::render_suit(s, a.big_size), false));
        a.mid_suit[s] = a.own(make_a8(art::render_suit(s, a.mid_size), false));
    }
    for (int k = 0; k < 3; ++k) a.emblem[k] = a.own(make_a8(art::render_emblem(k, a.ew, a.eh), false));
    const int bw = w - 2 * 2, bh = h - 2 * 2;
    a.back = a.own(make_argb(art::render_back(style_.back, bw, bh), bw, bh));
    art_ = std::move(art);
    card_w_made_ = w;
    back_made_ = style_.back;
    for (CardView &c : cards_) lv_obj_invalidate(c.obj);
}

/* ------------------------------------------------------------------------------------------------ drawing */

void BoardView::card_event_cb(lv_event_t *e)
{
    auto *ctx = static_cast<CardCtx *>(lv_event_get_user_data(e));
    if (!ctx || !ctx->view) return;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN: ctx->view->draw_card(e, ctx->id); break;
    case LV_EVENT_REFR_EXT_DRAW_SIZE:
        if (ctx->view->cards_[ctx->id].lifted) lv_event_set_ext_draw_size(e, ctx->view->layout_.card_h / 8 + 6);
        break;
    default: break;
    }
}

void BoardView::slot_event_cb(lv_event_t *e)
{
    auto *ctx = static_cast<CardCtx *>(lv_event_get_user_data(e));
    if (!ctx || !ctx->view) return;
    if (lv_event_get_code(e) == LV_EVENT_DRAW_MAIN) ctx->view->draw_slot(e, ctx->id);
}

void BoardView::draw_slot(lv_event_t *e, int pile)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(slots_[pile], &a);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = layout_.radius;
    rd.bg_color = lv_color_black();
    rd.bg_opa = LV_OPA_20;
    rd.border_color = lv_color_white();
    rd.border_opa = LV_OPA_40;
    rd.border_width = std::max(1, layout_.card_w / 36);
    lv_draw_rect(layer, &rd, &a);
    if (!game_ || !art_) return;
    const char *glyph = nullptr;
    if (pile == kStock) {
        const Move m = game_->stock_move();
        glyph = (game_->board().stock_size() == 0 && m.count > 0) ? LV_SYMBOL_REFRESH : nullptr;
        if (game_->board().stock_size() == 0 && m.count == 0) glyph = LV_SYMBOL_CLOSE;
    } else if (is_foundation(pile)) {
        glyph = "A";
    } else if (is_tableau(pile)) {
        glyph = game_->rules().fill == TableauFill::KingOnly ? "K" : nullptr;
    }
    if (!glyph) return;
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = (glyph[0] == 'A' || glyph[0] == 'K') ? art_->font : cp0_ui_font_px(even_clamp(layout_.card_h * 0.30, 12, 40));
    ld.color = lv_color_white();
    ld.opa = LV_OPA_50;
    lv_point_t sz{};
    lv_text_get_size(&sz, glyph, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_area_t ta = {a.x1 + (lv_area_get_width(&a) - sz.x) / 2, a.y1 + (lv_area_get_height(&a) - sz.y) / 2,
                          a.x1 + (lv_area_get_width(&a) + sz.x) / 2, a.y1 + (lv_area_get_height(&a) + sz.y) / 2};
    ld.text = glyph;
    lv_draw_label(layer, &ld, &ta);
}

void BoardView::draw_card(lv_event_t *e, int id)
{
    lv_area_t a;
    lv_obj_get_coords(cards_[id].obj, &a);
    draw_card_into(lv_event_get_layer(e), a, id, cards_[id].face_up, cards_[id].lifted);
}

bool BoardView::render_card(Card c, lv_draw_buf_t *dst)
{
    if (!art_ || !dst || dst->header.cf != LV_COLOR_FORMAT_ARGB8888 || static_cast<int>(dst->header.w) != layout_.card_w ||
        static_cast<int>(dst->header.h) != layout_.card_h)
        return false;
    lv_obj_t *cv = lv_canvas_create(parent_);
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(cv, dst);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    const lv_area_t a = {0, 0, layout_.card_w - 1, layout_.card_h - 1};
    draw_card_into(&layer, a, c, true, false);
    lv_canvas_finish_layer(cv, &layer);
    lv_obj_delete(cv);
    return true;
}

void BoardView::draw_card_into(lv_layer_t *layer, const lv_area_t &a, int id, bool face_up, bool lifted)
{
    if (!art_) return;
    const Art &art = *art_;
    const int w = lv_area_get_width(&a), h = lv_area_get_height(&a);

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = layout_.radius;
    rd.bg_color = lv_color_hex(art::kCardFaceRgb);
    rd.bg_opa = LV_OPA_COVER;
    rd.border_color = lv_color_hex(art::kCardEdgeRgb);
    rd.border_width = 1;
    rd.border_opa = LV_OPA_COVER;
    if (lifted) {
        rd.shadow_width = layout_.card_h / 8;
        rd.shadow_color = lv_color_black();
        rd.shadow_opa = LV_OPA_50;
        rd.shadow_offset_y = 3;
    }
    lv_draw_rect(layer, &rd, &a);

    if (!face_up) {
        if (art.back) {
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = art.back;
            const lv_area_t ba = {a.x1 + 2, a.y1 + 2, a.x1 + 1 + static_cast<int>(art.back->header.w), a.y1 + 1 + static_cast<int>(art.back->header.h)};
            lv_draw_image(layer, &d, &ba);
        }
        return;
    }
    const int suit = suit_of(static_cast<Card>(id)), rank = rank_of(static_cast<Card>(id));
    const lv_color_t color = lv_color_hex(art::suit_rgb(suit, style_.four_colour));

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = art.font;
    ld.color = color;
    ld.text = rank_text(rank);
    const int tx = a.x1 + art.pad, ty = a.y1 + art.text_dy;
    const lv_area_t ta = {tx, ty, tx + art.rank_w[rank] + 2, ty + art.rank_h};
    lv_draw_label(layer, &ld, &ta);
    draw_mask(layer, art.idx_suit[suit], tx + art.rank_w[rank] + std::max(1, w / 40), a.y1 + kIndexMargin + (art.ink_h - art.idx_size) / 2, color);

    const int ax0 = a.x1 + std::max(3, w / 9), ax1 = a.x2 - std::max(3, w / 9);
    const int ay0 = a.y1 + art.band, ay1 = a.y2 - std::max(3, h / 22);
    const int aw = ax1 - ax0 + 1, ah = ay1 - ay0 + 1;
    if (rank == 1) {
        draw_mask(layer, art.big_suit[suit], ax0 + (aw - art.big_size) / 2, ay0 + (ah - art.big_size) / 2, color);
    } else if (rank <= 10) {
        art::Pip pips[10];
        const int n = art::pip_layout(rank, pips);
        for (int i = 0; i < n; ++i) {
            const int px = ax0 + static_cast<int>(pips[i].x * aw) - art.pip_size / 2;
            const int py = ay0 + static_cast<int>(pips[i].y * ah) - art.pip_size / 2;
            draw_mask(layer, pips[i].flipped ? art.pip_flip[suit] : art.pip_suit[suit], px, py, color);
        }
    } else {
        /* face card: a thin frame, the emblem above, one big suit below */
        lv_draw_rect_dsc_t fr;
        lv_draw_rect_dsc_init(&fr);
        fr.radius = std::max(2, layout_.radius / 2);
        fr.bg_opa = LV_OPA_TRANSP;
        fr.border_color = color;
        fr.border_opa = LV_OPA_40;
        fr.border_width = 1;
        const lv_area_t fa = {ax0, ay0, ax1, ay1};
        lv_draw_rect(layer, &fr, &fa);
        const int cy1 = ay0 + static_cast<int>(ah * 0.30);
        const int cy2 = ay0 + static_cast<int>(ah * 0.69);
        draw_mask(layer, art.emblem[rank - 11], ax0 + (aw - art.ew) / 2, cy1 - art.eh / 2, color);
        draw_mask(layer, art.mid_suit[suit], ax0 + (aw - art.mid_size) / 2, cy2 - art.mid_size / 2, color);
    }
}

/* ------------------------------------------------------------------------------------------------ geometry */

Rect BoardView::card_rect(int pile, int idx) const
{
    const Rect slot = slot_rect(pile);
    if (!game_) return slot;
    const Board &b = game_->board();
    if (is_tableau(pile)) {
        const int c = pile - kTableau0;
        const int n = b.ntab[c], nd = b.ndown[c];
        if (n == 0 || idx < 0) return slot;
        const Fan f = fan_for(layout_, nd, n - nd);
        return Rect{slot.x, layout_.tab_y + fan_y(f, nd, std::min(idx, n - 1)), layout_.card_w, layout_.card_h};
    }
    if (pile == kWaste && b.tp > 0 && idx >= 0) {
        const int fan = game_->rules().draw >= 3 ? 3 : 1;
        const int m = std::min(fan, static_cast<int>(b.tp));
        const int k = b.tp - 1 - idx;
        if (k < m) return Rect{slot.x + layout_.waste_dir * layout_.waste_step * (m - 1 - k), slot.y, slot.w, slot.h};
    }
    return slot;
}

Rect BoardView::visible_rect(int pile, int idx) const
{
    Rect r = card_rect(pile, idx);
    if (!game_ || !is_tableau(pile)) return r;
    const int c = pile - kTableau0;
    const int n = game_->board().ntab[c];
    if (idx >= 0 && idx < n - 1) r.h = card_rect(pile, idx + 1).y - r.y;
    return r;
}

Rect BoardView::run_rect(int pile, int idx) const
{
    Rect r = card_rect(pile, idx);
    if (!game_ || !is_tableau(pile)) return r;
    const int n = game_->board().ntab[pile - kTableau0];
    if (n > 0 && idx >= 0) {
        const Rect top = card_rect(pile, n - 1);
        r.h = top.bottom() - r.y;
    }
    return r;
}

Hit BoardView::hit_test(int x, int y) const
{
    if (!game_) return {};
    const Board &b = game_->board();
    if (slot_rect(kStock).contains(x, y)) return {kStock, -1};
    {
        Rect w = slot_rect(kWaste);
        const int fan = game_->rules().draw >= 3 ? 3 : 1;
        const int m = std::max(1, std::min(fan, static_cast<int>(b.tp)));
        const int ext = layout_.waste_step * (m - 1);
        if (layout_.waste_dir > 0) w.w += ext;
        else {
            w.x -= ext;
            w.w += ext;
        }
        if (w.contains(x, y)) return {kWaste, b.tp > 0 ? b.tp - 1 : -1};
    }
    for (int f = 0; f < kFoundations; ++f) {
        const int p = foundation_pile(f);
        if (slot_rect(p).contains(x, y)) return {p, b.size(p) > 0 ? b.size(p) - 1 : -1};
    }
    for (int c = 0; c < kColumns; ++c) {
        const int x0 = layout_.col_x[c] - layout_.gap_x / 2, x1 = layout_.col_x[c] + layout_.card_w + layout_.gap_x / 2;
        if (x < x0 || x >= x1) continue;
        const int pile = tableau_pile(c);
        const int n = b.ntab[c];
        if (n == 0) {
            if (y >= layout_.tab_y && y < layout_.tab_y + layout_.card_h) return {pile, -1};
            return {};
        }
        for (int i = n - 1; i >= 0; --i) {
            const Rect v = visible_rect(pile, i);
            if (y >= v.y && y < v.bottom()) return {pile, i};
        }
        return {};
    }
    return {};
}

bool BoardView::pickable(const Hit &h, int *count) const
{
    if (!game_ || !h.valid() || h.idx < 0) return false;
    const Board &b = game_->board();
    if (h.pile == kWaste) {
        if (h.idx != b.tp - 1) return false;
        *count = 1;
        return true;
    }
    if (is_foundation(h.pile)) {
        if (h.idx != b.size(h.pile) - 1 || !game_->rules().foundation_back) return false;
        *count = 1;
        return true;
    }
    if (is_tableau(h.pile)) {
        const int c = h.pile - kTableau0;
        if (h.idx < b.ndown[c] || h.idx >= b.ntab[c]) return false;
        *count = b.ntab[c] - h.idx;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------------ sync */

void BoardView::layout_pile_targets(std::vector<int> &tx, std::vector<int> &ty, std::vector<char> &vis, std::vector<char> &face) const
{
    const Board &b = game_->board();
    tx.assign(kCards, 0);
    ty.assign(kCards, 0);
    vis.assign(kCards, 0);
    face.assign(kCards, 0);
    auto put = [&](Card c, int x, int y, bool visible, bool up) {
        tx[c] = x;
        ty[c] = y;
        vis[c] = visible;
        face[c] = up;
    };
    /* stock: all at the slot, the next card to be drawn shows */
    {
        const Rect s = slot_rect(kStock);
        const int n = b.stock_size();
        for (int i = 0; i < n; ++i) put(b.card(kStock, i), s.x, s.y, i == n - 1, false);
    }
    /* waste */
    {
        const Rect s = slot_rect(kWaste);
        const int n = b.waste_size();
        const int fan = game_->rules().draw >= 3 ? 3 : 1;
        const int m = std::min(fan, n);
        for (int i = 0; i < n; ++i) {
            const int k = n - 1 - i;
            if (k < m) put(b.card(kWaste, i), s.x + layout_.waste_dir * layout_.waste_step * (m - 1 - k), s.y, true, true);
            else put(b.card(kWaste, i), s.x, s.y, false, true);
        }
    }
    for (int f = 0; f < kFoundations; ++f) {
        const int p = foundation_pile(f);
        const Rect s = slot_rect(p);
        const int n = b.size(p);
        for (int i = 0; i < n; ++i) put(b.card(p, i), s.x, s.y, i == n - 1, true);
    }
    for (int c = 0; c < kColumns; ++c) {
        const int p = tableau_pile(c);
        const int n = b.ntab[c], nd = b.ndown[c];
        const Fan fn = fan_for(layout_, nd, n - nd);
        for (int i = 0; i < n; ++i) put(b.card(p, i), layout_.col_x[c], layout_.tab_y + fan_y(fn, nd, i), true, b.face_up(p, i));
    }
}

void BoardView::sync(const Game &game, bool animate)
{
    game_ = &game;
    if (layout_.card_w <= 0) return;
    if (drag_.active) end_drag(0, 0, true);
    std::vector<int> tx, ty;
    std::vector<char> vis, face;
    layout_pile_targets(tx, ty, vis, face);

    const bool do_anim = animate && anim_ms() > 0;
    lv_anim_delete(this, anim_exec_cb);
    anim_running_ = false;
    for (int id = 0; id < kCards; ++id) {
        CardView &c = cards_[id];
        c.moving = false;
        if (c.lifted) {
            c.lifted = false;
            lv_obj_invalidate(c.obj);
        }
        if (c.face_up != static_cast<bool>(face[id])) {
            c.face_up = face[id];
            lv_obj_invalidate(c.obj);
        }
        if (!vis[id]) {
            if (c.visible) {
                c.visible = false;
                lv_obj_add_flag(c.obj, LV_OBJ_FLAG_HIDDEN);
            }
            c.x = tx[id];
            c.y = ty[id];
            lv_obj_set_pos(c.obj, c.x, c.y);
            continue;
        }
        const bool was_visible = c.visible;
        if (!c.visible) {
            c.visible = true;
            lv_obj_remove_flag(c.obj, LV_OBJ_FLAG_HIDDEN);
        }
        if (do_anim && was_visible && (c.x != tx[id] || c.y != ty[id])) {
            c.x0 = c.x;
            c.y0 = c.y;
            c.x1 = tx[id];
            c.y1 = ty[id];
            c.moving = true;
        } else {
            c.x = tx[id];
            c.y = ty[id];
            lv_obj_set_pos(c.obj, c.x, c.y);
        }
    }
    restack(); /* piles in order, each bottom to top; moving cards are on top while they fly */
    bool any = false;
    for (int id = 0; id < kCards; ++id) any = any || cards_[id].moving;
    for (lv_obj_t *f : {frame_select_, frame_hint_src_, frame_hint_dst_, frame_cursor_}) lv_obj_move_foreground(f);
    for (lv_obj_t *s : slots_) lv_obj_invalidate(s);
    if (any) {
        anim_running_ = true;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_exec_cb(&a, anim_exec_cb);
        lv_anim_set_values(&a, 0, 1024);
        lv_anim_set_duration(&a, static_cast<uint32_t>(anim_ms()));
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, anim_done_cb);
        lv_anim_start(&a);
    }
    if (probes_) dump_probes();
    check_stacking("sync");
    if (const char *g = std::getenv("KLONDIKE_TEST_GHOST")) { /* test fixture: "pile,idx,dx,dy" shows a lifted run mid-drag */
        static bool done = false;
        int pile = 0, idx = 0, dx = 0, dy = 0, count = 0;
        if (!done && std::sscanf(g, "%d,%d,%d,%d", &pile, &idx, &dx, &dy) == 4 && pickable(Hit{pile, idx}, &count)) {
            done = true;
            finish_animations();
            for (int i = 0; i < count; ++i) {
                CardView &c = cards_[game_->board().card(pile, idx + i)];
                c.lifted = true;
                c.x += dx;
                c.y += dy;
                lv_obj_set_pos(c.obj, c.x, c.y);
                lv_obj_move_foreground(c.obj);
                lv_obj_invalidate(c.obj);
            }
        }
    }
}

/* The one place that sets the stacking order of the 52 card objects: every pile bottom to top in the board's order (piles
 * one after the other), then the cards that fly or are lifted, in the same order, then the marks. Called after every state
 * change that repositions cards (sync, animation end, drop); it only touches objects when the order is wrong. */
void BoardView::restack()
{
    if (!game_) return;
    const Board &b = game_->board();
    Card seq[kCards];
    int n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int p = 0; p < kPiles; ++p) {
            const int sz = b.size(p);
            for (int i = 0; i < sz; ++i) {
                const Card cd = b.card(p, i);
                const bool top = cards_[cd].moving || cards_[cd].lifted;
                if (top == (pass == 1) && n < kCards) seq[n++] = cd;
            }
        }
    bool ordered = true;
    for (int i = 1; i < n && ordered; ++i)
        ordered = lv_obj_get_index(cards_[seq[i - 1]].obj) < lv_obj_get_index(cards_[seq[i]].obj);
    if (ordered) return;
    for (int i = 0; i < n; ++i) lv_obj_move_foreground(cards_[seq[i]].obj);
    for (lv_obj_t *f : {frame_select_, frame_hint_src_, frame_hint_dst_, frame_cursor_}) lv_obj_move_foreground(f);
}

bool BoardView::stacking_ok(std::string *why) const
{
    if (!game_) return true;
    const Board &b = game_->board();
    auto fail = [&](const std::string &m) {
        if (why) *why = m;
        return false;
    };
    int top_floor = -1;
    for (int p = 0; p < kPiles; ++p) {
        int prev = -1;
        for (int i = 0; i < b.size(p); ++i) {
            const Card cd = b.card(p, i);
            const int ix = lv_obj_get_index(cards_[cd].obj);
            if (ix <= prev) return fail("pile " + std::to_string(p) + " card " + std::to_string(i) + " (id " + std::to_string(cd) + ", child " + std::to_string(ix) + ") is under the card before it (child " + std::to_string(prev) + ")");
            prev = ix;
            if (cards_[cd].moving || cards_[cd].lifted) top_floor = top_floor < 0 ? ix : std::min(top_floor, ix);
        }
    }
    int flat_max = -1;
    for (int p = 0; p < kPiles; ++p)
        for (int i = 0; i < b.size(p); ++i) {
            const Card cd = b.card(p, i);
            if (!cards_[cd].moving && !cards_[cd].lifted) flat_max = std::max(flat_max, lv_obj_get_index(cards_[cd].obj));
        }
    if (top_floor >= 0 && top_floor < flat_max) return fail("a flying or lifted card (child " + std::to_string(top_floor) + ") is under a resting card (child " + std::to_string(flat_max) + ")");
    return true;
}

void BoardView::check_stacking(const char *where)
{
    if (!test_zcheck_) return;
    std::string why;
    if (stacking_ok(&why)) return;
    std::fprintf(stderr, "ZFAIL stacking after %s: %s\n", where, why.c_str());
    if (zfail_) return;
    zfail_ = lv_label_create(parent_);
    lv_label_set_text(zfail_, "ZFAIL"); /* the harness matches a label's whole text; the reason is on stderr */
    lv_obj_set_size(zfail_, 40, 12);
    lv_obj_set_style_text_opa(zfail_, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(zfail_, 0, 0);
    lv_obj_remove_flag(zfail_, LV_OBJ_FLAG_CLICKABLE);
}

bool BoardView::test_drag(int pile, int idx, int to)
{
    if (!game_ || drag_.active || layout_.card_w <= 0) return false;
    int count = 0;
    const Hit h{pile, idx};
    if (!pickable(h, &count)) return false;
    const Rect r = card_rect(pile, idx);
    drag_.hit = h;
    drag_.count = count;
    drag_.press_x = r.x + r.w / 2;
    drag_.press_y = r.y + r.h / 2;
    begin_drag(drag_.press_x, drag_.press_y);
    check_stacking("drag begin");
    int px = drag_.press_x + layout_.card_w / 3, py = drag_.press_y + layout_.card_h / 3;
    if (to >= 0) {
        Rect z = slot_rect(to);
        if (is_tableau(to) && game_->board().ntab[to - kTableau0] > 0) z = card_rect(to, game_->board().ntab[to - kTableau0] - 1);
        px = z.x + z.w / 2;
        py = z.y + z.h / 2;
    }
    move_drag(px, py);
    check_stacking("drag move");
    end_drag(px, py, to < 0);
    return true;
}

void BoardView::anim_exec_cb(void *var, int32_t v)
{
    auto *self = static_cast<BoardView *>(var);
    for (CardView &c : self->cards_) {
        if (!c.moving) continue;
        c.x = c.x0 + (c.x1 - c.x0) * v / 1024;
        c.y = c.y0 + (c.y1 - c.y0) * v / 1024;
        lv_obj_set_pos(c.obj, c.x, c.y);
    }
}

void BoardView::anim_done_cb(lv_anim_t *a)
{
    auto *self = static_cast<BoardView *>(a->var);
    for (CardView &c : self->cards_) {
        if (!c.moving) continue;
        c.x = c.x1;
        c.y = c.y1;
        c.moving = false;
        lv_obj_set_pos(c.obj, c.x, c.y);
    }
    self->anim_running_ = false;
    self->restack();
    self->check_stacking("anim end");
}

void BoardView::finish_animations()
{
    lv_anim_delete(this, anim_exec_cb);
    for (CardView &c : cards_) {
        if (!c.moving) continue;
        c.x = c.x1;
        c.y = c.y1;
        c.moving = false;
        lv_obj_set_pos(c.obj, c.x, c.y);
    }
    anim_running_ = false;
    restack();
    check_stacking("finish");
}

void BoardView::card_pos(Card c, int *x, int *y) const
{
    if (x) *x = cards_[c].x;
    if (y) *y = cards_[c].y;
}

void BoardView::set_card_visible(Card c, bool visible)
{
    cards_[c].visible = visible;
    if (visible) lv_obj_remove_flag(cards_[c].obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(cards_[c].obj, LV_OBJ_FLAG_HIDDEN);
}

void BoardView::set_backdrop_visible(bool visible)
{
    for (lv_obj_t *o : {felt_}) {
        if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t *s : slots_) {
        if (visible) lv_obj_remove_flag(s, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    }
}

void BoardView::set_card_face(Card c, bool face_up)
{
    cards_[c].face_up = face_up;
    lv_obj_invalidate(cards_[c].obj);
}

/* ------------------------------------------------------------------------------------------------ marks */

void BoardView::set_frame(lv_obj_t *frame, const Rect &r, uint32_t rgb)
{
    const int g = std::max(3, layout_.card_w / 18);
    lv_obj_set_style_border_color(frame, lv_color_hex(rgb), 0);
    lv_obj_set_pos(frame, r.x - g, r.y - g);
    lv_obj_set_size(frame, r.w + 2 * g, r.h + 2 * g);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(frame);
}

void BoardView::show_cursor(const Hit &h, bool run_from_cursor)
{
    if (!h.valid() || !game_) return hide_cursor();
    Rect r = (h.idx >= 0 && run_from_cursor) ? run_rect(h.pile, h.idx) : card_rect(h.pile, h.idx >= 0 ? h.idx : -1);
    if (h.pile == kWaste || h.pile == kStock || is_foundation(h.pile)) r = slot_rect(h.pile);
    if (h.pile == kWaste && h.idx >= 0) r = card_rect(kWaste, h.idx);
    set_frame(frame_cursor_, r, 0x35E0FF);
}

void BoardView::hide_cursor()
{
    lv_obj_add_flag(frame_cursor_, LV_OBJ_FLAG_HIDDEN);
}

void BoardView::show_selection(int pile, int idx)
{
    if (!game_) return;
    set_frame(frame_select_, run_rect(pile, idx), 0x3D8BFF);
}

void BoardView::hide_selection()
{
    lv_obj_add_flag(frame_select_, LV_OBJ_FLAG_HIDDEN);
}

void BoardView::show_hint(int src_pile, int src_idx, int dst_pile)
{
    if (!game_) return;
    const Board &b = game_->board();
    set_frame(frame_hint_src_, src_pile == kStock ? slot_rect(kStock) : run_rect(src_pile, src_idx), 0xFFC83D);
    Rect d;
    if (is_tableau(dst_pile) && b.ntab[dst_pile - kTableau0] > 0) d = card_rect(dst_pile, b.ntab[dst_pile - kTableau0] - 1);
    else d = slot_rect(dst_pile);
    if (dst_pile == kWaste) d = slot_rect(kWaste);
    set_frame(frame_hint_dst_, d, 0xFFC83D);
}

void BoardView::hide_hint()
{
    lv_obj_add_flag(frame_hint_src_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(frame_hint_dst_, LV_OBJ_FLAG_HIDDEN);
}

void BoardView::hide_marks()
{
    hide_cursor();
    hide_selection();
    hide_hint();
}

/* ------------------------------------------------------------------------------------------------ pointer */

void BoardView::surface_event_cb(lv_event_t *e)
{
    auto *self = static_cast<BoardView *>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
        self->on_pointer(code);
}

void BoardView::on_pointer(lv_event_code_t code)
{
    if (!game_) return;
    lv_indev_t *indev = lv_indev_active();
    /* a keypad's Enter also presses the focused surface: only a pointer or a finger is a tap here */
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p{0, 0};
    lv_indev_get_point(indev, &p);
    const int thr = std::max(8, layout_.card_w / 6);
    switch (code) {
    case LV_EVENT_PRESSED:
        drag_.pressed = input_;
        drag_.active = false;
        drag_.press_x = p.x;
        drag_.press_y = p.y;
        drag_.hit = hit_test(p.x, p.y);
        break;
    case LV_EVENT_PRESSING:
        if (!drag_.pressed) break;
        if (!drag_.active) {
            int count = 0;
            const int dx = p.x - drag_.press_x, dy = p.y - drag_.press_y;
            if (dx * dx + dy * dy >= thr * thr && pickable(drag_.hit, &count)) {
                drag_.count = count;
                begin_drag(p.x, p.y);
            }
        } else {
            move_drag(p.x, p.y);
        }
        break;
    case LV_EVENT_RELEASED:
        if (drag_.active) {
            end_drag(p.x, p.y, false);
        } else if (drag_.pressed) {
            const Hit h = drag_.hit;
            if (h.valid()) {
                const std::uint32_t now = lv_tick_get();
                if (have_last_tap_ && last_tap_ == h && now - last_tap_ms_ < 420 && h.idx >= 0 && h.pile != kStock) {
                    have_last_tap_ = false;
                    if (events_.double_tap) events_.double_tap(h);
                } else {
                    have_last_tap_ = true;
                    last_tap_ = h;
                    last_tap_ms_ = now;
                    if (events_.tap) events_.tap(h);
                }
            } else {
                have_last_tap_ = false;
                if (events_.tap) events_.tap(Hit{});
            }
        }
        drag_.pressed = false;
        break;
    case LV_EVENT_PRESS_LOST:
        if (drag_.active) end_drag(p.x, p.y, true);
        drag_.pressed = false;
        break;
    default: break;
    }
}

void BoardView::begin_drag(int px, int py)
{
    finish_animations();
    const Hit h = drag_.hit;
    const Board &b = game_->board();
    drag_.active = true;
    drag_.ids.clear();
    drag_.ox.clear();
    drag_.oy.clear();
    for (int i = 0; i < drag_.count; ++i) {
        const int idx = h.idx + i;
        const Card c = b.card(h.pile, idx);
        drag_.ids.push_back(c);
        drag_.ox.push_back(cards_[c].x);
        drag_.oy.push_back(cards_[c].y);
    }
    for (int id : drag_.ids) {
        cards_[id].lifted = true;
        lv_obj_move_foreground(cards_[id].obj);
        lv_obj_invalidate(cards_[id].obj);
    }
    have_last_tap_ = false;
    if (events_.drag_started) events_.drag_started();
    move_drag(px, py);
}

void BoardView::move_drag(int px, int py)
{
    const int dx = px - drag_.press_x, dy = py - drag_.press_y;
    const int sw = screen_w(), sh = screen_h();
    for (size_t i = 0; i < drag_.ids.size(); ++i) {
        CardView &c = cards_[drag_.ids[i]];
        int x = drag_.ox[i] + dx, y = drag_.oy[i] + dy;
        x = std::min(std::max(x, 0), sw - layout_.card_w);
        y = std::min(std::max(y, layout_.hud.y), sh - layout_.card_h);
        if (x == c.x && y == c.y) continue;
        c.x = x;
        c.y = y;
        lv_obj_set_pos(c.obj, x, y);
    }
}

void BoardView::end_drag(int px, int py, bool cancelled)
{
    drag_.active = false;
    const Hit from = drag_.hit;
    const int count = drag_.count;
    const CardView &first = cards_[drag_.ids.front()];
    const double cx = first.x + layout_.card_w / 2.0, cy = first.y + layout_.card_h / 2.0;
    int best = -1;
    double best_d = 1e9;
    if (!cancelled && events_.can_move) {
        const Hit under = hit_test(px, py);
        for (int to = kFoundation0; to < kPiles; ++to) {
            if (to == from.pile || !events_.can_move(from.pile, to, count)) continue;
            Rect z = slot_rect(to);
            if (is_tableau(to) && game_->board().ntab[to - kTableau0] > 0) {
                const int n = game_->board().ntab[to - kTableau0];
                z = card_rect(to, n - 1);
            }
            const double dx = (cx - (z.x + z.w / 2.0)) / layout_.card_w;
            const double dy = (cy - (z.y + z.h / 2.0)) / layout_.card_h;
            double d = std::sqrt(dx * dx + dy * dy * 0.56);
            if (under.valid() && under.pile == to) d = std::min(d, 0.05);
            if (d < best_d) {
                best_d = d;
                best = to;
            }
        }
    }
    for (int id : drag_.ids) {
        cards_[id].lifted = false;
        lv_obj_invalidate(cards_[id].obj);
    }
    drag_.ids.clear();
    restack(); /* the dropped cards are no longer lifted: they take their place in their pile's order */
    check_stacking("drop");
    if (best >= 0 && best_d <= 1.0 && events_.move) {
        Move m;
        m.from = static_cast<std::uint8_t>(from.pile);
        m.to = static_cast<std::uint8_t>(best);
        m.count = static_cast<std::uint8_t>(count);
        events_.move(m);
    } else {
        sync(*game_, true); /* the cards fly back */
    }
}

/* ------------------------------------------------------------------------------------------------ test probes */

void BoardView::dump_probes()
{
    if (!probes_ || !game_) return;
    lv_obj_clean(probes_);
    const char *dump_path = std::getenv("KLONDIKE_TEST_DUMP");
    std::FILE *dump = dump_path && *dump_path ? std::fopen(dump_path, "a") : nullptr;
    auto add = [&](const std::string &name, const Rect &r) {
        if (r.w <= 0 || r.h <= 0) return;
        lv_obj_t *l = lv_label_create(probes_);
        lv_label_set_text(l, name.c_str());
        lv_obj_set_style_text_opa(l, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_pos(l, r.x, r.y);
        lv_obj_set_size(l, r.w, r.h);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        if (dump) std::fprintf(dump, "probe %s %d %d %d %d\n", name.c_str(), r.x, r.y, r.w, r.h);
    };
    const Board &b = game_->board();
    add("@S", slot_rect(kStock));
    add("@W", card_rect(kWaste, b.tp > 0 ? b.tp - 1 : -1));
    for (int f = 0; f < kFoundations; ++f) add("@F" + std::to_string(f + 1), slot_rect(foundation_pile(f)));
    for (int c = 0; c < kColumns; ++c) {
        const int p = tableau_pile(c);
        const int n = b.ntab[c];
        const std::string base = "@T" + std::to_string(c + 1);
        if (n == 0) add(base + ".slot", slot_rect(p));
        for (int i = 0; i < n; ++i) add(base + "." + std::to_string(i), visible_rect(p, i));
        if (n > 0) {
            add(base + ".top", card_rect(p, n - 1));
            add(base + ".first", visible_rect(p, b.ndown[c]));
        }
    }
    if (dump) {
        std::fprintf(dump, "layout card %dx%d side=%d hud=%d,%d,%d,%d dock=%d,%d,%d,%d\n", layout_.card_w, layout_.card_h, layout_.side_dock,
                     layout_.hud.x, layout_.hud.y, layout_.hud.w, layout_.hud.h, layout_.dock.x, layout_.dock.y, layout_.dock.w, layout_.dock.h);
        std::fclose(dump);
    }
}

} // namespace kui
