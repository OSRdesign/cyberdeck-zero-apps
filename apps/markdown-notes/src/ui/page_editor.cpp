/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: the editor (report 037 sections 2.1 and 2.4, the approved mock-up). The stock lv_textarea in the
 * monospace face (JetBrains Mono), word-wrapped, no colours, filling the body; the title is the note's name with a dot
 * while unsaved; a bar of two 9 mm toolbar keys at the bottom, Preview and More, hidden while the on-screen keyboard is
 * up (as Serial Console's). A tap places the cursor (and brings the on-screen keyboard back on a touch-only board).
 *
 * Keys (the app types into the textarea itself, through the key bridge): text, Enter, Backspace / Delete, the arrows
 * (Up / Down by display line), Home / End (the line), Ctrl+Home / Ctrl+End, Page Up / Down; Tab inserts a tab (shown as
 * the core's tab glyph, written back as a tab), Shift+Tab takes one indent off the line. Ctrl+S saves now, Ctrl+E
 * shows the preview, Ctrl+T inserts the date and time, Ctrl+Enter ticks the task of the cursor's line or follows the
 * link under the cursor, Ctrl+O the More menu. Esc (a physical keyboard's, or the header's) saves and goes back; the
 * on-screen keyboard's Esc puts the keyboard away.
 *
 * Saving: 1.5 s after the last edit, on Ctrl+S, on leaving, at the app's end and when it is hidden; the core's
 * save_note() checks the disk first: a note changed on disk meanwhile asks "Keep mine" / "Keep theirs", and the
 * version not kept is written beside the note as the core's conflict copy ("Name (deck 2026-10-24 1530).md"). At most
 * 128 KB is edited (larger notes open read-only in the preview).
 */
#include "notes_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"

#include "dates.hpp"
#include "fsutil.hpp"
#include "links.hpp"
#include "tasks.hpp"
#include "text_codec.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

using namespace mdnotes;

namespace notesui {

namespace {

constexpr std::uint64_t kAutosaveMs = 1500;
constexpr std::uint64_t kRetryMs = 5000;

void editor_event_cb(lv_event_t *e)
{
    auto *self = static_cast<App *>(lv_event_get_user_data(e));
    if (!self) return;
    lv_point_t p = {0, 0};
    if (lv_indev_t *indev = lv_event_get_indev(e)) lv_indev_get_point(indev, &p);
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: self->note_pressed(target, 0, p.x, p.y); break;
    case LV_EVENT_PRESSING: self->note_pressed(target, 1, p.x, p.y); break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: self->note_pressed(target, 2, p.x, p.y); break;
    case LV_EVENT_CLICKED: self->editor_tapped(p.x, p.y); break;
    default: break;
    }
}

} // namespace

int App::editor_px() const
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    return editor_text_px(settings_.text_size, m->text_px[CP0_TEXT_CAPTION], m->text_px[CP0_TEXT_BODY],
                          [](int px) { return cp0_ui_roller_font_px(px); });
}

void App::build_editor()
{
    lv_obj_t *p = make_page(PageId::Editor);
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const Geo g = geo();
    ed_ta_ = lv_textarea_create(p);
    if (lv_obj_get_group(ed_ta_)) lv_group_remove_obj(ed_ta_);
    lv_obj_remove_flag(ed_ta_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(ed_ta_, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_textarea_set_one_line(ed_ta_, false);
    // the cursor moves on a plain tap only (editor_tapped), never on a press: a 3 s hold opens the options, a drag
    // scrolls; no selection by dragging (it would fight the scroll)
    lv_textarea_set_cursor_click_pos(ed_ta_, false);
    lv_textarea_set_text_selection(ed_ta_, false);
    lv_obj_set_style_bg_color(ed_ta_, lv_color_hex(kEditorBg), 0);
    lv_obj_set_style_bg_opa(ed_ta_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ed_ta_, 2, 0);
    lv_obj_set_style_border_color(ed_ta_, lv_color_hex(kEditorBorder), 0);
    lv_obj_set_style_border_opa(ed_ta_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ed_ta_, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_pad_hor(ed_ta_, g.s + g.s / 2, 0);
    lv_obj_set_style_pad_ver(ed_ta_, g.s, 0);
    lv_obj_set_style_text_color(ed_ta_, lv_color_hex(kBodyText), 0);
    lv_obj_set_style_text_color(ed_ta_, lv_color_hex(0x6A6A6A), LV_PART_TEXTAREA_PLACEHOLDER);
    // the steady cursor of the toolkit's fields: a 2 px blue bar while the editor has the keys
    lv_obj_set_style_bg_opa(ed_ta_, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_side(ed_ta_, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ed_ta_, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ed_ta_, lv_color_hex(kLink), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ed_ta_, 0, LV_PART_CURSOR);
    lv_textarea_set_placeholder_text(ed_ta_, "Write here: plain text with Markdown");
    style_list_scrollbar(ed_ta_);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_CLICKED})
        lv_obj_add_event_cb(ed_ta_, editor_event_cb, code, this);
    layout_editor();
}

/* The textarea fills the body (above the on-screen keyboard while it is up); no bar: the options are a 3 s hold on
 * the text, or Ctrl+O (owner decision 2026-10-24). */
void App::layout_editor()
{
    if (!ed_ta_) return;
    const Geo g = geo();
    const int y0 = g.s / 2;
    const int bottom = panel_h_ - g.s / 2;
    lv_obj_set_pos(ed_ta_, g.x, y0);
    lv_obj_set_size(ed_ta_, g.cw, std::max(g.row, bottom - y0));
}

/* The textarea from the note just read: its text with the tabs drawn as the glyph, the cursor at the start. */
void App::editor_load()
{
    const std::uint64_t t0 = mono_ms();
    lv_obj_set_style_text_font(ed_ta_, font("JetBrainsMono-Regular.ttf", editor_px()), 0);
    const std::string edit = to_edit(note_.file.body.text, note_.file.body.fmt);
    lv_textarea_set_text(ed_ta_, edit.c_str());
    lv_textarea_set_cursor_pos(ed_ta_, 0);
    lv_obj_scroll_to_y(ed_ta_, 0, LV_ANIM_OFF);
    note_.dirty = false;
    note_.retry_ms = 0;
    ed_bytes_ = edit.size();
    layout_editor();
    lv_obj_update_layout(ed_ta_);
    log_line("editor " + note_.rel + ": " + std::to_string(edit.size()) + " bytes in " + std::to_string(mono_ms() - t0) + " ms");
}

std::string App::editor_text() const
{
    const char *t = ed_ta_ ? lv_textarea_get_text(ed_ta_) : "";
    return from_edit(t ? t : "", note_.file.body.fmt);
}

std::size_t App::editor_cursor_byte() const
{
    const char *t = lv_textarea_get_text(ed_ta_);
    return char_to_byte(t ? t : "", lv_textarea_get_cursor_pos(ed_ta_));
}

/* A plain tap on the text (no hold, no scroll: LVGL sends no click after a drag): the cursor goes there, typing, and the
 * keyboard is asked for again (the rule decides). A hold never moves the cursor (the textarea's own click-to-cursor is
 * off). */
void App::editor_tapped(int x, int y)
{
    if (tap_after_hold()) return;
    if (page() != PageId::Editor || popup_open() || mono_ms() - start_ms_ < 600) return;
    editor_flush();                       // what was typed goes where it was typed
    lv_obj_t *label = lv_textarea_get_label(ed_ta_);
    lv_area_t a;
    lv_obj_get_coords(label, &a);
    const int w = std::max(1, static_cast<int>(a.x2 - a.x1 + 1)), h = std::max(1, static_cast<int>(a.y2 - a.y1 + 1));
    lv_point_t rel = {std::clamp(x - static_cast<int>(a.x1), 0, w - 1), y - static_cast<int>(a.y1)};
    if (rel.y >= h) lv_textarea_set_cursor_pos(ed_ta_, LV_TEXTAREA_CURSOR_LAST);   // under the last line: the end
    else {
        rel.y = std::max<int32_t>(0, rel.y);
        lv_textarea_set_cursor_pos(ed_ta_, static_cast<int32_t>(lv_label_get_letter_on(label, &rel, true)));
    }
    editor_typing_ = true;
    osk_tap_ = true;
    invalidate();
}

void App::editor_changed()
{
    note_.dirty = true;
    note_.edit_ms = mono_ms();
    const char *t = lv_textarea_get_text(ed_ta_);
    ed_bytes_ = t ? std::strlen(t) : 0;
    invalidate();
}

void App::editor_insert(const std::string &text)
{
    editor_flush();
    if (text.empty()) return;
    if (ed_bytes_ + text.size() > kEditCap) {
        notice("The editor holds " + fmt_size(static_cast<std::int64_t>(kEditCap)) + " at most: this note is full", false);
        return;
    }
    lv_textarea_add_text(ed_ta_, text.c_str());
    editor_changed();
}

void App::editor_queue(const std::string &text)
{
    if (text.empty()) return;
    if (ed_bytes_ + ed_pending_.size() + text.size() > kEditCap) {
        notice("The editor holds " + fmt_size(static_cast<std::int64_t>(kEditCap)) + " at most: this note is full", false);
        return;
    }
    ed_pending_ += text;
    note_.dirty = true;
    note_.edit_ms = mono_ms();
}

void App::editor_flush()
{
    if (ed_pending_.empty() || !ed_ta_) return;
    std::string t;
    t.swap(ed_pending_);
    lv_textarea_add_text(ed_ta_, t.c_str());
    editor_changed();
}

void App::editor_tick(std::uint64_t now)
{
    if (page() != PageId::Editor || !note_.dirty || popup_open()) return;
    if (now - note_.edit_ms < kAutosaveMs || now < note_.retry_ms) return;
    editor_save(SaveWhy::Auto);
}

bool App::editor_save(SaveWhy why, std::function<void()> then)
{
    editor_flush();
    if (!note_.open() || !note_.dirty) {
        if (why == SaveWhy::Now && note_.open()) notice("Saved");
        if (then) then();
        return true;
    }
    const std::string body = editor_text();
    const SaveResult r = save_note(note_.path, body, note_.file.body.fmt, note_.file.stamp);
    switch (r.status) {
    case SaveStatus::Ok:
        note_.file.body.text = body;
        note_.dirty = false;
        note_.retry_ms = 0;
        last_save_ = r.recreated ? "recreated" : "ok";
        index_stale_ = true;
        if (r.recreated) notice("Saved again: the file had been deleted or moved");
        else if (why == SaveWhy::Now) notice("Saved");
        invalidate();
        if (then) then();
        return true;
    case SaveStatus::Conflict: {
        last_save_ = "conflict";
        if (why == SaveWhy::Quit) {            // no one to ask: ours goes beside it, theirs stays (both kept)
            const std::string copy = conflict_copy_path(note_.path, static_cast<std::time_t>(now_epoch()));
            int err = 0;
            if (create_note(copy, body, note_.file.body.fmt, nullptr, &err)) log_line("changed on disk: kept both, ours in " + copy);
            else log_line(std::string("changed on disk, and the copy failed: ") + std::strerror(err));
            note_.dirty = false;
            return false;
        }
        if (popup_open()) return false;        // already asking
        Popup p;
        p.kind = Popup::Confirm;
        p.title = "Changed on disk";
        p.body = conflict_body(note_.name);
        p.yes_text = "Keep mine";
        p.no_text = "Keep theirs";
        p.focus = 1;
        p.yn_keys = false;
        p.on_yes = [this, then] { resolve_conflict(true, then); };
        p.on_no = [this, then] { resolve_conflict(false, then); };
        editor_typing_ = false;                // the keyboard goes for the question
        popup_begin(std::move(p));
        return false;
    }
    case SaveStatus::Error:
        last_save_ = "error";
        note_.retry_ms = mono_ms() + kRetryMs;
        notice(std::string("Could not save: ") + std::strerror(r.err), false);
        if (why == SaveWhy::Leave && then && !popup_open()) {
            open_confirm("Could not save", "Your text could not be written (" + std::string(std::strerror(r.err)) +
                                               "). Leave anyway and lose the changes?",
                         [this, then] {
                             note_.dirty = false;
                             then();
                         },
                         "Leave", "Stay", true);
        }
        if (why == SaveWhy::Quit) log_line(std::string("could not save at the end: ") + std::strerror(r.err));
        return false;
    }
    return false;
}

/* Keep mine: the other version (on disk now) goes to the conflict copy, ours over the note. Keep theirs: ours goes to
 * the conflict copy, the note is read again into the editor. Both versions are kept either way. */
void App::resolve_conflict(bool keep_mine, std::function<void()> then)
{
    const std::string ours = editor_text();
    const std::string copy = conflict_copy_path(note_.path, static_cast<std::time_t>(now_epoch()));
    int err = 0;
    if (keep_mine) {
        std::string theirs;
        const ReadStatus rs = read_file(note_.path, theirs, kReadCap + 1, &err);
        if (rs == ReadStatus::Ok && !create_file_exclusive(copy, theirs, &err)) {
            notice(std::string("Could not keep the other version: ") + std::strerror(err), false);
            return;                            // nothing overwritten: the next save asks again
        }
        const SaveResult r = save_note(note_.path, ours, note_.file.body.fmt, note_.file.stamp, true);
        if (r.status != SaveStatus::Ok) {
            notice(std::string("Could not save: ") + std::strerror(r.err), false);
            return;
        }
        note_.file.body.text = ours;
        note_.dirty = false;
        last_save_ = "kept-mine";
        log_line("changed on disk: kept ours in " + note_.rel + ", theirs in " + copy);
        notice("Kept yours; the other version is in " + strip_md(base_name(copy)));
    } else {
        if (!create_note(copy, ours, note_.file.body.fmt, nullptr, &err)) {
            notice(std::string("Could not keep your version: ") + std::strerror(err), false);
            return;
        }
        note_.file = load_note(note_.path);
        last_save_ = "kept-theirs";
        log_line("changed on disk: kept theirs in " + note_.rel + ", ours in " + copy);
        if (note_.file.editable()) editor_load();
        notice("Kept theirs; your version is in " + strip_md(base_name(copy)));
        if (!note_.file.editable()) {
            post([this] { replace_top(Nav{PageId::Preview, "", note_.rel}); });
            index_stale_ = true;
            return;
        }
    }
    index_stale_ = true;
    invalidate();
    if (then) then();
}

/* The task of the cursor's line: one byte changes in the textarea (the box's mark), the cursor stays. */
void App::editor_tick_task()
{
    std::string t = lv_textarea_get_text(ed_ta_);
    std::size_t mark = 0;
    if (!toggle_task_on_line(t, editor_cursor_byte(), &mark)) {
        notice("This line is not a task: start it with - [ ]", false);
        return;
    }
    const uint32_t cur = lv_textarea_get_cursor_pos(ed_ta_);
    const std::size_t mark_ch = byte_to_char(t, mark);
    lv_textarea_set_cursor_pos(ed_ta_, static_cast<int32_t>(mark_ch + 1));
    lv_textarea_delete_char(ed_ta_);
    lv_textarea_add_text(ed_ta_, std::string(1, t[mark]).c_str());
    lv_textarea_set_cursor_pos(ed_ta_, static_cast<int32_t>(cur));
    editor_changed();
}

/* Ctrl+Enter: the task of the line, else the link under the cursor. */
void App::editor_ctrl_enter()
{
    const std::string t = lv_textarea_get_text(ed_ta_);
    const std::size_t pos = editor_cursor_byte();
    if (task_mark_on_line(t, pos) != std::string::npos) {
        editor_tick_task();
        return;
    }
    const LinkAt l = link_at(t, pos);
    if (!l.found) {
        notice("No task box or link here", false);
        return;
    }
    follow_link(plan_link(index_, note_.rel, l.wiki ? LinkKind::Wiki : classify_href(l.target), l.target));
}

void App::key_editor(const KeyEvent &e)
{
    if (!ed_ta_) return;
    const bool typing = !e.ctrl && !e.alt && (e.key == Key::Char || e.key == Key::Enter || e.key == Key::Tab);
    if (!typing) editor_flush();
    if (e.ctrl && !e.alt) {
        if (is_ctrl_letter(e, 's')) editor_save(SaveWhy::Now);
        else if (is_ctrl_letter(e, 'e')) editor_to_preview();
        else if (is_ctrl_letter(e, 'i')) show_note_info();
        else if (is_ctrl_letter(e, 'h')) open_help();
        else if (e.key == Key::Delete && !e.repeat) delete_this_note();
        else if (is_ctrl_letter(e, 't')) editor_insert(stamp_text(static_cast<std::time_t>(now_epoch())));
        else if (e.key == Key::Enter && !e.repeat) editor_ctrl_enter();
        else if (e.key == Key::Home) lv_textarea_set_cursor_pos(ed_ta_, 0);
        else if (e.key == Key::End) lv_textarea_set_cursor_pos(ed_ta_, LV_TEXTAREA_CURSOR_LAST);
        return;
    }
    if (e.alt) return;
    switch (e.key) {
    case Key::Char:
        if (e.text.empty()) return;
        editor_typing_ = true;
        editor_queue(e.text);
        break;
    case Key::Enter: editor_queue("\n"); break;
    case Key::Tab: editor_queue(note_.file.body.fmt.map_tabs ? kTabGlyph : "\t"); break;
    case Key::BackTab: {
        std::string t = lv_textarea_get_text(ed_ta_);
        std::size_t pos = editor_cursor_byte();
        if (!outdent_line(t, pos, note_.file.body.fmt.map_tabs ? kTabGlyph : nullptr)) return;
        lv_textarea_set_text(ed_ta_, t.c_str());
        lv_textarea_set_cursor_pos(ed_ta_, static_cast<int32_t>(byte_to_char(t, pos)));
        editor_changed();
        break;
    }
    case Key::Backspace:
        if (lv_textarea_get_cursor_pos(ed_ta_) == 0) return;
        lv_textarea_delete_char(ed_ta_);
        editor_changed();
        break;
    case Key::Delete: {
        const char *t = lv_textarea_get_text(ed_ta_);
        if (lv_textarea_get_cursor_pos(ed_ta_) >= byte_to_char(t ? t : "", t ? std::strlen(t) : 0)) return;
        lv_textarea_delete_char_forward(ed_ta_);
        editor_changed();
        break;
    }
    case Key::Left: lv_textarea_cursor_left(ed_ta_); break;
    case Key::Right: lv_textarea_cursor_right(ed_ta_); break;
    case Key::Up: lv_textarea_cursor_up(ed_ta_); break;
    case Key::Down: lv_textarea_cursor_down(ed_ta_); break;
    case Key::Home:
    case Key::End: {
        const std::string t = lv_textarea_get_text(ed_ta_);
        const std::size_t pos = editor_cursor_byte();
        const std::size_t to = e.key == Key::Home ? line_begin(t, pos) : line_end(t, pos);
        lv_textarea_set_cursor_pos(ed_ta_, static_cast<int32_t>(byte_to_char(t, to)));
        break;
    }
    case Key::PageUp:
    case Key::PageDown: {
        const lv_font_t *f = lv_obj_get_style_text_font(ed_ta_, LV_PART_MAIN);
        const int lines = std::max(1, static_cast<int>(lv_obj_get_content_height(ed_ta_)) / std::max(1, line_h(f)) - 1);
        for (int i = 0; i < lines; ++i) {
            if (e.key == Key::PageUp) lv_textarea_cursor_up(ed_ta_);
            else lv_textarea_cursor_down(ed_ta_);
        }
        break;
    }
    default: break;
    }
}

} // namespace notesui
