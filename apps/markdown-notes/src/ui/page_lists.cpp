/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: Home, the folder pages, Recent and Settings (the approved mock-ups). Every row is tagged (the
 * guidelines' row kinds): a row that opens a page has the chevron, New note the action dot, a read-out none. A tap
 * selects a row, a tap on the selected row acts; Up / Down, Enter (Right), Esc (Left) do the same with keys.
 */
#include "notes_app.hpp"

#include "app_style.hpp"
#include "build_info.hpp"
#include "hooks.hpp"

#include "dates.hpp"
#include "fsutil.hpp"
#include "names.hpp"

#include "osk_policy.hpp"

#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <time.h>

using namespace mdnotes;

namespace notesui {

namespace {

/* A bottom line of key hints (the mock-ups' Home and folder pages), grey, one line cut with dots, under a hairline: the
 * list ends above it, so a row the list cuts at its bottom edge is never under the hint. */
lv_obj_t *make_hint(lv_obj_t *parent, const lv_font_t *f, int x, int w, int pad, const std::string &text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(kMuted), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_border_side(l, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, lv_color_hex(CP0_UI_LIST_SECTION_RGB), 0);
    lv_obj_set_style_border_opa(l, LV_OPA_50, 0);
    lv_obj_set_style_pad_top(l, pad, 0);
    lv_obj_set_width(l, std::max(1, w));
    lv_obj_set_height(l, line_h(f) + pad + 1);
    lv_obj_set_x(l, x);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, text.c_str());
    return l;
}

/* The selection on a row that does something: from `sel`, the next selectable one, else the one before. */
int selectable_from(const RowList &list, int sel)
{
    const int n = list.count();
    if (n == 0) return -1;
    sel = std::clamp(sel, 0, n - 1);
    for (int i = sel; i < n; ++i)
        if (const Row *r = list.row(i); r && r->selectable && !r->section) return i;
    for (int i = sel; i >= 0; --i)
        if (const Row *r = list.row(i); r && r->selectable && !r->section) return i;
    return -1;
}

bool skip_row(const RowList &list, int i)
{
    const Row *r = list.row(i);
    return !r || !r->selectable || r->section;
}

} // namespace

/* The hint at the bottom of the page, the list above it from `top`. */
void App::place_hint(lv_obj_t *hint, RowList &list, int top)
{
    if (!list.obj()) return;
    int bottom = panel_h_;
    if (hint) {
        // the list ends where the hint's hairline starts: the hint's whole height is taken off the list
        const int h = line_h(f_caption_) + geo().s / 2 + 1 + geo().s / 2;     // (make_hint's height, then a gap)
        const bool room = panel_h_ - top >= geo().row + h;
        set_hidden(hint, !room);
        if (room) {
            bottom = panel_h_ - h;
            lv_obj_set_y(hint, bottom);
        }
    }
    if (lv_obj_get_y(list.obj()) != top) list.set_y(top);
    list.resize(std::max(1, bottom - top));
}

/* ================================================================== Home */

void App::build_home()
{
    lv_obj_t *p = make_page(PageId::Home);
    const Geo g = geo();
    make_list(home_list_, p, 0, panel_h_, 3);
    // portrait (PDA mode): the 480 px wide column cuts the whole hint, so the shorter one (landscape keeps its own)
    std::string hint = home_hint();
    if (layout_.h > layout_.w && text_w(hint, f_caption_) > g.cw) hint = home_hint_short();
    home_hint_ = make_hint(p, f_caption_, g.x, g.cw, g.s / 2, hint);
    place_hint(home_hint_, home_list_, 0);
}

void App::render_home()
{
    if (vault_.state == VaultState::NotAFolder) set_caption("Vault " + vault_shown() + " is a file, not a folder");
    else if (vault_.state == VaultState::Unusable) set_caption("Vault " + vault_shown() + ": " + std::strerror(vault_.err));
    else set_caption(home_caption(vault_shown(), index_.notes.size()));
    home_items_ = home_rows(index_.notes.size(), date_text(static_cast<std::time_t>(now_epoch())));
    const int sel = std::max(0, home_list_.selected());
    home_list_.set(build_rows(home_items_), std::min(sel, static_cast<int>(home_items_.size()) - 1));
}

void App::home_activate(const std::string &id)
{
    if (id == "open") push(Nav{PageId::Open});
    else if (id == "all") push(Nav{PageId::Folder});
    else if (id == "recent") push(Nav{PageId::Recent});
    else if (id == "today") open_today();
    else if (id == "new") {
        lv_textarea_set_text(new_ta_, "");
        new_error_.clear();
        push(Nav{PageId::NewNote});
    } else if (id == "settings") push(Nav{PageId::Settings});
}

void App::key_home(const KeyEvent &e)
{
    int sel = home_list_.selected();
    if (list_move(e, sel, home_list_.count(), 4)) {
        home_list_.set_selected(sel, true);
        return;
    }
    if ((e.key == Key::Enter || e.key == Key::Right) && !e.repeat) home_activate(home_list_.id_at(home_list_.selected()));
}

/* ================================================================== Folder, Recent */

void App::build_folder()
{
    lv_obj_t *p = make_page(PageId::Folder);
    const Geo g = geo();
    make_list(folder_list_, p, 0, panel_h_, 3);
    folder_hint_ = make_hint(p, f_caption_, g.x, g.cw, g.s / 2, folder_hint());
    place_hint(folder_hint_, folder_list_, 0);
}

void App::render_folder()
{
    const std::string &rel = stack_.back().folder;
    const int fi = index_.find_folder(rel);
    const FolderEntry *f = fi >= 0 ? &index_.folders[static_cast<std::size_t>(fi)] : nullptr;
    set_caption(folder_caption(f ? f->notes : 0, f ? f->folders : 0));
    folder_items_ = folder_rows(index_, rel, now_epoch());
    std::vector<Row> rows = build_rows(folder_items_);
    const int sel = std::max(0, folder_list_.selected());
    folder_list_.set(std::move(rows), sel);
    const int ok = selectable_from(folder_list_, sel);
    if (ok != folder_list_.selected()) folder_list_.set_selected(ok, false);
}

void App::build_recent()
{
    lv_obj_t *p = make_page(PageId::Recent);
    const Geo g = geo();
    make_list(recent_list_, p, 0, panel_h_, 3);
    recent_list_.set_empty_text("No notes yet: New note on Home makes one.", f_body_, kMuted);
    recent_hint_ = make_hint(p, f_caption_, g.x, g.cw, g.s / 2, folder_hint());
    place_hint(recent_hint_, recent_list_, 0);
}

void App::render_recent()
{
    recent_items_ = recent_rows(index_, now_epoch(), 20);
    set_caption(recent_caption(recent_items_.size()));
    const int sel = std::max(0, recent_list_.selected());
    recent_list_.set(build_rows(recent_items_), std::min(sel, std::max(0, static_cast<int>(recent_items_.size()) - 1)));
}

void App::key_list(RowList &list, const KeyEvent &e)
{
    const std::string id = list.id_at(list.selected());
    if (id.rfind("note:", 0) == 0 && !e.repeat) {
        const std::string rel = id.substr(5);
        if (is_ctrl_letter(e, 'i')) {
            note_options_action(rel, NoteAction::NoteInfo);
            return;
        }
        if (e.ctrl && e.key == Key::Delete) {
            note_options_action(rel, NoteAction::Delete);
            return;
        }
    }
    int sel = list.selected();
    if (list_move(e, sel, list.count(), 4, [&list](int i) { return skip_row(list, i); })) {
        list.set_selected(sel, true);
        return;
    }
    if (e.repeat) return;
    if (e.key == Key::Enter || e.key == Key::Right) list_activate(list, list.selected());
    else if (e.key == Key::Left) back(false);
}

void App::list_activate(RowList &list, int index)
{
    const std::string id = list.id_at(index);
    if (id == "new") {
        lv_textarea_set_text(new_ta_, "");
        new_error_.clear();
        push(Nav{PageId::NewNote, stack_.back().folder});
    } else if (id.rfind("folder:", 0) == 0) {
        push(Nav{PageId::Folder, id.substr(7)});
    } else if (id.rfind("note:", 0) == 0) {
        open_note(id.substr(5), OpenMode::Default);
    }
}

/* A note's options from a list (a 3 s hold on its row, Ctrl+O): Delete (asked first), Note info. */
void App::note_options(const std::string &rel)
{
    const std::vector<NoteAction> acts = list_note_actions();
    std::vector<std::string> labels, keys;
    std::vector<Mark> marks;
    for (NoteAction a : acts) {
        labels.push_back(note_action_label(a));
        keys.push_back(note_action_key(a));
        marks.push_back(note_action_asks(a) ? Mark::Chevron : Mark::Dot);
    }
    const std::string name = strip_md(base_name(rel));
    open_menu(options_title(name), labels, [this, rel, acts](int i) {
        if (i >= 0 && i < static_cast<int>(acts.size())) note_options_action(rel, acts[static_cast<std::size_t>(i)]);
    }, keys, marks);
    pop_.body = options_body(name);
}

void App::note_options_action(const std::string &rel, NoteAction a)
{
    if (a == NoteAction::Delete) {
        ask_delete(rel, [this] {
            rewalk();
            invalidate();
        });
    } else if (a == NoteAction::NoteInfo) {
        const std::string path = index_.abs_path(rel);
        const LoadedNote f = load_note(path);
        const FileStat fs = stat_path(path);
        open_info("Note info", note_info_text(rel, fs.size, fs.mtime_ns, now_epoch(), f.body.text, f.body.fmt,
                                              f.status != LoadStatus::ReadOnlyNotUtf8));
    }
}

/* ================================================================== Settings */

void App::build_settings()
{
    lv_obj_t *p = make_page(PageId::Settings);
    make_list(settings_list_, p, 0, panel_h_, 3);
}

/* The on-screen keyboard's rule as this app reads it (the same cdk::osk_policy every app uses): allowed now, and why
 * not (Settings > On-screen keyboard: the owner's check on a board). */
static void osk_diagnosis(std::string &value, std::string &detail)
{
    cdk::osk_policy::Policy *p = cdk::osk_policy::installed();
    if (!p) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const std::int64_t now_ms = static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    const bool allowed = p->allowed(now_ms);
    OskFacts f;
    f.have_file = p->from_file();
    const cp0_app_input_t &in = p->input();
    f.keyboards = in.keyboards;
    f.vkb = static_cast<int>(in.vkb);
    f.now_mono = now_ms / 1000;
    f.kb_gone = in.kb_gone;
    f.bt_gone = in.bt_gone;
    f.asleep_s = CP0_APP_OSK_BT_ASLEEP_S;
    f.grace_s = CP0_APP_OSK_GRACE_S;
    f.fallback_keyboards = p->fallback_keyboards();
    value = osk_value(allowed);
    detail = osk_detail(f, allowed);
}

void App::render_settings()
{
    set_caption(settings_caption(CDK_APP_VERSION));
    std::string osk_val, osk_det;
    osk_diagnosis(osk_val, osk_det);
    settings_items_ = settings_rows(vault_shown(), vault_from_env_, settings_.text_size, settings_.open_in, CDK_APP_VERSION, osk_val,
                                    osk_det);
    settings_list_.set(build_rows(settings_items_), settings_sel_);
    const int ok = selectable_from(settings_list_, settings_sel_);
    if (ok != settings_sel_) {
        settings_sel_ = ok;
        settings_list_.set_selected(ok, false);
    }
}

void App::key_settings(const KeyEvent &e)
{
    int sel = settings_sel_;
    if (list_move(e, sel, settings_list_.count(), 4, [this](int i) { return skip_row(settings_list_, i); })) {
        if (sel == settings_sel_) {
            // nothing to select further that way: the read-outs past it (Version, On-screen keyboard) scroll into view
            const bool down = e.key == Key::Down || e.key == Key::PageDown || e.key == Key::End;
            settings_list_.scroll_by(down ? geo().row * 2 : -geo().row * 2);
            return;
        }
        settings_sel_ = sel;
        settings_list_.set_selected(sel, true);
        return;
    }
    if (e.repeat) return;
    if (e.key == Key::Enter || e.key == Key::Right) settings_activate(settings_sel_);
    else if (e.key == Key::Left) back(false);
}

void App::settings_activate(int index)
{
    const std::string id = settings_list_.id_at(index);
    if (id == "text_size") {
        open_choice(text_size_choice(settings_.text_size), [this](int i) {
            settings_.text_size = text_size_from_index(i);
            save_settings();
            notice("Text size: " + text_size_label(settings_.text_size));
        });
    } else if (id == "open_in") {
        open_choice(open_in_choice(settings_.open_in), [this](int i) {
            settings_.open_in = open_in_from_index(i);
            save_settings();
            notice("Notes open in the " + std::string(settings_.open_in == OpenIn::Editor ? "editor" : "preview"));
        });
    }
}

void App::save_settings()
{
    int err = 0;
    if (!mdnotes::save_settings(settings_path_, settings_, &err)) notice(std::string("Could not save the settings: ") + std::strerror(err), false);
    invalidate();
}

} // namespace notesui
