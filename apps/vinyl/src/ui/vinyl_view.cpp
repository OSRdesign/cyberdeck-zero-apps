/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the pages (see vinyl_view.hpp). Every position comes from core/layout (the drafts' numbers) through the Fit of
 * the canvas; nothing is placed from a constant of this file but the drafts' own proportions (paddings, radii, font
 * sizes), scaled.
 */
#include "vinyl_view.hpp"

#include "seek/seek_scrub.hpp"

#include "gfx.hpp"
#include "theme.hpp"

#include "app.hpp"
#include "cp0_ui_metrics_lvgl.h"
#include "cp0_ui_roller.h"
#include "cp0_ui_widgets.hpp"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace vinyl {
namespace ui {

using layout::R;

namespace {

int ri(double v) { return static_cast<int>(std::lround(v)); }

/* ------------------------------------------------------------------------------------------------ hit boxes */

struct HitData {
    std::function<void()> fn;
};

void hit_click_cb(lv_event_t *e) {
    auto *d = static_cast<HitData *>(lv_obj_get_user_data(lv_event_get_target_obj(e)));
    if (d && d->fn) d->fn();
}
void hit_delete_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    delete static_cast<HitData *>(lv_obj_get_user_data(o));
    lv_obj_set_user_data(o, nullptr);
}

/* A transparent clickable box over the visual (x, y, w, h), grown to at least min_w x min_h around its centre. */
lv_obj_t *hit(lv_obj_t *parent, int x, int y, int w, int h, int min_w, int min_h, std::function<void()> fn) {
    const int hw = std::max(w, min_w), hh = std::max(h, min_h);
    lv_obj_t *o = lv_obj_create(parent);
    plain(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x - (hw - w) / 2, y - (hh - h) / 2);
    lv_obj_set_size(o, hw, hh);
    lv_obj_set_user_data(o, new HitData{std::move(fn)});
    lv_obj_add_event_cb(o, hit_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(o, hit_delete_cb, LV_EVENT_DELETE, nullptr);
    return o;
}

/* The press phases of a held button (the transport press: previous and next act on the release, a long press scans). */
enum Phase { kPress = 0, kRelease = 1, kCancel = 2 };

struct HoldData {
    std::function<void(int)> fn;
};

void hold_event_cb(lv_event_t *e) {
    auto *d = static_cast<HoldData *>(lv_obj_get_user_data(lv_event_get_target_obj(e)));
    if (!d || !d->fn) return;
    const lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) d->fn(kPress);
    else if (c == LV_EVENT_RELEASED) d->fn(kRelease);
    else if (c == LV_EVENT_PRESS_LOST) d->fn(kCancel); /* the finger slid off: no short action */
}
void hold_delete_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    delete static_cast<HoldData *>(lv_obj_get_user_data(o));
    lv_obj_set_user_data(o, nullptr);
}

/* hit() for a held button: press, release and cancel instead of a click */
lv_obj_t *hold_hit(lv_obj_t *parent, int x, int y, int w, int h, int min_w, int min_h, std::function<void(int)> fn) {
    const int hw = std::max(w, min_w), hh = std::max(h, min_h);
    lv_obj_t *o = lv_obj_create(parent);
    plain(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x - (hw - w) / 2, y - (hh - h) / 2);
    lv_obj_set_size(o, hw, hh);
    lv_obj_set_user_data(o, new HoldData{std::move(fn)});
    lv_obj_add_event_cb(o, hold_event_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(o, hold_event_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(o, hold_event_cb, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(o, hold_delete_cb, LV_EVENT_DELETE, nullptr);
    return o;
}

/* places a touch area (screen px, rounded edge by edge so that two areas that meet never overlap) */
void place_area(lv_obj_t *o, const layout::R &a) {
    const int x0 = ri(a.x), y0 = ri(a.y), x1 = ri(a.right()), y1 = ri(a.bottom());
    lv_obj_set_pos(o, x0, y0);
    lv_obj_set_size(o, std::max(1, x1 - x0), std::max(1, y1 - y0));
}

/* ------------------------------------------------------------------------------------------------ images */

struct Img {
    PixelImage px;
    lv_image_dsc_t dsc{};
    void bind(lv_obj_t *obj) {
        std::memset(&dsc, 0, sizeof dsc);
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = px.rgb565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
        dsc.header.w = static_cast<uint32_t>(px.w);
        dsc.header.h = static_cast<uint32_t>(px.h);
        dsc.header.stride = static_cast<uint32_t>(px.w * (px.rgb565 ? 2 : 4));
        dsc.data_size = static_cast<uint32_t>(px.px.size());
        dsc.data = px.px.data();
        lv_image_cache_drop(&dsc);
        lv_image_set_src(obj, &dsc);
    }
};

lv_obj_t *image(lv_obj_t *parent, int x, int y) {
    lv_obj_t *o = lv_image_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    return o;
}

/* ------------------------------------------------------------------------------------------------ the row list */

struct RowW {
    lv_obj_t *row = nullptr, *dot = nullptr, *chev = nullptr, *text = nullptr, *value = nullptr;
    int index = -1;
};

/* A list of fixed-height rows that only builds the rows in view (a folder or a queue can hold thousands). */
class RowList {
public:
    RowList(lv_obj_t *parent, int x, int y, int w, int h, int row_h, double s, int text_px, int value_px, double dot_x,
            double text_x, double value_r, double chev_r, std::function<void(int)> on_tap)
        : row_h_(row_h), s_(s), w_(w), text_px_(text_px), value_px_(value_px), dot_x_(dot_x), text_x_(text_x),
          value_r_(value_r), chev_r_(chev_r), on_tap_(std::move(on_tap)) {
        cont_ = lv_obj_create(parent);
        plain(cont_);
        lv_obj_set_pos(cont_, x, y);
        lv_obj_set_size(cont_, w, h);
        lv_obj_add_flag(cont_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(cont_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(cont_, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_width(cont_, std::max(2, ri(3 * s)), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(cont_, lv_color_hex(col::kRowValue), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(cont_, LV_OPA_50, LV_PART_SCROLLBAR);
        lv_obj_add_event_cb(cont_, scroll_cb, LV_EVENT_SCROLL, this);
        spacer_ = lv_obj_create(cont_);
        plain(spacer_);
        lv_obj_set_size(spacer_, 1, 1);
        visible_ = std::max(1, h / std::max(1, row_h)) + 2;
        for (int i = 0; i < visible_; ++i) pool_.push_back(make_row());
    }

    void set_rows(const std::vector<RowView> &rows, int selected, bool reset_scroll, int scroll_to) {
        const bool same = rows == rows_;
        rows_ = rows;
        selected_ = selected;
        lv_obj_set_pos(spacer_, 0, std::max(0, static_cast<int>(rows_.size()) * row_h_ - 1));
        if (reset_scroll) lv_obj_scroll_to_y(cont_, 0, LV_ANIM_OFF);
        if (scroll_to >= 0 && scroll_to < static_cast<int>(rows_.size())) {
            const int max_y = std::max(0, static_cast<int>(rows_.size()) * row_h_ - lv_obj_get_height(cont_));
            lv_obj_scroll_to_y(cont_, std::min(max_y, scroll_to * row_h_), LV_ANIM_OFF);
        }
        (void)same;
        if (!reset_scroll && scroll_to < 0 && selected >= 0) { /* keep the highlight in view (a row moved) */
            const int top = lv_obj_get_scroll_y(cont_), h = lv_obj_get_height(cont_);
            if (selected * row_h_ < top) lv_obj_scroll_to_y(cont_, selected * row_h_, LV_ANIM_OFF);
            else if ((selected + 1) * row_h_ > top + h) lv_obj_scroll_to_y(cont_, (selected + 1) * row_h_ - h, LV_ANIM_OFF);
        }
        rebind(true);
    }

    void set_selected(int i, bool ensure_visible) {
        selected_ = i;
        if (ensure_visible && i >= 0) {
            const int top = lv_obj_get_scroll_y(cont_), h = lv_obj_get_height(cont_);
            if (i * row_h_ < top) lv_obj_scroll_to_y(cont_, i * row_h_, LV_ANIM_OFF);
            else if ((i + 1) * row_h_ > top + h) lv_obj_scroll_to_y(cont_, (i + 1) * row_h_ - h, LV_ANIM_OFF);
        }
        rebind(true);
    }

    int size() const { return static_cast<int>(rows_.size()); }
    lv_obj_t *obj() const { return cont_; }

private:
    RowW make_row() {
        RowW r;
        r.row = lv_obj_create(cont_);
        plain(r.row);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_EVENT_BUBBLE); /* the drag scrolls the list */
        lv_obj_set_size(r.row, w_, row_h_);
        lv_obj_set_style_bg_color(r.row, lv_color_hex(col::kRowSel), 0);
        lv_obj_set_style_bg_opa(r.row, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(r.row, row_click_cb, LV_EVENT_CLICKED, this);
        const int dot = std::max(4, ri(10 * s_));
        r.dot = box(r.row, ri(dot_x_), (row_h_ - dot) / 2, dot, dot, col::kWhite, LV_RADIUS_CIRCLE);
        const int cw = std::max(8, ri(16 * s_));
        r.chev = icon_obj(r.row, Icon::Chevron, w_ - ri(chev_r_) - cw, (row_h_ - cw) / 2, cw, cw, cw, col::kRowValue);
        r.text = label(r.row, Face::Bold, text_px_, col::kRowText, ri(text_x_), 0, 10);
        lv_obj_set_style_text_line_space(r.text, 0, 0);
        r.value = label(r.row, Face::Mono, value_px_, col::kRowValue, 0, 0, 10, LV_TEXT_ALIGN_RIGHT); /* one line, dots */
        lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
        return r;
    }

    void bind(RowW &w, int i) {
        const RowView &v = rows_[static_cast<size_t>(i)];
        w.index = i;
        lv_obj_set_y(w.row, i * row_h_);
        show(w.row, true);
        lv_obj_set_style_bg_opa(w.row, i == selected_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        /* the row's kind (design QA D-45): a chevron opens (folders, playlists), a dot acts (tracks, queue rows, outputs;
         * blue for the playing track and the chosen output), no mark informs */
        const bool dot = v.mark == RowView::Mark::Dot;
        const bool chev = v.mark == RowView::Mark::Chevron;
        show(w.dot, dot);
        if (dot) set_bg(w.dot, v.dimmed ? col::kDotDim : v.playing ? col::kPlaying : col::kWhite);
        show(w.chev, chev);
        if (chev) icon_set(w.chev, Icon::Chevron, v.dimmed ? col::kDotDim : col::kRowValue);
        const lv_font_t *vf = font(v.mono_value ? Face::Mono : Face::Bold, value_px_);
        if (lv_obj_get_style_text_font(w.value, LV_PART_MAIN) != vf) lv_obj_set_style_text_font(w.value, vf, 0);
        set_text(w.value, v.value);
        const lv_font_t *tf = font(Face::Bold, text_px_);
        const int right = chev ? ri(chev_r_) + ri(16 * s_) + ri(4 * s_) : ri(value_r_);
        /* the name and the value share the row; when both do not fit, the name keeps its width up to 60 % of the room
         * and the value ends with dots (a narrow screen: "Auto" before "follows system: Evolve 65") */
        const int room = std::max(20, w_ - ri(text_x_) - right - ri(8 * s_));
        const int vnat = v.value.empty() ? 0 : text_width(vf, v.value) + 2;
        const int tnat = text_width(tf, v.text) + 2;
        int vw = vnat, tw = room - vnat;
        if (vnat > 0 && tnat + vnat > room) {
            tw = std::min(tnat, std::max(room - vnat, room * 3 / 5));
            vw = std::max(0, room - tw);
        }
        tw = std::max(10, tw);
        show(w.value, vw > 0);
        lv_obj_set_size(w.value, std::max(1, vw), lv_font_get_line_height(vf));
        lv_obj_set_pos(w.value, w_ - right - vw, (row_h_ - lv_font_get_line_height(vf)) / 2);
        const uint32_t tc = v.dimmed ? col::kRowDim : col::kRowText;
        lv_obj_set_style_text_color(w.text, lv_color_hex(tc), 0);
        lv_obj_set_style_text_color(w.value, lv_color_hex(v.dimmed ? col::kDotDim : col::kRowValue), 0);
        set_text(w.text, v.text);
        lv_obj_set_width(w.text, tw);
        lv_obj_set_y(w.text, (row_h_ - lv_font_get_line_height(tf)) / 2);
    }

    void rebind(bool force) {
        const int top = lv_obj_get_scroll_y(cont_);
        const int first = std::max(0, top / std::max(1, row_h_));
        if (!force && first == first_) return;
        first_ = first;
        for (int k = 0; k < visible_; ++k) {
            const int i = first + k;
            if (i < static_cast<int>(rows_.size())) bind(pool_[static_cast<size_t>(k)], i);
            else {
                pool_[static_cast<size_t>(k)].index = -1;
                show(pool_[static_cast<size_t>(k)].row, false);
            }
        }
    }

    static void scroll_cb(lv_event_t *e) { static_cast<RowList *>(lv_event_get_user_data(e))->rebind(false); }
    static void row_click_cb(lv_event_t *e) {
        auto *self = static_cast<RowList *>(lv_event_get_user_data(e));
        lv_obj_t *row = lv_event_get_target_obj(e);
        for (const RowW &w : self->pool_)
            if (w.row == row && w.index >= 0 && self->on_tap_) {
                self->on_tap_(w.index);
                return;
            }
    }

    lv_obj_t *cont_ = nullptr, *spacer_ = nullptr;
    int row_h_;
    double s_;
    int w_, text_px_, value_px_;
    double dot_x_, text_x_, value_r_, chev_r_;
    std::function<void(int)> on_tap_;
    std::vector<RowW> pool_;
    std::vector<RowView> rows_;
    int visible_ = 1, first_ = -1, selected_ = -1;
};

/* ------------------------------------------------------------------------------------------------ chips */

struct Chip {
    lv_obj_t *box = nullptr, *icon = nullptr, *text = nullptr;
    int x = 0, y = 0, h = 0, pad = 0, gap = 0, icon_px = 0, px = 0;
    Face face = Face::Bold;
    uint32_t ink = col::kInk;
    int max_w = 0;
    int width = 0;
    bool scroll = false; /* the output chip: a name too long for its row scrolls, never ends with dots */

    void build(lv_obj_t *parent, int x_, int y_, int h_, int pad_, int gap_, int icon_px_, int px_, uint32_t bg, uint32_t ink_,
               int radius, int max_w_) {
        x = x_;
        y = y_;
        h = h_;
        pad = pad_;
        gap = gap_;
        icon_px = icon_px_;
        px = px_;
        ink = ink_;
        max_w = max_w_;
        box = ui::box(parent, x, y, 10, h, bg, radius);
        icon = icon_obj(box, Icon::Bluetooth, pad, (h - icon_px) / 2, icon_px, icon_px, icon_px, ink);
        text = label(box, face, px, ink, pad, 0, 0);
        lv_obj_set_height(text, lv_font_get_line_height(font(face, px))); /* one line, dots when the row is full */
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
    }

    void set_plain() {
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        pad = 0;
    }

    /* information on a flat light fill (the 640x480 draft's format and Gapless chips): no key look, no action */
    void set_flat() {
        lv_obj_set_style_bg_color(box, lv_color_hex(col::kInfoChip), 0);
        ink = col::kInk2;
        lv_obj_set_style_text_color(text, lv_color_hex(ink), 0);
    }

    void set_ink(uint32_t c) {
        ink = c;
        lv_obj_set_style_text_color(text, lv_color_hex(ink), 0);
    }

    void set_scroll() {
        scroll = true;
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    }

    /* the width the text needs (0: no chip) */
    int natural(const std::string &t, Icon ic) const {
        if (t.empty() || !box) return 0;
        return pad + (ic != Icon::None ? icon_px + gap : 0) + text_width(font(face, px), t) + pad + 1;
    }

    /* the chip's width follows its text; `ic`: the Bluetooth mark of the output, the speaker-off mark of a muted level */
    void set(const std::string &t, Icon ic, int at_x = -1) {
        const bool bt = ic != Icon::None;
        show(box, !t.empty());
        if (t.empty()) {
            width = 0;
            return;
        }
        if (at_x >= 0) x = at_x;
        const lv_font_t *f = font(face, px);
        const int iw = bt ? icon_px + gap : 0;
        int tw = text_width(f, t) + 1;
        int w = pad + iw + tw + pad;
        if (max_w > 0 && w > max_w) {
            w = max_w;
            tw = w - 2 * pad - iw;
            lv_label_set_long_mode(text, scroll ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_DOTS);
        }
        lv_obj_set_width(text, std::max(1, tw + 1));
        show(icon, bt);
        if (bt) icon_set(icon, ic, ink);
        lv_obj_set_pos(text, pad + iw, (h - lv_font_get_line_height(f)) / 2);
        set_text(text, t);
        lv_obj_set_pos(box, x, y);
        lv_obj_set_width(box, w);
        width = w;
    }
};

/* a button: its drawn box, its icon (and word), and a hit box at least the touch target */
struct Button {
    lv_obj_t *vis = nullptr, *icon = nullptr, *word = nullptr, *hitbox = nullptr;
    Icon ic = Icon::None;
    uint32_t ink = col::kInk;
};

Icon icon_of(const std::string &id) {
    if (id == "shuffle") return Icon::Shuffle;
    if (id == "prev") return Icon::Prev;
    if (id == "play") return Icon::Pause;
    if (id == "next") return Icon::Next;
    if (id == "repeat") return Icon::Repeat;
    if (id == "queue") return Icon::Queue;
    if (id == "sleep") return Icon::Sleep;
    if (id == "browse") return Icon::Browse;
    if (id == "output") return Icon::Output;
    if (id == "eq") return Icon::Equalizer;
    return Icon::None;
}

const char *word_of(const std::string &id) {
    if (id == "queue") return "Queue";
    if (id == "browse") return "Browse";
    if (id == "output") return "Output";
    if (id == "eq") return "Equalizer";
    return "";
}


}  // namespace

/* ------------------------------------------------------------------------------------------------ the view */

struct View::Impl {
    lv_obj_t *screen = nullptr;
    Actions act;
    int W = 0, H = 0;
    layout::NowGeom g;
    Fit fit;
    int target = 44;
    int list_row = 24; /* the toolkit's LIST_ROW: 5 mm */
    cp0_ui_roller_page_t tk{};
    std::unique_ptr<cp0::ui::Page> page; /* the toolkit's header: title, Esc, the shared status strip */
    int body_y = 0;
    Page cur = Page::Now;

    /* pages */
    lv_obj_t *now_root = nullptr, *br_root = nullptr, *qu_root = nullptr, *out_root = nullptr, *not_root = nullptr;

    /* now playing */
    lv_obj_t *panel = nullptr;
    lv_obj_t *disc = nullptr, *labelimg = nullptr, *sleeve = nullptr;
    Img disc_img, label_img, sleeve_img;
    std::shared_ptr<const PixelImage> cover;
    Img cover_img;
    bool cover_shown = false;
    lv_obj_t *title = nullptr, *artist = nullptr, *album = nullptr;
    Chip chips[4]; /* format, (the old Vol chip: none), output, gapless */
    /* the output button and the volume group (owner, 2026-11-13; every class but the large one has the button) */
    lv_obj_t *outb_box = nullptr, *outb_icon = nullptr, *outb_text = nullptr, *outb_chev = nullptr;
    Button grp_minus, grp_plus;
    lv_obj_t *grp_value_box = nullptr, *grp_value = nullptr, *grp_glyph = nullptr;
    lv_obj_t *out_hit = nullptr; /* the output button (or the large class's output chip) opens the chooser */
    lv_obj_t *time = nullptr, *remain = nullptr, *seek_track = nullptr, *seek_fill = nullptr, *seek_knob = nullptr;
    lv_obj_t *seek_hit = nullptr;
    SeekScrub scrub;                  /* the seek bar's tap and drag (core/seek) */
    double hold_frac = -1;            /* after a release: the target shown until the engine's position gets there */
    uint32_t hold_until = 0;

    static void seek_event_cb(lv_event_t *e) { static_cast<Impl *>(lv_event_get_user_data(e))->seek_event(lv_event_get_code(e)); }

    void seek_event(lv_event_code_t code) {
        lv_point_t p{};
        if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
        scrub.set_bar(seek_x, seek_w);
        if (code == LV_EVENT_PRESSED) scrub.press(p.x);
        else if (code == LV_EVENT_PRESSING) scrub.move(p.x);
        else if (code == LV_EVENT_PRESS_LOST) scrub.cancel();
        else if (code == LV_EVENT_RELEASED) {
            double f = 0;
            if (!scrub.release(p.x, f)) return;
            hold_frac = f; /* shown until the engine is there (at most 0.8 s): the knob never jumps back */
            hold_until = lv_tick_get() + 800;
            if (act.seek_to) act.seek_to(f);
        }
        draw_time_row(last_now);
    }
    int seek_x = 0, seek_w = 1;
    std::map<std::string, Button> btns;
    lv_obj_t *repeat_dash = nullptr, *shuffle_dash = nullptr, *repeat_one = nullptr;
    lv_obj_t *up_box = nullptr, *up_cap = nullptr, *up_text = nullptr;
    lv_obj_t *qpane = nullptr, *qheading = nullptr;
    std::unique_ptr<RowList> qlist;
    uint64_t track_gen = UINT64_MAX;
    uint64_t queue_ver = UINT64_MAX;
    int queue_sel = -2;
    double angle = 0;
    uint32_t last_tick = 0;
    int disc_out_x = 0, disc_in_x = 0;
    NowView last_now;

    /* list pages */
    struct ListW {
        lv_obj_t *root = nullptr;
        lv_obj_t *heading = nullptr, *sort_box = nullptr, *sort_text = nullptr;
        std::unique_ptr<RowList> list;
        lv_obj_t *strip = nullptr, *strip_icon = nullptr, *strip_text = nullptr;
        /* browser mini player */
        lv_obj_t *mini_disc = nullptr, *mini_title = nullptr, *mini_sub = nullptr, *mini_time = nullptr, *mini_bar = nullptr,
                 *mini_fill = nullptr;
        int mini_text_w = 0;
        Button mini_play, mini_next;
        lv_obj_t *mini_open_hit = nullptr;
        Img mini_disc_img, mini_label_img;
        int strip_y0 = 0, strip_h0 = 0, list_y = 0; /* the output page's hint strip as laid out (it grows to its text) */
        std::vector<Button> qbtns;
        uint64_t rows_gen = UINT64_MAX, rows_ver = UINT64_MAX;
        int rows = 0, sel = -2;
    };
    ListW br, qu, out;

    /* notice */
    lv_obj_t *notice = nullptr, *notice_icon = nullptr, *notice_text = nullptr, *notice_hit = nullptr;
    lv_timer_t *notice_timer = nullptr;

    int X(double x) const { return fit.x(x); }
    int Y(double y) const { return fit.y(y); }
    int Wd(const R &r) const { return fit.w(r.x, r.w); }
    int Hd(const R &r) const { return fit.h(r.y, r.h); }
    int P(double v) const { return std::max(1, ri(v * fit.s)); }

    lv_obj_t *page_root() {
        lv_obj_t *o = lv_obj_create(page->root());
        plain(o);
        lv_obj_set_pos(o, 0, 0);
        lv_obj_set_size(o, W, H);
        return o;
    }

    /* the touch areas of now playing, by kind (layout_now_hits) */
    enum HitKind { kKey, kBottom, kChip, kSeek };
    struct NowHit {
        lv_obj_t *obj = nullptr;
        R drawn;
        HitKind kind = kKey;
    };
    std::vector<NowHit> now_hits;
    int hits_sig[4] = {-1, -1, -1, -1};

    /* Every area grown to the target around its drawing, then laid out by layout::place_now_hits: neighbours in a row
     * meet in the middle of their gaps, the rows meet at boundaries in the gaps between them, the target high where the
     * screen has the room (design QA run 3: no two areas overlap; tests/test_hits.cpp checks every layout). */
    /* the transport keys' shift (layout::now_keys_shift): a few px down where the rows above them miss the target */
    int keys_dy = 0;

    void layout_now_hits() {
        layout::NowHitSet set;
        std::vector<NowHit *> pk, pb, ps, pc;
        for (NowHit &e : now_hits) {
            if (!e.obj || e.drawn.w <= 0) continue;
            const layout::HitArea a{e.drawn, layout::grow_hit(e.drawn, e.kind == kSeek ? 0 : target, target)};
            if (e.kind == kKey) {
                pk.push_back(&e);
                set.keys.push_back(a);
            } else if (e.kind == kBottom) {
                pb.push_back(&e);
                set.bottom.push_back(a);
            } else if (e.kind == kSeek) {
                ps.push_back(&e);
                set.seek.push_back(a);
            } else if (e.kind == kChip) {
                pc.push_back(&e);
                set.chips.push_back(a);
            }
        }
        layout::place_now_hits(set, target, body_y, W, H);
        for (size_t i = 0; i < pk.size(); ++i) place_area(pk[i]->obj, set.keys[i].area);
        for (size_t i = 0; i < pb.size(); ++i) place_area(pb[i]->obj, set.bottom[i].area);
        for (size_t i = 0; i < ps.size(); ++i) place_area(ps[i]->obj, set.seek[i].area);
        for (size_t i = 0; i < pc.size(); ++i) place_area(pc[i]->obj, set.chips[i].area);
    }

    Button make_button(lv_obj_t *root, const layout::Btn &b, HitKind kind) {
        Button bt;
        const int x = X(b.box.x), y = Y(b.box.y) + (kind == kKey ? keys_dy : 0), w = Wd(b.box), h = Hd(b.box);
        bt.vis = box(root, x, y, w, h, b.teal ? col::kTeal : col::kButton, P(8));
        bt.ink = b.teal ? col::kWhite : col::kInk;
        bt.ic = icon_of(b.id);
        const int ip = P(b.icon);
        if (b.label) {
            const lv_font_t *f = font(Face::Bold, P(13));
            const int tw = text_width(f, word_of(b.id.c_str()));
            const int total = ip + P(8) + tw;
            bt.icon = icon_obj(bt.vis, bt.ic, (w - total) / 2, (h - ip) / 2, ip, ip, ip, bt.ink);
            bt.word = label(bt.vis, Face::Bold, P(13), bt.ink, (w - total) / 2 + ip + P(8), (h - lv_font_get_line_height(f)) / 2, 0);
            set_text(bt.word, word_of(b.id.c_str()));
        } else {
            bt.icon = icon_obj(bt.vis, bt.ic, (w - ip) / 2, (h - ip) / 2, ip, ip, ip, bt.ink);
        }
        if (!b.enabled) {
            lv_obj_set_style_opa(bt.vis, LV_OPA_40, 0); /* a P2 feature: drawn, no action */
            return bt;
        }
        const std::string id = b.id;
        if (id == "prev" || id == "next") /* the transport press: acts on the release, a long press scans */
            bt.hitbox = hold_hit(root, x, y, w, h, target, target, [this, id](int ph) { transport(id == "next", ph); });
        else
            bt.hitbox = hit(root, x, y, w, h, target, target, [this, id] { press(id); });
        now_hits.push_back(NowHit{bt.hitbox, R{double(x), double(y), double(w), double(h)}, kind});
        return bt;
    }

    void transport(bool next, int phase) {
        if (act.transport) act.transport(next, phase);
    }

    void press(const std::string &id) {
        if (id == "play") {
            if (act.toggle) act.toggle();
        } else if (id == "prev") {
            if (act.prev) act.prev();
        } else if (id == "next") {
            if (act.next) act.next();
        } else if (id == "shuffle") {
            if (act.shuffle) act.shuffle();
        } else if (id == "repeat") {
            if (act.repeat) act.repeat();
        } else if (act.button) {
            act.button(id);
        }
    }

    /* ---------------------------------------------------------------------------------------- now playing */

    void build_now() {
        now_root = page_root();
        lv_obj_t *r = now_root;
        panel = box(r, X(g.panel.x), Y(g.panel.y), Wd(g.panel), Hd(g.panel), col::kCream, P(10));

        if (g.has_queue_pane) {
            qpane = box(r, X(g.queue_pane.x), Y(g.queue_pane.y), Wd(g.queue_pane), Hd(g.queue_pane), col::kList, P(8));
            qheading = label(r, Face::Bold, P(g.queue_heading_px), col::kRowValue, X(g.queue_pane.x + 14), Y(g.queue_pane.y + 8),
                             Wd(g.queue_pane) - P(28), LV_TEXT_ALIGN_LEFT, ri(0.06 * g.queue_heading_px * fit.s));
            const int ly = Y(g.queue_pane.y + 32);
            const int lh = Y(g.queue_pane.bottom()) - ly;
            qlist = std::make_unique<RowList>(r, X(g.queue_pane.x), ly, Wd(g.queue_pane), std::max(1, lh),
                                              std::max(P(g.queue_row_h), list_row),
                                              fit.s, P(14), P(12), 14 * fit.s, 38 * fit.s, 14 * fit.s, 14 * fit.s,
                                              [this](int i) {
                                                  if (act.row) act.row(Page::Now, i);
                                              });
        }

        /* the record behind the sleeve */
        const int dd = Wd(g.disc);
        disc_img.px = render_disc(dd, fit.s);
        disc_out_x = X(g.disc.x);
        disc_in_x = X(g.sleeve.x);
        disc = image(r, disc_out_x, Y(g.disc.y));
        disc_img.bind(disc);
        const int ld = std::max(4, ri(g.label_d * fit.s)), hd = std::max(2, ri(g.hole_d * fit.s));
        label_img.px = render_label(ld, hd, fit.s);
        labelimg = image(disc, (dd - ld) / 2, (dd - ld) / 2);
        label_img.bind(labelimg);
        lv_image_set_pivot(labelimg, ld / 2, ld / 2);
        lv_image_set_antialias(labelimg, true);
        const int sd = Wd(g.sleeve);
        sleeve_img.px = render_sleeve(sd);
        sleeve = image(r, X(g.sleeve.x), Y(g.sleeve.y));
        sleeve_img.bind(sleeve);
        lv_obj_set_style_shadow_width(sleeve, P(6), 0);
        lv_obj_set_style_shadow_offset_x(sleeve, P(2), 0);
        lv_obj_set_style_shadow_color(sleeve, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(sleeve, 89, 0); /* rgba(0,0,0,.35) */
        lv_obj_set_style_radius(sleeve, P(4), 0);

        /* title, artist, album */
        auto make_text = [&](const layout::Text &t, uint32_t c) {
            lv_obj_t *l = label(r, t.bold ? Face::Bold : Face::Medium, P(t.px), c, X(t.box.x), Y(t.box.y), Wd(t.box));
            if (t.lines == 2) {
                const lv_font_t *f = font(t.bold ? Face::Bold : Face::Medium, P(t.px));
                const int lh = P(t.lh);
                lv_obj_set_style_text_line_space(l, lh - lv_font_get_line_height(f), 0);
                lv_obj_set_height(l, 2 * lh);
                lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            }
            return l;
        };
        title = make_text(g.title, col::kInk);
        if (g.title.lines == 1) lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        artist = make_text(g.artist, col::kInk2);
        album = make_text(g.album, col::kInk3);

        /* chips */
        const int chip_max = g.chips_in_row ? Wd(g.panel) - 2 * (X(g.chip0.x) - X(g.panel.x)) : X(g.panel.right()) - X(g.chip0.x) - P(12);
        /* the chip rows of the class (layout.hpp): the format and Gapless are information (plain text at the column's
         * edge; flat on the medium class's light fill; the small class's one line in the album's ink); the large class's
         * output chip opens the chooser */
        for (size_t row = 0; row < g.chip_layout.size(); ++row) {
            for (int id : g.chip_layout[row]) {
                if (id != 0 && id != 2 && id != 3) continue;
                const int cy = Y(g.chip0.y + static_cast<double>(row) * g.chip_step);
                chips[id].build(r, X(g.chip0.x), cy, P(g.chip0.h), g.chip_pad > 0 ? P(g.chip_pad) : 0, P(g.chip_gap), P(g.chip_icon),
                                P(g.chip_px), col::kChip, col::kInk, P(5), chip_max);
                if (id == 2) chips[id].set_scroll();
                if (id == 0 || id == 3) {
                    if (g.chip_flat) chips[id].set_flat();
                    else chips[id].set_plain();
                    if (g.gapless_in_format) chips[id].set_ink(col::kInk3);
                }
            }
        }
        if (g.out_button_on) build_out_button(r);

        out_hit = hit(r, 0, 0, 1, 1, 0, 0, [this] {
            if (act.button) act.button("output");
        });
        now_hits.push_back(NowHit{out_hit, R{}, kChip});
        /* time row */
        time = label(r, Face::Mono, P(g.time.px), col::kInk, X(g.time.box.x), Y(g.time.box.y), Wd(g.time.box));
        remain = label(r, Face::Mono, P(g.remain.px), col::kInk2, X(g.remain.box.x), Y(g.remain.box.y), Wd(g.remain.box),
                       LV_TEXT_ALIGN_RIGHT);
        for (lv_obj_t *l : {time, remain}) {
            const lv_font_t *f = font(Face::Mono, P(g.time.px));
            lv_obj_set_y(l, Y(g.time.box.y) + (P(g.time.lh) - lv_font_get_line_height(f)) / 2);
        }
        seek_x = X(g.seek.x);
        seek_w = std::max(1, Wd(g.seek));
        const int sh = std::max(2, Hd(g.seek));
        seek_track = box(r, seek_x, Y(g.seek.y), seek_w, sh, col::kChip, sh / 2);
        seek_fill = box(seek_track, 0, 0, sh, sh, col::kTeal, sh / 2);
        const int kd = P(18);
        seek_knob = box(r, seek_x - kd / 2, Y(g.seek.y) + sh / 2 - kd / 2, kd, kd, col::kInk, LV_RADIUS_CIRCLE);
        /* the seek bar: a tap seeks there, a drag scrubs (core/seek: the target follows the finger, the seek happens on the
         * release; the owner, 2026-11-13); press-locked, so a finger that slides off keeps the scrub and starts nothing
         * else */
        seek_hit = lv_obj_create(r);
        plain(seek_hit);
        lv_obj_add_flag(seek_hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(seek_hit, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(seek_hit, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_pos(seek_hit, seek_x - kd / 2, Y(g.seek.y) + sh / 2 - kd / 2);
        lv_obj_set_size(seek_hit, seek_w + kd, std::max(kd, target));
        for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
            lv_obj_add_event_cb(seek_hit, seek_event_cb, c, this);
        now_hits.push_back(NowHit{seek_hit, R{double(seek_x - kd / 2), double(Y(g.seek.y) + sh / 2 - kd / 2), double(seek_w + kd),
                                              double(kd)},
                                  kSeek});

        /* transport (moved down a few px where the rows above them need it: layout::now_keys_shift) */
        {
            std::vector<R> outs;
            if (g.out_button_on) outs.push_back(R{double(X(g.out_button.x)), double(Y(g.out_button.y)), double(Wd(g.out_button)),
                                                  double(Hd(g.out_button))});
            else
                for (size_t row = 0; row < g.chip_layout.size(); ++row)
                    for (int id : g.chip_layout[row])
                        if (id == 2)
                            outs.push_back(R{double(X(g.chip0.x)), double(Y(g.chip0.y + static_cast<double>(row) * g.chip_step)),
                                             double(X(g.title.box.right()) - X(g.chip0.x)), double(P(g.chip0.h))});
            keys_dy = layout::now_keys_shift(g, fit, target, outs, body_y, W, H);
            if (keys_dy) std::fprintf(stderr, "[vinyl] the transport keys %d px lower (the rows above them need it)\n", keys_dy);
        }
        for (const layout::Btn &b : g.transport) btns[b.id] = make_button(r, b, kKey);
        repeat_dash = box(r, X(g.repeat_dash.x), Y(g.repeat_dash.y) + keys_dy, Wd(g.repeat_dash), std::max(2, Hd(g.repeat_dash)),
                          col::kTeal, P(2));
        /* the same mark under shuffle when it is on (the draft draws the repeat one) */
        const layout::Btn *sb = nullptr, *rb = nullptr;
        for (const layout::Btn &b : g.transport) {
            if (b.id == "shuffle") sb = &b;
            if (b.id == "repeat") rb = &b;
        }
        if (sb && rb) {
            const double dx = g.repeat_dash.x - rb->box.x;
            shuffle_dash = box(r, X(sb->box.x + dx), Y(g.repeat_dash.y) + keys_dy, Wd(g.repeat_dash), std::max(2, Hd(g.repeat_dash)),
                               col::kTeal, P(2));
            repeat_one = label(r, Face::Bold, P(10), col::kTeal, X(rb->box.right() - 13), Y(rb->box.y + 3) + keys_dy, 0);
            set_text(repeat_one, "1");
        }

        if (g.has_upnext) {
            up_box = box(r, X(g.upnext.x), Y(g.upnext.y), Wd(g.upnext), Hd(g.upnext), col::kChip, P(7));
            up_cap = label(up_box, Face::Bold, P(11), col::kInk3, P(12), P(6), Wd(g.upnext) - P(24), LV_TEXT_ALIGN_LEFT,
                           ri(0.04 * 11 * fit.s));
            set_text(up_cap, "UP NEXT");
            up_text = label(up_box, Face::Bold, P(14), col::kInk, P(12), P(6) + lv_font_get_line_height(font(Face::Bold, P(11))),
                            Wd(g.upnext) - P(24));
        }
        for (const layout::Btn &b : g.bottom) btns[b.id] = make_button(r, b, kBottom);
        build_vol_group(r);
    }

    /* The output button (owner, 2026-11-13; every class but the large one): the chip colour, the Bluetooth or the
     * speaker icon, the device's name (scrolls when too long), a chevron; full width of the info column (portrait: right
     * of the format chip). A tap opens the Output page, the only way there on these layouts. */
    void build_out_button(lv_obj_t *r) {
        const int x = X(g.out_button.x), y = Y(g.out_button.y), w = Wd(g.out_button), h = Hd(g.out_button);
        outb_box = box(r, x, y, w, h, col::kChip, P(g.out_button.h >= 40 ? 8 : 5));
        const int ip = P(g.out_icon), pad = P(g.out_pad), gp = P(g.out_gap);
        outb_icon = icon_obj(outb_box, Icon::Output, pad, (h - ip) / 2, ip, ip, ip, col::kInk);
        outb_chev = icon_obj(outb_box, Icon::Chevron, w - pad - ip, (h - ip) / 2, ip, ip, ip, col::kInk);
        outb_text = label(outb_box, Face::Bold, P(g.out_px), col::kInk, pad + ip + gp, 0, std::max(1, w - 2 * pad - 2 * ip - 2 * gp));
        lv_label_set_long_mode(outb_text, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_y(outb_text, (h - lv_font_get_line_height(font(Face::Bold, P(g.out_px)))) / 2);
    }

    void place_out_button(const NowView &v) {
        if (!outb_box) return;
        int x = X(g.out_button.x);
        if (g.chips_in_row && chips[0].width > 0) x = std::max(x, chips[0].x + chips[0].width + P(g.chip_space));
        const int right = X(g.out_button.right()), w = std::max(P(60), right - x), h = lv_obj_get_height(outb_box);
        const int ip = P(g.out_icon), pad = P(g.out_pad), gp = P(g.out_gap);
        lv_obj_set_pos(outb_box, x, Y(g.out_button.y));
        lv_obj_set_width(outb_box, w);
        lv_obj_set_x(outb_chev, w - pad - ip);
        lv_obj_set_width(outb_text, std::max(1, w - 2 * pad - 2 * ip - 2 * gp));
        icon_set(outb_icon, v.output_bt ? Icon::Bluetooth : Icon::Output, col::kInk);
        set_text(outb_text, v.output.empty() ? std::string("Output") : v.output);
        for (NowHit &e : now_hits)
            if (e.obj == out_hit) e.drawn = R{double(x), double(Y(g.out_button.y)), double(w), double(h)};
        show(out_hit, true);
    }

    /* The volume group (owner, 2026-11-13; every class): "-", the level on teal, "+": the volume of the output Vinyl
     * plays on (D11 as changed), 5 % a press; the level itself is no control. */
    void build_vol_group(lv_obj_t *r) {
        auto make = [&](Button &b, const R &bx, Icon ic, int delta) {
            const int x = X(bx.x), y = Y(bx.y), w = Wd(bx), h = Hd(bx);
            b.vis = box(r, x, y, w, h, col::kButton, P(8));
            b.ic = ic;
            const int ip = P(g.vol_icon);
            b.icon = icon_obj(b.vis, ic, (w - ip) / 2, (h - ip) / 2, ip, ip, ip, col::kInk);
            b.hitbox = hit(r, x, y, w, h, target, target, [this, delta] {
                if (!last_now.vol_adjustable) { /* nothing can change it: say so (design QA run 2) */
                    if (act.volume_unavailable) act.volume_unavailable();
                    return;
                }
                if (act.volume_step) act.volume_step(delta);
            });
            now_hits.push_back(NowHit{b.hitbox, R{double(x), double(y), double(w), double(h)}, kBottom});
        };
        make(grp_minus, g.vol_minus, Icon::Minus, -1); /* one level of the output a press (core/volume) */
        make(grp_plus, g.vol_plus, Icon::Plus, 1);
        const int vx = X(g.vol_value.x), vy = Y(g.vol_value.y), vw = Wd(g.vol_value), vh = Hd(g.vol_value);
        grp_value_box = box(r, vx, vy, vw, vh, col::kTeal, P(8));
        const lv_font_t *f = font(Face::Bold, P(g.vol_text_px));
        grp_value = label(grp_value_box, Face::Bold, P(g.vol_text_px), col::kWhite, 0, (vh - lv_font_get_line_height(f)) / 2, vw,
                          LV_TEXT_ALIGN_CENTER);
        const int gp = P(14);
        grp_glyph = icon_obj(grp_value_box, Icon::Muted, 0, (vh - gp) / 2, gp, gp, gp, col::kWhite);
        lv_obj_add_flag(grp_glyph, LV_OBJ_FLAG_HIDDEN);
    }

    /* The level in the group: the number, "--" when not known; muted, the speaker-off mark and "Muted" (smaller when it
     * needs to; the mark alone where the word would go under P(10)), never the level it will come back at. */
    void update_vol_group(const NowView &v) {
        if (!grp_value) return;
        const bool muted = v.vol_muted && v.vol_level >= 0;
        const std::string t = v.vol_level < 0 ? std::string("--")
                              : muted         ? std::string("Muted")
                              : !v.vol_value.empty() ? v.vol_value
                                                     : std::to_string(v.vol_level);
        const int vw = lv_obj_get_width(grp_value_box), vh = lv_obj_get_height(grp_value_box);
        const int gp = lv_obj_get_width(grp_glyph), gap = P(3), margin = P(4);
        int px = P(g.vol_text_px), tx = 0, tw = vw;
        bool word = true;
        if (muted) {
            const int room = vw - 2 * margin - gp - gap;
            px = fit_px(Face::Bold, P(14), t, room, P(10));
            const int w = text_width(font(Face::Bold, px), t);
            word = w <= room;
            const int total = gp + (word ? gap + w : 0);
            const int gx = (vw - total) / 2;
            lv_obj_set_x(grp_glyph, gx);
            tx = gx + gp + gap;
            tw = std::max(1, w + 2);
        } else {
            px = fit_px(Face::Bold, P(g.vol_text_px), t, vw - P(8), P(10));
        }
        show(grp_glyph, muted);
        show(grp_value, word);
        const lv_font_t *f = font(Face::Bold, px);
        if (lv_obj_get_style_text_font(grp_value, LV_PART_MAIN) != f) lv_obj_set_style_text_font(grp_value, f, 0);
        lv_obj_set_height(grp_value, lv_font_get_line_height(f));
        lv_obj_set_y(grp_value, (vh - lv_font_get_line_height(f)) / 2);
        lv_obj_set_x(grp_value, tx);
        lv_obj_set_width(grp_value, tw);
        lv_obj_set_style_text_align(grp_value, muted ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
        set_text(grp_value, t);
    }

    /* The time labels as wide as their widest text in this format (and never narrower than the drafts' boxes), the seek
     * bar between them with the drafts' gaps (board screenshot 2026-11-13: a one-hour mixtape showed "-3..."). */
    int time_hours = -1;
    void layout_time_row(bool hours) {
        if (time_hours == (hours ? 1 : 0)) return;
        time_hours = hours ? 1 : 0;
        const lv_font_t *f = font(Face::Mono, P(g.time.px));
        const int tw = std::max(Wd(g.time.box), text_width(f, layout::track_time_widest(hours, false)) + 2);
        const int rw = std::max(Wd(g.remain.box), text_width(f, layout::track_time_widest(hours, true)) + 2);
        const int x0 = X(g.time.box.x), x1 = X(g.remain.box.right());
        const int gap = std::max(1, X(g.seek.x) - X(g.time.box.right()));
        lv_obj_set_width(time, tw);
        lv_obj_set_x(remain, x1 - rw);
        lv_obj_set_width(remain, rw);
        seek_x = x0 + tw + gap;
        seek_w = std::max(P(40), x1 - rw - gap - seek_x);
        lv_obj_set_x(seek_track, seek_x);
        lv_obj_set_width(seek_track, seek_w);
        const int kd = lv_obj_get_width(seek_knob);
        for (NowHit &e : now_hits)
            if (e.kind == kSeek) {
                e.drawn.x = seek_x - kd / 2;
                e.drawn.w = seek_w + kd;
            }
        layout_now_hits();
    }

    /* The time row: h:mm:ss for a track of an hour or more, the labels as wide as that format needs (never "-3..."). While
     * the seek bar is pressed it shows the target (the knob and the fill under the finger, the elapsed time the target's,
     * the remaining time from it); after the release the target until the engine's position is there. */
    void draw_time_row(const NowView &v) {
        if (!time) return;
        const uint64_t total = v.total_ms;
        uint64_t pos = std::min(v.pos_ms, total ? total : v.pos_ms);
        if (total && v.has_track) {
            if (scrub.active()) {
                pos = scrub.target_ms(total);
            } else if (hold_frac >= 0) {
                const uint64_t want = static_cast<uint64_t>(hold_frac * static_cast<double>(total));
                const uint64_t gap = pos > want ? pos - want : want - pos;
                if (gap > 1500 && static_cast<int32_t>(hold_until - lv_tick_get()) > 0) pos = want;
                else hold_frac = -1;
            }
        }
        const bool hours = (total ? total : pos) >= 3600000;
        layout_time_row(hours);
        set_text(time, v.has_track ? layout::track_time_text(pos, hours, true) : "--:--");
        set_text(remain, v.has_track && total ? "-" + layout::track_time_text(total - pos, hours, false) : "");
        const double frac = total ? static_cast<double>(pos) / total : 0.0;
        const int sh = lv_obj_get_height(seek_track);
        const int fw = std::max(sh, ri(frac * seek_w));
        lv_obj_set_width(seek_fill, fw);
        show(seek_fill, v.has_track && frac > 0);
        const int kd = lv_obj_get_width(seek_knob);
        lv_obj_set_x(seek_knob, seek_x + ri(frac * seek_w) - kd / 2);
    }

    /* the design-QA scenes: the scrubbing state as if a finger were held on the bar at `frac` */
    void simulate_scrub(double frac) {
        scrub.set_bar(seek_x, seek_w);
        scrub.press(seek_x + frac * seek_w);
        draw_time_row(last_now);
    }

    void set_cover(const std::shared_ptr<const PixelImage> &c) {
        if (c == cover) return;
        cover = c;
        if (c && c->w > 0) {
            cover_img.px = *c;
            if (cover_img.px.w != Wd(g.sleeve)) { /* sized for another class: shown as it is */ }
            cover_img.bind(sleeve);
            lv_obj_set_style_radius(sleeve, 0, 0);
            cover_shown = true;
        } else {
            sleeve_img.bind(sleeve);
            lv_obj_set_style_radius(sleeve, P(4), 0);
            cover_shown = false;
        }
    }

    void update_now(const NowView &v) {
        set_text(title, v.title);
        set_text(artist, v.artist);
        set_text(album, v.album);
        set_cover(v.cover);
        /* chips: the rows of the class; the last chip of a full row ends with dots (the output chip scrolls) */
        const std::string texts[4] = {v.format, "", v.output, v.extra_chip};
        const Icon bt[4] = {Icon::None, Icon::None, v.output_bt ? Icon::Bluetooth : Icon::None, Icon::None};
        const int x0 = X(g.chip0.x);
        const int row_right = X(g.title.box.right()); /* the info column's right edge */
        const int space = P(g.chip_space);
        for (size_t r = 0; r < g.chip_layout.size(); ++r) {
            int x = x0;
            for (int i : g.chip_layout[r]) {
                if (i < 0 || i > 3 || !chips[i].box) continue;
                chips[i].max_w = std::max(P(30), row_right - x);
                chips[i].y = Y(g.chip0.y + static_cast<double>(r) * g.chip_step);
                chips[i].set(texts[i], bt[i], x);
                if (chips[i].width) x += chips[i].width + space;
            }
        }
        /* the touch area over the large class's output chip follows the chip; the other classes have the button */
        for (NowHit &e : now_hits) {
            if (e.obj != out_hit || g.out_button_on) continue;
            const Chip &c = chips[2];
            show(e.obj, c.width > 0);
            e.drawn = c.width > 0 ? R{double(c.x), double(c.y), double(c.width), double(c.h)} : R{};
        }
        place_out_button(v);
        update_vol_group(v);
        const int outb_x = outb_box ? lv_obj_get_x(outb_box) : 0;
        const int sig[4] = {g.out_button_on ? outb_x : chips[2].x, chips[2].width, chips[2].y, outb_box ? lv_obj_get_width(outb_box) : 0};
        if (!std::equal(sig, sig + 4, hits_sig)) { /* the output chip or button moved */
            std::copy(sig, sig + 4, hits_sig);
            layout_now_hits();
        }
        draw_time_row(v);
        /* transport; with an empty queue the keys are dimmed: nothing to play (design QA run 3, D-81) */
        if (auto it = btns.find("play"); it != btns.end()) icon_set(it->second.icon, v.playing ? Icon::Pause : Icon::Play, col::kWhite);
        for (const layout::Btn &t : g.transport)
            if (t.enabled)
                if (auto it = btns.find(t.id); it != btns.end() && it->second.vis)
                    lv_obj_set_style_opa(it->second.vis, v.queue_empty ? LV_OPA_40 : LV_OPA_COVER, 0);
        show(repeat_dash, v.repeat != Repeat::Off);
        show(repeat_one, v.repeat == Repeat::One);
        show(shuffle_dash, v.shuffle);
        if (up_text) set_text(up_text, v.upnext);
        if (up_box) show(up_box, true);
        /* the record slides out of the sleeve for a new track */
        if (v.track_gen != track_gen) {
            const bool first = track_gen == UINT64_MAX;
            track_gen = v.track_gen;
            if (!first && v.has_track) slide_out();
        }
        if (qlist && v.queue_rows) {
            set_text(qheading, v.queue_heading);
            if (v.queue_ver != queue_ver || v.queue_selected != queue_sel) {
                const bool reset = v.queue_rows->size() != static_cast<size_t>(qlist->size());
                qlist->set_rows(*v.queue_rows, v.queue_selected, reset, reset ? v.queue_current : -1);
                queue_ver = v.queue_ver;
                queue_sel = v.queue_selected;
            }
        }
        last_now = v;
    }

    void slide_out() {
        lv_anim_delete(disc, nullptr);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, disc);
        lv_anim_set_values(&a, disc_in_x, disc_out_x);
        lv_anim_set_duration(&a, 450);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t *>(o), v); });
        lv_anim_start(&a);
    }

    /* ---------------------------------------------------------------------------------------- list pages */

    void build_list(ListW &L, Page p) {
        const layout::ListPage lp = p == Page::Browser ? layout::ListPage::Browser
                                    : p == Page::Queue ? layout::ListPage::Queue
                                                       : layout::ListPage::Output;
        const layout::ListGeom lg = layout::list_geom(lp, W, H, body_y);
        const double s = lg.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        L.root = page_root();
        lv_obj_t *r = L.root;
        box(r, ri(lg.panel.x), ri(lg.panel.y), ri(lg.panel.w), ri(lg.panel.h), col::kList, S(10));
        if (p != Page::Output) {
            L.heading = label(r, Face::Bold, S(12), col::kRowValue, ri(lg.heading.x), ri(lg.heading.y), ri(lg.heading.w),
                              LV_TEXT_ALIGN_LEFT, ri(0.04 * 12 * s));
            const lv_font_t *hf = font(Face::Bold, S(12));
            lv_obj_set_y(L.heading, ri(lg.heading.y) + (ri(lg.heading.h) - lv_font_get_line_height(hf)) / 2);
        }
        if (p == Page::Browser) {
            /* the drafts' "A-Z" pill is not built: sorting is no control in P1 (design QA run 3, D-81) */
        }
        const int text_px = S(15), value_px = S(13);
        const double dot_x = 16 * s, text_x = 40 * s, value_r = 16 * s, chev_r = 14 * s;
        L.list = std::make_unique<RowList>(r, ri(lg.list.x), ri(lg.list.y), ri(lg.list.w), std::max(1, ri(lg.list.h)),
                                           std::max(ri(lg.row_h), list_row), s, text_px, p == Page::Output ? S(12) : value_px, dot_x, text_x,
                                           value_r, chev_r, [this, p](int i) {
                                               if (act.row) act.row(p, i);
                                           });
        const int sx = ri(lg.strip.x), sy = ri(lg.strip.y), sw = ri(lg.strip.w), shh = ri(lg.strip.h);
        if (p == Page::Browser) {
            L.strip = box(r, sx, sy, sw, shh, col::kCream, S(8));
            const int md = S(30);
            L.mini_disc_img.px = render_disc(md, s * 0.5);
            L.mini_disc = image(L.strip, S(8), (shh - md) / 2);
            L.mini_disc_img.bind(L.mini_disc);
            const int ml = S(10);
            L.mini_label_img.px = render_label(ml, 0, s);
            lv_obj_t *lab = image(L.mini_disc, (md - ml) / 2, (md - ml) / 2);
            L.mini_label_img.bind(lab);
            L.mini_title = label(L.strip, Face::Bold, S(13), col::kInk, S(48), S(4), sw - S(48) - S(100));
            /* the second line: the artist (dots when long) and the times, whole (design QA run 3, D-18) */
            L.mini_sub = label(L.strip, Face::Medium, S(11), col::kInk3, S(48), S(21), sw - S(48) - S(100));
            L.mini_time = label(L.strip, Face::Medium, S(11), col::kInk3, S(48), S(21), 0);
            L.mini_text_w = sw - S(48) - S(100);
            L.mini_bar = box(L.strip, S(8), shh - S(3), sw - S(16), S(3), col::kChip, 0);
            L.mini_fill = box(L.mini_bar, 0, 0, 1, S(3), col::kTeal, 0);
            /* the whole strip opens now playing; its two buttons act (next: the transport press). Their touch areas never
             * overlap (design QA D-33) and keep the touch target: play and next meet in the middle of the gap between
             * them (layout::split_pair), the open area ends where play's begins; all three are the target high from
             * the strip's top down, moved up where the screen's bottom is nearer (the list then ends where they begin). */
            L.mini_open_hit = hit(r, sx, sy, sw - S(100), shh, 0, 0, [this] {
                if (act.mini_open) act.mini_open();
            });
            auto mini_btn = [&](Button &b, int x, int w, Icon ic, const char *id) {
                const int y = sy + S(3), h = S(36);
                b.vis = box(r, x, y, w, h, col::kButton, S(8));
                const int ip = S(20);
                b.icon = icon_obj(b.vis, ic, (w - ip) / 2, (h - ip) / 2, ip, ip, ip, col::kInk);
                const std::string sid = id;
                if (sid == "next")
                    b.hitbox = hold_hit(r, x, y, w, h, 0, 0, [this](int ph) { transport(true, ph); });
                else
                    b.hitbox = hit(r, x, y, w, h, 0, 0, [this, sid] { press(sid); });
            };
            /* the touch areas from the core (layout::mini_touch_areas: no overlap, the target high; tests/test_hits.cpp) */
            const layout::MiniBoxes mb = layout::mini_touch_areas(lg, W, H, target);
            mini_btn(L.mini_play, ri(mb.play.x), ri(mb.play.w), Icon::Pause, "play");
            mini_btn(L.mini_next, ri(mb.next.x), ri(mb.next.w), Icon::Next, "next");
            place_area(L.mini_play.hitbox, mb.play_area);
            place_area(L.mini_next.hitbox, mb.next_area);
            place_area(L.mini_open_hit, mb.open_area);
            if (mb.list_bottom < lg.list.bottom()) lv_obj_set_height(L.list->obj(), std::max(1, ri(mb.list_bottom) - ri(lg.list.y)));
        } else if (p == Page::Queue) {
            static const struct {
                const char *word;
                Icon ic;
            } qb[4] = {{"Move up", Icon::Up}, {"Move down", Icon::Down}, {"Remove", Icon::Minus}, {"Clear all", Icon::Trash}};
            for (int i = 0; i < 4 && i < static_cast<int>(lg.queue_buttons.size()); ++i) {
                const R &b = lg.queue_buttons[static_cast<size_t>(i)];
                Button bt;
                const bool danger = i == 3;
                const int x = ri(b.x), y = ri(b.y), w = ri(b.w), h = ri(b.h);
                bt.vis = box(r, x, y, w, h, danger ? col::kDanger : col::kButton, S(8));
                const uint32_t ink = danger ? col::kWhite : col::kInk;
                const int ip = S(16);
                const lv_font_t *f = font(Face::Bold, S(12));
                const int tw = text_width(f, qb[i].word);
                const int total = ip + S(6) + tw;
                bt.icon = icon_obj(bt.vis, qb[i].ic, (w - total) / 2, (h - ip) / 2, ip, ip, ip, ink);
                bt.word = label(bt.vis, Face::Bold, S(12), ink, (w - total) / 2 + ip + S(6), (h - lv_font_get_line_height(f)) / 2, 0);
                set_text(bt.word, qb[i].word);
                bt.hitbox = hit(r, x, y, w, h, 0, 0, [this, i] {
                    if (act.queue_button) act.queue_button(i);
                });
                L.qbtns.push_back(bt);
            }
            /* the touch areas (layout::queue_touch_areas): no two overlap, the target high, the list above them shorter
             * where they reach over its last row (design QA run 3, D-33) */
            double list_bottom = 0;
            const std::vector<R> qa = layout::queue_touch_areas(lg, H, target, list_bottom);
            for (size_t i = 0; i < qa.size() && i < L.qbtns.size(); ++i) place_area(L.qbtns[i].hitbox, qa[i]);
            if (list_bottom < lg.list.bottom()) lv_obj_set_height(L.list->obj(), std::max(1, ri(list_bottom) - ri(lg.list.y)));
        } else {
            L.strip = box(r, sx, sy, sw, shh, col::kCream, S(8));
            const int ip = S(18);
            L.strip_icon = icon_obj(L.strip, Icon::Info, S(12), (shh - ip) / 2, ip, ip, ip, col::kInk2);
            L.strip_text = label(L.strip, Face::Bold, S(12), col::kInk2, S(12) + ip + S(10), 0, sw - S(24) - ip - S(10));
            lv_label_set_long_mode(L.strip_text, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_height(L.strip_text, LV_SIZE_CONTENT);
            lv_obj_set_style_text_line_space(L.strip_text, S(16) - lv_font_get_line_height(font(Face::Bold, S(12))), 0);
            L.strip_y0 = sy;
            L.strip_h0 = shh;
            L.list_y = ri(lg.list.y);
        }
        lv_obj_add_flag(L.root, LV_OBJ_FLAG_HIDDEN);
    }

    void update_list(ListW &L, Page p, const ListView &v) {
        if (L.heading) set_text(L.heading, v.heading);
        if (L.sort_box) {
            const lv_font_t *sf = lv_obj_get_style_text_font(L.sort_text, LV_PART_MAIN);
            const int pad = lv_obj_get_x(L.sort_text);
            set_text(L.sort_text, v.sort);
            const int w = text_width(sf, v.sort) + 2 * pad;
            lv_obj_set_width(L.sort_box, w);
            lv_obj_set_y(L.sort_text, (lv_obj_get_height(L.sort_box) - lv_font_get_line_height(sf)) / 2);
            /* right-aligned in the panel, 12 design px from its edge */
            const layout::ListGeom lg = layout::list_geom(layout::ListPage::Browser, W, H, body_y);
            lv_obj_set_x(L.sort_box, ri(lg.panel.right() - 12 * lg.s) - w);
            show(L.sort_box, !v.sort.empty());
        }
        const bool reset = v.rows_gen != L.rows_gen;
        L.rows_gen = v.rows_gen;
        if (v.rows && (reset || v.rows_ver != L.rows_ver || v.selected != L.sel)) {
            if (!reset && v.rows_ver == L.rows_ver) L.list->set_selected(v.selected, true); /* a key moved the highlight */
            else L.list->set_rows(*v.rows, v.selected, reset, reset ? v.scroll_to : -1);
            L.rows_ver = v.rows_ver;
            L.sel = v.selected;
            L.rows = static_cast<int>(v.rows->size());
        }
        if (p == Page::Queue) /* an empty queue: the buttons dimmed, they do nothing (design QA run 3, D-81) */
            for (Button &b : L.qbtns) lv_obj_set_style_opa(b.vis, v.rows && !v.rows->empty() ? LV_OPA_COVER : LV_OPA_40, 0);
        if (p == Page::Browser && L.strip) {
            show(L.strip, v.mini.shown);
            show(L.mini_open_hit, v.mini.shown);
            show(L.mini_play.vis, v.mini.shown);
            show(L.mini_play.hitbox, v.mini.shown);
            show(L.mini_next.vis, v.mini.shown);
            show(L.mini_next.hitbox, v.mini.shown);
            set_text(L.mini_title, v.mini.title);
            /* the times right of the artist, whole; the artist gets the rest, with dots */
            const lv_font_t *mf = font(Face::Medium, std::max(1, ri(11 * layout::list_geom(layout::ListPage::Browser, W, H, body_y).s)));
            const std::string sep = v.mini.sub.empty() ? std::string() : std::string(", ");
            const int tw = text_width(mf, sep + v.mini.time) + 2;
            set_text(L.mini_time, sep + v.mini.time);
            lv_obj_set_width(L.mini_time, tw);
            const int aw = std::min(text_width(mf, v.mini.sub) + 2, std::max(1, L.mini_text_w - tw));
            lv_obj_set_width(L.mini_sub, std::max(1, aw));
            lv_obj_set_x(L.mini_time, lv_obj_get_x(L.mini_sub) + (v.mini.sub.empty() ? 0 : aw));
            set_text(L.mini_sub, v.mini.sub);
            const int bw = lv_obj_get_width(L.mini_bar);
            lv_obj_set_width(L.mini_fill, std::max(1, ri(std::clamp(v.mini.progress, 0.0, 1.0) * bw)));
            icon_set(L.mini_play.icon, v.mini.playing ? Icon::Pause : Icon::Play, col::kInk);
        }
        if (p == Page::Output && L.strip_text) {
            icon_set(L.strip_icon, v.hint_notice ? Icon::Bluetooth : Icon::Info, v.hint_notice ? col::kInk : col::kInk2);
            lv_obj_set_style_text_color(L.strip_text, lv_color_hex(v.hint_notice ? col::kInk : col::kInk2), 0);
            set_text(L.strip_text, v.hint);
            lv_obj_update_layout(L.strip_text);
            /* the strip grows upwards to its text (three lines on a narrow screen: design QA D-18), the list above it
             * gives the height up */
            const double s = layout::list_geom(layout::ListPage::Output, W, H, body_y).s;
            const int th = lv_obj_get_height(L.strip_text);
            const int h = std::max(L.strip_h0, th + 2 * std::max(1, ri(8 * s)));
            const int y = L.strip_y0 + L.strip_h0 - h;
            lv_obj_set_y(L.strip, y);
            lv_obj_set_height(L.strip, h);
            lv_obj_set_height(L.list->obj(), std::max(1, y - L.list_y));
            lv_obj_set_y(L.strip_text, (h - th) / 2);
            const int ip = lv_obj_get_height(L.strip_icon);
            lv_obj_set_y(L.strip_icon, (h - ip) / 2);
        }
    }

    /* ---------------------------------------------------------------------------------------- notices */

    void build_notices_demo() {
        /* the notices draft (Notices-480x320), for the scenes: each notice in context, drawn by the same notice code */
        not_root = page_root();
        const layout::ListGeom lg = layout::list_geom(layout::ListPage::Output, W, H, body_y);
        const double s = lg.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        box(not_root, ri(lg.panel.x), ri(lg.panel.y), ri(lg.panel.w), ri(lg.panel.h), col::kList, S(10));
        struct N {
            const char *cap;
            Notice::Kind kind;
            const char *text;
            double y;
        };
        const N ns[4] = {{"ON OPENING A FILE", Notice::Kind::Info, "The FFmpeg libraries could not be loaded. Reinstall Vinyl.", 20},
                         {"ON THE OUTPUT CHOOSER", Notice::Kind::Bluetooth, "Bluetooth audio is not set up on this board.", 90},
                         {"WHILE PLAYING ON A HEADSET IN CALL MODE", Notice::Kind::Call, "Call quality (16 kHz): the headset is in call mode.", 160},
                         {"WHEN THE OUTPUT DISCONNECTS", Notice::Kind::Danger, "Evolve 65 disconnected. Paused.", 230}};
        for (const N &n : ns) {
            const int x = ri(lg.panel.x + 8 * s);
            lv_obj_t *cap = label(not_root, Face::Bold, S(10), col::kRowValue, x, ri(lg.panel.y + (n.y - 14) * s), S(448),
                                  LV_TEXT_ALIGN_LEFT, ri(0.06 * 10 * s));
            set_text(cap, n.cap);
            lv_obj_t *o = nullptr, *ic = nullptr, *tx = nullptr;
            make_notice(not_root, n.kind, x, ri(lg.panel.y + n.y * s), S(448), s, o, ic, tx);
            set_text(tx, n.text);
            lv_obj_update_layout(tx);
            lv_obj_set_y(tx, (lv_obj_get_height(o) - lv_obj_get_height(tx)) / 2);
        }
        lv_obj_add_flag(not_root, LV_OBJ_FLAG_HIDDEN);
    }

    /* the notice strip of the draft: cream, 50 high, 13 px semibold with a 20 px icon; the danger one dark red, 30 high */
    void make_notice(lv_obj_t *parent, Notice::Kind k, int x, int y, int w, double s, lv_obj_t *&o, lv_obj_t *&ic, lv_obj_t *&tx,
                     int fit_h = 0) {
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        const bool danger = k == Notice::Kind::Danger;
        const int h = fit_h > 0 ? fit_h : danger ? S(30) : S(50);
        o = box(parent, x, y, w, h, danger ? col::kDanger : col::kCream, S(8));
        const int ip = danger ? S(16) : S(20);
        const Icon icon = k == Notice::Kind::Bluetooth ? Icon::Bluetooth
                          : k == Notice::Kind::Call    ? Icon::Phone
                          : danger                     ? Icon::Warning
                                                       : Icon::Info;
        const uint32_t ink = danger ? col::kWhite : col::kInk;
        ic = icon_obj(o, icon, S(12), (h - ip) / 2, ip, ip, ip, ink);
        const int px = danger ? S(12) : S(13);
        tx = label(o, Face::Bold, px, ink, S(12) + ip + S(10), 0, w - S(24) - ip - S(10));
        lv_label_set_long_mode(tx, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_height(tx, LV_SIZE_CONTENT);
        lv_obj_set_style_text_line_space(tx, S(17) - lv_font_get_line_height(font(Face::Bold, px)), 0);
    }

    void show_notice(const Notice &n) {
        hide_notice();
        lv_obj_t *root = cur == Page::Now ? now_root : cur == Page::Browser ? br.root : cur == Page::Queue ? qu.root : out.root;
        if (!root) return;
        const bool danger = n.kind == Notice::Kind::Danger;
        int x = 0, y = 0, w = 0, fit_h = 0;
        double s = fit.s;
        if (cur == Page::Now) {
            /* where it covers nothing that matters (design QA run 3, D-18): over the seek row when the band between the
             * chips, the output button or the small class's volume group and the transport keys holds it; else over the
             * up-next box (square); else in that band as high as it is when that is at least the danger strip's 30
             * design px (the small class: 38 px, two lines of text still fit); else over the bottom row (the keys come
             * back when it goes, 6-8 s, or with a tap on it) */
            const R b = layout::notice_box_now(g, danger ? 30 : 50);
            const int h = ri((danger ? 30 : 50) * s);
            x = X(b.x);
            w = Wd(b);
            int bot_lim = H;
            for (const layout::Btn &t : g.transport) bot_lim = std::min(bot_lim, Y(t.box.y) + keys_dy);
            int top_lim = 0;
            for (const Chip &c : chips)
                if (c.box && c.width > 0) top_lim = std::max(top_lim, c.y + c.h);
            if (outb_box) top_lim = std::max(top_lim, lv_obj_get_y(outb_box) + lv_obj_get_height(outb_box));
            if (grp_value_box && lv_obj_get_y(grp_value_box) < bot_lim)
                top_lim = std::max(top_lim, lv_obj_get_y(grp_value_box) + lv_obj_get_height(grp_value_box));
            top_lim += P(4);
            bot_lim -= P(4);
            const int band = bot_lim - top_lim;
            if (band >= h) {
                y = top_lim + (band - h) / 2;
            } else if (g.has_upnext) {
                y = Y(g.upnext.y) + (Hd(g.upnext) - h) / 2;
            } else if (band >= ri(30 * s)) {
                y = top_lim;
                fit_h = band;
            } else {
                int row_top = H, row_bottom = 0;
                for (const layout::Btn &t : g.bottom) {
                    row_top = std::min(row_top, Y(t.box.y));
                    row_bottom = std::max(row_bottom, Y(t.box.y) + Hd(t.box));
                }
                y = row_bottom > 0 ? row_top + (row_bottom - row_top - h) / 2 : Y(b.bottom()) - h;
            }
        } else {
            const layout::ListPage lp = cur == Page::Browser ? layout::ListPage::Browser
                                        : cur == Page::Queue ? layout::ListPage::Queue
                                                             : layout::ListPage::Output;
            const layout::ListGeom lg = layout::list_geom(lp, W, H, body_y);
            s = lg.s;
            x = ri(lg.strip.x);
            w = ri(lg.strip.w);
            y = ri(lg.strip.bottom()) - ri((danger ? 30 : 50) * s);
        }
        make_notice(root, n.kind, x, y, w, s, notice, notice_icon, notice_text, fit_h);
        set_text(notice_text, n.text);
        lv_obj_update_layout(notice_text);
        lv_obj_set_y(notice_text, (lv_obj_get_height(notice) - lv_obj_get_height(notice_text)) / 2);
        /* a tap dismisses it: its hit box is at least the touch target high (the danger strip is drawn 30 high) */
        notice_hit = hit(root, x, lv_obj_get_y(notice), w, lv_obj_get_height(notice), 0, target, [this] {
            lv_async_call([](void *p) { static_cast<Impl *>(p)->hide_notice(); }, this);
        });
        notice_timer = lv_timer_create([](lv_timer_t *t) { static_cast<Impl *>(lv_timer_get_user_data(t))->hide_notice(); },
                                       danger ? 8000 : 6000, this);
        lv_timer_set_repeat_count(notice_timer, 1);
        lv_timer_set_auto_delete(notice_timer, false);
    }

    /* "Clear the queue?" (design QA run 3: Clear all destroys the queue): a dialog in the notice style over the queue
     * buttons, Cancel (the default) and Clear; modal: a tap anywhere but Clear cancels; Esc cancels, Enter takes the
     * highlighted button, Left / Right / Tab move the highlight. */
    lv_obj_t *dlg_scrim = nullptr, *dlg_box = nullptr;
    Button dlg_btn[2];
    int dlg_sel = 0;

    void ask_clear_queue() {
        close_dialog();
        if (!qu.root) return;
        const layout::ListGeom lg = layout::list_geom(layout::ListPage::Queue, W, H, body_y);
        const double s = lg.s;
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        dlg_scrim = hit(qu.root, 0, body_y, W, H - body_y, 0, 0, [this] { /* the page under the header: a tap outside cancels */
            lv_async_call([](void *p) { static_cast<Impl *>(p)->dialog_choose(0); }, this);
        });
        R area = lg.strip;
        const bool two_rows = lg.queue_buttons.size() == 4 && lg.queue_buttons[2].y > lg.queue_buttons[0].y;
        const int bx = ri(area.x), by = ri(area.y), bw = ri(area.w), bh = ri(area.h);
        dlg_box = box(qu.root, bx, by, bw, bh, col::kCream, S(8));
        const lv_font_t *f = font(Face::Bold, S(13));
        const int pad = S(10), gap = S(8);
        int tx, ty, tw, btn_y, btn_h, btn_w, b0x;
        if (two_rows) { /* the question on the top half, the buttons side by side on the bottom half */
            const int half = (bh - gap) / 2;
            tx = pad;
            tw = bw - 2 * pad;
            ty = (half - lv_font_get_line_height(f)) / 2;
            btn_y = half + gap;
            btn_h = bh - btn_y - S(4);
            btn_w = (bw - 2 * pad - gap) / 2;
            b0x = pad;
        } else { /* one row: the question at the left, the buttons at the right */
            btn_h = bh - 2 * S(4);
            btn_y = S(4);
            btn_w = std::max(S(84), text_width(f, "Cancel") + 2 * pad);
            b0x = bw - pad - 2 * btn_w - gap;
            tx = pad;
            tw = std::max(1, b0x - 2 * pad);
            ty = (bh - lv_font_get_line_height(f)) / 2;
        }
        lv_obj_t *q = label(dlg_box, Face::Bold, S(13), col::kInk, tx, ty, tw);
        set_text(q, "Clear the queue?");
        const char *words[2] = {"Cancel", "Clear"};
        for (int i = 0; i < 2; ++i) {
            Button &b = dlg_btn[i];
            const int x = b0x + i * (btn_w + gap);
            b.vis = box(dlg_box, x, btn_y, btn_w, btn_h, i == 1 ? col::kDanger : col::kButton, S(8));
            b.word = label(b.vis, Face::Bold, S(13), i == 1 ? col::kWhite : col::kInk, 0, (btn_h - lv_font_get_line_height(f)) / 2,
                           btn_w, LV_TEXT_ALIGN_CENTER);
            set_text(b.word, words[i]);
            lv_obj_set_style_border_color(b.vis, lv_color_hex(col::kTeal), 0);
            lv_obj_set_style_border_width(b.vis, 0, 0);
        }
        /* the two touch areas: the target high and wide where the screen allows, meeting in the middle of their gap */
        layout::HitArea a0{R{double(bx + b0x), double(by + btn_y), double(btn_w), double(btn_h)}, R{}};
        layout::HitArea a1{R{double(bx + b0x + btn_w + gap), double(by + btn_y), double(btn_w), double(btn_h)}, R{}};
        for (layout::HitArea *a : {&a0, &a1}) {
            a->area = layout::grow_hit(a->drawn, target, target);
            if (a->area.bottom() > H) a->area.y = H - a->area.h;
        }
        layout::split_pair(a0, a1, target);
        for (int i = 0; i < 2; ++i) {
            dlg_btn[i].hitbox = hit(qu.root, 0, 0, 1, 1, 0, 0, [this, i] {
                lv_async_call(i == 0 ? +[](void *p) { static_cast<Impl *>(p)->dialog_choose(0); }
                                     : +[](void *p) { static_cast<Impl *>(p)->dialog_choose(1); },
                              this);
            });
            place_area(dlg_btn[i].hitbox, i == 0 ? a0.area : a1.area);
        }
        dlg_sel = 0;
        highlight_dialog();
    }

    void highlight_dialog() {
        for (int i = 0; i < 2; ++i)
            if (dlg_btn[i].vis) lv_obj_set_style_border_width(dlg_btn[i].vis, i == dlg_sel ? std::max(2, P(3)) : 0, 0);
    }

    void close_dialog() {
        for (lv_obj_t **o : {&dlg_btn[0].hitbox, &dlg_btn[1].hitbox, &dlg_box, &dlg_scrim})
            if (*o) {
                lv_obj_delete(*o);
                *o = nullptr;
            }
        dlg_btn[0] = Button();
        dlg_btn[1] = Button();
    }

    void dialog_choose(int which) {
        if (!dlg_box) return;
        close_dialog();
        if (which == 1 && act.queue_clear) act.queue_clear();
    }

    bool dialog_key(uint32_t k) {
        if (!dlg_box) return false;
        if (k == LV_KEY_ESC) dialog_choose(0);
        else if (k == LV_KEY_ENTER) dialog_choose(dlg_sel);
        else if (k == LV_KEY_LEFT || k == LV_KEY_RIGHT || k == LV_KEY_NEXT || k == LV_KEY_PREV || k == 9) {
            dlg_sel = 1 - dlg_sel;
            highlight_dialog();
        }
        return true;
    }

    void hide_notice() {
        if (notice_timer) {
            lv_timer_delete(notice_timer);
            notice_timer = nullptr;
        }
        if (notice) {
            lv_obj_delete(notice);
            notice = nullptr;
        }
        if (notice_hit) {
            lv_obj_delete(notice_hit);
            notice_hit = nullptr;
        }
    }

    void tick(bool playing) {
        const uint32_t now = lv_tick_get();
        const uint32_t dt = last_tick ? lv_tick_diff(now, last_tick) : 0;
        last_tick = now;
        if (!playing || cur != Page::Now || cdk::hidden() || !labelimg) return;
        angle = std::fmod(angle + 200.0 * dt / 1000.0, 360.0); /* 33 1/3 rpm */
        lv_image_set_rotation(labelimg, static_cast<int32_t>(angle * 10));
    }
};

View::View(lv_obj_t *screen, Actions actions) : d_(std::make_unique<Impl>()) {
    Impl &d = *d_;
    d.screen = screen;
    d.act = std::move(actions);
    d.W = cdk::screen_w();
    d.H = cdk::screen_h();
    if (d.W <= 0 || d.H <= 0) {
        lv_display_t *disp = lv_obj_get_display(screen);
        d.W = static_cast<int>(lv_display_get_horizontal_resolution(disp));
        d.H = static_cast<int>(lv_display_get_vertical_resolution(disp));
    }
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    d.target = std::max(m->tok[CP0_TOK_TARGET], m->tok[CP0_TOK_ROW_H]);
    d.list_row = std::min(m->tok[CP0_TOK_ROW_H], m->tok[CP0_TOK_LIST_ROW_H]);
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &d.tk);
    /* the launcher's band: the toolkit's header with the title, the Esc and the shared status strip (as Cassette) */
    d.page = std::make_unique<cp0::ui::Page>(screen, d.tk, cp0::ui::add_title, [&d] {
        if (d.act.back) d.act.back();
    });
    cp0::ui::StatusStrip::add(d.page->header());
    /* the title as the toolkit places it: none at the narrow portrait 320x480 (the toolkit's title_hidden), the last part
     * of the path everywhere else, portrait 480x640 and 480x800 included */
    d.page->set_title({"Vinyl"});
    d.body_y = d.tk.body_y;
    d.g = layout::now_geom(layout::size_class(d.W, d.H));
    d.fit = layout::now_fit(d.g, d.W, d.H, d.body_y);
    d.build_now();
    d.build_list(d.br, Page::Browser);
    d.build_list(d.qu, Page::Queue);
    d.build_list(d.out, Page::Output);
    d.build_notices_demo();
    d.cur = Page::Now;
}

View::~View() {
    d_->hide_notice();
    d_->close_dialog();
    d_->qlist.reset();
    d_->br.list.reset();
    d_->qu.list.reset();
    d_->out.list.reset();
    d_->page.reset(); /* deletes its root and every page on it */
}

layout::SizeClass View::size_class() const { return d_->g.cls; }

void View::show_page(Page p) {
    Impl &d = *d_;
    if (p == d.cur) return;
    d.hide_notice();
    d.close_dialog();
    d.cur = p;
    show(d.now_root, p == Page::Now);
    show(d.br.root, p == Page::Browser);
    show(d.qu.root, p == Page::Queue);
    show(d.out.root, p == Page::Output);
    show(d.not_root, p == Page::Notices);
    /* the title per page (Browse: the toolkit shows the last part on a narrow header; at 320x480 it shows none) */
    switch (p) {
    case Page::Browser: d.page->set_title({"Vinyl", "Browse"}); break;
    case Page::Queue: d.page->set_title({"Queue"}); break;
    case Page::Output: d.page->set_title({"Output"}); break;
    default: d.page->set_title({"Vinyl"}); break;
    }
}

Page View::page() const { return d_->cur; }
void View::update_now(const NowView &v) { d_->update_now(v); }

void View::update_list(Page p, const ListView &v) {
    Impl &d = *d_;
    if (p == Page::Browser) d.update_list(d.br, p, v);
    else if (p == Page::Queue) d.update_list(d.qu, p, v);
    else if (p == Page::Output) d.update_list(d.out, p, v);
}

void View::show_notice(const Notice &n) { d_->show_notice(n); }
void View::hide_notice() { d_->hide_notice(); }
bool View::notice_shown() const { return d_->notice != nullptr; }
void View::ask_clear_queue() { d_->ask_clear_queue(); }
bool View::dialog_shown() const { return d_->dlg_box != nullptr; }
bool View::dialog_key(uint32_t key) { return d_->dialog_key(key); }
void View::simulate_scrub(double frac) { d_->simulate_scrub(frac); }
void View::tick(bool playing) { d_->tick(playing); }
int View::cover_side() const { return d_->Wd(d_->g.sleeve); }

int View::list_rows(Page p) const {
    const Impl &d = *d_;
    if (p == Page::Browser) return d.br.rows;
    if (p == Page::Queue) return d.qu.rows;
    if (p == Page::Output) return d.out.rows;
    return 0;
}

} // namespace ui
} // namespace vinyl
