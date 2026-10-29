/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: Open or create (Ctrl+P) and New note.
 *
 *   Open or create  a field on top, the ranked matches under it (core fuzzy.hpp); 'Create "query"' first when no note
 *                   has exactly that name (at the vault root, or in the folders the query names; a refused name says
 *                   why on the row). Enter opens the selected row (the first). With the on-screen keyboard up only the
 *                   field fits at 640x480: Enter then puts the keyboard away and shows the matches. Picking a row
 *                   replaces this page: Esc from the note goes back to where Ctrl+P was pressed.
 *   New note        a field for the name and Create (Enter): the note is made empty and opens in the editor; a name
 *                   taken (ignoring case) asks "Open it?"; a refused name says why.
 */
#include "notes_app.hpp"

#include "app_style.hpp"
#include "hooks.hpp"

#include "fsutil.hpp"
#include "names.hpp"

#include <algorithm>

using namespace mdnotes;

namespace notesui {

namespace {
constexpr std::size_t kMaxQuery = 240;
}

/* ================================================================== Open or create */

void App::build_open()
{
    lv_obj_t *p = make_page(PageId::Open);
    const Geo g = geo();
    open_ta_ = make_text_field(p, kTagOpenField, kMaxQuery, g.row, "Part of a note's name");
    make_list(open_list_, p, g.row + g.s, panel_h_ - g.row - g.s, 3);
    open_list_.set_empty_text("No note yet: type a name, Enter creates it.", f_body_, kMuted);
    layout_open();
}

void App::layout_open()
{
    if (!open_ta_) return;
    const Geo g = geo();
    const int y0 = g.s / 2;
    lv_obj_set_pos(open_ta_, g.x, y0);
    lv_obj_set_size(open_ta_, g.cw, g.row);
    const int ly = y0 + g.row + g.s / 2;
    const int room = panel_h_ - ly;
    open_list_.set_hidden(room < g.row);                 // under the keyboard: only the field (the guidelines' rule)
    if (lv_obj_get_y(open_list_.obj()) != ly) open_list_.set_y(ly);
    open_list_.resize(std::max(1, room));
}

void App::refresh_open_rows()
{
    open_query_ = lv_textarea_get_text(open_ta_);
    open_items_ = open_rows(index_, vault_.path, open_query_, now_epoch());
    open_sel_ = 0;
    open_list_.set(build_rows(open_items_), open_items_.empty() ? -1 : 0);
    open_list_.scroll_to_top();
}

void App::render_open()
{
    std::size_t matches = 0;
    for (const ListItem &it : open_items_)
        if (it.id.rfind("note:", 0) == 0) ++matches;
    const bool empty = clean_typed_name(open_query_).empty();
    set_caption(open_caption(empty, matches));
    layout_open();
}

void App::key_open(const KeyEvent &e)
{
    if (e.key == Key::Up || e.key == Key::Down || e.key == Key::PageUp || e.key == Key::PageDown) {
        int sel = open_list_.selected();
        if (list_move(e, sel, open_list_.count(), 4)) {
            open_sel_ = sel;
            open_list_.set_selected(sel, true);
        }
        return;
    }
    if (e.key == Key::Enter) {
        if (e.repeat) return;
        // the keyboard goes, the matches show; a portrait screen keeps its matches under the field (the keyboard, or the
        // writing zone's band, leaves them room), so there Enter opens the selected one at once
        const bool matches_hidden = open_list_.hidden();
        if (open_typing_ && cp0::ui::osk::visible() && (matches_hidden || layout_.h <= layout_.w)) {
            open_typing_ = false;
            invalidate();
            return;
        }
        open_activate(open_list_.selected());
        return;
    }
    if (is_typing(e) || e.key == Key::Backspace || e.key == Key::Delete || e.key == Key::Left || e.key == Key::Right ||
        e.key == Key::Home || e.key == Key::End) {
        open_typing_ = true;
        const std::string before = lv_textarea_get_text(open_ta_);
        field_key(open_ta_, e, kMaxQuery);
        if (before != lv_textarea_get_text(open_ta_)) refresh_open_rows();
        invalidate();
    }
}

void App::open_activate(int index)
{
    const std::string id = open_list_.id_at(index);
    if (id.empty()) {
        if (!clean_typed_name(open_query_).empty()) notice("Choose a row first", false);
        return;
    }
    if (id == "create") {
        const NewTarget t = new_note_target(vault_.path, "", open_query_);
        if (t.exists) {
            open_note(t.existing_rel, OpenMode::Default, true);
            return;
        }
        if (!t.ok) {
            notice(t.error, false);
            return;
        }
        create_and_edit(t.rel, true);
        return;
    }
    if (id.rfind("note:", 0) == 0) open_note(id.substr(5), OpenMode::Default, true);
}

/* ================================================================== New note */

void App::build_new()
{
    lv_obj_t *p = make_page(PageId::NewNote);
    const Geo g = geo();
    new_ta_ = make_text_field(p, kTagNewField, kNameMaxBytes, g.row, "Name of the note");
    new_create_ = make_button(p, "Create", kTagNewCreate, true);
    new_status_ = lv_label_create(p);
    lv_label_set_long_mode(new_status_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(new_status_, f_body_, 0);
    lv_obj_remove_flag(new_status_, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(new_status_, "");
    layout_new();
}

void App::layout_new()
{
    if (!new_ta_) return;
    const Geo g = geo();
    const int create_w = std::max(g.row * 3 / 2, text_w("Create", f_button_) + 2 * g.m);
    const int y0 = g.s / 2;
    lv_obj_set_pos(new_ta_, g.x, y0);
    lv_obj_set_size(new_ta_, std::max(g.row, g.cw - create_w - g.s), g.row);
    lv_obj_set_pos(new_create_, g.x + g.cw - create_w, y0);
    lv_obj_set_size(new_create_, create_w, g.row);
    const int sy = y0 + g.row + g.m;
    lv_obj_set_pos(new_status_, g.x, sy);
    lv_obj_set_width(new_status_, g.cw);
    lv_obj_update_layout(new_status_);
    const int sh = std::max(line_h(f_body_), static_cast<int>(lv_obj_get_height(new_status_)));
    set_hidden(new_status_, sy + sh > panel_h_);
}

void App::render_new()
{
    const std::string &folder = stack_.back().folder;
    set_caption(folder.empty() ? std::string("Type a name, Enter creates") : "In " + folder + ": type a name, Enter creates");
    std::string status = new_error_;
    std::uint32_t color = kWarn;
    if (status.empty()) {
        status = "A folder path (Projects/Mesh/Name) makes the folders too. The note opens in the editor.";
        color = kMuted;
    }
    if (status != lv_label_get_text(new_status_)) lv_label_set_text(new_status_, status.c_str());
    set_color(new_status_, color);
    layout_new();
}

void App::key_new(const KeyEvent &e)
{
    if (e.key == Key::Enter) {
        if (!e.repeat) new_create();
        return;
    }
    if (is_typing(e) || e.key == Key::Backspace || e.key == Key::Delete || e.key == Key::Left || e.key == Key::Right ||
        e.key == Key::Home || e.key == Key::End) {
        new_typing_ = true;
        new_error_.clear();
        field_key(new_ta_, e, kNameMaxBytes);
        invalidate();
    }
}

void App::new_create()
{
    if (page() != PageId::NewNote) return;
    const NewTarget t = new_note_target(vault_.path, stack_.back().folder, lv_textarea_get_text(new_ta_));
    if (t.exists) {
        const std::string rel = t.existing_rel;
        open_confirm("Already exists", exists_body(strip_md(base_name(rel))), [this, rel] { open_note(rel, OpenMode::Default, true); },
                     "Open", "Cancel");
        return;
    }
    if (!t.ok) {
        new_error_ = t.error;
        if (is_hidden(new_status_)) notice(t.error, false);   // the line under the field is hidden by the keyboard
        invalidate();
        return;
    }
    new_error_.clear();
    create_and_edit(t.rel, true);
}

} // namespace notesui
