/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes: the shell (header, the grey line, the page bodies), the vault, the navigation, the keys, the 3 s
 * hold, the on-screen keyboard, the notes (open, create, delete, links) and the dialogs. The pages are in
 * page_lists.cpp (Home, Folder, Recent, Settings), page_open.cpp (Open or create, New note), page_preview.cpp and
 * page_editor.cpp. RSS Reader's shell (rss_app.cpp), adapted.
 */
#include "notes_app.hpp"

#include "app.hpp"
#include "app_style.hpp"
#include "build_info.hpp"
#include "hooks.hpp"

#include "dates.hpp"
#include "fsutil.hpp"
#include "names.hpp"
#include "trash.hpp"

#include "osk_policy.hpp"

#include "cp0_lvgl_app.h"
#include "cp0_ui_metrics_lvgl.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unistd.h>

using namespace mdnotes;

namespace notesui {

namespace {

constexpr std::uint32_t kStartGuardMs = 600;   /* the tap or Enter that started the app must not act */
constexpr std::uint32_t kNoticeMs = 4000;
constexpr int kColumnCapW = 960;               /* the list pages' column (Mesh Hop's, Net Toolbox's, RSS Reader's cap) */

App *g_app = nullptr;

void trampoline_button(lv_event_t *e)
{
    if (!g_app || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_app->button_clicked(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void trampoline_popitem(lv_event_t *e)
{
    if (!g_app || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_app->popup_pick(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

std::string font_file(const char *name)
{
    if (const char *d = std::getenv("MDNOTES_FONT_DIR"); d && *d) {
        const std::string p = std::string(d) + "/" + name;
        if (::access(p.c_str(), R_OK) == 0) return p;
    }
    const std::string rel = std::string("share/font/") + name;
    if (const char *own = cp0_file_path_c(rel.c_str()); own && own[0] && ::access(own, R_OK) == 0) return own;
    const std::string installed = std::string("/usr/share/APPLaunch/") + rel;
    if (::access(installed.c_str(), R_OK) == 0) return installed;
    return "";
}

} // namespace

void log_line(const std::string &s)
{
    std::fprintf(stderr, "[markdown-notes] %s\n", s.c_str());
}

/* ================================================================== construction */

App::App(Platform &plat) : plat_(plat)
{
    g_app = this;
    start_ms_ = mono_ms();
    open_vault();

    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &layout_);
    load_fonts();
    build_shell();
    build_home();
    build_folder();
    build_recent();
    build_open();
    build_new();
    build_preview();
    build_editor();
    build_settings();
    // the cue of a held row: a box with a bar that fills over 3 s (never clickable: it must not take the touch)
    {
        const Geo g = geo();
        hold_box_ = make_box(body_, g.x, 0, g.cw, 10);
        lv_obj_remove_flag(hold_box_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hold_box_, lv_color_hex(kPanel), 0);
        lv_obj_set_style_bg_opa(hold_box_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hold_box_, m->tok[CP0_TOK_RADIUS_M], 0);
        lv_obj_set_style_border_width(hold_box_, 2, 0);
        lv_obj_set_style_border_color(hold_box_, lv_color_hex(kAccent), 0);
        const int lh = line_h(f_caption_), pad = g.s;
        hold_label_ = make_label(hold_box_, f_caption_, kText, pad, pad - 2, g.cw - 2 * pad - 4, lh);
        hold_track_w_ = std::max(1, g.cw - 2 * pad - 4);
        hold_track_ = make_box(hold_box_, pad, pad + lh + pad / 2 - 2, hold_track_w_, pad);
        lv_obj_remove_flag(hold_track_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hold_track_, lv_color_hex(kSelected), 0);
        lv_obj_set_style_bg_opa(hold_track_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hold_track_, LV_RADIUS_CIRCLE, 0);
        hold_bar_ = make_box(hold_track_, 0, 0, 1, pad);
        lv_obj_remove_flag(hold_bar_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hold_bar_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(hold_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hold_bar_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_height(hold_box_, pad + lh + pad / 2 + pad + pad);
        set_hidden(hold_box_, true);
    }
    plat_.on_key = [this](const KeyEvent &e) { on_key(e); };
    inset_listener_ = cp0::ui::osk::add_inset_listener([this](int inset) { on_osk_inset(inset); });
    kbd_present_ = keyboard_present();
    enter_page();
    render();
}

App::~App()
{
    plat_.on_key = nullptr;
    // the app ends (Esc at Home, the launcher's close, SIGTERM): the editor's text is never lost (also under the help page)
    editor_flush();
    if (note_.dirty) editor_save(SaveWhy::Quit);
    if (inset_listener_) cp0::ui::osk::remove_inset_listener(inset_listener_);
    inset_listener_ = 0;
    release_osk();
    dialog_.reset();
    shell_.reset();
    for (lv_font_t *f : owned_fonts_) lv_freetype_font_delete(f);
    owned_fonts_.clear();
    g_app = nullptr;
}

const lv_font_t *App::font(const char *face, int px, bool italic)
{
    const std::string key = std::string(face) + ":" + std::to_string(px) + (italic ? ":i" : "");
    if (auto it = fonts_.find(key); it != fonts_.end()) return it->second;
    auto create = [this](const char *name, int size, bool ital) -> lv_font_t * {
        const std::string path = font_file(name);
        if (path.empty()) return nullptr;
        lv_font_t *f = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, static_cast<uint32_t>(size),
                                               ital ? LV_FREETYPE_FONT_STYLE_ITALIC : LV_FREETYPE_FONT_STYLE_NORMAL);
        if (f) owned_fonts_.push_back(f);
        return f;
    };
    // the fallback chain of this size (DejaVu Sans: accents, arrows, marks; Noto Sans CJK), shared by every face of it
    const std::string chain_key = "chain:" + std::to_string(px);
    lv_font_t *chain = nullptr;
    if (auto it = fonts_.find(chain_key); it != fonts_.end()) {
        chain = const_cast<lv_font_t *>(it->second);
    } else {
        lv_font_t *dejavu = create("DejaVuSans.ttf", px, false);
        lv_font_t *cjk = create("NotoSansCJK-Regular.ttc", px, false);
        if (dejavu) dejavu->fallback = cjk;
        chain = dejavu ? dejavu : cjk;
        fonts_[chain_key] = chain;
    }
    const lv_font_t *result = nullptr;
    if (lv_font_t *f = create(face, px, italic)) {
        f->fallback = chain;
        result = f;
    } else {
        result = cp0_ui_font_px(px);
    }
    fonts_[key] = result;
    return result;
}

void App::load_fonts()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int body = m->text_px[CP0_TEXT_BODY], heading = m->text_px[CP0_TEXT_HEADING], caption = m->text_px[CP0_TEXT_CAPTION];
    const int label = cp0_ui_roller_font_px((body + heading) / 2);          // the lists' labels (RSS Reader's, Mesh Hop's)
    f_row_ = font("Montserrat-Medium.ttf", label);
    f_body_ = font("Montserrat-Medium.ttf", body);
    f_caption_ = font("Montserrat-Medium.ttf", caption);
    f_value_ = font("Montserrat-Medium.ttf", cp0_ui_roller_font_px((body * 9 + 5) / 10));
    f_field_ = font("DejaVuSans.ttf", body);
    f_title_ = font("Montserrat-Medium.ttf", layout_.title_px);
    f_button_ = f_body_;
}

lv_obj_t *App::make_label(lv_obj_t *parent, const lv_font_t *f, std::uint32_t color, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    set_color(l, color);
    lv_label_set_text(l, "");
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, std::max(1, w), std::max(1, h));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* A button: primary = the keyboard's Enter blue. Its size is set by the page (the 9 mm toolbar key for the bars, ROW
 * for a page's Create). */
lv_obj_t *App::make_button(lv_obj_t *parent, const char *text, int tag, bool primary)
{
    lv_obj_t *b = lv_button_create(parent);
    if (lv_obj_get_group(b)) lv_group_remove_obj(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_radius(b, layout_.esc_radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? kPrimary : kSelected), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? CP0_UI_OSK_ENTER_PRESSED_RGB : kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(kText), 0);
    lv_obj_add_event_cb(b, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f_button_, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    return b;
}

namespace {
constexpr std::uint32_t kFieldBg = 0x1C1C1C;
constexpr std::uint32_t kFieldBorder = 0x3A3A3A;
constexpr std::uint32_t kFieldEditing = 0x2D9CDB;
constexpr std::uint32_t kFieldText = 0xEEEEEE;
constexpr std::uint32_t kFieldPlaceholder = 0x777777;
} // namespace

/* A one-line text field drawn as the toolkit's cp0::ui::TextField (its colours, the border that turns blue while it is
 * typed into, the steady cursor); the app types into it itself (the key bridge: US or FR layout), as RSS Reader and
 * Mesh Hop do. */
lv_obj_t *App::make_text_field(lv_obj_t *parent, int tag, std::size_t max_len, int h, const char *placeholder)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    lv_obj_t *ta = lv_textarea_create(parent);
    if (lv_obj_get_group(ta)) lv_group_remove_obj(ta);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_height(ta, h);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(ta, f_field_, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kFieldText), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(kFieldBg), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kFieldBorder), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(ta, std::max(layout_.gap, h / 5), 0);
    lv_obj_set_style_pad_ver(ta, std::max(0, (h - 4 - line_h(f_field_)) / 2), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kFieldPlaceholder), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_hex(kFieldEditing), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_textarea_set_max_length(ta, static_cast<uint32_t>(max_len));
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    if (tag) lv_obj_add_event_cb(ta, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
    return ta;
}

void App::style_field(lv_obj_t *ta, bool editing)
{
    if (!ta) return;
    lv_obj_set_style_border_color(ta, lv_color_hex(editing ? kFieldEditing : kFieldBorder), 0);
    if (editing) lv_obj_add_state(ta, LV_STATE_FOCUSED);
    else lv_obj_remove_state(ta, LV_STATE_FOCUSED);
}

App::Geo App::geo() const
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    Geo g;
    g.w = layout_.w;
    g.inset = m->tok[CP0_TOK_SPACE_L];
    g.s = m->tok[CP0_TOK_SPACE_S];
    g.m = m->tok[CP0_TOK_SPACE_M];
    const PageColumn c = page_column(layout_.w, g.inset, kMinW, kColumnCapW);
    g.x = c.x;
    g.cw = c.w;
    g.lx = g.x - g.inset;
    g.lw = g.cw + 2 * g.inset;
    g.h = panel_h_;
    g.row = std::max(m->tok[CP0_TOK_ROW_H], line_h(f_row_) + g.s);
    g.two_line = std::max(m->tok[CP0_TOK_ROW_H], line_h(f_row_) + line_h(f_caption_) + g.s);
    g.key = cp0_ui_toolbar_key_px(m->ppmm_x100);
    return g;
}

RowList::Config App::list_config(int max_cells) const
{
    const Geo g = geo();
    RowList::Config cfg;
    cfg.max_cells = max_cells;
    cfg.min_row_h = g.row;
    cp0_ui_list_layout(cp0_ui_metrics_get(), CP0_UI_LIST_COMFORTABLE, g.lx, 0, g.lw, std::max(1, full_h_), &cfg.marks);
    cfg.marks.text_px = cp0_ui_roller_font_px((cp0_ui_metrics_get()->text_px[CP0_TEXT_BODY] + cp0_ui_metrics_get()->text_px[CP0_TEXT_HEADING]) / 2);
    cfg.lead_x = cfg.marks.inset;
    return cfg;
}

void App::make_list(RowList &list, lv_obj_t *parent, int y, int h, int max_cells)
{
    const Geo g = geo();
    list.create(parent, g.lx, y, g.lw, std::max(1, h), list_config(max_cells));
    list.on_click = [this, &list](int index) { row_clicked(&list, index); };
    list.on_press = [this, &list](int what, int index, int x, int y2) { row_pressed(&list, what, index, x, y2); };
}

/* A row of the lists (RSS Reader's build_row): one line or two, a value at the right (before the marker), the marker
 * of its kind: Open / Edit a chevron, Action a dot, Info a grey read-out the selection skips. */
Row App::build_row(const ListItem &t) const
{
    const Geo g = geo();
    const RowList::Config cfg = list_config(4);
    const cp0_ui_list_layout_t &ml = cfg.marks;
    Row r;
    r.id = t.id;
    r.section = t.section;
    r.selectable = !t.section && t.aff != Affordance::Info;
    if (t.section) {
        r.h = line_h(f_caption_) + g.m;
        Cell c;
        c.text = t.label;
        c.color = CP0_UI_LIST_SECTION_RGB;
        c.font = f_caption_;
        c.x = ml.inset;
        c.y = r.h - line_h(f_caption_) - g.s / 2;
        c.w = g.lw - 2 * ml.inset;
        r.cells.push_back(c);
        return r;
    }
    r.trail = t.aff == Affordance::Open || t.aff == Affordance::Edit ? Mark::Chevron : t.aff == Affordance::Action ? Mark::Dot : Mark::None;
    const bool two = !t.line2.empty();
    r.h = two ? g.two_line : g.row;
    const cp0_ui_list_affordance_t aff = r.trail == Mark::Chevron ? CP0_UI_LIST_AFFORDANCE_OPEN
                                         : r.trail == Mark::None ? CP0_UI_LIST_AFFORDANCE_UNSPECIFIED : CP0_UI_LIST_AFFORDANCE_ACTION;
    const int trail = cp0_ui_list_trail_w(&ml, aff);
    const int x0 = ml.inset;
    const int right_edge = g.lw - ml.inset - trail;
    const int rw = t.value.empty() ? 0 : std::min(text_w(t.value, f_value_) + 2, std::max(10, (right_edge - x0) * 3 / 5));
    const int lh1 = line_h(f_row_), lh2 = line_h(f_caption_), lhv = line_h(f_value_);
    const int top = (r.h - (two ? lh1 + lh2 : lh1)) / 2;
    Cell c1;
    c1.text = t.label;
    c1.color = t.aff == Affordance::Info ? kMuted : kText;
    c1.font = f_row_;
    c1.x = x0;
    c1.y = top;
    c1.w = std::max(10, right_edge - x0 - (rw ? rw + ml.gap : 0));
    r.cells.push_back(c1);
    if (two) {
        Cell c2;
        c2.text = t.line2;
        c2.color = t.warn ? kWarn : kMuted;
        c2.font = f_caption_;
        c2.x = x0;
        c2.y = top + lh1;
        c2.w = std::max(10, right_edge - x0);
        r.cells.push_back(c2);
    }
    if (rw) {
        Cell cr;
        cr.text = t.value;
        cr.color = t.aff == Affordance::Edit ? kValue : kMuted;
        cr.font = f_value_;
        cr.x = right_edge - rw;
        cr.y = two ? top + (lh1 - lhv) / 2 : (r.h - lhv) / 2;
        cr.w = rw;
        cr.align = LV_TEXT_ALIGN_RIGHT;
        r.cells.push_back(cr);
    }
    return r;
}

std::vector<Row> App::build_rows(const std::vector<ListItem> &items) const
{
    std::vector<Row> rows;
    rows.reserve(items.size());
    for (const ListItem &it : items) rows.push_back(build_row(it));
    return rows;
}

/* The shell: cp0::ui::Page (the header: the title, the Esc button, the shared status strip) with the title in the
 * launcher's Montserrat as FreeType (note names may have accents or any script), the grey line under it, the pages. */
void App::build_shell()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int cap_lh = line_h(f_caption_);
    layout_.caption_y = layout_.body_y + m->tok[CP0_TOK_SPACE_S] / 4;
    body_y_ = layout_.caption_y + cap_lh + m->tok[CP0_TOK_SPACE_S] / 2;
    full_h_ = layout_.h - body_y_;
    panel_h_ = full_h_;
    const lv_font_t *title_font = f_title_;
    shell_ = std::make_unique<cp0::ui::Page>(
        plat_.screen(), layout_,
        [title_font](lv_obj_t *parent, const char *text) {
            lv_obj_t *t = cp0::ui::add_title(parent, text);
            if (title_font) lv_obj_set_style_text_font(t, title_font, 0);
            return t;
        },
        [this] { header_back(); });
    cp0::ui::StatusStrip::add(shell_->header());
    shell_->set_title({kHomeTitle});
    cap_ = make_label(shell_->root(), f_caption_, kMuted, layout_.caption_x, layout_.caption_y, std::max(1, layout_.caption_w), cap_lh);
    body_ = make_box(shell_->root(), 0, body_y_, layout_.w, panel_h_);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *App::make_page(PageId id)
{
    lv_obj_t *p = make_box(body_, 0, 0, layout_.w, panel_h_);
    lv_obj_set_style_bg_color(p, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    set_hidden(p, true);
    pages_[id] = p;
    return p;
}

/* The room under the shell changed (the on-screen keyboard came or went): the pages end above the keyboard. */
void App::layout_pages()
{
    panel_h_ = std::max(1, full_h_ - osk_inset_);
    lv_obj_set_height(body_, panel_h_);
    for (auto &kv : pages_) lv_obj_set_height(kv.second, panel_h_);
    place_hint(home_hint_, home_list_, 0);
    place_hint(folder_hint_, folder_list_, 0);
    place_hint(recent_hint_, recent_list_, 0);
    settings_list_.resize(panel_h_);
    layout_open();
    layout_new();
    layout_preview();
    layout_editor();
    invalidate();
}

/* ================================================================== the vault */

void App::open_vault()
{
    home_ = home_dir();
    settings_path_ = settings_path(home_);
    settings_ = load_settings(settings_path_);
    const char *env = std::getenv("NOTES_VAULT");
    vault_from_env_ = env && *env;
    const std::string path = resolve_vault(settings_, env, home_);
    vault_ = mdnotes::open_vault(path);
    switch (vault_.state) {
    case VaultState::Ready:
    case VaultState::Created: {
        index_ = walk_vault(vault_.path);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s %s, vault %s%s%s: %zu notes in %zu folders%s%s", kAppTitle, CDK_APP_VERSION,
                      vault_.path.c_str(), vault_.state == VaultState::Created ? " (created)" : "", vault_from_env_ ? " (NOTES_VAULT)" : "",
                      index_.notes.size(), index_.folders.size(), index_.capped_notes ? ", capped" : "",
                      index_.capped_depth ? ", too deep skipped" : "");
        log_line(buf);
        break;
    }
    case VaultState::NotAFolder:
        index_.root = vault_.path;
        log_line("vault " + vault_.path + " is not a folder");
        break;
    case VaultState::Unusable:
        index_.root = vault_.path;
        log_line("vault " + vault_.path + " cannot be used: " + std::strerror(vault_.err));
        break;
    }
}

void App::rewalk()
{
    if (vault_.state != VaultState::Ready && vault_.state != VaultState::Created) return;
    index_ = walk_vault(vault_.path);
    index_stale_ = false;
}

std::string App::vault_shown() const
{
    return tilde_path(vault_.path, home_);
}

/* ================================================================== navigation */

void App::push(Nav n)
{
    if (popup_open()) return;
    hold_.cancel();
    leave_page();                              // the page under keeps its place for the way back
    stack_.push_back(std::move(n));
    enter_page();
    invalidate();
}

void App::replace_top(Nav n)
{
    hold_.cancel();
    leave_page();
    stack_.back() = std::move(n);
    enter_page();
    invalidate();
}

void App::go_up()
{
    if (stack_.size() <= 1) return;
    hold_.cancel();
    leave_page();
    help_return_ = stack_.back().page == PageId::Help;
    stack_.pop_back();
    enter_page();
    invalidate();
}

/* The page on top goes (popped or replaced): its list position is kept in its Nav for the way back. */
void App::leave_page()
{
    Nav &n = stack_.back();
    switch (n.page) {
    case PageId::Home: n.sel = home_list_.selected(); break;
    case PageId::Folder: n.sel = folder_list_.selected(); break;
    case PageId::Recent: n.sel = recent_list_.selected(); break;
    case PageId::Preview:
        n.scroll = pv_ ? lv_obj_get_scroll_y(pv_) : -1;
        break;
    case PageId::Open: open_typing_ = false; break;
    case PageId::NewNote: new_typing_ = false; break;
    case PageId::Editor:
        n.typing = editor_typing_;
        editor_typing_ = false;
        n.sel = ed_ta_ ? static_cast<int>(lv_textarea_get_cursor_pos(ed_ta_)) : 0;   // the cursor, for the way back
        break;
    default: break;
    }
    release_osk();
}

/* The page on top shows, new or again: lists from a fresh walk when files changed, a note read from the disk. */
void App::enter_page()
{
    Nav &n = stack_.back();
    const bool from_help = help_return_;   // back from the help page: the preview / editor below stay as they were
    help_return_ = false;
    switch (n.page) {
    case PageId::Home:
    case PageId::Folder:
    case PageId::Recent:
    case PageId::Open:
        if (index_stale_) rewalk();
        break;
    default: break;
    }
    switch (n.page) {
    case PageId::Home:
        render_home();
        home_list_.set_selected(std::max(0, n.sel), true);
        break;
    case PageId::Folder:
        if (index_.find_folder(n.folder) < 0 && !n.folder.empty()) n.folder.clear();   // gone meanwhile: the root
        render_folder();
        folder_list_.scroll_to_top();
        folder_list_.set_selected(std::clamp(n.sel, 0, std::max(0, folder_list_.count() - 1)), true);
        break;
    case PageId::Recent:
        render_recent();
        recent_list_.scroll_to_top();
        recent_list_.set_selected(std::clamp(n.sel, 0, std::max(0, recent_list_.count() - 1)), true);
        break;
    case PageId::Open:
        open_typing_ = true;
        lv_textarea_set_text(open_ta_, "");      // a new search each time (the page is replaced by what it opens)
        refresh_open_rows();
        break;
    case PageId::NewNote:
        new_typing_ = true;
        break;
    case PageId::Preview:
        if (note_.rel != n.note || !note_.file.readable()) load_note_into(n.note);
        else note_.file = load_note(note_.path);          // always read anew: another device may have changed it
        if (!note_.file.readable()) {
            notice(load_failure_text(note_.file.status, note_.file.err), false);
            post([this] { go_up(); });
            return;
        }
        ring_ = -1;
        preview_keys_ = false;
        render_preview_content(false);
        if (n.scroll > 0) {
            lv_obj_update_layout(pv_);
            lv_obj_scroll_to_y(pv_, n.scroll, LV_ANIM_OFF);
        }
        break;
    case PageId::Editor:
        if (from_help && note_.rel == n.note && note_.file.editable()) {
            // the text as it was (its unsaved part too), the cursor and the scroll where they were
            editor_typing_ = n.typing;
            break;
        }
        if (note_.rel != n.note || !note_.file.readable()) load_note_into(n.note);
        else note_.file = load_note(note_.path);
        if (!note_.file.editable()) {
            notice(note_.file.readable() ? readonly_text(note_.file.status) : load_failure_text(note_.file.status, note_.file.err), false);
            post([this] { go_up(); });
            return;
        }
        editor_load();
        if (n.sel > 0) lv_textarea_set_cursor_pos(ed_ta_, n.sel);      // back from a page opened from here
        editor_typing_ = true;
        break;
    case PageId::Settings:
        render_settings();
        settings_list_.scroll_to_top();
        break;
    case PageId::Help:
        ring_ = -1;
        preview_keys_ = false;
        render_preview_content(false);
        break;
    }
}

void App::with_saved_note(std::function<void()> then)
{
    if (note_.dirty) {                     // the editor, or the help page over it
        editor_save(SaveWhy::Leave, std::move(then));
        return;
    }
    if (then) then();
}

/* Esc (a key: the physical keyboard's or the on-screen keyboard's) and the header's Esc button: back one step. A field
 * being typed into with the on-screen keyboard up stops first (a key only: the header's Esc always goes back); the
 * editor saves on the way; Home quits (owner decision: a short Esc at the root ends the app). */
void App::back(bool from_header)
{
    if (popup_open()) {
        popup_cancel();
        return;
    }
    const bool osk_up = osk_inset_ > 0;
    switch (page()) {
    case PageId::Home: cdk::request_quit(0); return;
    case PageId::Open:
        if (!from_header && open_typing_ && osk_up) {
            open_typing_ = false;
            invalidate();
            return;
        }
        break;
    case PageId::NewNote:
        if (!from_header && new_typing_ && osk_up) {
            new_typing_ = false;
            invalidate();
            return;
        }
        break;
    case PageId::Editor:
        if (!from_header && editor_typing_ && osk_up) {
            editor_typing_ = false;
            invalidate();
            return;
        }
        with_saved_note([this] { go_up(); });
        return;
    default: break;
    }
    go_up();
}

void App::header_back()
{
    post([this] {
        if (mono_ms() - start_ms_ < kStartGuardMs) return;
        back(true);
    });
}

void App::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
}

void App::show_page()
{
    const PageId p = page() == PageId::Help ? PageId::Preview : page();   // the help is drawn by the preview
    for (auto &kv : pages_) set_hidden(kv.second, kv.first != p);
}

void App::update_title()
{
    std::vector<std::string> path;
    switch (page()) {
    case PageId::Home: path = {kHomeTitle}; break;
    case PageId::Folder: path = folder_title(stack_.back().folder); break;
    case PageId::Recent: path = {"Notes", "Recent"}; break;
    case PageId::Open: path = {"Open or create"}; break;
    case PageId::NewNote: path = {"New note"}; break;
    case PageId::Preview: path = {note_.name}; break;
    case PageId::Editor: path = {editor_title(note_.name, note_.dirty)}; break;
    case PageId::Settings: path = {"Settings"}; break;
    case PageId::Help: path = {"Markdown help"}; break;
    }
    std::string key;
    for (const std::string &p : path) key += (key.empty() ? "" : " > ") + p;
    if (key == title_key_) return;
    title_key_ = key;
    shell_->set_title(path);
}

void App::set_caption(const std::string &text)
{
    const bool show_notice = !notice_text_.empty();
    const std::string &t = show_notice ? notice_text_ : text;
    if (std::string(lv_label_get_text(cap_)) != t) lv_label_set_text(cap_, t.c_str());
    set_color(cap_, show_notice ? (notice_ok_ ? kOk : kWarn) : kMuted);
}

void App::notice(const std::string &text, bool ok)
{
    notice_text_ = text;
    notice_ok_ = ok;
    notice_ms_ = mono_ms();
    log_line("notice: " + text);
    invalidate();
}

/* ================================================================== tick and render */

void App::tick()
{
    editor_flush();
    while (!posted_.empty()) {
        std::vector<std::function<void()>> now;
        now.swap(posted_);
        for (auto &fn : now) fn();
    }
    const std::uint64_t now = mono_ms();
    if (pop_dirty_) {                              // the widgets of a new dialog are built here, never inside the click of another
        pop_dirty_ = false;
        popup_build();
        popup_refresh();
    }
    lifecycle_tick();
    if ((page() == PageId::Preview || page() == PageId::Editor) && now - kbd_check_ms_ >= 1000) {
        kbd_check_ms_ = now;                       // the captions' hint follows a keyboard coming or going
        const bool k = keyboard_present();
        if (k != kbd_present_) {
            kbd_present_ = k;
            dirty_ = true;
        }
    }
    hold_tick(now);
    editor_tick(now);
    if (!notice_text_.empty() && now > notice_ms_ && now - notice_ms_ >= kNoticeMs) {
        notice_text_.clear();
        dirty_ = true;
    }
    // the ages of the lists move on (once a minute is enough), Settings' keyboard line every second; never under a finger
    if (now - last_render_ms_ >= (page() == PageId::Settings ? 1000u : 60000u) && !plat_.touching()) dirty_ = true;
    if (dirty_) render();
}

/* Multitasking (X-Background=freeze): hidden, the process is stopped right after; the autosave is the guard, and a
 * last save is tried here in case a tick runs. Shown again: a note not being edited is read anew when it changed on
 * disk meanwhile (Syncthing). */
void App::lifecycle_tick()
{
    const bool hidden = cdk::hidden();
    if (hidden == was_hidden_) return;
    was_hidden_ = hidden;
    if (hidden) {
        if (note_.dirty) editor_save(SaveWhy::Auto);
        return;
    }
    index_stale_ = true;
    if ((page() == PageId::Preview || page() == PageId::Editor) && note_.open() && !note_.dirty) {
        DiskStamp st = note_.file.stamp;
        if (check_disk(note_.path, st) == DiskState::Changed) {
            enter_page();
            notice("Read again: " + note_.name + " changed on disk");
        }
    }
    invalidate();
}

void App::render()
{
    last_render_ms_ = mono_ms();
    dirty_ = false;
    show_page();
    switch (page()) {
    case PageId::Home: render_home(); break;
    case PageId::Folder: render_folder(); break;
    case PageId::Recent: render_recent(); break;
    case PageId::Open: render_open(); break;
    case PageId::NewNote: render_new(); break;
    case PageId::Preview:
        set_caption(preview_caption(note_.file.status, note_.file.stamp.mtime_ns, now_epoch(), kbd_present_));
        layout_preview();
        break;
    case PageId::Editor:
        set_caption(editor_caption(ed_bytes_, kbd_present_));
        layout_editor();
        break;
    case PageId::Settings: render_settings(); break;
    case PageId::Help:
        set_caption(help_caption());
        layout_preview();
        break;
    }
    update_title();
    sync_osk();
}

/* ================================================================== the on-screen keyboard */

void App::release_osk()
{
    if (osk_field_) cp0::ui::osk::release(osk_field_);
    osk_field_ = nullptr;
}

/* The field the state says is being typed into asks for the keyboard (the shared rule decides whether it shows: never
 * with a physical keyboard); the one before gives it back. */
lv_obj_t *App::osk_object(OskField f) const
{
    switch (f) {
    case OskField::Open: return open_ta_;
    case OskField::New: return new_ta_;
    case OskField::Editor: return ed_ta_;
    case OskField::None: break;
    }
    return nullptr;
}

OskField App::osk_field_of(lv_obj_t *obj) const
{
    if (!obj) return OskField::None;
    if (obj == open_ta_) return OskField::Open;
    if (obj == new_ta_) return OskField::New;
    if (obj == ed_ta_) return OskField::Editor;
    return OskField::None;
}

void App::sync_osk()
{
    const OskField want = osk_field_for(page(), popup_open(), open_typing_, new_typing_, editor_typing_);
    style_field(open_ta_, page() == PageId::Open && open_typing_);
    style_field(new_ta_, page() == PageId::NewNote && new_typing_);
    if (ed_ta_) {
        if (page() == PageId::Editor) lv_obj_add_state(ed_ta_, LV_STATE_FOCUSED);    // the cursor shows
        else lv_obj_remove_state(ed_ta_, LV_STATE_FOCUSED);
    }
    const OskStep step = osk_step(want, osk_field_of(osk_field_), osk_field_of(cp0::ui::osk::owner()), osk_tap_);
    osk_tap_ = false;
    switch (step) {
    case OskStep::Keep: return;
    case OskStep::Release: release_osk(); return;
    case OskStep::Switch: release_osk(); break;
    case OskStep::Request: break;
    }
    osk_field_ = osk_object(want);
    if (osk_field_) cp0::ui::osk::request(osk_field_, cp0::ui::TextType::Text, want == OskField::Editor);
}

void App::on_osk_inset(int inset)
{
    if (inset == osk_inset_) return;
    osk_inset_ = inset;
    layout_pages();
}

bool App::field_key(lv_obj_t *ta, const KeyEvent &e, std::size_t max_len)
{
    if (!ta) return false;
    switch (e.key) {
    case Key::Backspace: lv_textarea_delete_char(ta); break;
    case Key::Delete: lv_textarea_delete_char_forward(ta); break;
    case Key::Left: lv_textarea_cursor_left(ta); break;
    case Key::Right: lv_textarea_cursor_right(ta); break;
    case Key::Home: lv_textarea_set_cursor_pos(ta, 0); break;
    case Key::End: lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); break;
    case Key::Char: {
        if (e.ctrl || e.alt || e.text.empty()) return false;
        const char *cur = lv_textarea_get_text(ta);
        if ((cur ? std::strlen(cur) : 0) + e.text.size() > max_len) return true;
        lv_textarea_add_text(ta, e.text.c_str());
        break;
    }
    default: return false;
    }
    invalidate();
    return true;
}

/* ================================================================== keys, taps, the hold */

void App::on_key(const KeyEvent &e)
{
    if (e.key == Key::None) return;
    if (!ed_pending_.empty() && (page() != PageId::Editor || popup_open() || e.ctrl || e.alt ||
                                 (e.key != Key::Char && e.key != Key::Enter && e.key != Key::Tab)))
        editor_flush();
    if (popup_open()) {
        popup_key(e);
        return;
    }
    if (mono_ms() - start_ms_ < kStartGuardMs && (e.key == Key::Enter || e.key == Key::Esc)) return;
    if (e.key == Key::Esc) {
        if (!e.repeat) back(false);
        return;
    }
    if (is_ctrl_letter(e, 'o')) {
        open_options();
        return;
    }
    if (global_key(e)) return;
    switch (page()) {
    case PageId::Home: key_home(e); break;
    case PageId::Folder: key_list(folder_list_, e); break;
    case PageId::Recent: key_list(recent_list_, e); break;
    case PageId::Open: key_open(e); break;
    case PageId::NewNote: key_new(e); break;
    case PageId::Preview: key_preview(e); break;
    case PageId::Editor: key_editor(e); break;
    case PageId::Settings: key_settings(e); break;
    case PageId::Help: key_preview(e); break;
    }
}

/* Ctrl+P (Open or create), Ctrl+N (New note: in the folder shown), Ctrl+D (Today's note): from any page; the editor
 * saves first. */
bool App::global_key(const KeyEvent &e)
{
    if (is_ctrl_letter(e, 'p')) {
        if (page() != PageId::Open) with_saved_note([this] { push(Nav{PageId::Open}); });
        return true;
    }
    if (is_ctrl_letter(e, 'n')) {
        if (page() == PageId::NewNote) return true;
        Nav n{PageId::NewNote};
        if (page() == PageId::Folder) n.folder = stack_.back().folder;
        with_saved_note([this, n] {
            lv_textarea_set_text(new_ta_, "");
            new_error_.clear();
            push(n);
        });
        return true;
    }
    if (is_ctrl_letter(e, 'd')) {
        with_saved_note([this] { open_today(); });
        return true;
    }
    return false;
}

/* The options of what the page shows (Ctrl+O, or a 3 s hold on a note's row): a note's options in a list, the More
 * menu of the preview and of the editor. */
void App::open_options()
{
    if (popup_open()) return;
    switch (page()) {
    case PageId::Folder:
    case PageId::Recent: {
        RowList &l = page() == PageId::Folder ? folder_list_ : recent_list_;
        const std::string &id = l.id_at(l.selected());
        if (id.rfind("note:", 0) == 0) note_options(id.substr(5));
        else notice("Options are for a note: choose one first");
        break;
    }
    case PageId::Preview:
    case PageId::Editor: note_options(); break;
    default: break;
    }
}

/* The options box of the note shown: every action of the page with its Ctrl key in grey (owner decision 2026-10-24:
 * the bars went; a 3 s hold on the note, or Ctrl+O, opens this). The on-screen keyboard goes for the box and comes back
 * after it when the editor was being typed into. */
void App::note_options()
{
    if (popup_open() || (page() != PageId::Preview && page() != PageId::Editor)) return;
    const std::vector<NoteAction> acts = page() == PageId::Editor ? editor_actions() : preview_actions();
    std::vector<std::string> labels, keys;
    std::vector<Mark> marks;
    for (NoteAction a : acts) {
        labels.push_back(note_action_label(a));
        keys.push_back(note_action_key(a));
        marks.push_back(note_action_asks(a) ? Mark::Chevron : Mark::Dot);
    }
    open_menu(options_title(note_.name), labels, [this, acts](int i) {
        if (i >= 0 && i < static_cast<int>(acts.size())) note_action(acts[static_cast<std::size_t>(i)]);
    }, keys, marks);
    pop_.body = options_body(note_.name);
}

void App::note_action(NoteAction a)
{
    switch (a) {
    case NoteAction::Edit: preview_to_editor(); break;
    case NoteAction::Preview: editor_to_preview(); break;
    case NoteAction::TickTask:
        if (page() == PageId::Editor) editor_tick_task();
        break;
    case NoteAction::InsertStamp:
        if (page() == PageId::Editor) editor_insert(stamp_text(static_cast<std::time_t>(now_epoch())));
        break;
    case NoteAction::NoteInfo: show_note_info(); break;
    case NoteAction::Help: open_help(); break;
    case NoteAction::Delete: delete_this_note(); break;
    }
}

/* The help page goes over the preview or the editor, which stay as they are: Esc comes back to the same scroll, the same
 * cursor, the unsaved text as it was (no autosave meanwhile; the app's end and a hide still save it). */
void App::open_help()
{
    if (page() != PageId::Preview && page() != PageId::Editor) return;
    editor_flush();
    push(Nav{PageId::Help, "", note_.rel});
}

void App::preview_to_editor()
{
    if (page() != PageId::Preview) return;
    if (!note_.file.editable()) {
        notice(readonly_text(note_.file.status), false);
        return;
    }
    replace_top(Nav{PageId::Editor, "", note_.rel});
}

void App::editor_to_preview()
{
    if (page() == PageId::Editor) with_saved_note([this] { replace_top(Nav{PageId::Preview, "", note_.rel}); });
}

void App::delete_this_note()
{
    if (page() == PageId::Preview) {
        ask_delete(note_.rel, [this] { go_up(); });
        return;
    }
    if (page() != PageId::Editor) return;
    ask_delete(note_.rel, [this] { go_up(); }, [this] {
        // the trash gets the text as typed; a note changed on disk meanwhile keeps ours beside it
        editor_flush();
        if (!note_.dirty) return;
        const std::string body = editor_text();
        const SaveResult r = save_note(note_.path, body, note_.file.body.fmt, note_.file.stamp);
        if (r.status == SaveStatus::Conflict) {
            int err = 0;
            create_note(conflict_copy_path(note_.path, static_cast<std::time_t>(now_epoch())), body, note_.file.body.fmt, nullptr, &err);
        }
        note_.dirty = false;
    });
}

/* A physical keyboard is there (input.state, as the on-screen keyboard's rule reads it; asked at most once a second). */
bool App::keyboard_present()
{
    cdk::osk_policy::Policy *p = cdk::osk_policy::installed();
    if (!p) return true;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    p->allowed(static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);     // reads the file again when it changed
    if (p->from_file()) return p->input().keyboards > 0;
    return p->fallback_keyboards() > 0;
}

void App::row_pressed(RowList *list, int what, int index, int x, int y)
{
    if (what == 1) {
        hold_.move(x, y);
        return;
    }
    if (what == 2) {
        if (hold_.visible(mono_ms())) hold_release_ms_ = mono_ms();     // the cue showed: letting go is not a tap
        hold_.cancel();
        return;
    }
    const bool holdable = (list == &folder_list_ && page() == PageId::Folder) || (list == &recent_list_ && page() == PageId::Recent);
    if (!holdable || popup_open() || list->id_at(index).rfind("note:", 0) != 0) {
        hold_.cancel();
        hold_.take_swallow();
        return;
    }
    hold_list_ = list;
    hold_scroller_ = list->obj();
    hold_index_ = index;
    hold_scroll_y_ = lv_obj_get_scroll_y(list->obj());
    hold_.begin(list->id_at(index), x, y, mono_ms());
}

void App::note_pressed(lv_obj_t *scroller, int what, int x, int y)
{
    if (what == 1) {
        hold_.move(x, y);                  // a move beyond the slop (a scroll) gives the hold up
        return;
    }
    if (what == 2) {
        if (hold_.visible(mono_ms())) hold_release_ms_ = mono_ms();
        hold_.cancel();
        return;
    }
    if (popup_open() || (page() != PageId::Preview && page() != PageId::Editor)) {
        hold_.cancel();
        hold_.take_swallow();
        return;
    }
    hold_list_ = nullptr;
    hold_scroller_ = scroller;
    hold_index_ = -1;
    hold_scroll_y_ = lv_obj_get_scroll_y(scroller);
    hold_.begin("note", x, y, mono_ms());
}

bool App::tap_after_hold()
{
    if (hold_.take_swallow()) return true;              // the release of a completed 3 s hold is not a tap
    if (hold_release_ms_ && mono_ms() - hold_release_ms_ < 300) {   // nor the release of a hold given up after its cue
        hold_release_ms_ = 0;
        return true;
    }
    return false;
}

void App::hold_tick(std::uint64_t now)
{
    if (!hold_.active()) {
        set_hidden(hold_box_, true);
        return;
    }
    if (popup_open() || !hold_scroller_ || lv_obj_get_scroll_y(hold_scroller_) != hold_scroll_y_) {
        hold_.cancel();
        set_hidden(hold_box_, true);
        return;
    }
    if (hold_.poll(now)) {
        set_hidden(hold_box_, true);
        if (hold_list_) hold_list_->set_selected(hold_index_, false);
        open_options();
        return;
    }
    if (hold_.visible(now)) {
        std::string what = "options";
        if (!hold_list_) what = "options for " + truncate_text(note_.name, 28);
        else if (const Row *r = hold_list_->row(hold_index_); r && !r->cells.empty()) what = "options for " + truncate_text(r->cells[0].text, 28);
        lv_label_set_text(hold_label_, ("Keep holding: " + what).c_str());
        lv_obj_set_width(hold_bar_, std::max(1, static_cast<int>(hold_.progress(now) * hold_track_w_)));
        const int h = lv_obj_get_height(hold_box_);
        lv_obj_set_y(hold_box_, std::max(0, panel_h_ - h - geo().s));
        lv_obj_move_foreground(hold_box_);
        set_hidden(hold_box_, false);
    }
}

void App::row_clicked(RowList *list, int index)
{
    if (tap_after_hold()) return;
    if (mono_ms() - start_ms_ < kStartGuardMs || popup_open()) return;
    const Row *r = list->row(index);
    if (!r || !r->selectable || r->section) return;
    const bool again = index == list->selected();
    if (list == &open_list_ && open_typing_) open_typing_ = false;   // a tap on the matches puts the keyboard away
    list->set_selected(index, false);
    if (again) {
        if (list == &home_list_) home_activate(r->id);
        else if (list == &folder_list_ || list == &recent_list_) list_activate(*list, index);
        else if (list == &open_list_) open_activate(index);
        else if (list == &settings_list_) settings_activate(index);
    } else if (list == &open_list_) {
        open_sel_ = index;
    } else if (list == &settings_list_) {
        settings_sel_ = index;
    }
    invalidate();
}

void App::button_clicked(int tag)
{
    editor_flush();
    if (mono_ms() - start_ms_ < kStartGuardMs) return;
    if (tag >= kTagPopLeft && tag <= kTagPopNo) {
        post([this, tag] {
            if (popup_open()) popup_click(tag);
        });
        return;
    }
    if (popup_open()) return;
    switch (tag) {
    case kTagOpenField:            // a tap on the field: typing there, and the keyboard asked for again (the rule decides)
        open_typing_ = true;
        osk_tap_ = true;
        break;
    case kTagNewField:
        new_typing_ = true;
        osk_tap_ = true;
        break;
    case kTagNewCreate: post([this] { new_create(); }); break;
    default: break;
    }
    invalidate();
}

/* ================================================================== the notes */

bool App::load_note_into(const std::string &rel)
{
    note_ = Note{};
    note_.rel = rel;
    note_.path = index_.abs_path(rel);
    note_.name = strip_md(base_name(rel));
    note_.file = load_note(note_.path);
    return note_.file.readable();
}

bool App::open_note(const std::string &rel, OpenMode mode, bool replace, const std::string &heading)
{
    const LoadedNote f = load_note(index_.abs_path(rel));
    if (!f.readable()) {
        notice(load_failure_text(f.status, f.err), false);
        return false;
    }
    PageId target = mode == OpenMode::Editor ? PageId::Editor
                    : mode == OpenMode::Preview ? PageId::Preview
                    : settings_.open_in == OpenIn::Editor ? PageId::Editor : PageId::Preview;
    if (target == PageId::Editor && !f.editable()) {
        target = PageId::Preview;
        notice(readonly_text(f.status), false);
    }
    Nav n{target, "", rel};
    if (replace) replace_top(n);
    else push(n);
    if (!heading.empty() && page() == PageId::Preview) scroll_to_heading(heading);
    return true;
}

/* A new empty note (its folders made), opened in the editor. A name taken meanwhile opens that note. */
void App::create_and_edit(const std::string &rel, bool replace)
{
    int err = 0;
    DiskStamp stamp;
    if (!create_note(index_.abs_path(rel), "", TextFormat{}, &stamp, &err)) {
        if (err != EEXIST) {
            notice(std::string("Could not create it: ") + std::strerror(err), false);
            return;
        }
    } else {
        log_line("created " + rel);
    }
    index_stale_ = true;
    rewalk();
    open_note(rel, OpenMode::Editor, replace);
}

/* Today's note, YYYY-MM-DD.md at the vault root (core daily_note_file), made when missing; it opens in the editor.
 * The boards have no RTC: a clock that looks unset asks first. */
void App::open_today()
{
    const std::time_t now = static_cast<std::time_t>(now_epoch());
    const std::string rel = daily_note_file(now);
    if (stat_path(index_.abs_path(rel)).exists) {
        open_note(rel, OpenMode::Editor);
        return;
    }
    if (clock_looks_unsynced(now, build_year(CDK_BUILD_DATE))) {
        open_confirm("The clock looks unset", clock_warning_body(date_text(now)), [this, rel] { create_and_edit(rel, false); }, "Create",
                     "Cancel");
        return;
    }
    create_and_edit(rel, false);
}

void App::ask_delete(const std::string &rel, std::function<void()> after, std::function<void()> before)
{
    const std::string name = strip_md(base_name(rel));
    open_confirm("Delete this note?", delete_body(name), [this, rel, name, after, before] {
        if (before) before();
        const TrashResult t = move_to_trash(vault_.path, rel);
        if (!t.ok) {
            notice(std::string("Could not delete it: ") + std::strerror(t.err), false);
            return;
        }
        log_line("trashed " + rel + " -> " + t.trashed);
        index_stale_ = true;
        notice("Moved to .trash: " + name);
        if (after) after();
    }, "Yes", "No", true);
}

void App::show_note_info()
{
    const FileStat fs = stat_path(note_.path);
    const std::string body = page() == PageId::Editor ? editor_text() : note_.file.body.text;
    open_info("Note info", note_info_text(note_.rel, fs.size, fs.mtime_ns, now_epoch(), body, note_.file.body.fmt,
                                          note_.file.status != LoadStatus::ReadOnlyNotUtf8));
}

void App::follow_link(const LinkPlan &p)
{
    switch (p.go) {
    case LinkGo::OpenNote: {
        const std::string rel = p.rel, heading = p.heading;
        with_saved_note([this, rel, heading] { open_note(rel, OpenMode::Preview, false, heading); });
        break;
    }
    case LinkGo::CreateNote: {
        const std::string rel = p.rel;
        with_saved_note([this, rel] { create_and_edit(rel, false); });
        break;
    }
    case LinkGo::SameNote:
        if (page() == PageId::Preview) scroll_to_heading(p.heading);
        break;
    case LinkGo::External: open_info("Web address", p.address, external_link_note()); break;
    case LinkGo::File:
    case LinkGo::Invalid: notice(link_problem_text(p), false); break;
    }
}

/* ================================================================== dialogs (RSS Reader's) */

void App::popup_begin(Popup p)
{
    hold_.cancel();
    p.opened_ms = mono_ms();
    pop_ = std::move(p);
    pop_dirty_ = true;                             // built in the next tick
    release_osk();
    invalidate();
}

void App::close_popup()
{
    pop_.kind = Popup::None;
    pop_.on_accept = nullptr;
    pop_.on_yes = nullptr;
    pop_.on_no = nullptr;
    pop_dirty_ = false;
    pop_items_.clear();
    pop_value_ = pop_detail_ = pop_pos_ = pop_left_ = pop_right_ = nullptr;
    dialog_.reset();
    invalidate();
}

void App::open_choice(notesui::Choice c, std::function<void(int)> on_accept)
{
    Popup p;
    p.kind = Popup::Choice;
    p.title = c.title;
    p.choice = std::move(c);
    p.on_accept = std::move(on_accept);
    popup_begin(std::move(p));
}

void App::open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes,
                       const std::string &no, bool danger, std::function<void()> on_no)
{
    Popup p;
    p.kind = Popup::Confirm;
    p.title = title;
    p.body = body;
    p.yes_text = yes;
    p.no_text = no;
    p.danger = danger;
    p.focus = danger ? 0 : 1;                      // Enter on a deletion means No until the user moves
    p.on_yes = std::move(on_yes);
    p.on_no = std::move(on_no);
    popup_begin(std::move(p));
}

void App::open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick,
                    const std::vector<std::string> &values, const std::vector<Mark> &marks)
{
    Popup p;
    p.kind = Popup::Menu;
    p.title = title;
    p.items = items;
    p.values = values;
    p.marks = marks;
    p.on_accept = std::move(on_pick);
    popup_begin(std::move(p));
}

void App::open_info(const std::string &title, const std::string &body, const std::string &small)
{
    Popup p;
    p.kind = Popup::Info;
    p.title = title;
    p.body = body;
    p.small = small;
    popup_begin(std::move(p));
}

lv_obj_t *App::dialog_button(std::size_t index) const
{
    if (!dialog_) return nullptr;
    lv_obj_t *backdrop = dialog_->root();
    if (!backdrop || lv_obj_get_child_count(backdrop) < 2) return nullptr;
    lv_obj_t *box = lv_obj_get_child(backdrop, 1);
    std::size_t n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); ++i) {
        lv_obj_t *c = lv_obj_get_child(box, static_cast<int32_t>(i));
        if (!lv_obj_check_type(c, &lv_button_class)) continue;
        if (n++ == index) return c;
    }
    return nullptr;
}

lv_obj_t *App::dialog_label(const std::string &text, bool small, std::uint32_t color)
{
    lv_obj_t *l = lv_label_create(dialog_->body());
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, dialog_->geometry().content_w);
    lv_obj_set_style_text_font(l, small ? f_caption_ : f_body_, 0);
    set_color(l, color);
    lv_label_set_text(l, text.c_str());
    return l;
}

/* A row of a menu: the ROW token high, the label, a grey value at the right, the marker (a dot: it acts; a chevron:
 * it opens a dialog or a choice). A tap picks it. */
lv_obj_t *App::dialog_row(const std::string &text, const std::string &value, Mark mark, int index)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int w = dialog_->geometry().content_w, h = std::max(m->tok[CP0_TOK_ROW_H], line_h(f_body_) + m->tok[CP0_TOK_SPACE_S]);
    cp0_ui_list_layout_t l{};
    cp0_ui_list_layout(m, CP0_UI_LIST_COMFORTABLE, 0, 0, w, h, &l);
    l.text_px = m->text_px[CP0_TEXT_BODY];
    lv_obj_t *row = make_box(dialog_->body(), 0, 0, w, h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    const bool chev = mark == Mark::Chevron, dot = mark == Mark::Dot || mark == Mark::DotRunning;
    const int trail = cp0_ui_list_trail_w(&l, chev ? CP0_UI_LIST_AFFORDANCE_OPEN : dot ? CP0_UI_LIST_AFFORDANCE_ACTION : CP0_UI_LIST_AFFORDANCE_UNSPECIFIED);
    const int lh = line_h(f_body_), vh = line_h(f_value_);
    const int vw = value.empty() ? 0 : std::min(text_w(value, f_value_) + 2, (w - 2 * l.inset - trail) / 2);
    lv_obj_t *label = make_label(row, f_body_, kText, l.inset, (h - lh) / 2, std::max(10, w - 2 * l.inset - trail - (vw ? vw + l.gap : 0)), lh);
    lv_label_set_text(label, text.c_str());
    if (vw) {   // (a Ctrl key of an options box: grey)
        lv_obj_t *v = make_label(row, f_value_, kMuted, w - l.inset - trail - vw, (h - vh) / 2, vw, vh);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(v, value.c_str());
    }
    if (dot) {
        const int d = cp0_ui_list_dot_px(&l);
        lv_obj_t *o = make_box(row, cp0_ui_list_dot_x(&l, w), (h - d) / 2, d, d);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(cp0_ui_list_dot_rgb(CP0_UI_LIST_AFFORDANCE_ACTION)), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    } else if (chev) {
        const lv_font_t *cf = mark_font(cp0_ui_list_chevron_px(&l));
        lv_obj_t *c = lv_label_create(row);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_font_glyph_dsc_t glyph{};
        const bool has = lv_font_get_glyph_dsc(cf, &glyph, CP0_UI_LIST_CHEVRON_CODEPOINT, 0);
        if (!has) lv_font_get_glyph_dsc(cf, &glyph, static_cast<uint32_t>(CP0_UI_LIST_CHEVRON_FALLBACK[0]), 0);
        lv_label_set_text(c, has ? CP0_UI_LIST_CHEVRON_TEXT : CP0_UI_LIST_CHEVRON_FALLBACK);
        lv_obj_set_style_text_font(c, cf, 0);
        lv_obj_set_style_text_color(c, lv_color_hex(CP0_UI_LIST_CHEVRON_RGB), 0);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_RIGHT, 0);
        const int clh = line_h(cf);
        const int ink_top = (clh - cf->base_line) - glyph.box_h - glyph.ofs_y;
        lv_obj_set_size(c, cp0_ui_list_chevron_w(&l), clh);
        lv_obj_set_pos(c, w - l.inset - cp0_ui_list_chevron_w(&l), cp0_ui_list_chevron_y(h, ink_top, glyph.box_h));
    }
    lv_obj_add_event_cb(row, trampoline_popitem, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
    return row;
}

/* The widgets of the pending dialog (from the tick, never inside an event of a dialog's button): a cp0::ui::Dialog, which
 * keeps the header free and follows the on-screen keyboard. Titles stay ASCII (the toolkit draws them in its builtin
 * face); note names go in the body, in the app's face. */
void App::popup_build()
{
    dialog_.reset();
    pop_items_.clear();
    pop_value_ = pop_detail_ = pop_pos_ = pop_left_ = pop_right_ = nullptr;
    if (pop_.kind == Popup::None) return;
    using cp0::ui::ButtonRole;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    auto tapped = [this](int tag) {
        return [this, tag] {
            post([this, tag] {
                if (popup_open()) popup_click(tag);
            });
        };
    };
    cp0::ui::DialogSpec spec;
    spec.title = pop_.title;
    spec.page = shell_.get();
    switch (pop_.kind) {
    case Popup::Choice:
        spec.buttons.push_back({"Cancel", ButtonRole::Cancel, tapped(kTagPopCancel)});
        spec.buttons.push_back({"OK", ButtonRole::Primary, tapped(kTagPopOk)});
        break;
    case Popup::Confirm:
        spec.buttons.push_back({pop_.no_text, ButtonRole::Cancel, tapped(kTagPopNo)});
        spec.buttons.push_back({pop_.yes_text, ButtonRole::Primary, tapped(kTagPopYes)});
        break;
    case Popup::Menu: spec.buttons.push_back({"Cancel", ButtonRole::Cancel, tapped(kTagPopCancel)}); break;
    case Popup::Info: spec.buttons.push_back({"OK", ButtonRole::Primary, tapped(kTagPopOk)}); break;
    case Popup::None: break;
    }
    dialog_ = std::make_unique<cp0::ui::Dialog>(shell_->root(), layout_, std::move(spec));
    const int cw = dialog_->geometry().content_w;
    switch (pop_.kind) {
    case Popup::Choice: {
        // the value in the middle, an arrow on each side (ROW square: 7 mm); Left / Right do the same
        const int side = m->tok[CP0_TOK_ROW_H], gap = m->tok[CP0_TOK_SPACE_M];
        lv_obj_t *row = lv_obj_create(dialog_->body());
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row, cw, side);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto arrow = [&](const char *symbol, int tag) {
            lv_obj_t *b = lv_button_create(row);
            if (lv_obj_get_group(b)) lv_group_remove_obj(b);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_size(b, side, side);
            lv_obj_set_style_radius(b, layout_.esc_radius, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(kSelected), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(kPressed), LV_STATE_PRESSED);
            lv_obj_set_style_shadow_width(b, 0, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_add_event_cb(b, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, symbol);
            lv_obj_set_style_text_font(l, cp0_ui_font(CP0_TEXT_SYMBOL), 0);
            lv_obj_center(l);
            return b;
        };
        pop_left_ = arrow(LV_SYMBOL_LEFT, kTagPopLeft);
        pop_value_ = lv_label_create(row);
        lv_label_set_long_mode(pop_value_, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(pop_value_, std::max(10, cw - 2 * side - 2 * gap));
        lv_obj_set_style_text_align(pop_value_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(pop_value_, font("Montserrat-Bold.ttf", cp0_ui_roller_font_px((m->text_px[CP0_TEXT_BODY] + m->text_px[CP0_TEXT_HEADING]) / 2)), 0);
        set_color(pop_value_, kText);
        pop_right_ = arrow(LV_SYMBOL_RIGHT, kTagPopRight);
        pop_detail_ = dialog_label("", false, kText);
        pop_pos_ = dialog_label("", true, kMuted);
        lv_obj_set_style_text_align(pop_detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(pop_pos_, LV_TEXT_ALIGN_CENTER, 0);
        if (!pop_.choice.note.empty()) {
            lv_obj_t *note = dialog_label(pop_.choice.note, true, kMuted);
            lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        }
        break;
    }
    case Popup::Confirm: dialog_label(pop_.body, false, kText); break;
    case Popup::Menu:
        lv_obj_set_style_pad_row(dialog_->body(), 0, 0);       // the rows of a menu touch, as in a list
        if (!pop_.body.empty()) dialog_label(pop_.body, true, kMuted);
        for (std::size_t i = 0; i < pop_.items.size(); ++i) {
            const std::string value = i < pop_.values.size() ? pop_.values[i] : std::string();
            const Mark mk = i < pop_.marks.size() ? pop_.marks[i] : Mark::Dot;
            pop_items_.push_back(dialog_row(pop_.items[i], value, mk, static_cast<int>(i)));
        }
        break;
    case Popup::Info:
        dialog_label(pop_.body, false, kText);
        if (!pop_.small.empty()) dialog_label(pop_.small, true, kMuted);
        break;
    case Popup::None: break;
    }
    dialog_->relayout();
    if (lv_obj_t *vp = lv_obj_get_parent(dialog_->body())) style_list_scrollbar(vp);
    if (pop_.kind == Popup::Confirm && pop_.danger)
        if (lv_obj_t *yes = dialog_button(1)) lv_obj_set_style_bg_color(yes, lv_color_hex(kDangerRed), 0);
}

void App::popup_refresh()
{
    switch (pop_.kind) {
    case Popup::Choice: {
        if (!pop_value_) return;
        lv_label_set_text(pop_value_, pop_.choice.label().c_str());
        lv_label_set_text(pop_detail_, pop_.choice.detail().c_str());
        lv_label_set_text(pop_pos_, pop_.choice.position().c_str());
        set_color(lv_obj_get_child(pop_left_, 0), pop_.choice.can_prev() ? kText : kPressed);
        set_color(lv_obj_get_child(pop_right_, 0), pop_.choice.can_next() ? kText : kPressed);
        if (dialog_) dialog_->relayout();
        break;
    }
    case Popup::Confirm: {
        lv_obj_t *no = dialog_button(0), *yes = dialog_button(1);
        if (no) lv_obj_set_style_border_width(no, pop_.focus == 0 ? 3 : 0, 0);
        if (yes) lv_obj_set_style_border_width(yes, pop_.focus == 1 ? 3 : 0, 0);
        if (no) lv_obj_set_style_border_color(no, lv_color_hex(kText), 0);
        if (yes) lv_obj_set_style_border_color(yes, lv_color_hex(kText), 0);
        break;
    }
    case Popup::Menu:
        for (std::size_t i = 0; i < pop_items_.size(); ++i) {
            const bool sel = static_cast<int>(i) == pop_.focus;
            lv_obj_set_style_bg_color(pop_items_[i], lv_color_hex(kSelected), 0);
            lv_obj_set_style_bg_opa(pop_items_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        if (pop_.focus >= 0 && pop_.focus < static_cast<int>(pop_items_.size()))
            lv_obj_scroll_to_view_recursive(pop_items_[static_cast<std::size_t>(pop_.focus)], LV_ANIM_OFF);
        break;
    default: break;
    }
}

void App::popup_accept()
{
    auto cb = pop_.on_accept;
    const int idx = pop_.kind == Popup::Choice ? pop_.choice.index : pop_.focus;
    close_popup();
    if (cb) cb(idx);
}

void App::popup_cancel()
{
    // a two-button question: Esc is its left button (No); its answer may be needed (the changed-on-disk question)
    if (pop_.kind == Popup::Confirm) {
        popup_no();
        return;
    }
    close_popup();
}

void App::popup_no()
{
    auto cb = pop_.on_no;
    close_popup();
    if (cb) cb();
}

void App::popup_key(const KeyEvent &e)
{
    if (mono_ms() - pop_.opened_ms < 300 && e.key == Key::Enter) return;      // the Enter that opened it must not answer it
    if (pop_dirty_) return;
    switch (pop_.kind) {
    case Popup::Choice: {
        const ChoiceResult r = choice_key(e, pop_.choice);
        if (r == ChoiceResult::Moved) popup_refresh();
        else if (r == ChoiceResult::Accept) popup_accept();
        else if (r == ChoiceResult::Cancel) popup_cancel();
        break;
    }
    case Popup::Confirm: {
        if (!pop_.yn_keys && e.key == Key::Char) return;
        const ConfirmResult r = confirm_key(e, pop_.focus);
        if (r == ConfirmResult::Moved) popup_refresh();
        else if (r == ConfirmResult::Yes) {
            auto cb = pop_.on_yes;
            close_popup();
            if (cb) cb();
        } else if (r == ConfirmResult::No) popup_no();
        break;
    }
    case Popup::Menu: {
        const MenuResult r = menu_key(e, pop_.focus, static_cast<int>(pop_.items.size()));
        if (r == MenuResult::Moved) popup_refresh();
        else if (r == MenuResult::Accept) popup_accept();
        else if (r == MenuResult::Cancel) popup_cancel();
        break;
    }
    case Popup::Info:
        if (!e.repeat && (e.key == Key::Enter || e.key == Key::Esc)) popup_cancel();
        break;
    case Popup::None: break;
    }
}

void App::popup_click(int tag)
{
    if (mono_ms() - pop_.opened_ms < 150 || pop_dirty_) return;
    switch (tag) {
    case kTagPopLeft:
        if (pop_.kind == Popup::Choice && pop_.choice.step(-1)) popup_refresh();
        break;
    case kTagPopRight:
        if (pop_.kind == Popup::Choice && pop_.choice.step(1)) popup_refresh();
        break;
    case kTagPopOk:
        if (pop_.kind == Popup::Choice) popup_accept();
        else close_popup();
        break;
    case kTagPopCancel: close_popup(); break;
    case kTagPopNo: popup_no(); break;
    case kTagPopYes: {
        auto cb = pop_.on_yes;
        close_popup();
        if (cb) cb();
        break;
    }
    default: break;
    }
}

void App::popup_pick(int index)
{
    if (pop_dirty_ || pop_.kind != Popup::Menu) return;
    if (mono_ms() - pop_.opened_ms < 150 || index < 0 || index >= static_cast<int>(pop_.items.size())) return;
    pop_.focus = index;
    post([this] {
        if (pop_.kind == Popup::Menu) popup_accept();
    });
}

/* ================================================================== the state line of the scripted tests */

std::string App::debug_state() const
{
    const char *pk = pop_.kind == Popup::Choice ? "choice" : pop_.kind == Popup::Confirm ? "confirm" : pop_.kind == Popup::Menu ? "menu"
                     : pop_.kind == Popup::Info ? "info" : "none";
    const RowList *list = page() == PageId::Home ? &home_list_ : page() == PageId::Folder ? &folder_list_
                          : page() == PageId::Recent ? &recent_list_ : page() == PageId::Open ? &open_list_
                          : page() == PageId::Settings ? &settings_list_ : nullptr;
    std::string sel_id = list ? list->id_at(list->selected()) : std::string();
    std::string s = std::string("state page=") + page_key(page()) + " depth=" + std::to_string(stack_.size()) +
                    " folder='" + stack_.back().folder + "' note='" + note_.rel + "' dirty=" + (note_.dirty ? "1" : "0") +
                    " notes=" + std::to_string(index_.notes.size()) + " rows=" + std::to_string(list ? list->count() : 0) +
                    " sel=" + std::to_string(list ? list->selected() : -1) + " sel_id='" + sel_id + "'" +
                    " blocks=" + std::to_string(doc_.blocks.size()) + " targets=" + std::to_string(targets_.size()) +
                    " boxes=" + std::to_string(boxes_.size()) + " ring=" + std::to_string(ring_) +
                    " truncated=" + (doc_.truncated ? "1" : "0") + " preview_px=" + std::to_string(preview_built_px_) +
                    " scroll=" + std::to_string(pv_ && page() == PageId::Preview ? lv_obj_get_scroll_y(pv_) : 0) +
                    " typing=" + (open_typing_ ? "open" : new_typing_ ? "new" : editor_typing_ ? "editor" : "-") +
                    " osk=" + (cp0::ui::osk::visible() ? "up" : "down") +
                    " osk_req=" + osk_field_key(osk_field_of(cp0::ui::osk::owner())) + " kbd=" + (kbd_present_ ? "1" : "0") +
                    " ed_scroll=" + std::to_string(ed_ta_ ? lv_obj_get_scroll_y(ed_ta_) : 0) +
                    " cursor=" + std::to_string(ed_ta_ ? lv_textarea_get_cursor_pos(ed_ta_) : 0) +
                    " text_size=" + text_size_key(settings_.text_size) + " open_in=" + open_in_key(settings_.open_in) +
                    " query='" + open_query_ + "' popup=" + pk + " popup_title='" + (pop_.kind == Popup::None ? std::string() : pop_.title) +
                    "' popup_value='" + (pop_.kind == Popup::Choice ? pop_.choice.label() : std::string()) + "' last_save='" + last_save_ +
                    "' notice='" + notice_text_ + "' title='" + (shell_ ? std::string(title_key_) : std::string()) +
                    "' caption='" + (cap_ ? lv_label_get_text(cap_) : "") + "'";
    return s;
}

} // namespace notesui
