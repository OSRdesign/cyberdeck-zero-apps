/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the pages (see vinyl_view.hpp). Every position comes from core/layout (the drafts' numbers) through the Fit of
 * the canvas; nothing is placed from a constant of this file but the drafts' own proportions (paddings, radii, font
 * sizes), scaled.
 */
#include "vinyl_view.hpp"

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

    /* the width the text needs (0: no chip) */
    int natural(const std::string &t, bool bt) const {
        if (t.empty() || !box) return 0;
        return pad + (bt ? icon_px + gap : 0) + text_width(font(face, px), t) + pad + 1;
    }

    /* the chip's width follows its text */
    void set(const std::string &t, bool bt, int at_x = -1) {
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
            lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
        }
        lv_obj_set_width(text, std::max(1, tw + 1));
        show(icon, bt);
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

std::string mmss(uint64_t ms, bool lead_zero) {
    const uint64_t s = ms / 1000;
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%u:%02u:%02u", unsigned(s / 3600), unsigned((s / 60) % 60), unsigned(s % 60));
    else if (lead_zero) std::snprintf(b, sizeof b, "%02u:%02u", unsigned(s / 60), unsigned(s % 60));
    else std::snprintf(b, sizeof b, "%u:%02u", unsigned(s / 60), unsigned(s % 60));
    return b;
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
    Chip chips[4]; /* format, volume, output, gapless */
    lv_obj_t *out_hit = nullptr; /* the output chip opens the chooser (the 480x320 draft has no Output key) */
    lv_obj_t *vol_hit = nullptr; /* the volume chip shows the volume strip (design QA D-28) */
    lv_obj_t *time = nullptr, *remain = nullptr, *seek_track = nullptr, *seek_fill = nullptr, *seek_knob = nullptr;
    lv_obj_t *seek_hit = nullptr;
    int seek_x = 0, seek_w = 1;
    /* the volume strip: for a few seconds in place of the seek row, "-", the level, its bar, "+" */
    lv_obj_t *vol_root = nullptr, *vol_num = nullptr, *vol_track = nullptr, *vol_fill = nullptr;
    Button vol_minus, vol_plus;
    lv_timer_t *vol_timer = nullptr;
    bool vol_mode = false;
    static constexpr uint32_t kVolStripMs = 4000;
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
        lv_obj_t *mini_disc = nullptr, *mini_title = nullptr, *mini_sub = nullptr, *mini_bar = nullptr, *mini_fill = nullptr;
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
    enum HitKind { kKey, kBottom, kStrip, kChip, kSeek };
    struct NowHit {
        lv_obj_t *obj = nullptr;
        R drawn;
        HitKind kind = kKey;
    };
    std::vector<NowHit> now_hits;
    int hits_sig[5] = {-1, -1, -1, -1, -1};

    /* Every area grown to the target around its drawing, the stacked chips split (layout::split_pair); while the volume
     * strip is open its buttons take the band between the chips and the transport keys and the neighbours give way
     * (layout::place_strip_hits, design QA run 2): no strip area overlaps another one. */
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
            } else if (e.kind == kStrip && vol_mode) {
                ps.push_back(&e);
                set.strip.push_back(a);
            } else if (e.kind == kChip) {
                pc.push_back(&e);
                set.chips.push_back(a);
            } else if (e.kind == kSeek && !vol_mode) {
                place_area(e.obj, a.area);
            }
        }
        if (set.chips.size() == 2) layout::split_pair(set.chips[0], set.chips[1], target);
        if (vol_mode) layout::place_strip_hits(set, target, body_y, H);
        for (size_t i = 0; i < pk.size(); ++i) place_area(pk[i]->obj, set.keys[i].area);
        for (size_t i = 0; i < pb.size(); ++i) place_area(pb[i]->obj, set.bottom[i].area);
        for (size_t i = 0; i < ps.size(); ++i) place_area(ps[i]->obj, set.strip[i].area);
        for (size_t i = 0; i < pc.size(); ++i) place_area(pc[i]->obj, set.chips[i].area);
    }

    Button make_button(lv_obj_t *root, const layout::Btn &b, HitKind kind) {
        Button bt;
        const int x = X(b.box.x), y = Y(b.box.y), w = Wd(b.box), h = Hd(b.box);
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
        /* the chip rows of the class (layout.hpp); the volume chip teal with white text */
        for (size_t row = 0; row < g.chip_layout.size(); ++row) {
            for (int id : g.chip_layout[row]) {
                if (id < 0 || id > 3) continue;
                const int cy = Y(g.chip0.y + static_cast<double>(row) * g.chip_step);
                const bool vol = id == 1;
                chips[id].build(r, X(g.chip0.x), cy, P(g.chip0.h), P(vol ? g.vol_pad : g.chip_pad), P(g.chip_gap), P(g.chip_icon),
                                P(vol ? g.vol_px : g.chip_px), vol ? col::kTeal : col::kChip, vol ? col::kWhite : col::kInk, P(5),
                                chip_max);
            }
        }

        out_hit = hit(r, 0, 0, 1, 1, 0, 0, [this] {
            if (act.button) act.button("output");
        });
        vol_hit = hit(r, 0, 0, 1, 1, 0, 0, [this] { toggle_vol_strip(); });
        now_hits.push_back(NowHit{out_hit, R{}, kChip});
        now_hits.push_back(NowHit{vol_hit, R{}, kChip});
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
        seek_hit = hit(r, seek_x - kd / 2, Y(g.seek.y) + sh / 2 - kd / 2, seek_w + kd, kd, 0, target, [this] {
            lv_point_t p{};
            if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
            const double frac = std::clamp((p.x - seek_x) / static_cast<double>(seek_w), 0.0, 1.0);
            if (act.seek_to) act.seek_to(frac);
        });
        now_hits.push_back(NowHit{seek_hit, R{double(seek_x - kd / 2), double(Y(g.seek.y) + sh / 2 - kd / 2), double(seek_w + kd),
                                              double(kd)},
                                  kSeek});

        /* transport */
        for (const layout::Btn &b : g.transport) btns[b.id] = make_button(r, b, kKey);
        repeat_dash = box(r, X(g.repeat_dash.x), Y(g.repeat_dash.y), Wd(g.repeat_dash), std::max(2, Hd(g.repeat_dash)), col::kTeal,
                          P(2));
        /* the same mark under shuffle when it is on (the draft draws the repeat one) */
        const layout::Btn *sb = nullptr, *rb = nullptr;
        for (const layout::Btn &b : g.transport) {
            if (b.id == "shuffle") sb = &b;
            if (b.id == "repeat") rb = &b;
        }
        if (sb && rb) {
            const double dx = g.repeat_dash.x - rb->box.x;
            shuffle_dash = box(r, X(sb->box.x + dx), Y(g.repeat_dash.y), Wd(g.repeat_dash), std::max(2, Hd(g.repeat_dash)),
                               col::kTeal, P(2));
            repeat_one = label(r, Face::Bold, P(10), col::kTeal, X(rb->box.right() - 13), Y(rb->box.y + 3), 0);
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
        build_vol_strip(r);
    }

    /* The volume strip (design QA D-28): a tap on the volume chip puts it in place of the seek row for kVolStripMs (each
     * "-" / "+" starts the time again; a second tap on the chip closes it): the transport keys' cream buttons "-" and
     * "+" (aligned with the first and the last transport key, as wide as the first), the level and its bar between them.
     * The buttons are the touch target high where the band between the chips and the transport keys allows it (P(4)
     * clear of each); where the band is lower they fill it. Their touch areas take the band between the interactive
     * chips and the keys, and the neighbours' areas give way while the strip is open (layout_now_hits). */
    void build_vol_strip(lv_obj_t *r) {
        vol_root = lv_obj_create(r);
        plain(vol_root);
        lv_obj_set_pos(vol_root, 0, 0);
        lv_obj_set_size(vol_root, W, H);
        const int x0 = X(g.time.box.x), x1 = X(g.remain.box.right());
        const int sh = std::max(2, Hd(g.seek));
        const int cy = Y(g.seek.y) + sh / 2;
        const int top_lim = Y(g.chip0.y + (static_cast<double>(g.chip_layout.size()) - 1) * g.chip_step) + P(g.chip0.h) + P(4);
        int bot_lim = H;
        for (const layout::Btn &b : g.transport) bot_lim = std::min(bot_lim, Y(b.box.y));
        bot_lim -= P(4);
        const int row_h = std::max(P(g.time.lh), sh);
        const int bh = std::max(row_h, std::min(target, bot_lim - top_lim));
        const int by = std::max(top_lim, std::min(cy - bh / 2, bot_lim - bh));
        const int bw = Wd(g.transport.front().box);
        auto make = [&](Button &b, int x, Icon ic, int delta) {
            b.vis = box(vol_root, x, by, bw, bh, col::kButton, P(8));
            b.ic = ic;
            const int ip = P(22);
            b.icon = icon_obj(b.vis, ic, (bw - ip) / 2, (bh - ip) / 2, ip, ip, ip, col::kInk);
            b.hitbox = hit(vol_root, x, by, bw, bh, target, target, [this, delta] {
                if (act.volume_step) act.volume_step(delta);
                restart_vol_timer();
            });
            now_hits.push_back(NowHit{b.hitbox, R{double(x), double(by), double(bw), double(bh)}, kStrip});
        };
        make(vol_minus, x0, Icon::Minus, -5);
        make(vol_plus, x1 - bw, Icon::Plus, 5);
        const int my = by + bh / 2; /* the level and its bar on the buttons' middle */
        const lv_font_t *nf = font(Face::Mono, P(g.time.px));
        const int nw = text_width(nf, "100") + 2;
        const int nx = x0 + bw + P(10);
        vol_num = label(vol_root, Face::Mono, P(g.time.px), col::kInk, nx, my - lv_font_get_line_height(nf) / 2, nw,
                        LV_TEXT_ALIGN_CENTER);
        const int tx = nx + nw + P(8), tw = std::max(P(20), x1 - bw - P(12) - tx);
        vol_track = box(vol_root, tx, my - sh / 2, tw, sh, col::kChip, sh / 2);
        vol_fill = box(vol_track, 0, 0, sh, sh, col::kTeal, sh / 2);
        lv_obj_add_flag(vol_root, LV_OBJ_FLAG_HIDDEN);
        std::fprintf(stderr, "[vinyl] volume strip: buttons %dx%d (band %d, target %d), + at %.3f %.3f\n", bw, bh,
                     bot_lim - top_lim, target, (x1 - bw / 2.0) / W, (by + bh / 2.0) / H);
    }

    void set_vol_mode(bool on) {
        if (on == vol_mode) return;
        vol_mode = on;
        show(vol_root, on);
        for (lv_obj_t *o : {time, remain, seek_track, seek_knob, seek_hit}) show(o, !on);
        if (on) restart_vol_timer();
        else if (vol_timer) {
            lv_timer_delete(vol_timer);
            vol_timer = nullptr;
        }
        layout_now_hits();
    }

    void restart_vol_timer() {
        if (!vol_mode) return;
        if (!vol_timer) {
            vol_timer = lv_timer_create([](lv_timer_t *t) { static_cast<Impl *>(lv_timer_get_user_data(t))->vol_timeout(); },
                                        kVolStripMs, this);
            lv_timer_set_repeat_count(vol_timer, 1);
            lv_timer_set_auto_delete(vol_timer, false);
        } else {
            lv_timer_set_period(vol_timer, kVolStripMs);
            lv_timer_reset(vol_timer);
            lv_timer_set_repeat_count(vol_timer, 1);
            lv_timer_resume(vol_timer);
        }
    }

    void vol_timeout() { set_vol_mode(false); /* deletes the timer from its own callback: LVGL 9 allows it */ }

    void toggle_vol_strip() {
        if (!vol_mode && !last_now.vol_adjustable) { /* nothing the strip could change: say so (design QA run 2) */
            if (act.volume_unavailable) act.volume_unavailable();
            return;
        }
        set_vol_mode(!vol_mode);
        if (vol_mode) update_vol_strip(last_now);
    }

    void update_vol_strip(const NowView &v) {
        if (!vol_root) return;
        const int lvl = std::clamp(v.vol_level, 0, 100);
        set_text(vol_num, v.vol_level < 0 ? "--" : std::to_string(lvl));
        const int tw = lv_obj_get_width(vol_track), sh = lv_obj_get_height(vol_track);
        lv_obj_set_width(vol_fill, std::max(sh, tw * lvl / 100));
        show(vol_fill, v.vol_level > 0);
        if (vol_mode && !v.vol_adjustable) set_vol_mode(false);
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
        /* chips */
        const std::string texts[4] = {v.format, v.vol, v.output, v.extra_chip};
        const bool bt[4] = {false, false, v.output_bt, false};
        const int x0 = X(g.chip0.x);
        const int row_right = X(g.title.box.right()); /* the info column's right edge */
        const int space = P(g.chip_space);
        for (const std::vector<int> &row : g.chip_layout) {
            /* the volume chip keeps its whole width ("Vol 60", design QA D-18); the others share what is left, the last
             * of a full row ending with dots */
            int vol_need = 0;
            for (int i : row)
                if (i == 1 && chips[1].box) vol_need = chips[1].natural(texts[1], bt[1]);
            int x = x0;
            bool vol_placed = false;
            for (int i : row) {
                if (i < 0 || i > 3 || !chips[i].box) continue;
                const int reserve = (i != 1 && !vol_placed && vol_need > 0) ? vol_need + space : 0;
                chips[i].max_w = i == 1 ? 0 : std::max(P(30), row_right - x - reserve);
                chips[i].set(texts[i], bt[i], x);
                if (i == 1) vol_placed = true;
                if (chips[i].width) x += chips[i].width + space;
            }
        }
        /* the touch areas over the output and the volume chips follow the chips (layout_now_hits) */
        for (NowHit &e : now_hits) {
            const Chip *c = e.obj == out_hit ? &chips[2] : e.obj == vol_hit ? &chips[1] : nullptr;
            if (!c) continue;
            show(e.obj, c->width > 0);
            e.drawn = c->width > 0 ? R{double(c->x), double(c->y), double(c->width), double(c->h)} : R{};
        }
        const int sig[5] = {chips[1].x, chips[1].width, chips[2].x, chips[2].width, vol_mode ? 2 : 1};
        if (!std::equal(sig, sig + 5, hits_sig)) { /* the chips moved, or the strip opened or closed */
            std::copy(sig, sig + 5, hits_sig);
            layout_now_hits();
        }
        update_vol_strip(v);
        /* time */
        const uint64_t total = v.total_ms;
        const uint64_t pos = std::min(v.pos_ms, total ? total : v.pos_ms);
        set_text(time, v.has_track ? mmss(pos, true) : "--:--");
        set_text(remain, v.has_track && total ? "-" + mmss(total - pos, false) : "");
        const double frac = total ? static_cast<double>(pos) / total : 0.0;
        const int sh = lv_obj_get_height(seek_track);
        const int fw = std::max(sh, ri(frac * seek_w));
        lv_obj_set_width(seek_fill, fw);
        show(seek_fill, v.has_track && frac > 0);
        const int kd = lv_obj_get_width(seek_knob);
        lv_obj_set_x(seek_knob, seek_x + ri(frac * seek_w) - kd / 2);
        /* transport */
        if (auto it = btns.find("play"); it != btns.end()) icon_set(it->second.icon, v.playing ? Icon::Pause : Icon::Play, col::kWhite);
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
            L.heading = label(r, Face::Bold, S(12), col::kRowValue, ri(lg.heading.x), ri(lg.heading.y), ri(lg.heading.w) - S(90),
                              LV_TEXT_ALIGN_LEFT, ri(0.04 * 12 * s));
            const lv_font_t *hf = font(Face::Bold, S(12));
            lv_obj_set_y(L.heading, ri(lg.heading.y) + (ri(lg.heading.h) - lv_font_get_line_height(hf)) / 2);
        }
        if (p == Page::Browser) {
            L.sort_box = box(r, 0, ri(lg.panel.y) + S(4), S(40), S(22), col::kRowSel, S(5));
            L.sort_text = label(L.sort_box, Face::Bold, S(12), col::kRowText, S(10), 0, 0);
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
            L.mini_sub = label(L.strip, Face::Medium, S(11), col::kInk3, S(48), S(21), sw - S(48) - S(100));
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
            const int px = sx + sw - S(94), pw = S(44), nx = sx + sw - S(46), nw = S(40);
            mini_btn(L.mini_play, px, pw, Icon::Pause, "play");
            mini_btn(L.mini_next, nx, nw, Icon::Next, "next");
            const int hit_h = std::min(H, std::max(shh, target));
            const int hit_y = std::min(sy, H - hit_h); /* from the strip's top down, moved up only as far as the screen asks */
            layout::HitArea ap{R{double(px), double(sy), double(pw), double(shh)}, R{}}, an{R{double(nx), double(sy), double(nw), double(shh)}, R{}};
            ap.area = layout::grow_hit(ap.drawn, target, 0);
            an.area = layout::grow_hit(an.drawn, target, 0);
            layout::split_pair(ap, an, target);
            if (an.area.right() > W) { /* never past the screen's edge: both move left */
                const double d = an.area.right() - W;
                an.area.x -= d;
                ap.area.x -= d;
            }
            for (layout::HitArea *a : {&ap, &an}) {
                a->area.y = hit_y;
                a->area.h = hit_h;
            }
            place_area(L.mini_play.hitbox, ap.area);
            place_area(L.mini_next.hitbox, an.area);
            place_area(L.mini_open_hit, R{double(sx), double(hit_y), std::max(1.0, ap.area.x - sx), double(hit_h)});
            if (hit_y < sy) lv_obj_set_height(L.list->obj(), std::max(1, hit_y - ri(lg.list.y)));
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
                bt.hitbox = hit(r, x, y, w, h, target, target, [this, i] {
                    if (act.queue_button) act.queue_button(i);
                });
                L.qbtns.push_back(bt);
            }
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
        if (p == Page::Browser && L.strip) {
            show(L.strip, v.mini.shown);
            show(L.mini_open_hit, v.mini.shown);
            show(L.mini_play.vis, v.mini.shown);
            show(L.mini_play.hitbox, v.mini.shown);
            show(L.mini_next.vis, v.mini.shown);
            show(L.mini_next.hitbox, v.mini.shown);
            set_text(L.mini_title, v.mini.title);
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
        const N ns[4] = {{"ON OPENING A FILE", Notice::Kind::Info, "This format needs the FFmpeg libraries. Install them from Settings, Apps.", 20},
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
    void make_notice(lv_obj_t *parent, Notice::Kind k, int x, int y, int w, double s, lv_obj_t *&o, lv_obj_t *&ic, lv_obj_t *&tx) {
        auto S = [&](double v) { return std::max(1, ri(v * s)); };
        const bool danger = k == Notice::Kind::Danger;
        const int h = danger ? S(30) : S(50);
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
        int x = 0, y = 0, w = 0;
        double s = fit.s;
        if (cur == Page::Now) {
            const R b = layout::notice_box_now(g, danger ? 30 : 50);
            x = X(b.x);
            y = Y(b.bottom()) - ri((danger ? 30 : 50) * s);
            w = Wd(b);
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
        make_notice(root, n.kind, x, y, w, s, notice, notice_icon, notice_text);
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
    if (d_->vol_timer) lv_timer_delete(d_->vol_timer);
    d_->vol_timer = nullptr;
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
    d.cur = p;
    show(d.now_root, p == Page::Now);
    show(d.br.root, p == Page::Browser);
    show(d.qu.root, p == Page::Queue);
    show(d.out.root, p == Page::Output);
    show(d.not_root, p == Page::Notices);
    if (p != Page::Now) d.set_vol_mode(false);
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
