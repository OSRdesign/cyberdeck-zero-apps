/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: "Add a network folder" (see share_form.hpp). Positions from core/layout list_geom (the list pages' dark panel at
 * the class's scale); paddings, text sizes and the pills are the design boards' (Add-480x320, Add-320x480), scaled.
 */
#include "share_form.hpp"

#include "gfx.hpp"
#include "library_pages.hpp"
#include "theme.hpp"

#include "cp0_ui_widgets.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace vinyl {
namespace ui {

namespace {
int ri(double v) { return static_cast<int>(std::lround(v)); }
} // namespace

struct ShareForm::Impl {
    std::function<lv_obj_t *()> make_root;
    cp0_ui_roller_page_t tk{};
    int W = 0, H = 0, body_y = 0, target = 44;
    ShareFormEvents ev;
    layout::ListGeom lg;
    double s = 1;
    bool two_col = true;

    lv_obj_t *root = nullptr, *area = nullptr, *status = nullptr, *hint = nullptr, *sign_cap = nullptr;
    lv_obj_t *cur_in = nullptr, *cur_out = nullptr; /* the keys' cursor: in the scrolled area (fields, pills), on the page (buttons) */
    struct F {
        shares::Field id = shares::Field::None;
        lv_obj_t *cap = nullptr;
        std::unique_ptr<cp0::ui::TextField> field;
        int x = 0, y = 0, w = 0;
    };
    std::array<F, 6> f; /* server, share, folder, name, user, password */
    LibButton guest, user, cancel, ok;
    bool guest_on = true, busy = false;
    int area_y = 0, area_h = 0, field_h = 0, cap_h = 0, sign_y = 0, user_y = 0, status_y = 0;
    int cursor = 0;
    int osk_listener = 0, inset = 0;
    std::string status_text;
    bool status_error = false;

    int S(double v) const { return std::max(1, ri(v * s)); }

    /* the keys' items, in order: the fields above the pills, the pills, the user's fields, Cancel, Check and add */
    enum Item { kF0, kF1, kF2, kF3, kGuest, kUser, kF4, kF5, kCancel, kOk };
    std::vector<int> items() const {
        std::vector<int> v = {kF0, kF1, kF2, kF3, kGuest, kUser};
        if (!guest_on) {
            v.push_back(kF4);
            v.push_back(kF5);
        }
        v.push_back(kCancel);
        v.push_back(kOk);
        return v;
    }
    static int field_of(int item) {
        switch (item) {
        case kF0: return 0;
        case kF1: return 1;
        case kF2: return 2;
        case kF3: return 3;
        case kF4: return 4;
        case kF5: return 5;
        default: return -1;
        }
    }
    static int item_of_field(int i) { return i < 4 ? i : i == 4 ? kF4 : kF5; }

    static void field_press_cb(lv_event_t *e) {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(e));
        lv_obj_t *o = lv_event_get_target_obj(e);
        for (int i = 0; i < 6; ++i)
            if (self->f[i].field && self->f[i].field->focus_object() == o) {
                self->end_others(i); /* before the field's own click starts its typing: one field types at a time */
                self->cursor = item_of_field(i);
                self->draw_cursor();
            }
    }

    void end_others(int keep) {
        for (int i = 0; i < 6; ++i)
            if (i != keep && f[i].field && f[i].field->editing()) f[i].field->end_edit();
    }

    void build() {
        lg = layout::list_geom(layout::ListPage::Browser, W, H, body_y);
        s = lg.s;
        two_col = lg.panel.w / s >= 400;
        root = make_root();
        box(root, ri(lg.panel.x), ri(lg.panel.y), ri(lg.panel.w), ri(lg.panel.h), col::kList, S(10));
        const int px = ri(lg.panel.x) + S(12), pw = ri(lg.panel.w) - 2 * S(12);
        /* the buttons at the panel's bottom */
        const int bh = std::max(S(two_col ? 38 : 40), std::min(target, S(46)));
        const int by = ri(lg.panel.bottom()) - S(10) - bh;
        if (two_col) {
            const int okw = S(124), cw = S(96), gap = S(10);
            ok.build(root, px + pw - okw, by, okw, bh, S(14), col::kPrimary, col::kWhite, "Check and add", target, [this] { tap(kOk); });
            cancel.build(root, px + pw - okw - gap - cw, by, cw, bh, S(14), col::kRowSel, col::kRowText, "Cancel", target, [this] { tap(kCancel); });
            hint = label(root, Face::Bold, S(11), col::kRowValue, px, by + (bh - lv_font_get_line_height(font(Face::Bold, S(11)))) / 2,
                         pw - okw - cw - 2 * gap);
            set_text(hint, "Enter: next field");
        } else {
            const int gap = S(8), cw = (pw - gap) * 10 / 26;
            cancel.build(root, px, by, cw, bh, S(14), col::kRowSel, col::kRowText, "Cancel", target, [this] { tap(kCancel); });
            ok.build(root, px + cw + gap, by, pw - cw - gap, bh, S(14), col::kPrimary, col::kWhite, "Check and add", target, [this] { tap(kOk); });
        }
        /* the status line over the buttons, two lines high (always in view: a refusal is never scrolled away), the fields and
         * the pills in a scrolled area above it */
        const lv_font_t *stf = font(Face::Bold, S(11));
        const int st_line = std::max(S(15), lv_font_get_line_height(stf));
        const int st_h = 2 * st_line;
        const int st_y = by - (bh < target ? (target - bh) / 2 : 0) - S(4) - st_h;
        status = label(root, Face::Bold, S(11), col::kRowValue, px, st_y, pw);
        lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_height(status, st_h);
        lv_obj_set_style_text_line_space(status, st_line - lv_font_get_line_height(stf), 0);
        area_y = ri(lg.panel.y) + S(8);
        area_h = std::max(S(40), st_y - S(4) - area_y);
        area = lv_obj_create(root);
        plain(area);
        lv_obj_set_pos(area, px - S(4), area_y);
        lv_obj_set_size(area, pw + S(8), area_h);
        lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(area, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_width(area, std::max(2, ri(3 * s)), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(area, lv_color_hex(col::kRowValue), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(area, LV_OPA_50, LV_PART_SCROLLBAR);
        cur_in = box(area, 0, 0, 10, 10, col::kList, S(8), LV_OPA_TRANSP);
        lv_obj_set_style_border_width(cur_in, 2, 0);
        lv_obj_set_style_border_color(cur_in, lv_color_hex(col::kShareOk), 0);
        lv_obj_set_style_border_opa(cur_in, LV_OPA_COVER, 0);
        cur_out = box(root, 0, 0, 10, 10, col::kList, S(8), LV_OPA_TRANSP);
        lv_obj_set_style_border_width(cur_out, 2, 0);
        lv_obj_set_style_border_color(cur_out, lv_color_hex(col::kShareOk), 0);
        lv_obj_set_style_border_opa(cur_out, LV_OPA_COVER, 0);
        lv_obj_add_flag(cur_in, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cur_out, LV_OBJ_FLAG_HIDDEN);

        const lv_font_t *cf = font(Face::Bold, S(10));
        cap_h = lv_font_get_line_height(cf) + S(3);
        field_h = tk.row_h > 0 ? tk.row_h : S(34);
        const int ax = S(4), aw = pw;
        const int gap = S(10);
        const int c0 = two_col ? (aw - gap) * 13 / 23 : aw, c1 = two_col ? aw - gap - c0 : aw;
        struct Spec {
            const char *cap, *placeholder;
            size_t max;
            shares::Field id;
            cp0::ui::TextType type;
        };
        /* the hints in the fields: short (the toolkit's field text is large on the bigger classes) */
        const Spec specs[6] = {{"SERVER (IP OR NAME)", "192.168.1.20", 253, shares::Field::Host, cp0::ui::TextType::Text},
                               {"SHARE NAME", "music", 80, shares::Field::Share, cp0::ui::TextType::Text},
                               {"FOLDER IN THE SHARE (OPTIONAL)", "/", 200, shares::Field::Subfolder, cp0::ui::TextType::Text},
                               {"NAME IN VINYL (OPTIONAL)", "", 32, shares::Field::Name, cp0::ui::TextType::Text},
                               {"USER NAME", "", 64, shares::Field::User, cp0::ui::TextType::Text},
                               {"PASSWORD", "", 256, shares::Field::Password, cp0::ui::TextType::Password}};
        const int row = cap_h + field_h + S(8);
        auto place = [&](int i, int x, int y, int w) {
            F &fd = f[static_cast<size_t>(i)];
            fd.id = specs[i].id;
            fd.x = x;
            fd.y = y;
            fd.w = w;
            fd.cap = label(area, Face::Bold, S(10), col::kRowValue, x, y, w, LV_TEXT_ALIGN_LEFT, ri(0.06 * 10 * s));
            lv_label_set_long_mode(fd.cap, LV_LABEL_LONG_MODE_DOTS);
            set_text(fd.cap, specs[i].cap);
            cp0_ui_roller_page_t t = tk;
            t.roller_w = std::max(10, w);
            cp0::ui::TextFieldSpec sp;
            sp.label = "";
            sp.placeholder = specs[i].placeholder;
            sp.max_bytes = specs[i].max;
            sp.type = specs[i].type;
            cp0::ui::TextFieldEvents fe;
            fe.submit = [this, i](const std::string &) { next_after(i); };
            fd.field = std::make_unique<cp0::ui::TextField>(area, t, std::move(sp), std::move(fe));
            lv_obj_t *r = fd.field->row();
            if (lv_obj_get_child_count(r) > 1) lv_obj_add_flag(lv_obj_get_child(r, 0), LV_OBJ_FLAG_HIDDEN); /* our own caption above */
            lv_obj_set_pos(r, x, y + cap_h);
            lv_obj_add_event_cb(fd.field->focus_object(), field_press_cb, LV_EVENT_PRESSED, this);
        };
        int y = S(2);
        if (two_col) {
            place(0, ax, y, c0);
            place(1, ax + c0 + gap, y, c1);
            y += row;
            place(2, ax, y, c0);
            place(3, ax + c0 + gap, y, c1);
            y += row;
        } else {
            for (int i = 0; i < 4; ++i) {
                place(i, ax, y, aw);
                y += row;
            }
        }
        sign_y = y;
        sign_cap = label(area, Face::Bold, S(10), col::kRowValue, ax, y, aw, LV_TEXT_ALIGN_LEFT, ri(0.06 * 10 * s));
        set_text(sign_cap, "SIGN IN");
        const int ph = S(34);
        const lv_font_t *pf = font(Face::Bold, S(13));
        const std::string uw = two_col ? "User name and password" : "User and password";
        const int gw = text_width(pf, "Guest") + 2 * S(18), uwid = std::min(aw - gw - S(8), text_width(pf, uw) + 2 * S(two_col ? 18 : 12));
        guest.build(area, ax, y + cap_h, gw, ph, S(13), col::kTeal, col::kWhite, "Guest", target, [this] { tap(kGuest); });
        user.build(area, ax + gw + S(8), y + cap_h, uwid, ph, S(13), col::kRowSel, col::kRowText, uw, target, [this] { tap(kUser); });
        lv_obj_set_style_radius(guest.vis, ph / 2, 0);
        lv_obj_set_style_radius(user.vis, ph / 2, 0);
        y += cap_h + ph + S(10);
        user_y = y;
        if (two_col) {
            place(4, ax, y, c0);
            place(5, ax + c0 + gap, y, c1);
        } else {
            place(4, ax, y, aw);
            place(5, ax, y + row, aw);
        }
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        apply_guest();
        osk_listener = cp0::ui::osk::add_inset_listener([this](int in) { on_inset(in); });
    }

    void apply_guest() {
        set_bg(guest.vis, guest_on ? col::kTeal : col::kRowSel);
        lv_obj_set_style_text_color(guest.word, lv_color_hex(guest_on ? col::kWhite : col::kRowText), 0);
        set_bg(user.vis, guest_on ? col::kRowSel : col::kTeal);
        lv_obj_set_style_text_color(user.word, lv_color_hex(guest_on ? col::kRowText : col::kWhite), 0);
        for (int i = 4; i < 6; ++i) {
            show(f[static_cast<size_t>(i)].cap, !guest_on);
            show(f[static_cast<size_t>(i)].field->row(), !guest_on);
            if (guest_on && f[static_cast<size_t>(i)].field->editing()) f[static_cast<size_t>(i)].field->end_edit();
        }
        draw_cursor();
    }

    void set_status() {
        const std::string def = "Read only. Vinyl checks the share before it adds it.";
        set_text(status, status_text.empty() ? def : status_text);
        lv_obj_set_style_text_color(status, lv_color_hex(status_error ? col::kAmber : status_text.empty() ? col::kRowValue : col::kRowText), 0);
    }

    bool any_editing() const {
        for (const F &fd : f)
            if (fd.field && fd.field->editing()) return true;
        return false;
    }

    /* Enter in field i: the next visible field types; after the last one the cursor sits on Check and add */
    void next_after(int i) {
        lv_async_call([](void *p) {
            auto *pair = static_cast<std::pair<Impl *, int> *>(p);
            Impl *self = pair->first;
            const int from = pair->second;
            delete pair;
            const int last = self->guest_on ? 3 : 5;
            if (self->f[static_cast<size_t>(from)].field) self->f[static_cast<size_t>(from)].field->end_edit();
            if (from >= last) {
                self->cursor = kOk;
                self->draw_cursor();
                return;
            }
            self->edit_field(from + 1);
        }, new std::pair<Impl *, int>(this, i));
    }

    void edit_field(int i) {
        if (i < 0 || i > 5 || busy) return;
        if (i >= 4 && guest_on) return;
        end_others(i);
        cursor = item_of_field(i);
        f[static_cast<size_t>(i)].field->begin_edit();
        draw_cursor();
    }

    void tap(int item) {
        end_others(-1);
        cursor = item;
        act(item);
        draw_cursor();
    }

    void act(int item) {
        if (field_of(item) >= 0) {
            edit_field(field_of(item));
            return;
        }
        switch (item) {
        case kGuest:
        case kUser: {
            const bool g = item == kGuest;
            if (g != guest_on) {
                guest_on = g;
                apply_guest();
                if (ev.sign_in) ev.sign_in(guest_on);
                if (!guest_on) edit_field(4); /* the user's name types at once (and comes into view) */
            }
            break;
        }
        case kCancel:
            if (ev.cancel) ev.cancel();
            break;
        case kOk:
            if (!busy && ev.submit) ev.submit();
            break;
        default: break;
        }
    }

    /* the cursor's outline around the current item (hidden while a field types: its own border shows it) */
    void draw_cursor() {
        const int it = cursor;
        const int pad = S(3);
        show(cur_in, false);
        show(cur_out, false);
        if (any_editing()) return;
        const int fi = field_of(it);
        if (fi >= 0) {
            const F &fd = f[static_cast<size_t>(fi)];
            lv_obj_set_pos(cur_in, fd.x - pad, fd.y + cap_h - pad);
            lv_obj_set_size(cur_in, fd.w + 2 * pad, field_h + 2 * pad);
            show(cur_in, true);
            lv_obj_move_foreground(cur_in);
            lv_obj_scroll_to_view(cur_in, LV_ANIM_OFF);
            return;
        }
        const LibButton *b = it == kGuest ? &guest : it == kUser ? &user : it == kCancel ? &cancel : &ok;
        lv_obj_t *box_ = (it == kGuest || it == kUser) ? cur_in : cur_out;
        lv_obj_set_pos(box_, b->x - pad, b->y - pad);
        lv_obj_set_size(box_, b->w + 2 * pad, b->h + 2 * pad);
        lv_obj_set_style_radius(box_, it == kGuest || it == kUser ? (b->h + 2 * pad) / 2 : S(8), 0);
        show(box_, true);
        lv_obj_move_foreground(box_);
        if (box_ == cur_in) lv_obj_scroll_to_view(cur_in, LV_ANIM_OFF);
    }

    bool key(uint32_t k) {
        if (any_editing()) return false;
        const std::vector<int> v = items();
        auto pos = std::find(v.begin(), v.end(), cursor);
        int i = pos == v.end() ? 0 : static_cast<int>(pos - v.begin());
        if (k == LV_KEY_UP || k == LV_KEY_DOWN) {
            i = std::clamp(i + (k == LV_KEY_DOWN ? 1 : -1), 0, static_cast<int>(v.size()) - 1);
            cursor = v[static_cast<size_t>(i)];
            draw_cursor();
            return true;
        }
        if ((k == LV_KEY_LEFT || k == LV_KEY_RIGHT) && (cursor == kGuest || cursor == kUser)) {
            cursor = k == LV_KEY_LEFT ? kGuest : kUser;
            act(cursor);
            draw_cursor();
            return true;
        }
        if (k == LV_KEY_ENTER) {
            act(cursor);
            draw_cursor();
            return true;
        }
        return false;
    }

    void on_inset(int in) {
        inset = in;
        if (!area) return;
        const int bottom = in > 0 ? std::min(area_y + area_h, H - in - S(4)) : area_y + area_h;
        lv_obj_set_height(area, std::max(S(30), bottom - area_y));
        /* nothing of the page under the keyboard: the buttons go while it is up (Enter on the last field reaches them) */
        cancel.show(in == 0);
        ok.show(in == 0);
        if (hint) show(hint, in == 0);
        if (status) show(status, in == 0);
        if (in > 0) show(cur_out, false);
        for (const F &fd : f)
            if (fd.field && fd.field->editing()) lv_obj_scroll_to_view_recursive(fd.field->row(), LV_ANIM_OFF);
    }
};

ShareForm::ShareForm(std::function<lv_obj_t *()> make_root, const cp0_ui_roller_page_t &tk, int screen_w, int screen_h, int body_y, int target,
                     ShareFormEvents ev)
    : d_(std::make_unique<Impl>()) {
    d_->make_root = std::move(make_root);
    d_->tk = tk;
    d_->W = screen_w;
    d_->H = screen_h;
    d_->body_y = body_y;
    d_->target = target;
    d_->ev = std::move(ev);
    d_->build();
}

ShareForm::~ShareForm() {
    if (d_->osk_listener) cp0::ui::osk::remove_inset_listener(d_->osk_listener);
    for (auto &fd : d_->f) fd.field.reset();
}

lv_obj_t *ShareForm::root() const { return d_->root; }

void ShareForm::open(const shares::AddForm &prefill, bool edit) {
    Impl &d = *d_;
    d.end_others(-1);
    const std::string texts[6] = {prefill.host, prefill.share, prefill.subfolder, prefill.name, prefill.user, prefill.password};
    for (int i = 0; i < 6; ++i) {
        if (i == 5) d.f[5].field->wipe();
        d.f[static_cast<size_t>(i)].field->set_text(texts[i]);
    }
    d.guest_on = prefill.guest;
    d.busy = false;
    d.status_text.clear();
    d.status_error = false;
    d.set_status();
    d.apply_guest();
    d.ok.set_enabled(true);
    lv_obj_scroll_to_y(d.area, 0, LV_ANIM_OFF);
    d.cursor = Impl::kF0;
    if (edit) d.edit_field(0);
    else d.draw_cursor();
}

void ShareForm::update(const ShareFormView &v) {
    Impl &d = *d_;
    if (v.guest != d.guest_on) {
        d.guest_on = v.guest;
        d.apply_guest();
    }
    if (v.status != d.status_text || v.status_error != d.status_error) {
        d.status_text = v.status;
        d.status_error = v.status_error;
        d.set_status();
    }
    if (v.busy != d.busy) {
        d.busy = v.busy;
        d.ok.set_enabled(!v.busy);
        if (v.busy) d.end_others(-1);
    }
}

shares::AddForm ShareForm::values() const {
    const Impl &d = *d_;
    shares::AddForm a;
    a.host = d.f[0].field->text();
    a.share = d.f[1].field->text();
    a.subfolder = d.f[2].field->text();
    a.name = d.f[3].field->text();
    a.guest = d.guest_on;
    if (!d.guest_on) {
        a.user = d.f[4].field->text();
        a.password = d.f[5].field->text();
    }
    return a;
}

void ShareForm::focus(shares::Field fl) {
    Impl &d = *d_;
    for (int i = 0; i < 6; ++i)
        if (d.f[static_cast<size_t>(i)].id == fl) d.edit_field(i);
}

void ShareForm::end_edit() {
    d_->end_others(-1);
    d_->draw_cursor();
}

bool ShareForm::editing() const { return d_->any_editing(); }

bool ShareForm::key(uint32_t lv_key) { return d_->key(lv_key); }

void ShareForm::wipe() {
    Impl &d = *d_;
    if (d.f[5].field) d.f[5].field->wipe();
}

} // namespace ui
} // namespace vinyl
