/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: the preview (report 037 section 2.3, the approved mock-up). The note through md4c's model
 * (core markdown.hpp): one lv_spangroup per block in a scrolling column (RSS Reader's block-per-label reader, with
 * inline runs): headings in sizes (h2 gold), bold (Montserrat Bold), italic (FreeType's synthetic oblique: there is no
 * italic face), strike-through, inline code in the monospace face, code blocks in it on a darker box, quotes with a bar,
 * nested and numbered lists, rules, task boxes drawn as boxes, links underlined in blue, the frontmatter muted at the
 * top. At most 400 blocks, then a muted "Preview truncated" line.
 *
 * Touch: a drag scrolls; a tap on a task box ticks it (core toggle_task_at: one byte, then the atomic save) and the
 * preview is drawn again in place; a tap on a link follows it (a missing note is created and opens in the editor; a web
 * address shows in a box). Keys: Up / Down, Page Up / Down, Space, Home / End scroll; Tab / Shift+Tab move a ring over
 * the boxes and the links, Enter acts on the ringed one; Ctrl+E edits. The column is the one interactive object (the
 * blocks are not clickable: a tap is hit-tested here), so the shared layout check sees one big target.
 */
#include "notes_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"

#include "fsutil.hpp"
#include "tasks.hpp"
#include "utf8.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstring>

using namespace mdnotes;

namespace notesui {

namespace {

void preview_event_cb(lv_event_t *e)
{
    auto *self = static_cast<App *>(lv_event_get_user_data(e));
    if (!self) return;
    lv_point_t p = {0, 0};
    if (lv_indev_t *indev = lv_event_get_indev(e)) lv_indev_get_point(indev, &p);
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED: self->preview_clicked(p.x, p.y); break;
    case LV_EVENT_PRESSED:
        self->preview_touched();
        self->note_pressed(target, 0, p.x, p.y);
        break;
    case LV_EVENT_PRESSING: self->note_pressed(target, 1, p.x, p.y); break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: self->note_pressed(target, 2, p.x, p.y); break;
    default: break;
    }
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return o;
}

bool inside(const lv_area_t &a, int x, int y, int pad)
{
    return x >= a.x1 - pad && x <= a.x2 + pad && y >= a.y1 - pad && y <= a.y2 + pad;
}

} // namespace

int App::preview_px() const
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    return preview_text_px(settings_.text_size, m->text_px[CP0_TEXT_CAPTION], m->text_px[CP0_TEXT_BODY], m->text_px[CP0_TEXT_HEADING],
                           m->text_px[CP0_TEXT_TITLE], [](int px) { return cp0_ui_roller_font_px(px); });
}

void App::build_preview()
{
    lv_obj_t *p = make_page(PageId::Preview);
    const Geo g = geo();
    // the whole body is the note (no bar: its options are a 3 s hold on it, or Ctrl+O)
    pv_ = lv_obj_create(p);
    lv_obj_remove_style_all(pv_);
    lv_obj_set_scroll_dir(pv_, LV_DIR_VER);
    lv_obj_remove_flag(pv_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(pv_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(pv_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(pv_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(pv_, g.inset, 0);
    lv_obj_set_style_pad_top(pv_, g.s / 2, 0);
    lv_obj_set_style_pad_bottom(pv_, g.m * 2, 0);
    style_list_scrollbar(pv_);
    for (lv_event_code_t code : {LV_EVENT_CLICKED, LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(pv_, preview_event_cb, code, this);
    layout_preview();
}

void App::layout_preview()
{
    if (!pv_) return;
    const Geo g = geo();
    lv_obj_set_pos(pv_, 0, 0);
    lv_obj_set_size(pv_, g.w, std::max(1, panel_h_));
}

/* The note's blocks as widgets (keep_scroll: drawn again in place after a tick, a text size change). */
void App::render_preview_content(bool keep_scroll)
{
    const std::uint64_t t0 = mono_ms();
    const int scroll = keep_scroll ? lv_obj_get_scroll_y(pv_) : 0;
    ring_obj_ = nullptr;
    lv_obj_clean(pv_);
    boxes_.clear();
    spans_.clear();
    block_objs_.clear();
    const bool help = page() == PageId::Help;
    const std::string &source = help ? markdown_help_text() : note_.file.body.text;
    doc_ = parse_markdown(source);
    targets_ = preview_targets(doc_);
    if (ring_ >= static_cast<int>(targets_.size())) ring_ = static_cast<int>(targets_.size()) - 1;

    const Geo g = geo();
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int px = preview_px();
    preview_built_px_ = px;
    auto snap = [](int v) { return cp0_ui_roller_font_px(v); };
    const lv_font_t *f_text = font("Montserrat-Medium.ttf", px);
    const lv_font_t *f_small = font("Montserrat-Medium.ttf", snap(px * 5 / 6));
    const lv_font_t *f_mono = font("JetBrainsMono-Regular.ttf", snap(px * 9 / 10));
    const int tw = std::max(10, g.w - 2 * g.inset);
    const int line_space = std::max(2, px / 5);
    const int mw = std::max(px * 3 / 2, text_w("00.", f_text) + g.s);      // the marker column of a list item
    const int quote_bar = std::max(3, g.s / 2);
    lv_obj_set_style_pad_row(pv_, std::max(g.s / 2, px / 3), 0);

    /* the face of a run: bold / italic / code of the block's base (a heading's base is bold at its size) */
    auto run_font = [&](std::uint8_t style, int size, bool base_bold) -> const lv_font_t * {
        if (style & kStyleCode) return font("JetBrainsMono-Regular.ttf", snap(size * 9 / 10));
        const bool bold = base_bold || (style & kStyleBold);
        const bool ital = style & kStyleItalic;
        return font(bold ? "Montserrat-Bold.ttf" : "Montserrat-Medium.ttf", size, ital);
    };
    auto spangroup = [&](lv_obj_t *parent, const Block &b, int bi, int width, int size, bool base_bold, std::uint32_t color) {
        lv_obj_t *sg = lv_spangroup_create(parent);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_width(sg, std::max(10, width));
        lv_obj_set_height(sg, LV_SIZE_CONTENT);
        lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
        lv_obj_set_style_text_font(sg, run_font(0, size, base_bold), 0);
        lv_obj_set_style_text_color(sg, lv_color_hex(color), 0);
        lv_obj_set_style_text_line_space(sg, line_space, 0);
        for (const Run &r : b.runs) {
            const std::string text = display_text(r.text);
            if (text.empty()) continue;
            lv_span_t *sp = lv_spangroup_add_span(sg);
            lv_span_set_text(sp, text.c_str());
            lv_style_t *st = lv_span_get_style(sp);
            lv_style_set_text_font(st, run_font(r.style, size, base_bold));
            std::uint32_t c = color;
            if (r.style & kStyleCode) c = kInlineCode;
            if (r.link >= 0) c = kLink;
            lv_style_set_text_color(st, lv_color_hex(c));
            int decor = LV_TEXT_DECOR_NONE;
            if (r.style & kStyleStrike) decor |= LV_TEXT_DECOR_STRIKETHROUGH;
            if (r.link >= 0) decor |= LV_TEXT_DECOR_UNDERLINE;
            if (decor != LV_TEXT_DECOR_NONE) lv_style_set_text_decor(st, static_cast<lv_text_decor_t>(decor));
            if (r.link >= 0) spans_.push_back({sg, sp, r.link, bi});
        }
        lv_spangroup_refresh(sg);
        return sg;
    };
    auto label = [&](lv_obj_t *parent, const std::string &text, const lv_font_t *f, std::uint32_t color, int width) {
        lv_obj_t *l = lv_label_create(parent);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, std::max(10, width));
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_line_space(l, line_space, 0);
        set_color(l, color);
        lv_label_set_text(l, text.c_str());
        return l;
    };

    if (doc_.blocks.empty()) label(pv_, empty_note_text(), f_text, kMuted, tw);
    for (std::size_t i = 0; i < doc_.blocks.size(); ++i) {
        const Block &b = doc_.blocks[i];
        const int bi = static_cast<int>(i);
        // the quote: a bar at the left, the content after it (deeper quotes indent further)
        lv_obj_t *outer = pv_;
        int width = tw;
        lv_obj_t *top_obj = nullptr;
        if (b.quote > 0) {
            lv_obj_t *q = plain(pv_);
            lv_obj_set_size(q, tw, LV_SIZE_CONTENT);
            lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(q, quote_bar, 0);
            lv_obj_set_style_border_color(q, lv_color_hex(kMuted), 0);
            lv_obj_set_style_border_opa(q, LV_OPA_COVER, 0);
            const int pad = g.m * b.quote;
            lv_obj_set_style_pad_left(q, pad, 0);
            outer = q;
            top_obj = q;
            width = tw - pad - quote_bar;
        }
        // a list: the marker column (bullet, number or task box), indented by depth
        lv_obj_t *parent = outer;
        if (b.list > 0) {
            lv_obj_t *row = plain(outer);
            lv_obj_set_size(row, width, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            const int indent = (b.list - 1) * mw;
            lv_obj_set_style_pad_left(row, indent, 0);
            lv_obj_t *cell = plain(row);
            const int lh = line_h(f_text) + line_space;
            lv_obj_set_size(cell, mw, lh);
            if (b.item && b.task) {
                const int bs = std::max(12, line_h(f_text) * 7 / 10);
                lv_obj_t *box = make_box(cell, 0, (lh - bs) / 2, bs, bs);
                lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_style_radius(box, std::max(3, bs / 5), 0);
                lv_obj_set_style_border_width(box, 2, 0);
                lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(box, lv_color_hex(b.checked ? kBoxOnBorder : kBoxBorder), 0);
                lv_obj_set_style_bg_color(box, lv_color_hex(kBoxOn), 0);
                lv_obj_set_style_bg_opa(box, b.checked ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                if (b.checked) {
                    lv_obj_t *tick = lv_label_create(box);
                    lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_style_text_font(tick, font("DejaVuSans.ttf", std::max(10, bs * 4 / 5)), 0);
                    set_color(tick, 0xFFFFFF);
                    lv_label_set_text(tick, "\xE2\x9C\x93");      // ✓
                    lv_obj_center(tick);
                }
                boxes_.push_back({box, bi});
            } else if (b.item) {
                lv_obj_t *mk = label(cell, list_marker(b), f_text, b.ordered ? kBodyText : kMuted, mw - g.s / 2);
                lv_label_set_long_mode(mk, LV_LABEL_LONG_MODE_CLIP);
            }
            parent = row;
            width = width - indent - mw;
            if (!top_obj) top_obj = row;
        }
        lv_obj_t *made = nullptr;
        switch (b.kind) {
        case BlockKind::Properties: made = label(parent, properties_text(b), f_small, kProps, width); break;
        case BlockKind::Heading: {
            made = spangroup(parent, b, bi, width, heading_px(b.level, px, snap), true, heading_gold(b.level) ? kGold : kText);
            if (!top_obj && i > 0) lv_obj_set_style_margin_top(made, g.s, 0);
            break;
        }
        case BlockKind::Paragraph: made = spangroup(parent, b, bi, width, px, false, kBodyText); break;
        case BlockKind::Code: {
            lv_obj_t *box = plain(parent);
            lv_obj_set_size(box, width, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(box, lv_color_hex(kCodeBg), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(box, m->tok[CP0_TOK_RADIUS_S], 0);
            lv_obj_set_style_pad_all(box, g.s, 0);
            label(box, display_text(b.code), f_mono, kCodeText, width - 2 * g.s);
            made = box;
            break;
        }
        case BlockKind::Rule: {
            made = make_box(parent, 0, 0, width, 2);
            lv_obj_remove_flag(made, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(made, lv_color_hex(kSelected), 0);
            lv_obj_set_style_bg_opa(made, LV_OPA_COVER, 0);
            break;
        }
        }
        if (b.quote > 0 && b.kind == BlockKind::Paragraph && made) lv_obj_set_style_text_color(made, lv_color_hex(kQuoteText), 0);
        block_objs_.push_back(top_obj ? top_obj : made);
    }
    if (doc_.truncated) label(pv_, truncated_text(), f_small, kMuted, tw);
    lv_obj_update_layout(pv_);
    if (keep_scroll) lv_obj_scroll_to_y(pv_, scroll, LV_ANIM_OFF);
    else lv_obj_scroll_to_y(pv_, 0, LV_ANIM_OFF);
    update_ring();
    // the board test reads this line (report 037: the preview's layout time on the Zero 2 W)
    log_line("preview " + (help ? std::string("(Markdown help)") : note_.rel) + ": " + std::to_string(source.size()) + " bytes, " + std::to_string(doc_.blocks.size()) +
             " blocks" + (doc_.truncated ? " (cut)" : "") + " in " + std::to_string(mono_ms() - t0) + " ms");
}

void App::preview_touched()
{
    if (!preview_keys_) return;
    preview_keys_ = false;
    update_ring();
}

lv_obj_t *App::task_box(int n) const
{
    return n >= 0 && n < static_cast<int>(boxes_.size()) ? boxes_[static_cast<std::size_t>(n)].box : nullptr;
}

/* A tap: a task box (its area widened to a finger's), else the link span under the finger. */
void App::preview_clicked(int x, int y)
{
    if (tap_after_hold()) return;
    if (mono_ms() - start_ms_ < 600 || popup_open() || (page() != PageId::Preview && page() != PageId::Help)) return;
    if (page() == PageId::Help) {          // the examples act in notes, not here (a box would tick the note below)
        notice("An example: in your notes a tap on it acts");
        return;
    }
    const Geo g = geo();
    const int pad = std::max(g.s, (g.row - (boxes_.empty() ? 0 : static_cast<int>(lv_obj_get_height(boxes_[0].box)))) / 3);
    for (const TaskBox &tb : boxes_) {
        lv_area_t a;
        lv_obj_get_coords(tb.box, &a);
        if (inside(a, x, y, pad)) {
            const int block = tb.block;
            post([this, block] { toggle_task(block); });
            return;
        }
    }
    const lv_point_t p = {x, y};
    lv_obj_t *done = nullptr;
    for (const LinkSpan &ls : spans_) {
        if (ls.group == done) continue;
        lv_area_t a;
        lv_obj_get_coords(ls.group, &a);
        if (!inside(a, x, y, 0)) continue;
        done = ls.group;
        lv_span_t *hit = lv_spangroup_get_span_by_point(ls.group, &p);
        if (!hit) continue;
        for (const LinkSpan &l2 : spans_) {
            if (l2.span != hit || l2.link < 0 || l2.link >= static_cast<int>(doc_.links.size())) continue;
            const DocLink link = doc_.links[static_cast<std::size_t>(l2.link)];
            post([this, link] { follow_link(plan_link(index_, note_.rel, link.kind, link.target)); });
            return;
        }
    }
}

void App::preview_act(int target)
{
    if (target < 0 || target >= static_cast<int>(targets_.size())) return;
    if (page() != PageId::Preview) {
        notice("An example: in your notes Enter acts on it");
        return;
    }
    const PreviewTarget t = targets_[static_cast<std::size_t>(target)];
    if (t.task) {
        toggle_task(t.block);
        return;
    }
    if (t.link < 0 || t.link >= static_cast<int>(doc_.links.size())) return;
    const DocLink link = doc_.links[static_cast<std::size_t>(t.link)];
    follow_link(plan_link(index_, note_.rel, link.kind, link.target));
}

/* The box of a task: one byte of the body changes (core toggle_task_at), the atomic save checks the disk first; the
 * preview is drawn again where it was. A note changed on disk meanwhile is read again instead (tick it again). */
void App::toggle_task(int block)
{
    if (page() != PageId::Preview || block < 0 || block >= static_cast<int>(doc_.blocks.size())) return;
    const Block &b = doc_.blocks[static_cast<std::size_t>(block)];
    if (!b.task || b.task_offset == kNoOffset) return;
    if (!note_.file.editable()) {
        notice(readonly_text(note_.file.status), false);
        return;
    }
    std::string body = note_.file.body.text;
    if (!toggle_task_at(body, b.task_offset)) {
        notice("That box could not be ticked", false);
        return;
    }
    const SaveResult r = save_note(note_.path, body, note_.file.body.fmt, note_.file.stamp);
    switch (r.status) {
    case SaveStatus::Ok:
        note_.file.body.text = body;
        last_save_ = r.recreated ? "recreated" : "ok";
        index_stale_ = true;
        render_preview_content(true);
        break;
    case SaveStatus::Conflict:
        last_save_ = "conflict";
        note_.file = load_note(note_.path);
        render_preview_content(true);
        notice("Changed on disk: read again, tick it again", false);
        break;
    case SaveStatus::Error:
        last_save_ = "error";
        notice(std::string("Could not save: ") + std::strerror(r.err), false);
        break;
    }
    invalidate();
}

void App::preview_scroll(int dy)
{
    lv_obj_scroll_by_bounded(pv_, 0, -dy, LV_ANIM_OFF);
    lv_obj_update_layout(pv_);
    invalidate();
}

void App::preview_ring(int dir)
{
    if (targets_.empty()) {
        notice("No task box or link in this note");
        return;
    }
    preview_keys_ = true;
    ring_ = ring_next(ring_, dir, static_cast<int>(targets_.size()));
    update_ring();
}

/* The ring of the keys: an outline round the ringed box, or round the first line of the ringed link; scrolled into
 * view. None while the touch leads. */
void App::update_ring()
{
    if (ring_obj_) {
        lv_obj_delete(ring_obj_);
        ring_obj_ = nullptr;
    }
    for (const TaskBox &tb : boxes_) lv_obj_set_style_outline_width(tb.box, 0, 0);
    if (!preview_keys_ || ring_ < 0 || ring_ >= static_cast<int>(targets_.size())) return;
    const PreviewTarget t = targets_[static_cast<std::size_t>(ring_)];
    lv_obj_t *view = nullptr;
    if (t.task) {
        for (const TaskBox &tb : boxes_) {
            if (tb.block != t.block) continue;
            lv_obj_set_style_outline_width(tb.box, 3, 0);
            lv_obj_set_style_outline_color(tb.box, lv_color_hex(kAccent), 0);
            lv_obj_set_style_outline_pad(tb.box, 3, 0);
            lv_obj_set_style_outline_opa(tb.box, LV_OPA_COVER, 0);
            view = tb.box;
            break;
        }
    } else {
        for (const LinkSpan &ls : spans_) {
            if (ls.block != t.block || ls.link != t.link) continue;
            lv_obj_update_layout(ls.group);
            const lv_span_coords_t c = lv_spangroup_get_span_coords(ls.group, ls.span);
            const lv_area_t &a = c.heading;
            ring_obj_ = make_box(ls.group, a.x1 - 3, a.y1 - 1, std::max(4, a.x2 - a.x1 + 7), std::max(4, a.y2 - a.y1 + 3));
            lv_obj_add_flag(ring_obj_, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_remove_flag(ring_obj_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_width(ring_obj_, 2, 0);
            lv_obj_set_style_border_color(ring_obj_, lv_color_hex(kAccent), 0);
            lv_obj_set_style_border_opa(ring_obj_, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(ring_obj_, 4, 0);
            view = ls.group;
            break;
        }
    }
    if (view) {
        lv_obj_update_layout(pv_);
        lv_obj_scroll_to_view_recursive(view, LV_ANIM_OFF);
    }
    invalidate();
}

void App::scroll_to_heading(const std::string &heading)
{
    if (heading.empty()) return;
    for (std::size_t i = 0; i < doc_.blocks.size() && i < block_objs_.size(); ++i) {
        const Block &b = doc_.blocks[i];
        if (b.kind != BlockKind::Heading || !equal_ci(block_text(b), heading)) continue;
        lv_obj_update_layout(pv_);
        lv_obj_scroll_to_y(pv_, std::max(0, static_cast<int>(lv_obj_get_y(block_objs_[i])) - geo().s), LV_ANIM_OFF);
        return;
    }
    notice("No heading \"" + heading + "\" in this note", false);
}

void App::key_preview(const KeyEvent &e)
{
    const int view = std::max(1, static_cast<int>(lv_obj_get_height(pv_)));
    const int step = std::max(8, preview_built_px_ * 2);
    if (page() == PageId::Help) {          // the help: it scrolls and rings; its examples do nothing
        if (is_ctrl_letter(e, 'h')) back(false);
    } else if (is_ctrl_letter(e, 'h')) {
        open_help();
        return;
    } else if (is_ctrl_letter(e, 'e')) {
        preview_to_editor();
        return;
    }
    if (page() == PageId::Help) {
        if (e.ctrl) return;
    } else if (is_ctrl_letter(e, 'i')) {
        show_note_info();
        return;
    } else if (e.ctrl && e.key == Key::Delete && !e.repeat) {
        delete_this_note();
        return;
    }
    switch (e.key) {
    case Key::Up: preview_scroll(-step); break;
    case Key::Down: preview_scroll(step); break;
    case Key::PageUp: preview_scroll(-(view - step)); break;
    case Key::PageDown: preview_scroll(view - step); break;
    case Key::Home:
        lv_obj_scroll_to_y(pv_, 0, LV_ANIM_OFF);
        invalidate();
        break;
    case Key::End: preview_scroll(1 << 20); break;
    case Key::Tab: preview_ring(1); break;
    case Key::BackTab: preview_ring(-1); break;
    case Key::Enter:
        if (e.repeat) break;
        if (ring_ >= 0 && preview_keys_) preview_act(ring_);
        else notice("Tab rings a task box or a link, Enter acts on it");
        break;
    case Key::Left:
        if (!e.repeat) back(false);
        break;
    case Key::Char:
        if (e.text == " " && !e.ctrl && !e.alt) preview_scroll(e.shift ? -(view - step) : view - step);
        break;
    default: break;
    }
}

} // namespace notesui
