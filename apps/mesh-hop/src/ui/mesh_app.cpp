/*
 * SPDX-License-Identifier: MIT
 */

#include "mesh_app.hpp"

#include "app.hpp"
#include "clock_policy.hpp"
#include "cp0_font_service.hpp"
#include "cp0_ui_metrics_lvgl.h"
#include "input_state.hpp"
#include "position.hpp"
#include "protocol.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace meshzero;

namespace meshhop {

namespace {

constexpr const char *kVersion = "0.3.0";

// ---- the colours: the launcher's (cp0_ui_list.h and cp0_ui_osk.h), not an own palette. Green, amber and red are the
// status tones (ValueTone Ok / Warn / Bad), the accent blue marks what is selected, focused or unread.
constexpr uint32_t kBackground = 0x000000;
constexpr uint32_t kPanel = CP0_UI_OSK_KEY_RGB;
constexpr uint32_t kSelected = CP0_UI_LIST_BAR_RGB;
constexpr uint32_t kSelectedDim = 0x202020;
constexpr uint32_t kPressed = CP0_UI_OSK_KEY_PRESSED_RGB;
constexpr uint32_t kText = CP0_UI_LIST_TEXT_RGB;
constexpr uint32_t kMuted = CP0_UI_LIST_MUTED_RGB;
constexpr uint32_t kAccent = CP0_UI_LIST_ACCENT_RGB;
constexpr uint32_t kWarn = CP0_UI_LIST_WARN_RGB;
constexpr uint32_t kGreen = CP0_UI_LIST_OK_RGB;
constexpr uint32_t kBlue = CP0_UI_LIST_ACCENT_RGB;
constexpr uint32_t kRed = CP0_UI_LIST_BAD_RGB;
constexpr uint32_t kOkGreen = CP0_UI_OSK_ENTER_RGB;      // the primary button: the keyboard's Enter blue
constexpr uint32_t kDangerRed = 0x7A2E24;
constexpr uint32_t kOnAccent = 0x101010;                  // the text on an accent blue pill

// ---- layout: the stage is 640 px wide (kScreenW); its height is what is left under the shell (App::layout_body)
constexpr int kListW = 204;                // the left pane of Chats
constexpr uint32_t kStartGuardMs = 600;    // the tap or Enter that started the app must not act
constexpr uint32_t kNoticeMs = 4000;
constexpr int kMaxShownMessages = 60;
constexpr uint32_t kLoadingMinMsDefault = 90;   // the "Loading..." popup is drawn at least once before the list is rebuilt

uint32_t loading_min_ms()
{
    static const uint32_t v = [] {
        const char *e = std::getenv("MESHHOP_LOADING_MS");        // a longer popup for the screenshots of the scripted run
        return e && *e ? static_cast<uint32_t>(std::atoi(e)) : kLoadingMinMsDefault;
    }();
    return v;
}
constexpr int kContactRowH = 44;

enum Tag {
    kTagTab = 0,           // 0..4
    kTagSend = 10, kTagMuteChan, kTagAdvertFlood, kTagAdvertZero, kTagRefresh, kTagSort, kTagDetailMsg, kTagDetailClose, kTagEdOk,
    kTagEdCancel, kTagCompose,
    kTagHdr = 30,          // 30..35: the column titles of the contacts table
    kTagTypeChip = 40,     // 40..44
    kTagAgeChip = 50,      // 50..53
    kTagPopLeft = 60, kTagPopRight, kTagPopOk, kTagPopCancel, kTagPopYes, kTagPopNo,
    kTagPopItem = 70,      // 70..79
    kTagTitleHold = 80,    // the title of the open conversation (hold 3 s: options)
    // phase 2
    kTagNearby = 90, kTagSelect, kTagGroup, kTagSelDone, kTagSelMenu, kTagSelDelete, kTagSelGroup,
    kTagDetailGroups, kTagDetailDelete, kTagChatDetails, kTagChatOptions, kTagNearbyClose, kTagNearbyScan, kTagNearbyIgnored,
    kTagSearchOpen, kTagSearchClose, kTagSearchConv, kTagSearchDate, kTagSearchDir, kTagSearchBox,
    kTagStatsClose, kTagStatsRefresh, kTagStatsAuto,
    // the Position box
    kTagPosOk, kTagPosCancel, kTagPosClear, kTagPosGps, kTagPosLat, kTagPosLon,
    // phase 3: the packet log
    kTagLogToggle, kTagLogClear, kTagLogClose,
};

App *g_app = nullptr;

uint32_t tone_color(Tone t)
{
    switch (t) {
    case Tone::Normal: return kText;
    case Tone::Muted: return kMuted;
    case Tone::Gold: return kWarn;
    case Tone::Green: return kGreen;
    case Tone::Blue: return kBlue;
    case Tone::Red: return kRed;
    }
    return kText;
}

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    return box;
}

void set_color(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

uint32_t fnv(uint32_t h, const std::string &s)
{
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return (h ^ 0xFF) * 16777619u;
}

uint32_t fnv_int(uint32_t h, int v)
{
    return fnv(h, std::to_string(v));
}

int centre(int row_h, const lv_font_t *f)
{
    return (row_h - static_cast<int>(lv_font_get_line_height(f))) / 2;
}

void trampoline_button(lv_event_t *e)
{
    if (!g_app) return;
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_app->button_clicked(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

/* The point of the event's indev in stage coordinates (the 640x480 layout the hit tests by x and the hold use). */
lv_point_t event_point(lv_event_t *e)
{
    lv_point_t p = {0, 0};
    if (lv_indev_t *indev = lv_event_get_indev(e)) lv_indev_get_point(indev, &p);
    return stage_point(p);
}

void trampoline_row(lv_event_t *e)
{
    if (!g_app) return;
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
    auto *list = static_cast<RowList *>(lv_event_get_user_data(e));
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    const lv_point_t p = event_point(e);
    switch (code) {
    case LV_EVENT_CLICKED: g_app->row_clicked(list, index, p.x); break;
    case LV_EVENT_PRESSED: g_app->hold_event(0, list, index, p.x, p.y); break;
    case LV_EVENT_PRESSING: g_app->hold_event(1, list, index, p.x, p.y); break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: g_app->hold_event(2, list, index, p.x, p.y); break;
    default: break;
    }
}

/* a row of a menu or a pick list in a dialog */
void trampoline_popitem(lv_event_t *e)
{
    if (!g_app || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_app->popup_pick(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

/* the title of the open conversation: a tap does nothing, a 3 s hold opens the options */
void trampoline_title(lv_event_t *e)
{
    if (!g_app) return;
    const lv_event_code_t code = lv_event_get_code(e);
    const lv_point_t p = event_point(e);
    switch (code) {
    case LV_EVENT_CLICKED: g_app->button_clicked(kTagTitleHold); break;
    case LV_EVENT_PRESSED: g_app->hold_event(0, nullptr, 0, p.x, p.y); break;
    case LV_EVENT_PRESSING: g_app->hold_event(1, nullptr, 0, p.x, p.y); break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: g_app->hold_event(2, nullptr, 0, p.x, p.y); break;
    default: break;
    }
}

void trampoline_scroll(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SCROLL) return;
    static_cast<RowList *>(lv_event_get_user_data(e))->update_window();
}

std::string join_lines(const std::vector<std::string> &v)
{
    std::string out;
    for (const std::string &s : v) {
        if (!out.empty()) out += "\n";
        out += s;
    }
    return out;
}

} // namespace

/* ================================================================== RowList */

void RowList::create(lv_obj_t *parent, int x, int y, int w, int h, const Config &cfg)
{
    cfg_ = cfg;
    w_ = w;
    h_ = h;
    cont_ = lv_obj_create(parent);
    lv_obj_remove_style_all(cont_);
    lv_obj_set_pos(cont_, x, y);
    lv_obj_set_size(cont_, w, h);
    lv_obj_set_scroll_dir(cont_, LV_DIR_VER);
    lv_obj_remove_flag(cont_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(cont_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(cont_, lv_color_hex(kMuted), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(cont_, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(cont_, 4, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(cont_, trampoline_scroll, LV_EVENT_SCROLL, this);
    spacer_ = make_box(cont_, 0, 0, w, 1);
    lv_obj_remove_flag(spacer_, LV_OBJ_FLAG_CLICKABLE);

    const int pool = (h + cfg.min_row_h - 1) / cfg.min_row_h + 4;
    pool_.resize(static_cast<size_t>(pool));
    for (Slot &s : pool_) {
        s.row = make_box(cont_, 0, 0, w, cfg.min_row_h);
        lv_obj_set_style_bg_opa(s.row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s.row, lv_color_hex(kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_border_side(s.row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(s.row, lv_color_hex(0x1E1E1E), 0);
        for (lv_event_code_t code : {LV_EVENT_CLICKED, LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
            lv_obj_add_event_cb(s.row, trampoline_row, code, this);
        for (int k = 0; k < cfg.max_boxes; ++k) {
            lv_obj_t *b = make_box(s.row, 0, 0, 10, 10);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);                 // a tap on a button-like cell belongs to the row (the x of the tap says which)
            lv_obj_set_style_radius(b, 6, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            set_hidden(b, true);
            s.boxes.push_back(b);
        }
        for (int k = 0; k < cfg.max_cells; ++k) {
            lv_obj_t *l = lv_label_create(s.row);
            lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
            set_hidden(l, true);
            s.labels.push_back(l);
        }
        s.bar = make_box(s.row, 0, 0, 4, 10);
        lv_obj_remove_flag(s.bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(s.bar, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, 0);
        set_hidden(s.bar, true);
        s.pill = make_box(s.row, 0, 0, 34, 22);
        lv_obj_remove_flag(s.pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(s.pill, 11, 0);
        lv_obj_set_style_bg_color(s.pill, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(s.pill, LV_OPA_COVER, 0);
        s.pill_label = lv_label_create(s.pill);
        lv_obj_set_style_text_font(s.pill_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s.pill_label, lv_color_hex(kOnAccent), 0);
        set_hidden(s.pill, true);
        hide(s);
    }
}

void RowList::resize(int h)
{
    if (!cont_ || h == h_) return;
    h_ = h;
    lv_obj_set_height(cont_, h);
    lv_obj_update_layout(cont_);
    const int max_scroll = std::max(0, total_ - h_);
    if (lv_obj_get_scroll_y(cont_) > max_scroll) lv_obj_scroll_to_y(cont_, max_scroll, LV_ANIM_OFF);
    update_window();
}

void RowList::reset()
{
    pool_.clear();
    rows_.clear();
    off_.clear();
    cont_ = spacer_ = nullptr;
    total_ = 0;
    selected_ = -1;
}

void RowList::hide(Slot &s)
{
    s.index = -1;
    set_hidden(s.row, true);
}

void RowList::bind(Slot &s, int i)
{
    const RowSpec &r = rows_[static_cast<size_t>(i)];
    s.index = i;
    set_hidden(s.row, false);
    lv_obj_set_pos(s.row, 0, off_[static_cast<size_t>(i)]);
    lv_obj_set_size(s.row, w_, r.h);
    lv_obj_set_user_data(s.row, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    const bool sel = i == selected_;
    lv_obj_set_style_bg_opa(s.row, (sel || r.header) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s.row, lv_color_hex(r.header ? kPanel : sel ? (focused_ ? kSelected : kSelectedDim) : kBackground), 0);
    lv_obj_set_style_border_width(s.row, r.header ? 0 : 1, 0);
    if (r.selectable) lv_obj_add_flag(s.row, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(s.row, LV_OBJ_FLAG_CLICKABLE);
    if (sel && focused_) {
        lv_obj_set_size(s.bar, 4, r.h);
        set_hidden(s.bar, false);
    } else {
        set_hidden(s.bar, true);
    }
    size_t used_l = 0, used_b = 0;
    for (const Cell &c : r.cells) {
        if (c.text.empty()) continue;
        if (used_l >= s.labels.size()) break;
        lv_obj_t *l = s.labels[used_l++];
        const lv_font_t *f = c.font ? c.font : &lv_font_montserrat_16;
        const int lh = static_cast<int>(lv_font_get_line_height(f));
        int ly = c.y;
        lv_text_align_t align = c.align;
        if (c.box != 0 && used_b < s.boxes.size()) {                  // a button-like cell: the box, the text centred in it
            lv_obj_t *b = s.boxes[used_b++];
            const int bh = r.h - 2 * c.y;
            lv_obj_set_pos(b, c.x, c.y);
            lv_obj_set_size(b, c.w, bh);
            lv_obj_set_style_bg_color(b, lv_color_hex(c.box), 0);
            set_hidden(b, false);
            ly = c.y + (bh - lh) / 2;
            align = LV_TEXT_ALIGN_CENTER;
        }
        lv_obj_set_style_text_font(l, f, 0);
        set_color(l, c.color);
        lv_obj_set_style_text_align(l, align, 0);
        lv_obj_set_width(l, c.w);
        lv_obj_set_height(l, lh);
        lv_obj_set_pos(l, c.x, ly);
        lv_label_set_text(l, c.text.c_str());
        set_hidden(l, false);
    }
    for (size_t k = used_l; k < s.labels.size(); ++k) set_hidden(s.labels[k], true);
    for (size_t k = used_b; k < s.boxes.size(); ++k) set_hidden(s.boxes[k], true);
    if (r.badge > 0) {
        lv_obj_set_pos(s.pill, w_ - 44, (r.h - 22) / 2);
        lv_label_set_text(s.pill_label, fmt_count_badge(r.badge).c_str());
        lv_obj_center(s.pill_label);
        set_hidden(s.pill, false);
    } else {
        set_hidden(s.pill, true);
    }
}

void RowList::update_window()
{
    const int n = static_cast<int>(rows_.size());
    if (n == 0) {
        for (Slot &s : pool_) hide(s);
        return;
    }
    const int y0 = lv_obj_get_scroll_y(cont_);
    int first = static_cast<int>(std::upper_bound(off_.begin(), off_.end(), y0) - off_.begin()) - 1;
    first = std::max(first, 0);
    int last = static_cast<int>(std::lower_bound(off_.begin(), off_.end(), y0 + h_) - off_.begin());
    last = std::min(last, n - 1);
    const int lo = std::max(0, first - 1), hi = std::min(n - 1, last + 1);
    std::vector<char> have(static_cast<size_t>(hi - lo + 1), 0);
    std::vector<Slot *> spare;
    for (Slot &s : pool_) {
        if (s.index >= lo && s.index <= hi) have[static_cast<size_t>(s.index - lo)] = 1;
        else spare.push_back(&s);
    }
    size_t next = 0;
    for (int i = lo; i <= hi; ++i) {
        if (have[static_cast<size_t>(i - lo)]) continue;
        if (next >= spare.size()) break;
        bind(*spare[next++], i);
    }
    for (; next < spare.size(); ++next)
        if (spare[next]->index >= 0) hide(*spare[next]);
}

void RowList::set(std::vector<RowSpec> rows, int selected, bool focused)
{
    if (!cont_) return;
    const int old_scroll = lv_obj_get_scroll_y(cont_);
    rows_ = std::move(rows);
    selected_ = selected;
    focused_ = focused;
    off_.assign(rows_.size(), 0);
    total_ = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        off_[i] = total_;
        total_ += rows_[i].h;
    }
    lv_obj_set_size(spacer_, w_, std::max(total_, 1));
    for (Slot &s : pool_) hide(s);
    lv_obj_update_layout(cont_);
    lv_obj_scroll_to_y(cont_, std::min(old_scroll, std::max(0, total_ - h_)), LV_ANIM_OFF);
    update_window();
}

void RowList::set_selected(int selected, bool focused, bool scroll)
{
    if (!cont_) return;
    const int old = selected_;
    const bool focus_changed = focused != focused_;
    selected_ = selected;
    focused_ = focused;
    for (Slot &s : pool_)
        if (s.index >= 0 && (focus_changed || s.index == old || s.index == selected)) bind(s, s.index);
    if (scroll) scroll_to_selected();
}

void RowList::scroll_to_selected()
{
    const int n = static_cast<int>(rows_.size());
    if (selected_ < 0 || selected_ >= n) return;
    const int y0 = lv_obj_get_scroll_y(cont_);
    const int top = off_[static_cast<size_t>(selected_)], bot = top + rows_[static_cast<size_t>(selected_)].h;
    int target = y0;
    if (top < y0) target = top;
    else if (bot > y0 + h_) target = bot - h_;
    if (target != y0) lv_obj_scroll_to_y(cont_, target, LV_ANIM_OFF);
    update_window();
}

void RowList::scroll_to_top()
{
    if (!cont_) return;
    lv_obj_scroll_to_y(cont_, 0, LV_ANIM_OFF);
    update_window();
}

const std::string &RowList::id_at(int index) const
{
    static const std::string none;
    if (index < 0 || index >= static_cast<int>(rows_.size())) return none;
    return rows_[static_cast<size_t>(index)].id;
}

int RowList::bound_rows() const
{
    int n = 0;
    for (const Slot &s : pool_)
        if (s.index >= 0) ++n;
    return n;
}

/* ================================================================== construction */

namespace {

const lv_font_t *load_ttf(const std::vector<std::string> &dirs, const char *file, int size, const lv_font_t *fallback)
{
    for (const std::string &d : dirs) {
        const std::string path = d + "/" + file;
        if (::access(path.c_str(), R_OK) != 0) continue;
        lv_font_t *f = lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, static_cast<uint32_t>(size),
                                               LV_FREETYPE_FONT_STYLE_NORMAL);
        if (f) {
            f->fallback = fallback;
            return f;
        }
    }
    return nullptr;
}

std::vector<std::string> font_dirs()
{
    std::vector<std::string> dirs;
    if (const char *e = std::getenv("MESHHOP_FONT_DIR"); e && *e) dirs.push_back(e);
    dirs.push_back("/usr/share/APPLaunch/share/font");
    dirs.push_back("/usr/share/fonts/truetype/dejavu");
    return dirs;
}

} // namespace

/* The token text sizes: Montserrat from the launcher's font folder (the dialogs' and the status line's face), else the builtin one. */
static const lv_font_t *token_font(int px, bool bold)
{
    if (const lv_font_t *f = cp0_fonts().get(bold ? "Montserrat-Bold.ttf" : "Montserrat-Medium.ttf", static_cast<uint16_t>(px))) return f;
    return cp0_ui_font_px(px);
}

void App::load_fonts()
{
    f_small_ = &lv_font_montserrat_14;
    f_ui_ = &lv_font_montserrat_16;
    f_big_ = &lv_font_montserrat_18;
    f_value_small_ = &lv_font_montserrat_22;
    const auto dirs = font_dirs();
    f_text_ = load_ttf(dirs, "DejaVuSans.ttf", 16, f_ui_);
    f_text_small_ = load_ttf(dirs, "DejaVuSans.ttf", 14, f_small_);
    dejavu_ = f_text_ != nullptr;
    if (!f_text_) f_text_ = f_ui_;
    if (!f_text_small_) f_text_small_ = f_small_;
    // the shell and the dialogs: the token text sizes of the layout service
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    f_body_ = token_font(m->text_px[CP0_TEXT_BODY], false);
    f_caption_ = token_font(m->text_px[CP0_TEXT_CAPTION], false);
    f_title_ = token_font(m->text_px[CP0_TEXT_HEADING], true);
    f_field_ = load_ttf(dirs, "DejaVuSans.ttf", m->text_px[CP0_TEXT_BODY], f_body_);
    if (!f_field_) f_field_ = f_body_;
}

lv_obj_t *App::make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w, int h, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    set_color(label, color);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_text(label, "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);                 // a fixed size: the "dots" mode wraps when the height is automatic
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

lv_obj_t *App::make_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, int tag, lv_obj_t **label_out)
{
    lv_obj_t *b = make_box(parent, x, y, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(kPressed), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
    lv_obj_t *l = make_label(b, f_ui_, kText, 4, (h - static_cast<int>(lv_font_get_line_height(f_ui_))) / 2, w - 8,
                             static_cast<int>(lv_font_get_line_height(f_ui_)), LV_TEXT_ALIGN_CENTER);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, text);
    if (label_out) *label_out = l;
    return b;
}

App::App(Platform &plat) : plat_(plat), serial_(Store::default_dir())
{
    g_app = this;
    start_ms_ = mono_ms();
    store_ = std::make_unique<Store>(Store::default_dir());
    store_->attach(model_);
    log_ = std::make_unique<FileLog>(store_->dir() + "/mesh-hop.log");
    log_->line(std::string("mesh-hop ") + kVersion + " started");
    store_->set_log([this](const std::string &s) { if (log_) log_->line(s); });
    {   // the radio preset list: the saved copy of the last successful download, else the list built into the app
        std::vector<RadioPreset> saved;
        std::string date, src;
        if (load_preset_cache(store_->dir(), saved, date, src)) {
            set_presets(std::move(saved), PresetOrigin::Cached, date);
            log_->line("presets: using the saved copy (" + date + ", " + std::to_string(radio_presets().size()) + " entries)");
        } else {
            const bool had_file = ::access((store_->dir() + "/presets.jsonl").c_str(), F_OK) == 0;
            log_->line(std::string("presets: ") + (had_file ? "the saved copy is damaged, " : "") + "using the list built into the app (" + presets_date() + ")");
        }
        fetcher_ = std::make_unique<PresetFetcher>(store_->dir());
        preset_sched_ = std::make_unique<PresetSchedule>(start_ms_);
    }
    client_ = std::make_unique<Client>(serial_, model_);
    client_->set_log([this](const std::string &s) { if (log_) log_->line(s); });
    if (const char *d = std::getenv("MESHHOP_DEBUG"); d && *d && *d != '0') client_->options().log_frames = true;
    client_->set_deck_clock([this] { return deck_clock_.state(); }, [this] { deck_clock_.refresh(); });
    deck_clock_.refresh();                      // is the deck clock NTP synchronised? answered by a worker thread

    load_fonts();
    kbd_ok_ = keyboard_ready();                 // the key hint line takes room only while a keyboard is there
    build_screen();
    edit_ = model_.radio_settings();
    chat_rows_ = build_chat_rows(model_, conv_.rfind("d:", 0) == 0 ? conv_ : std::string());
    chat_sel_ = first_selectable(chat_rows_);
    if (chat_sel_ >= 0 && chat_rows_[static_cast<size_t>(chat_sel_)].kind != ChatRow::Add) {
        chat_sel_key_ = chat_rows_[static_cast<size_t>(chat_sel_)].key;
        conv_ = chat_sel_key_;
    }
    plat_.on_key = [this](const KeyEvent &e) { on_key(e); };
    render();
}

App::~App()
{
    plat_.on_key = nullptr;
    dialog_.reset();
    ed_dialog_.reset();
    shell_.reset();
    g_app = nullptr;
}

void App::post(std::function<void()> fn)
{
    posted_.push_back(std::move(fn));
}

/* What a widget's event asked for and cannot do itself (it would delete the widget that is running): run at the start of the next tick. */
void App::run_posted()
{
    while (!posted_.empty()) {
        std::vector<std::function<void()>> now;
        now.swap(posted_);
        for (auto &fn : now) fn();
    }
}

/* The shell: cp0::ui::Page (the header with the title, the Esc button and the shared status strip, and under it the
 * caption line: the board and the notices), the tab row, and the stage (a 640 px column centred under them) that holds
 * the page bodies. Nothing here is a pixel constant: the sizes come from the layout service. */
void App::build_shell()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    cp0_ui_roller_page(m, cp0::ui::status_strip_left(), &layout_);
    const int cap_lh = lv_font_get_line_height(cp0_ui_font_px(layout_.caption_px));
    layout_.caption_y = layout_.body_y + m->tok[CP0_TOK_SPACE_S] / 4;      // right under the header, the tab row follows it
    tabs_y_ = layout_.caption_y + cap_lh + m->tok[CP0_TOK_SPACE_S] / 2;
    tab_h_ = layout_.row_h;
    stage_x_ = std::max(0, (layout_.w - kScreenW) / 2);
    hint_h_ = lv_font_get_line_height(f_caption_) + m->tok[CP0_TOK_SPACE_S];
    panel_h_ = layout_.h - tabs_y_;                 // the largest room (no hint line): the lists are made for it
    page_h_ = panel_h_ - tab_h_;

    shell_ = std::make_unique<cp0::ui::Page>(plat_.screen(), layout_, cp0::ui::add_title, [this] { header_back(); });
    cp0::ui::StatusStrip::add(shell_->header());
    shell_->set_title({"Mesh Hop", "Chats"});
}

/* The stage: a 640 px column under the caption line that holds the page bodies and the sub-pages; made after the tab row,
 * so that a sub-page (and the "Loading..." box) lies over it. */
void App::build_stage()
{
    lv_obj_t *stage = lv_obj_create(shell_->root());
    lv_obj_remove_style_all(stage);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_CLICKABLE);                // the tab row under its top part must stay tappable
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_pos(stage, stage_x_, tabs_y_);
    lv_obj_set_size(stage, kScreenW, panel_h_);
    plat_.adopt_stage(stage);
    screen_ = stage;
}

/* The tab row: Chats, Contacts, Settings (Map and Terminal come back when they exist), ROW high, over the top of the stage. */
void App::build_tabs()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    tabs_ = make_box(shell_->root(), stage_x_, tabs_y_, kScreenW, tab_h_);
    lv_obj_remove_flag(tabs_, LV_OBJ_FLAG_CLICKABLE);
    static const Tab shown[3] = {Tab::Chats, Tab::Contacts, Tab::Settings};
    const int w = kScreenW / 3;
    const int lh = static_cast<int>(lv_font_get_line_height(f_body_));
    const int bar_h = std::max(3, m->tok[CP0_TOK_SPACE_S] / 2);
    for (int k = 0; k < 3; ++k) {
        const int i = static_cast<int>(shown[k]);
        const int bw = k == 2 ? kScreenW - 2 * w : w;
        lv_obj_t *b = make_box(tabs_, k * w, 0, bw, tab_h_);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(b, lv_color_hex(kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(kTagTab + i)));
        tab_btn_[i] = b;
        tab_label_[i] = make_label(b, f_body_, kMuted, 0, (tab_h_ - lh) / 2, bw, lh, LV_TEXT_ALIGN_LEFT);
        lv_obj_remove_flag(tab_label_[i], LV_OBJ_FLAG_CLICKABLE);
        tab_bar_line_[i] = make_box(b, m->tok[CP0_TOK_SPACE_M], tab_h_ - bar_h, bw - 2 * m->tok[CP0_TOK_SPACE_M], bar_h);
        lv_obj_set_style_bg_color(tab_bar_line_[i], lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(tab_bar_line_[i], LV_OPA_COVER, 0);
        set_hidden(tab_bar_line_[i], true);
    }
    // the unread total on the "Chats" tab: a pill with the number next to the word
    const int bh = static_cast<int>(lv_font_get_line_height(f_caption_)) + m->tok[CP0_TOK_SPACE_S];
    tab_badge_ = make_box(tab_btn_[0], 0, (tab_h_ - bh) / 2, bh, bh);
    lv_obj_remove_flag(tab_badge_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(tab_badge_, bh / 2, 0);
    lv_obj_set_style_bg_color(tab_badge_, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(tab_badge_, LV_OPA_COVER, 0);
    tab_badge_label_ = lv_label_create(tab_badge_);
    lv_obj_set_style_text_font(tab_badge_label_, f_caption_, 0);
    lv_obj_set_style_text_color(tab_badge_label_, lv_color_hex(kOnAccent), 0);
    lv_label_set_text(tab_badge_label_, "");
    lv_obj_center(tab_badge_label_);
    set_hidden(tab_badge_, true);
}

void App::build_screen()
{
    build_shell();
    build_tabs();
    build_stage();
    for (int i = 0; i < 5; ++i) page_[i] = make_box(screen_, 0, tab_h_, kScreenW, page_h_);
    build_chats();
    build_placeholder(&page_[static_cast<int>(Tab::Map)], "Map", "The map of the nodes with a position comes in phase 3.\nUntil then the Contacts tab shows the distances.");
    build_contacts();
    build_placeholder(&page_[static_cast<int>(Tab::Terminal)], "Terminal", "A meshcore-cli style command line comes in a later version.");
    build_settings();
    build_hint_line();
    build_popup();
    layout_body();
    update_title();
}

void App::build_placeholder(lv_obj_t **page, const char *title, const char *text)
{
    lv_obj_t *t = make_label(*page, f_big_, kText, 20, 150, 600, 28, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(t, title);
    lv_obj_t *l = lv_label_create(*page);
    lv_obj_set_style_text_font(l, f_ui_, 0);
    set_color(l, kMuted);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 520);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, 60, 190);
}

void App::build_chats()
{
    lv_obj_t *p = page_[static_cast<int>(Tab::Chats)];
    RowList::Config cfg;
    cfg.max_cells = 3;
    cfg.max_boxes = 0;
    cfg.min_row_h = 26;
    chat_list_.create(p, 0, 0, kListW, page_h_ - 44, cfg);
    search_btn_ = make_button(p, 4, page_h_ - 44, kListW - 8, 44, "Search messages", kTagSearchOpen);
    chat_div_ = make_box(p, kListW, 0, 1, page_h_);
    lv_obj_set_style_bg_color(chat_div_, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(chat_div_, LV_OPA_COVER, 0);

    const int rx = kListW + 1;                    // the right pane
    // the head of the conversation: its name with a line under it (the count left to type, muted, the nodes heard), and at
    // the right the buttons: Details (a direct chat) and Options (the 3 s hold on the name and Ctrl+O stay as shortcuts)
    chat_title_ = make_label(p, f_text_, kText, rx + 10, 3, 200, 22);
    chat_info_ = make_label(p, f_small_, kMuted, rx + 10, 25, 200, 18);
    options_btn_ = make_button(p, kScreenW - 104, 0, 98, 44, "Options", kTagChatOptions);
    set_hidden(options_btn_, true);
    details_btn_ = make_button(p, kScreenW - 206, 0, 98, 44, "Details", kTagChatDetails);
    set_hidden(details_btn_, true);
    // the name of the open conversation: hold it for 3 s to open the options (a tap does nothing)
    title_hold_ = make_box(p, rx, 0, 240, 44);
    lv_obj_add_flag(title_hold_, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t code : {LV_EVENT_CLICKED, LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(title_hold_, trampoline_title, code, nullptr);
    set_hidden(title_hold_, true);
    lv_obj_t *line = make_box(p, rx, 44, kScreenW - rx, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    msgs_ = lv_obj_create(p);
    lv_obj_remove_style_all(msgs_);
    lv_obj_set_pos(msgs_, rx, 46);
    lv_obj_set_size(msgs_, kScreenW - rx, page_h_ - 104);
    lv_obj_set_flex_flow(msgs_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(msgs_, LV_DIR_VER);
    lv_obj_remove_flag(msgs_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_all(msgs_, 6, 0);
    lv_obj_set_style_pad_row(msgs_, 4, 0);
    lv_obj_set_scrollbar_mode(msgs_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(msgs_, lv_color_hex(kMuted), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(msgs_, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(msgs_, 4, LV_PART_SCROLLBAR);

    chat_line2_ = make_box(p, rx, page_h_ - 56, kScreenW - rx, 1);
    lv_obj_set_style_bg_color(chat_line2_, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(chat_line2_, LV_OPA_COVER, 0);

    compose_ = lv_textarea_create(p);
    lv_obj_set_pos(compose_, rx + 6, page_h_ - 50);
    lv_obj_set_size(compose_, kScreenW - rx - 6 - 96, 46);
    lv_textarea_set_one_line(compose_, false);
    lv_obj_set_scrollbar_mode(compose_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(compose_, f_text_, 0);
    lv_obj_set_style_text_color(compose_, lv_color_hex(kText), 0);
    lv_obj_set_style_bg_color(compose_, lv_color_hex(kPanel), 0);
    lv_obj_set_style_bg_opa(compose_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(compose_, 6, 0);
    lv_obj_set_style_border_width(compose_, 2, 0);
    lv_obj_set_style_border_color(compose_, lv_color_hex(kSelected), 0);
    lv_obj_set_style_pad_all(compose_, 6, 0);
    lv_obj_set_style_text_color(compose_, lv_color_hex(kMuted), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_opa(compose_, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_side(compose_, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(compose_, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(compose_, lv_color_hex(kAccent), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_max_length(compose_, 400);
    lv_obj_add_event_cb(compose_, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(kTagCompose)));
    send_btn_ = make_button(p, kScreenW - 90, page_h_ - 47, 84, 40, "Send", kTagSend, &send_label_);
    lv_obj_set_style_bg_color(send_btn_, lv_color_hex(kOkGreen), 0);
    build_search();
}

void App::build_contacts()
{
    lv_obj_t *p = page_[static_cast<int>(Tab::Contacts)];
    // the toolbar (every button 44 px high): normal, and the one of the select mode
    tb_normal_ = make_box(p, 0, 0, 640, 44);
    make_button(tb_normal_, 4, 0, 72, 44, "Advert", kTagAdvertFlood);
    make_button(tb_normal_, 80, 0, 84, 44, "Zero-hop", kTagAdvertZero);
    make_button(tb_normal_, 168, 0, 72, 44, "Refresh", kTagRefresh);
    nearby_btn_ = make_button(tb_normal_, 244, 0, 88, 44, "Nearby", kTagNearby, &nearby_btn_label_);
    make_button(tb_normal_, 336, 0, 72, 44, "Select", kTagSelect);
    make_button(tb_normal_, 412, 0, 128, 44, "Group: All", kTagGroup, &group_btn_label_);
    make_button(tb_normal_, 544, 0, 92, 44, "Sort", kTagSort, &sort_label_);
    tb_select_ = make_box(p, 0, 0, 640, 44);
    make_button(tb_select_, 4, 0, 76, 44, "Done", kTagSelDone);
    make_button(tb_select_, 84, 0, 104, 44, "Select...", kTagSelMenu);
    del_btn_ = make_button(tb_select_, 192, 0, 132, 44, "Delete", kTagSelDelete, &del_btn_label_);
    make_button(tb_select_, 328, 0, 104, 44, "Group...", kTagSelGroup);
    sel_count_ = make_label(tb_select_, f_small_, kMuted, 436, 12, 196, 20, LV_TEXT_ALIGN_RIGHT);
    set_hidden(tb_select_, true);

    // the filters (C2): node type, and time since the node was last heard. Each chip is a 44 px high touch target.
    static const int type_x[5] = {8, 66, 130, 222, 288}, type_w[5] = {54, 60, 88, 62, 76};
    static const int age_x[4] = {380, 460, 514, 574}, age_w[4] = {76, 50, 56, 50};
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *c = make_box(p, type_x[i], 44, type_w[i] + 4, 44);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(kTagTypeChip + i)));
        lv_obj_t *pill = make_box(c, 0, 5, type_w[i], 34);
        lv_obj_set_style_radius(pill, 17, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(kPressed), LV_STATE_PRESSED);
        type_chip_[i] = pill;
        type_chip_label_[i] = make_label(pill, f_small_, kMuted, 2, 8, type_w[i] - 4, 18, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(type_chip_label_[i], type_filter_name(static_cast<TypeFilter>(i)));
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *c = make_box(p, age_x[i], 44, age_w[i] + 4, 44);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(kTagAgeChip + i)));
        lv_obj_t *pill = make_box(c, 0, 5, age_w[i], 34);
        lv_obj_set_style_radius(pill, 17, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        age_chip_[i] = pill;
        age_chip_label_[i] = make_label(pill, f_small_, kMuted, 2, 8, age_w[i] - 4, 18, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(age_chip_label_[i], age_filter_name(static_cast<AgeFilter>(i)));
    }
    lv_obj_t *sep = make_box(p, 372, 52, 1, 28);
    lv_obj_set_style_bg_color(sep, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    static const char *const names[6] = {"Name", "Type", "Heard", "SNR", "Hops", "Distance"};
    static const int xs[6] = {12, 212, 292, 360, 432, 484};
    static const int ws[6] = {196, 76, 66, 70, 48, 100};
    lv_obj_t *hdr = make_box(p, 0, 88, 640, 22);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(kPanel), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    for (int i = 0; i < 6; ++i) {
        lv_obj_t *b = make_box(hdr, xs[i] - 6, 0, ws[i] + 6, 22);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(kTagHdr + i)));
        col_hdr_[i] = make_label(b, f_small_, kMuted, 6, 2, ws[i], 18);
        lv_obj_remove_flag(col_hdr_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(col_hdr_[i], names[i]);
    }
    contacts_count_ = make_label(hdr, f_small_, kMuted, 80, 2, 124, 18, LV_TEXT_ALIGN_RIGHT);     // "12 of 161", over the Name title (not clickable)
    RowList::Config cfg;
    cfg.max_cells = 8;
    cfg.max_boxes = 2;
    cfg.min_row_h = kContactRowH;
    contact_list_.create(p, 0, 110, 640, page_h_ - 110, cfg);

    // the detail panel of a contact (C8): a sub-page over the table and the tab row, static facts and large buttons
    detail_ = make_box(screen_, 0, 0, 640, panel_h_);
    lv_obj_set_style_bg_color(detail_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(detail_, LV_OPA_COVER, 0);
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_CLICKABLE);                  // swallows taps meant for the table below
    detail_title_ = make_label(detail_, f_big_, kText, 16, 6, 600, 26);
    static const char *const keys[10] = {"Name", "Type", "Groups", "Public key", "Route", "Last advert", "Heard here", "Position", "Distance", "Last SNR"};
    static const int ys[10] = {38, 64, 90, 116, 164, 190, 216, 242, 268, 294};
    for (int i = 0; i < 10; ++i) {
        lv_obj_t *k = make_label(detail_, f_small_, kMuted, 16, ys[i] + 4, 110, 20);
        lv_label_set_text(k, keys[i]);
        detail_val_[i] = make_label(detail_, i == 3 ? f_text_small_ : f_text_, kText, 132, ys[i], 496, i == 3 ? 44 : 24);
        if (i == 3 || i == 4) lv_label_set_long_mode(detail_val_[i], LV_LABEL_LONG_MODE_WRAP);
    }
    detail_msg_btn_ = detail_btn_[0] = make_button(detail_, 8, 352, 148, 44, "Message", kTagDetailMsg);
    detail_btn_[1] = make_button(detail_, 160, 352, 148, 44, "Groups", kTagDetailGroups);
    detail_btn_[2] = make_button(detail_, 312, 352, 148, 44, "Delete", kTagDetailDelete);
    lv_obj_set_style_bg_color(detail_btn_[2], lv_color_hex(kDangerRed), 0);
    detail_btn_[3] = make_button(detail_, 464, 352, 164, 44, "Close", kTagDetailClose);
    set_hidden(detail_, true);
    build_nearby();
}

/* The Nearby list (C4): adverts heard, pending contacts of the manual add mode and the answers to a zero-hop scan, with Add / Ignore. */
void App::build_nearby()
{
    nearby_ = make_box(screen_, 0, 0, 640, panel_h_);
    lv_obj_set_style_bg_color(nearby_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(nearby_, LV_OPA_COVER, 0);
    lv_obj_add_flag(nearby_, LV_OBJ_FLAG_CLICKABLE);
    nearby_title_ = make_label(nearby_, f_big_, kText, 12, 10, 330, 26);
    make_button(nearby_, 346, 0, 92, 44, "Scan", kTagNearbyScan, &nearby_scan_label_);
    make_button(nearby_, 442, 0, 96, 44, "Ignored", kTagNearbyIgnored, &nearby_ign_label_);
    make_button(nearby_, 542, 0, 94, 44, "Close", kTagNearbyClose);
    RowList::Config cfg;
    cfg.max_cells = 6;
    cfg.max_boxes = 2;
    cfg.min_row_h = 56;
    nearby_list_.create(nearby_, 0, 48, 640, panel_h_ - 48, cfg);
    nearby_empty_ = make_label(nearby_, f_ui_, kMuted, 30, 150, 580, 100, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(nearby_empty_, LV_LABEL_LONG_MODE_WRAP);
    set_hidden(nearby_, true);
}

/* Search in the message archive (M7): a sub-page over the Chats page and the tab row. */
void App::build_search()
{
    search_panel_ = make_box(screen_, 0, 0, 640, panel_h_);
    lv_obj_set_style_bg_color(search_panel_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(search_panel_, LV_OPA_COVER, 0);
    lv_obj_add_flag(search_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = make_label(search_panel_, f_big_, kText, 12, 10, 400, 26);
    lv_label_set_text(t, "Search messages");
    make_button(search_panel_, 540, 0, 96, 44, "Close", kTagSearchClose);
    sr_query_ = lv_textarea_create(search_panel_);
    lv_obj_set_pos(sr_query_, 8, 48);
    lv_obj_set_size(sr_query_, 624, 44);
    lv_textarea_set_one_line(sr_query_, true);
    lv_obj_set_scrollbar_mode(sr_query_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(sr_query_, f_text_, 0);
    lv_obj_set_style_text_color(sr_query_, lv_color_hex(kText), 0);
    lv_obj_set_style_bg_color(sr_query_, lv_color_hex(kPanel), 0);
    lv_obj_set_style_bg_opa(sr_query_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sr_query_, 6, 0);
    lv_obj_set_style_border_width(sr_query_, 2, 0);
    lv_obj_set_style_border_color(sr_query_, lv_color_hex(kAccent), 0);
    lv_obj_set_style_pad_all(sr_query_, 8, 0);
    lv_obj_set_style_text_color(sr_query_, lv_color_hex(kMuted), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_opa(sr_query_, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_side(sr_query_, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(sr_query_, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(sr_query_, lv_color_hex(kAccent), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_max_length(sr_query_, 60);
    lv_obj_add_state(sr_query_, LV_STATE_FOCUSED);
    lv_textarea_set_placeholder_text(sr_query_, "Type the words to find (Bluetooth keyboard)");
    make_button(search_panel_, 8, 96, 200, 44, "", kTagSearchConv, &sr_conv_label_);
    make_button(search_panel_, 212, 96, 232, 44, "", kTagSearchDate, &sr_date_label_);
    make_button(search_panel_, 448, 96, 184, 44, "", kTagSearchDir, &sr_dir_label_);
    sr_count_ = make_label(search_panel_, f_small_, kMuted, 12, 146, 616, 18);
    RowList::Config cfg;
    cfg.max_cells = 3;
    cfg.max_boxes = 0;
    cfg.min_row_h = 56;
    search_list_.create(search_panel_, 0, 166, 640, panel_h_ - 166, cfg);
    set_hidden(search_panel_, true);
}

/* The statistics screen (D9): a sub-page over the Settings page and the tab row. */
void App::build_stats()
{
    stats_panel_ = make_box(screen_, 0, 0, 640, panel_h_);
    lv_obj_set_style_bg_color(stats_panel_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(stats_panel_, LV_OPA_COVER, 0);
    lv_obj_add_flag(stats_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = make_label(stats_panel_, f_big_, kText, 12, 10, 290, 26);
    lv_label_set_text(t, "Statistics");
    make_button(stats_panel_, 304, 0, 104, 44, "Refresh", kTagStatsRefresh);
    make_button(stats_panel_, 412, 0, 120, 44, "", kTagStatsAuto, &st_auto_label_);
    make_button(stats_panel_, 540, 0, 96, 44, "Close", kTagStatsClose);
    for (int i = 0; i < 10; ++i) {
        st_label_[0][i] = make_label(stats_panel_, f_ui_, kMuted, 12, 56 + i * 28, 130, 22);
        st_value_[0][i] = make_label(stats_panel_, f_text_, kText, 146, 56 + i * 28, 176, 22);
        st_label_[1][i] = make_label(stats_panel_, f_ui_, kMuted, 336, 56 + i * 28, 150, 22);
        st_value_[1][i] = make_label(stats_panel_, f_text_, kText, 490, 56 + i * 28, 140, 22);
    }
    st_updated_ = make_label(stats_panel_, f_small_, kMuted, 12, panel_h_ - 26, 400, 18);
    st_note_ = make_label(stats_panel_, f_small_, kMuted, 336, panel_h_ - 26, 296, 18, LV_TEXT_ALIGN_RIGHT);
    set_hidden(stats_panel_, true);
}

void App::build_settings()
{
    RowList::Config cfg;
    cfg.max_cells = 6;
    cfg.max_boxes = 3;
    cfg.min_row_h = 28;
    settings_list_.create(page_[static_cast<int>(Tab::Settings)], 0, 0, 640, page_h_, cfg);
    build_stats();
    build_packet_log();
}

namespace {
// the columns of the packet log (x, width): time, route, type, hops, SNR, RSSI, payload size, channel
constexpr int kLogCol[8][2] = {{12, 66}, {80, 70}, {152, 90}, {244, 42}, {288, 50}, {340, 44}, {386, 40}, {428, 204}};
constexpr int kLogRowH = 28;
} // namespace

/* The packet log (D8): a sub-page over the Settings page, like the statistics. The rows on top, the bytes of the selected row below. */
void App::build_packet_log()
{
    log_panel_ = make_box(screen_, 0, 0, 640, panel_h_);
    lv_obj_set_style_bg_color(log_panel_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(log_panel_, LV_OPA_COVER, 0);
    lv_obj_add_flag(log_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = make_label(log_panel_, f_big_, kText, 12, 10, 130, 26);
    lv_label_set_text(t, "Packet log");
    log_status_ = make_label(log_panel_, f_small_, kMuted, 144, 14, 222, 18);
    log_toggle_ = make_button(log_panel_, 370, 0, 92, 44, "", kTagLogToggle, &log_toggle_label_);
    make_button(log_panel_, 466, 0, 80, 44, "Clear", kTagLogClear);
    make_button(log_panel_, 550, 0, 86, 44, "Close", kTagLogClose);
    static const char *const titles[8] = {"Time", "Route", "Type", "Hops", "SNR", "RSSI", "Size", "Channel"};
    for (int i = 0; i < 8; ++i) {
        lv_obj_t *h = make_label(log_panel_, f_small_, kMuted, kLogCol[i][0], 50, kLogCol[i][1], 18);
        lv_label_set_text(h, titles[i]);
    }
    RowList::Config cfg;
    cfg.max_cells = 8;
    cfg.max_boxes = 0;
    cfg.min_row_h = kLogRowH;
    log_list_.create(log_panel_, 0, 70, 640, panel_h_ - 184, cfg);
    log_div_ = make_box(log_panel_, 0, panel_h_ - 112, 640, 1);
    lv_obj_set_style_bg_color(log_div_, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(log_div_, LV_OPA_COVER, 0);
    log_detail_ = make_label(log_panel_, f_small_, kMuted, 12, panel_h_ - 106, 616, 18);
    log_hex_ = make_label(log_panel_, f_text_small_, kText, 12, panel_h_ - 86, 616, 85);     // up to 5 lines; the dots mode cuts a longer packet
    set_hidden(log_panel_, true);
}

/* The key hint line at the bottom of the screen: shown only while a keyboard is there (the touch has its buttons). */
void App::build_hint_line()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    hint_box_ = make_box(shell_->root(), stage_x_, layout_.h - hint_h_, kScreenW, hint_h_);
    lv_obj_remove_flag(hint_box_, LV_OBJ_FLAG_CLICKABLE);
    const int lh = static_cast<int>(lv_font_get_line_height(f_caption_));
    hint_label_ = make_label(hint_box_, f_caption_, kMuted, m->tok[CP0_TOK_SPACE_S], (hint_h_ - lh) / 2, kScreenW - 2 * m->tok[CP0_TOK_SPACE_S], lh);

    // the cue of a touch held on a conversation name: a bar that fills over 3 s (never clickable: it must not take the touch)
    hold_box_ = make_box(screen_, 215, panel_h_ - 114, 420, 56);
    lv_obj_remove_flag(hold_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(hold_box_, lv_color_hex(kPanel), 0);
    lv_obj_set_style_bg_opa(hold_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hold_box_, 8, 0);
    lv_obj_set_style_border_width(hold_box_, 2, 0);
    lv_obj_set_style_border_color(hold_box_, lv_color_hex(kAccent), 0);
    hold_label_ = make_label(hold_box_, f_small_, kText, 12, 8, 392, 18, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *track = make_box(hold_box_, 12, 34, 392, 10);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(track, lv_color_hex(kSelected), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 5, 0);
    hold_bar_ = make_box(track, 0, 0, 1, 10);
    lv_obj_remove_flag(hold_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(hold_bar_, lv_color_hex(kAccent), 0);
    lv_obj_set_style_bg_opa(hold_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hold_bar_, 5, 0);
    set_hidden(hold_box_, true);
}

/* What depends on the room under the shell: the stage and the page bodies take the height left by the shell and, while
 * a keyboard is there, by the key hint line. The lists, the entry line of Chats, the buttons at the bottom of a sub-page. */
void App::layout_body()
{
    const bool hint = kbd_ok_;
    panel_h_ = layout_.h - tabs_y_ - (hint ? hint_h_ : 0);
    page_h_ = panel_h_ - tab_h_;
    lv_obj_set_pos(screen_, stage_x_, tabs_y_);
    lv_obj_set_size(screen_, kScreenW, panel_h_);
    for (int i = 0; i < 5; ++i) lv_obj_set_pos(page_[i], 0, tab_h_), lv_obj_set_size(page_[i], kScreenW, page_h_);
    // Chats: the list and its Search button, the conversation and the entry line at the bottom
    chat_list_.resize(page_h_ - 44);
    lv_obj_set_y(search_btn_, page_h_ - 44);
    lv_obj_set_height(chat_div_, page_h_);
    lv_obj_set_height(msgs_, page_h_ - 104);
    lv_obj_set_y(chat_line2_, page_h_ - 56);
    lv_obj_set_y(compose_, page_h_ - 50);
    lv_obj_set_y(send_btn_, page_h_ - 47);
    contact_list_.resize(page_h_ - 110);
    settings_list_.resize(page_h_);
    // the sub-pages cover the tab row too
    for (lv_obj_t *panel : {search_panel_, nearby_, detail_, stats_panel_, log_panel_}) lv_obj_set_size(panel, kScreenW, panel_h_);
    search_list_.resize(panel_h_ - 166);
    nearby_list_.resize(panel_h_ - 48);
    log_list_.resize(panel_h_ - 184);
    lv_obj_set_y(log_div_, panel_h_ - 112);
    lv_obj_set_y(log_detail_, panel_h_ - 106);
    lv_obj_set_y(log_hex_, panel_h_ - 86);
    lv_obj_set_height(log_hex_, 85);
    lv_obj_set_y(st_updated_, panel_h_ - 26);
    lv_obj_set_y(st_note_, panel_h_ - 26);
    for (lv_obj_t *b : detail_btn_) lv_obj_set_y(b, panel_h_ - 48);
    lv_obj_set_y(hold_box_, panel_h_ - 114);
    lv_obj_set_size(loading_, kScreenW, panel_h_);
    set_hidden(hint_box_, !hint);
    lv_obj_set_pos(hint_box_, stage_x_, layout_.h - hint_h_);
    invalidate();
}

lv_point_t App::script_origin() const
{
    lv_area_t a;
    lv_obj_get_coords(screen_, &a);
    const bool sub = nav_.search_open || nav_.nearby_open || nav_.detail_open || nav_.stats_open || nav_.packet_log_open;
    return {a.x1, a.y1 + (sub ? -56 : tab_h_ - 56)};
}

void App::build_popup()
{
    // "Loading..." while the contacts are sorted or filtered: a small box in the middle of the stage
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    loading_ = make_box(screen_, 0, 0, kScreenW, panel_h_);
    lv_obj_set_style_bg_color(loading_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(loading_, LV_OPA_50, 0);
    lv_obj_add_flag(loading_, LV_OBJ_FLAG_CLICKABLE);
    const int lh = static_cast<int>(lv_font_get_line_height(f_title_));
    lv_point_t sz = {0, 0};
    lv_text_get_size(&sz, "Loading...", f_title_, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int pad = m->tok[CP0_TOK_SPACE_L];
    lv_obj_t *lp = make_box(loading_, 0, 0, sz.x + 2 * pad, lh + 2 * pad);
    lv_obj_align(lp, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(lp, lv_color_hex(kPanel), 0);
    lv_obj_set_style_bg_opa(lp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lp, m->tok[CP0_TOK_RADIUS_M], 0);
    lv_obj_set_style_border_width(lp, 1, 0);
    lv_obj_set_style_border_color(lp, lv_color_hex(CP0_UI_OSK_BORDER_RGB), 0);
    loading_label_ = make_label(lp, f_title_, kText, pad, pad, sz.x + 2, lh, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(loading_label_, "Loading...");
    set_hidden(loading_, true);
}

std::string App::debug_state() const
{
    const char *compose = compose_ ? lv_textarea_get_text(compose_) : "";
    const char *editor = ed_ta_ && editor_open_ ? lv_textarea_get_text(ed_ta_) : "";
    const char *pk = pop_.kind == Popup::Choice ? "choice" : pop_.kind == Popup::Confirm ? "confirm" : pop_.kind == Popup::Menu ? "menu"
                     : pop_.kind == Popup::Info ? "info" : pop_.kind == Popup::List ? "list" : pop_.kind == Popup::Progress ? "progress" : "none";
    std::string last_status;
    if (!conv_.empty()) {
        const auto msgs = model_.messages(conv_);
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
            if ((*it)->dir == Dir::Out) {                  // the status of the last message of ours
                last_status = message_status(**it, conv_.rfind("c:", 0) == 0).text;
                break;
            }
    }
    std::string s = std::string("state tab=") + tab_name(nav_.tab) + " link=" + client_->link_text() + " conv=" + conv_ +
                    " msgs=" + std::to_string(conv_.empty() ? 0 : model_.messages(conv_).size()) + " unread=" + std::to_string(model_.unread_total()) +
                    " contacts=" + std::to_string(model_.contact_count()) + " chat_rows=" + std::to_string(chat_rows_.size()) +
                    " chat_sel=" + std::to_string(chat_sel_) + " compose_focus=" + (nav_.compose_focus ? "1" : "0") + " compose='" + compose + "'" +
                    " editor=" + (editor_open_ ? "1" : "0") + " editor_text='" + editor + "'" + " detail=" + (nav_.detail_open ? "1" : "0") +
                    " contact_sel=" + std::to_string(contact_sel_) + " sort=" + sort_name(cview_.sort) + (cview_.reverse ? "(rev)" : "") +
                    " shown=" + std::to_string(csnap_.rows.size()) + " first='" + (csnap_.rows.empty() ? std::string() : csnap_.rows[0].name) + "'" +
                    " sel_key=" + contact_sel_key_.substr(0, 4) + " filter=" + type_filter_name(cview_.filter.type) + "/" + age_filter_name(cview_.filter.age) +
                    " loading=" + (view_pending_ ? "1" : "0") + " popup=" + pk + " popup_value='" + (pop_.kind == Popup::Choice ? pop_.choice.label() : std::string()) + "'" +
                    " settings_sel=" + std::to_string(settings_sel_) + " radio_dirty=" + (dirty_radio_ ? "1" : "0") + " freq=" + fmt_freq(edit_.freq_mhz) +
                    " bw=" + fmt_bw(edit_.bw_khz) + " sf=" + std::to_string(edit_.sf) + " cr=" + std::to_string(edit_.cr) + " name='" + edit_.name + "'" +
                    " conv_title='" + (conv_.empty() ? std::string() : model_.conv_title(conv_)) + "'" +
                    " detail_name='" + (nav_.detail_open && model_.find_contact(detail_key_) ? model_.find_contact(detail_key_)->c.name : std::string()) + "'" +
                    " last_status='" + last_status + "'" +
                    " retry=" + std::to_string(model_.retry().attempts) + "/" + std::to_string(model_.retry().reset_after) +
                    " live_rows=" + std::to_string(contact_list_.bound_rows()) + " notice='" + notice_text_ + "'" +
                    " presets=" + (presets_origin() == PresetOrigin::Fetched ? "fetched" : presets_origin() == PresetOrigin::Cached ? "cached" : "bundled") + "/" +
                    std::to_string(radio_presets().size()) + "/" + presets_list_date() + " all_msgs=" + std::to_string(model_.message_count()) +
                    " popup_title='" + (pop_.kind == Popup::None ? std::string() : pop_.title) + "'" + " muted_conv=" + (conv_.empty() ? "-" : model_.is_muted(conv_) ? "1" : "0");
    // phase 2
    const DeviceInfo *dev = model_.device() ? &*model_.device() : nullptr;
    s += std::string(" select=") + (nav_.select_mode ? "1" : "0") + " selected=" + std::to_string(csel_.size()) + " group='" + cview_.group + "'" +
         " nearby_open=" + (nav_.nearby_open ? "1" : "0") + " nearby=" + std::to_string(nearby_rows_.size()) + "/" + std::to_string(model_.nearby_count()) +
         " pending=" + std::to_string(model_.pending_count()) + " ignored=" + std::to_string(model_.ignored().size()) +
         " search_open=" + (nav_.search_open ? "1" : "0") + " search_hits=" + std::to_string(search_rows_.size()) + " search_text='" + search_.text + "'" +
         " stats_open=" + (nav_.stats_open ? "1" : "0") + " stats=" + (model_.stats().core ? "core" : "-") + (model_.stats().radio ? "+radio" : "") + (model_.stats().packets ? "+packets" : "") +
         " path_hash=" + (dev ? std::to_string(dev->path_hash_mode) : std::string("-")) + " repeat=" + (dev && dev->has_repeat ? (dev->repeat ? "on" : "off") : "-") +
         " manual_add=" + (model_.self() ? (model_.self()->manual_add_contacts ? "1" : "0") : "-") +
         " autoadd=" + (model_.caps().autoadd == Cap::Yes ? std::to_string(model_.caps().autoadd_config.config) : std::string("-")) +
         " advert=" + std::to_string(model_.advert_schedule().interval_hours) + (model_.advert_schedule().flood ? "h/flood" : "h/zero") +
         " groups=" + std::to_string(model_.groups().groups().size()) + " bulk=" + (client_->bulk().active ? "active" : "idle") + "/" + std::to_string(client_->bulk().done) + "/" + std::to_string(client_->bulk().failed) +
         " scan=" + (client_->discover().active ? "active" : "idle") + "/" + std::to_string(client_->discover().found) + " focus_seq=" + std::to_string(focus_seq_) +
         " board=" + (model_.board_known() ? model_.board_key().substr(0, 12) : std::string("-")) + " epoch=" + std::to_string(model_.board_epoch()) +
         " board_dir=" + store_->board_id() + " others=" + std::to_string(other_boards_n_) +
         " reset=" + (client_->reset_phase() == Client::ResetPhase::None ? "-" : client_->reset_phase() == Client::ResetPhase::Waiting ? "waiting"
                      : client_->reset_phase() == Client::ResetPhase::Done ? "done" : "kept-keys");
    // the Position box
    const bool pos_box = editor_open_ && edit_kind_ == EditKind::Position;
    s += std::string(" pos_box=") + (pos_box ? "1" : "0") + " pos_focus=" + std::to_string(pos_box ? pos_focus_ : -1) +
         " pos_fields='" + (pos_box ? std::string(lv_textarea_get_text(pos_ta_[0])) + "|" + lv_textarea_get_text(pos_ta_[1]) : std::string()) + "'" +
         " pos_gps_row=" + (pos_box && pos_gps_shown_ ? "1" : "0") + " pos_err='" + (pos_box ? pos_err_text_ : std::string()) + "'" +
         " self_pos='" + (model_.self() ? fmt_position(position_is_set(model_.self()->lat, model_.self()->lon), model_.self()->lat, model_.self()->lon) : std::string("-")) + "'" +
         " gps=" + (model_.caps().gps_listed ? (model_.caps().gps_on ? "on" : "off") : "-");
    // phase 3: heard back, the packet log
    {
        const PacketLog &pl = client_->packet_log();
        const int64_t first = static_cast<int64_t>(pl.dropped());
        const bool sel_ok = log_sel_abs_ >= first && log_sel_abs_ < first + static_cast<int64_t>(pl.size());
        const LoggedPacket *sel = pl.size() == 0 ? nullptr : sel_ok ? &pl.at(static_cast<size_t>(log_sel_abs_ - first)) : &pl.at(pl.size() - 1);
        const PacketRow pr = sel ? build_packet_row(model_, *sel) : PacketRow();
        s += std::string(" echoes=") + std::to_string(client_->echoes_open()) + " log_open=" + (nav_.packet_log_open ? "1" : "0") +
             " log_capture=" + (pl.capturing() ? "1" : "0") + " log_n=" + std::to_string(pl.size()) + " log_sel=" + std::to_string(log_sel_abs_) +
             " log_row='" + (sel ? pr.route + " " + pr.type + " " + pr.hops + " " + pr.size + " " + pr.channel : std::string()) + "'";
    }
    return s;
}

/* ================================================================== small helpers */

void App::notice(const std::string &text, bool ok)
{
    notice_text_ = text;
    notice_ok_ = ok;
    notice_ms_ = mono_ms();
    invalidate();
}

bool App::keyboard_ready() const
{
    return plat_.keyboard_connected() || keyboard_present();
}

void App::select_tab(Tab t)
{
    if (editor_open_ || nav_.popup_open) return;
    nav_.tab = t;
    nav_.detail_open = false;
    set_hidden(detail_, true);
    detail_key_.clear();
    // every layer over a page closes with the tab (the search, the Nearby list, the statistics, the select mode)
    if (nav_.search_open) close_search();
    if (nav_.stats_open) close_stats();
    if (nav_.packet_log_open) close_packet_log();
    if (nav_.nearby_open) close_nearby();
    if (nav_.select_mode) leave_select_mode();
    if (t == Tab::Settings && !dirty_radio_) edit_ = model_.radio_settings();
    if (t == Tab::Settings && client_->ready() && mono_ms() - self_refresh_ms_ > 20000) {     // the board position and settings as the board holds them now
        self_refresh_ms_ = mono_ms();
        client_->refresh_self();
    }
    invalidate();
    set_page_visible();
}

void App::set_page_visible()
{
    for (int i = 0; i < 5; ++i) set_hidden(page_[i], i != static_cast<int>(nav_.tab));
}

/* The page title of the header: "Mesh Hop > Chats", and one more part inside a sub-page (a contact, Nearby, Search...). */
void App::update_title()
{
    std::vector<std::string> path = {"Mesh Hop"};
    switch (nav_.tab) {
    case Tab::Chats:
        path.push_back("Chats");
        if (nav_.search_open) path.push_back("Search");
        break;
    case Tab::Contacts:
        path.push_back("Contacts");
        if (nav_.detail_open) {
            const ContactRec *r = model_.find_contact(detail_key_);
            path.push_back(r ? truncate_ellipsis(r->c.name, 24) : std::string("Contact"));
        } else if (nav_.nearby_open) {
            path.push_back("Nearby");
        }
        break;
    case Tab::Settings:
        path.push_back("Settings");
        if (nav_.stats_open) path.push_back("Statistics");
        else if (nav_.packet_log_open) path.push_back("Packet log");
        break;
    default: path.push_back(tab_name(nav_.tab)); break;
    }
    std::string key;
    for (const std::string &s : path) key += s + "\x1f";
    if (key == title_key_) return;
    title_key_ = key;
    shell_->set_title(path);
}

int App::max_compose_bytes() const
{
    return conv_.empty() ? 0 : static_cast<int>(client_->max_text(conv_));
}

void App::toggle_mute(const std::string &conv)
{
    if (conv.empty()) return;
    const bool now_muted = !model_.is_muted(conv);
    model_.set_muted(conv, now_muted);
    notice(model_.conv_title(conv) + (now_muted ? " muted: no unread count" : " unmuted"), true);
}

/* ---- options of a conversation, deleting history (phase 1b) */

void App::open_conversation_options(const std::string &conv)
{
    if (conv.size() < 3 || editor_open_ || nav_.popup_open) return;
    if (conv.rfind("c:", 0) == 0) {
        const ChannelRec *c = model_.find_channel(std::atoi(conv.c_str() + 2));
        if (!c || c->empty) return;
    } else if (conv.rfind("d:", 0) != 0) {
        return;
    }
    const ConvOptions o = conversation_options(model_, conv);
    open_menu(o.title, o.labels, [this, conv, o](int i) {
        if (i < 0 || i >= static_cast<int>(o.actions.size())) return;
        switch (o.actions[static_cast<size_t>(i)]) {
        case ConvAction::Mute:
        case ConvAction::Unmute: toggle_mute(conv); break;
        case ConvAction::DeleteConversation:
        case ConvAction::DeleteMessages: confirm_delete_conversation(conv); break;
        }
    });
}

void App::after_deletion()
{
    // a direct conversation without messages leaves the left pane (it was only listed while open); a channel stays
    if (conv_.rfind("d:", 0) == 0 && model_.message_count_in(conv_) == 0) {
        if (chat_sel_key_ == conv_) chat_sel_key_.clear();
        conv_.clear();
        nav_.compose_focus = false;
        lv_textarea_set_text(compose_, "");
    }
    built_conv_.clear();
    built_sig_ = 0;
    chat_sig_ = 0;
    invalidate();
}

/* Another board (another public key) is connected, or the first one was recognised: the model already holds that board's own data. What the UI
 * kept for the previous board (open conversation, search, selection, group filter, an open box) must not leak into the new one. */
void App::on_board_changed()
{
    seen_epoch_ = model_.board_epoch();
    if (log_) log_->line("board identity: " + (model_.board_known() ? model_.board_key().substr(0, 12) : std::string("none")) + ", the conversations, contacts, groups and flags were reloaded");
    if (editor_open_) close_editor(false);
    if (nav_.popup_open || pop_.kind != Popup::None) close_popup();
    if (nav_.search_open) close_search();
    if (nav_.stats_open) close_stats();
    if (nav_.packet_log_open) close_packet_log();
    if (nav_.nearby_open) close_nearby();
    if (nav_.select_mode) leave_select_mode();
    nav_.detail_open = false;
    set_hidden(detail_, true);
    detail_key_.clear();
    csel_.clear();
    cview_.group.clear();
    csnap_valid_ = false;
    contact_sel_key_.clear();
    contact_sel_ = 0;
    conv_.clear();
    chat_sel_key_.clear();
    chat_sel_ = -1;
    nav_.compose_focus = false;
    if (compose_) lv_textarea_set_text(compose_, "");
    focus_conv_.clear();
    focus_seq_ = 0;
    search_ = SearchState();
    search_rows_.clear();
    built_conv_.clear();
    built_sig_ = 0;
    chat_sig_ = 0;
    settings_sig_ = 0;
    nearby_sig_ = 0;
    dirty_radio_ = false;
    edit_ = model_.radio_settings();
    refresh_other_boards(true);
    invalidate();
}

void App::refresh_other_boards(bool force)
{
    const uint64_t now = mono_ms();
    if (!force && other_cache_ms_ != 0 && now - other_cache_ms_ < 5000) return;
    other_cache_ms_ = now;
    other_boards_n_ = 0;
    other_boards_bytes_ = 0;
    if (!store_) return;
    for (const Store::SavedBoard &b : store_->other_boards()) {
        ++other_boards_n_;
        other_boards_bytes_ += b.bytes;
    }
}

void App::confirm_forget_board()
{
    if (!model_.board_known()) {
        notice("No board is known yet", false);
        return;
    }
    const DeletePrompt p = forget_board_prompt(model_, model_.board_key().substr(0, 12));
    open_confirm(p.title, p.body,
                 [this] {
                     model_.forget_board_app_data();
                     cview_.group.clear();
                     csnap_valid_ = false;
                     after_deletion();
                     notice("This board's saved data was forgotten", true);
                 },
                 "Forget", "No", 0, true);
}

void App::confirm_forget_others()
{
    refresh_other_boards(true);
    const DeletePrompt p = forget_others_prompt(other_boards_n_, other_boards_bytes_);
    if (p.empty) {
        notice("No other board has saved data", true);
        return;
    }
    open_confirm(p.title, p.body,
                 [this] {
                     const size_t n = store_->forget_other_boards();
                     refresh_other_boards(true);
                     settings_sig_ = 0;
                     notice("Forgot the saved data of " + fmt_boards(n), true);
                 },
                 "Forget", "No", 0, true);
}

void App::confirm_delete_conversation(const std::string &conv)
{
    const DeletePrompt p = delete_conversation_prompt(model_, conv);
    if (p.empty) {
        notice("No messages to delete in " + truncate_ellipsis(model_.conv_title(conv), 24), true);
        return;
    }
    open_confirm(p.title, p.body,
                 [this, conv] {
                     const size_t n = model_.delete_conversation(conv);
                     if (log_) log_->line("history: deleted " + std::to_string(n) + " messages of " + conv);
                     after_deletion();
                     notice("Deleted " + fmt_message_count(n), true);
                 },
                 "Delete");
}

void App::confirm_delete_all()
{
    const DeletePrompt p = delete_all_prompt(model_);
    if (p.empty) {
        notice("There are no messages to delete", true);
        return;
    }
    open_confirm(p.title, p.body,
                 [this] {
                     const size_t n = model_.delete_all_messages();
                     if (log_) log_->line("history: deleted all " + std::to_string(n) + " messages");
                     after_deletion();
                     notice("Deleted " + fmt_message_count(n), true);
                 },
                 "Delete all");
}

void App::choose_delete_older()
{
    Popup p;
    p.kind = Popup::Choice;
    p.choice = make_days_choice(days_choice_);
    p.title = p.choice.title;
    p.on_accept = [this](int idx) {
        const auto &opts = history_day_options();
        if (idx < 0 || idx >= static_cast<int>(opts.size())) return;
        days_choice_ = opts[static_cast<size_t>(idx)];
        confirm_delete_older(days_choice_);
    };
    popup_begin(std::move(p));
}

void App::confirm_delete_older(int days)
{
    if (history_cutoff(now_unix(), days) == 0) {
        notice("The clock is not set: the age of the messages is unknown", false);
        return;
    }
    const DeletePrompt p = delete_older_prompt(model_, days, now_unix());
    if (p.empty) {
        notice("No message is older than " + std::to_string(days) + " days", true);
        return;
    }
    open_confirm(p.title, p.body,
                 [this, days] {
                     const size_t n = model_.delete_older_than(history_cutoff(now_unix(), days));
                     if (log_) log_->line("history: deleted " + std::to_string(n) + " messages older than " + std::to_string(days) + " days");
                     after_deletion();
                     notice("Deleted " + fmt_message_count(n), true);
                 },
                 "Delete");
}

void App::hold_event(int what, RowList *list, int index, int x, int y)
{
    if (what == 1) {
        hold_.move(x, y);
        return;
    }
    if (what == 2) {
        hold_.cancel();
        return;
    }
    // pressed: is this a name that has options?
    std::string target;
    if (list == &chat_list_) {
        const std::string &id = chat_list_.id_at(index);
        if (id.rfind("c:", 0) == 0 || id.rfind("d:", 0) == 0) target = id;
    } else if (list == nullptr) {
        const bool add_selected = chat_sel_ >= 0 && chat_sel_ < static_cast<int>(chat_rows_.size()) &&
                                  chat_rows_[static_cast<size_t>(chat_sel_)].kind == ChatRow::Add;
        if (!add_selected && (conv_.rfind("c:", 0) == 0 || conv_.rfind("d:", 0) == 0)) target = conv_;
    }
    if (target.empty() || nav_.popup_open || editor_open_ || view_pending_ || nav_.tab != Tab::Chats) {
        hold_.cancel();
        hold_.take_swallow();
        return;
    }
    hold_scroll_y_ = lv_obj_get_scroll_y(chat_list_.obj());
    hold_.begin(target, x, y, mono_ms());
}

void App::hold_tick(uint64_t now)
{
    if (!hold_.active()) {
        if (hold_box_ && !lv_obj_has_flag(hold_box_, LV_OBJ_FLAG_HIDDEN)) set_hidden(hold_box_, true);
        return;
    }
    if (nav_.popup_open || editor_open_ || nav_.tab != Tab::Chats || lv_obj_get_scroll_y(chat_list_.obj()) != hold_scroll_y_) {
        hold_.cancel();                                  // a scroll of the list, or something else took over
        set_hidden(hold_box_, true);
        return;
    }
    if (hold_.poll(now)) {
        set_hidden(hold_box_, true);
        open_conversation_options(hold_.target());
        return;
    }
    if (hold_.visible(now)) {
        lv_label_set_text(hold_label_, ("Keep holding: options for " + truncate_ellipsis(model_.conv_title(hold_.target()), 26)).c_str());
        lv_obj_set_width(hold_bar_, std::max(1, static_cast<int>(hold_.progress(now) * 392)));
        set_hidden(hold_box_, false);
    }
}

void App::presets_tick(uint64_t now)
{
    // a list fetched in the background is adopted only while no popup is open (the preset popup holds indexes into the list)
    if (preset_pending_ready_ && !nav_.popup_open) {
        preset_pending_ready_ = false;
        const bool changed = preset_pending_.size() != radio_presets().size() ||
                             !std::equal(preset_pending_.begin(), preset_pending_.end(), radio_presets().begin(), [](const RadioPreset &a, const RadioPreset &b) {
                                 return a.name == b.name && a.freq_mhz == b.freq_mhz && a.bw_khz == b.bw_khz && a.sf == b.sf && a.cr == b.cr;
                             });
        set_presets(std::move(preset_pending_), PresetOrigin::Fetched, preset_pending_date_);
        preset_pending_.clear();
        if (changed) notice("Radio preset list updated (" + std::to_string(radio_presets().size()) + " entries)", true);
        invalidate();
    }
    if (!fetcher_ || !preset_sched_) return;
    if (fetcher_->state() == PresetFetcher::State::Running) {
        if (now - preset_poll_ms_ < 250) return;
        preset_poll_ms_ = now;
        const PresetFetcher::State st = fetcher_->poll(now);
        if (st == PresetFetcher::State::Done) {
            std::string date = fetch_date_text(deck_unix());
            if (date.empty()) date = "unknown";
            if (log_) log_->line("presets: fetched " + fetcher_->message() + " from " + fetcher_->url());
            if (!save_preset_cache(store_->dir(), fetcher_->list(), date, fetcher_->url()) && log_) log_->line("presets: the saved copy could not be written");
            preset_pending_ = fetcher_->list();
            preset_pending_date_ = date;
            preset_pending_ready_ = true;
            preset_sched_->succeeded();
        } else if (st == PresetFetcher::State::Failed) {
            preset_sched_->failed(now, true);                 // the download was really tried: one more try about 60 s later, if the deck is online
            if (log_)
                log_->line("presets: refresh failed (" + fetcher_->message() + "), keeping the " + (presets_origin() == PresetOrigin::Bundled ? "built-in" : "saved") +
                           " list" + (preset_sched_->finished() ? "" : "; one more try in 60 s"));
        }
        return;
    }
    // 4 s after the start, once the deck is online (offline: looked at again every 30 s); the second and last try 60 s after a failure
    static bool offline_logged = false;
    const bool due = preset_sched_->due(now, [this] {
        const bool on = network_online();
        if (!on && !offline_logged && log_) {
            offline_logged = true;
            log_->line("presets: the deck is offline, no refresh (checking again every 30 s)");
        }
        return on;
    });
    if (!due) return;
    const bool retry = preset_sched_->attempts() >= 1;
    preset_sched_->started();
    if (log_) log_->line(std::string("presets: online, downloading the list in the background") + (retry ? " (second try)" : ""));
    if (!fetcher_->start(now)) {
        if (log_) log_->line("presets: refresh not started (" + fetcher_->message() + ")");
        preset_sched_->failed(now, false);                     // no curl, or it cannot start: nothing to retry
    }
}
/* ================================================================== tick and render */

void App::tick()
{
    run_posted();
    const uint64_t now = mono_ms();
    if (now - last_poll_ms_ >= 20) {
        last_poll_ms_ = now;
        client_->poll(now);
    }
    if (model_.board_epoch() != seen_epoch_) on_board_changed();
    if (pop_dirty_) {                                   // the widgets of a new popup are built here, never inside the click of another
        popup_build();
        popup_refresh();
        pop_dirty_ = false;
    }
    if (view_pending_ && now - view_ms_ >= loading_min_ms()) apply_contact_view();
    hold_tick(now);
    presets_tick(now);
    tick_phase2(now);
    if (share_pending_) {                               // a new private channel: show its key once the board holds it
        const ChannelRec *c = model_.find_channel_by_name(share_name_);
        if (c && c->has_secret && c->secret == share_key_) {
            share_pending_ = false;
            open_info("Key of " + share_name_, channel_key_grouped(share_key_),
                      "Share this key as text. Whoever adds a private channel with this key (Settings > Add channel > Private channel) reads and writes in it.");
        } else if (now > share_deadline_) {
            share_pending_ = false;
        }
    }
    bool need = dirty_;
    if (model_.revision() != last_revision_) need = true;
    const uint32_t link = static_cast<uint32_t>(client_->link());
    if (link != last_link_) {
        if (log_) log_->line("link: " + client_->link_text());
        need = true;
    }
    if (client_->notice().id != last_notice_id_) {
        last_notice_id_ = client_->notice().id;
        notice(client_->notice().text, client_->notice().ok);
        need = true;
    }
    if (!notice_text_.empty() && now > notice_ms_ && now - notice_ms_ >= kNoticeMs) {          // (a notice set later in this very tick has a later time than `now`)
        notice_text_.clear();
        need = true;
    }
    if (now - last_render_ms_ >= 1000) need = true;                       // ages ("5 min") move on
    if (now - kbd_check_ms_ >= 1000) {
        kbd_check_ms_ = now;
        const bool k = keyboard_ready();
        if (k != kbd_ok_) {
            kbd_ok_ = k;
            layout_body();                                   // the key hint line comes or goes: the page bodies take the room
            need = true;
        }
    }
    if (holding_ && now - hold_ms_ >= 4000) holding_ = false;
    if (!dirty_radio_ && !holding_ && !editor_open_) {
        const RadioSettings cur = model_.radio_settings();
        if (cur.name != edit_.name || !cur.same_radio(edit_)) {
            edit_ = cur;
            need = true;
        }
    }
    if (client_->clock_prompt_pending() && !editor_open_ && !nav_.popup_open && (prompt_ms_ == 0 || now - prompt_ms_ >= 500)) {
        prompt_ms_ = now;
        if (!keyboard_ready()) {
            client_->clock_skip();
            notice("Keyboard needed to set the board clock: Settings > Sync clock now", false);
        } else {
            open_editor(EditKind::Clock, "Board clock", "The board has no GPS and the deck clock is not synchronised: type the start date and time (local).",
                        format_local_datetime(now_unix()));
        }
    }
    if (nav_.tab == Tab::Chats && !conv_.empty()) model_.mark_read(conv_);
    if (need) render();
}

void App::render()
{
    last_render_ms_ = mono_ms();
    last_revision_ = model_.revision();
    last_link_ = static_cast<uint32_t>(client_->link());
    dirty_ = false;
    set_page_visible();
    render_tabs();
    switch (nav_.tab) {
    case Tab::Chats:
        render_chats();
        set_hidden(search_panel_, !nav_.search_open);
        if (nav_.search_open) render_search();
        break;
    case Tab::Contacts: render_contacts(); break;
    case Tab::Settings:
        render_settings();
        set_hidden(stats_panel_, !nav_.stats_open);
        if (nav_.stats_open) render_stats();
        set_hidden(log_panel_, !nav_.packet_log_open);
        if (nav_.packet_log_open) render_packet_log();
        break;
    default: break;
    }
    position_refresh();
    render_footer();
    const bool sub = nav_.search_open || nav_.nearby_open || nav_.detail_open || nav_.stats_open || nav_.packet_log_open;
    set_hidden(tabs_, sub);                                  // a sub-page covers the tab row
    update_title();
}

void App::render_tabs()
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int unread = model_.unread_total();
    static const Tab shown[3] = {Tab::Chats, Tab::Contacts, Tab::Settings};
    for (const Tab t : shown) {
        const int i = static_cast<int>(t);
        const bool active = t == nav_.tab;
        const char *name = tab_name(t);
        lv_label_set_text(tab_label_[i], name);
        set_color(tab_label_[i], active ? kAccent : kMuted);
        lv_obj_set_style_bg_opa(tab_btn_[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(tab_btn_[i], lv_color_hex(kPanel), 0);
        set_hidden(tab_bar_line_[i], !active);
        // the word, and next to it the unread total of Chats (a pill with the number, 99+ above 99), centred together
        lv_point_t sz = {0, 0};
        lv_text_get_size(&sz, name, f_body_, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int tab_w = lv_obj_get_width(tab_btn_[i]);
        int total = sz.x;
        int badge_w = 0;
        if (t == Tab::Chats && unread > 0) {
            const std::string text = fmt_count_badge(unread);
            lv_point_t bs = {0, 0};
            lv_text_get_size(&bs, text.c_str(), f_caption_, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            const int bh = lv_obj_get_height(tab_badge_);
            badge_w = std::max(bh, bs.x + bh / 2);
            lv_label_set_text(tab_badge_label_, text.c_str());
            total += m->tok[CP0_TOK_SPACE_S] + badge_w;
        }
        const int x0 = std::max(0, (tab_w - total) / 2);
        lv_obj_set_width(tab_label_[i], sz.x + 4);
        lv_obj_set_x(tab_label_[i], x0);
        if (t == Tab::Chats) {
            if (badge_w) {
                lv_obj_set_width(tab_badge_, badge_w);
                lv_obj_set_x(tab_badge_, x0 + sz.x + m->tok[CP0_TOK_SPACE_S]);
                lv_obj_center(tab_badge_label_);
                set_hidden(tab_badge_, false);
            } else {
                set_hidden(tab_badge_, true);
            }
        }
    }
}

void App::update_compose_style()
{
    const bool focus = nav_.compose_focus && nav_.tab == Tab::Chats && !editor_open_;
    if (focus) lv_obj_add_state(compose_, LV_STATE_FOCUSED);
    else lv_obj_remove_state(compose_, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(compose_, lv_color_hex(focus ? kAccent : kSelected), 0);
    lv_textarea_set_placeholder_text(compose_, !kbd_ok_ ? "Wake the Bluetooth keyboard to type" : conv_.empty() ? "" : "Type a message (keyboard)");
}

void App::render_chats()
{
    // the left list: a title and the number of unread messages (no last message), "muted" for a muted channel
    // before the first SELF_INFO no board is known: an empty list (the conversations of the last board are not shown as if they were this one's)
    const std::vector<ChatRow> rows = model_.board_known() ? build_chat_rows(model_, conv_.rfind("d:", 0) == 0 ? conv_ : std::string()) : std::vector<ChatRow>();
    chat_rows_ = rows;
    chat_sel_ = chat_sel_key_.empty() ? -1 : find_chat_row(chat_rows_, chat_sel_key_);
    if (chat_sel_ < 0 && chat_sel_key_ != "+") chat_sel_ = first_selectable(chat_rows_);
    if (chat_sel_ >= 0 && chat_rows_[static_cast<size_t>(chat_sel_)].kind != ChatRow::Add) {
        chat_sel_key_ = chat_rows_[static_cast<size_t>(chat_sel_)].key;
        if (conv_.empty() || find_chat_row(chat_rows_, conv_) < 0) conv_ = chat_sel_key_;
    }
    std::vector<RowSpec> specs;
    for (const ChatRow &r : chat_rows_) {
        RowSpec s;
        s.id = r.key;
        if (r.kind == ChatRow::Header) {
            s.h = 26;
            s.header = true;
            s.selectable = false;
            s.cells.push_back({r.title, kMuted, 12, 180, LV_TEXT_ALIGN_LEFT, f_small_, centre(26, f_small_)});
        } else if (r.kind == ChatRow::Add) {
            s.h = 44;
            s.cells.push_back({r.title, kBlue, 12, 188, LV_TEXT_ALIGN_LEFT, f_ui_, centre(44, f_ui_)});
        } else {
            s.h = 44;
            s.badge = r.unread;
            std::string title = r.title;
            if (r.kind == ChatRow::Channel && !title.empty() && title[0] == '#') title = title.substr(1);
            if (r.kind == ChatRow::Channel) title = "# " + title;
            const bool tail = r.unread > 0 || r.muted;
            s.cells.push_back({title, r.unread > 0 ? kAccent : kText, 12, tail ? 124 : 180, LV_TEXT_ALIGN_LEFT, f_text_, centre(44, f_text_)});
            if (r.muted) s.cells.push_back({"muted", kMuted, 136, 60, LV_TEXT_ALIGN_RIGHT, f_small_, centre(44, f_small_)});
        }
        specs.push_back(s);
    }
    uint32_t sig = fnv_int(0, chat_sel_);
    sig = fnv_int(sig, nav_.compose_focus ? 1 : 0);
    for (const RowSpec &s : specs) {
        for (const Cell &c : s.cells) sig = fnv(sig, c.text);
        sig = fnv_int(sig, s.badge);
    }
    if (sig != chat_sig_ && !plat_.touching()) {         // never rebuild a list under the finger: the tap or drag would be lost
        chat_sig_ = sig;
        chat_list_.set(std::move(specs), chat_sel_, !nav_.compose_focus);
    }
    if (nav_.compose_focus) chat_list_.scroll_to_selected();

    // the right pane
    const bool add_selected = chat_sel_ >= 0 && chat_rows_[static_cast<size_t>(chat_sel_)].kind == ChatRow::Add;
    std::string title;
    if (add_selected) title = "New channel";
    else if (!conv_.empty()) title = model_.conv_title(conv_);
    else title = "No conversation";
    const bool is_channel = conv_.rfind("c:", 0) == 0 && !add_selected;
    bool has_details = false;                                   // a direct chat with a contact we know: the Details button (touch)
    if (!add_selected && conv_.rfind("d:", 0) == 0 && conv_.size() == 14) {
        KeyPrefix dp{};
        has_details = from_hex(conv_.substr(2), dp.data(), 6) && model_.find_by_prefix(dp) != nullptr;
    }
    const bool side_button = has_details;
    set_hidden(details_btn_, !has_details);
    set_hidden(options_btn_, add_selected || conv_.empty());
    set_hidden(title_hold_, add_selected || conv_.empty());
    const int btn_left = side_button ? kScreenW - 206 : kScreenW - 104;      // the name and the line under it end before the buttons
    lv_obj_set_width(chat_title_, btn_left - 6 - (kListW + 11));
    lv_obj_set_width(chat_info_, btn_left - 6 - (kListW + 11));
    lv_obj_set_width(title_hold_, btn_left - kListW - 5);
    lv_label_set_text(chat_title_, truncate_ellipsis(title, side_button ? 22 : 32).c_str());
    std::string info;
    if (nav_.compose_focus && !conv_.empty()) {
        const char *cur = lv_textarea_get_text(compose_);
        const int left = max_compose_bytes() - static_cast<int>(cur ? std::strlen(cur) : 0);
        info = std::to_string(std::max(0, left)) + " left";
    } else if (!add_selected && !conv_.empty() && model_.is_muted(conv_)) {
        info = "muted";
    } else {
        int heard = 0;
        const uint32_t now = now_unix();
        for (const ContactRec *r : model_.contacts_sorted())
            if (r->last_heard() && now >= r->last_heard() && now - r->last_heard() < 3600) ++heard;
        info = std::to_string(heard) + (heard == 1 ? " node heard" : " nodes heard") + " (1 h)";
    }
    lv_label_set_text(chat_info_, info.c_str());
    render_chat_messages(false);
    update_compose_style();
    set_hidden(compose_, add_selected || conv_.empty());
    set_hidden(send_btn_, add_selected || conv_.empty());
    lv_obj_set_style_bg_opa(send_btn_, nav_.compose_focus ? LV_OPA_COVER : LV_OPA_70, 0);
}

void App::render_chat_messages(bool force)
{
    const bool add_selected = chat_sel_ >= 0 && chat_rows_[static_cast<size_t>(chat_sel_)].kind == ChatRow::Add;
    const auto msgs = conv_.empty() || add_selected ? std::vector<const Message *>() : model_.messages(conv_);
    uint32_t sig = fnv(0, conv_);
    sig = fnv_int(sig, model_.board_known() ? 1 : 0);
    sig = fnv_int(sig, add_selected ? 1 : 0);
    sig = fnv_int(sig, static_cast<int>(msgs.size()));
    sig = fnv_int(sig, conv_ == focus_conv_ ? static_cast<int>(focus_seq_) : 0);
    for (const Message *m : msgs) {
        sig = fnv_int(sig, static_cast<int>(m->seq));
        sig = fnv_int(sig, static_cast<int>(m->state));
        sig = fnv(sig, m->note);
        sig = fnv_int(sig, m->heard_back);
    }
    // the clock of the "HH:MM" does not move; the footer and the list carry the ages
    if (!force && built_sig_ == sig && built_conv_ == conv_) return;
    if (!force && built_conv_ == conv_ && plat_.touching()) return;       // not under a finger that scrolls the conversation
    const bool first = built_conv_ != conv_;
    const bool at_bottom = first || lv_obj_get_scroll_bottom(msgs_) <= 10;
    const int old_scroll = lv_obj_get_scroll_y(msgs_);
    built_sig_ = sig;
    built_conv_ = conv_;
    lv_obj_clean(msgs_);
    lv_obj_t *last = nullptr;
    const int inner_w = 640 - (kListW + 1) - 12;
    if (add_selected || msgs.empty()) {
        lv_obj_t *l = lv_label_create(msgs_);
        lv_obj_set_width(l, inner_w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(l, f_ui_, 0);
        set_color(l, kMuted);
        lv_label_set_text(l, add_selected ? "Press Enter or tap the row to add a channel:\na hashtag channel (#name), or a private channel with its own key."
                          : !model_.board_known() ? "No board yet.\nPlug in a MeshCore board: its own conversations appear here."
                          : conv_.empty() ? "Select a channel or a contact on the left."
                                          : "No messages yet. Type a message with the keyboard and press Enter,\nor tap the entry box.");
        last = l;
    }
    size_t start = msgs.size() > static_cast<size_t>(kMaxShownMessages) ? msgs.size() - kMaxShownMessages : 0;
    size_t end = msgs.size();
    const uint32_t want_seq = conv_ == focus_conv_ ? focus_seq_ : 0;        // a message found by the search: the window is put around it
    if (want_seq) {
        for (size_t i = 0; i < msgs.size(); ++i)
            if (msgs[i]->seq == want_seq) {
                start = i > 20 ? i - 20 : 0;
                end = std::min(msgs.size(), start + static_cast<size_t>(kMaxShownMessages));
            }
    }
    lv_obj_t *focus_item = nullptr;
    const bool channel = conv_.rfind("c:", 0) == 0;
    for (size_t i = start; i < end; ++i) {
        const Message &m = *msgs[i];
        lv_obj_t *item = lv_obj_create(msgs_);
        lv_obj_remove_style_all(item);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(item, inner_w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(item, 5, 0);
        lv_obj_set_style_pad_row(item, 1, 0);
        lv_obj_set_style_radius(item, 6, 0);
        if (m.dir == Dir::Out) {
            lv_obj_set_style_bg_color(item, lv_color_hex(kPanel), 0);
            lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        }
        if (want_seq && m.seq == want_seq) {                                 // the found message: a gold frame
            focus_item = item;
            lv_obj_set_style_border_width(item, 2, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(kAccent), 0);
        }
        lv_obj_t *hdr = make_box(item, 0, 0, inner_w - 10, 20);
        lv_obj_t *who = make_label(hdr, f_text_small_, m.dir == Dir::Out ? kGreen : kBlue, 0, 0, 200, 20);
        lv_label_set_text(who, (m.sender + "   " + fmt_clock(m.ts)).c_str());
        const StatusText st = message_status(m, channel);
        if (!st.text.empty()) {
            std::string mark = m.state == MsgState::Delivered && !channel ? (dejavu_ ? "\xE2\x9C\x93\xE2\x9C\x93 " : "") :
                               (m.state == MsgState::Sent || (m.state == MsgState::Delivered && channel)) && (m.note.empty() || (channel && m.heard_back > 0)) ? (dejavu_ ? "\xE2\x9C\x93 " : "") : "";
            lv_obj_t *sl = make_label(hdr, f_text_small_, tone_color(st.tone), inner_w - 10 - 230, 0, 230, 20, LV_TEXT_ALIGN_RIGHT);
            lv_label_set_text(sl, (mark + st.text).c_str());
        }
        lv_obj_t *body = lv_label_create(item);
        lv_obj_set_width(body, inner_w - 10);
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(body, f_text_, 0);
        set_color(body, kText);
        lv_label_set_text(body, m.text.c_str());
        last = item;
    }
    lv_obj_update_layout(msgs_);
    if (focus_item && first) lv_obj_scroll_to_view(focus_item, LV_ANIM_OFF);
    else if (at_bottom && last && !want_seq) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
    else lv_obj_scroll_to_y(msgs_, old_scroll, LV_ANIM_OFF);
}

/* ---- contacts */

void App::build_contact_specs()
{
    std::vector<RowSpec> specs;
    specs.reserve(csnap_.rows.size());
    const bool select = nav_.select_mode;
    for (const ContactRow &r : csnap_.rows) {
        RowSpec s;
        s.h = kContactRowH;
        s.id = r.key_hex;
        const uint32_t type_color = r.type == advtype::kRepeater ? kBlue : r.type == advtype::kChat ? kText : kAccent;
        const int y = centre(kContactRowH, f_ui_);
        const bool marked = select && csel_.contains(r.key_hex);
        if (select) {                                       // a check box at the left; the whole row is the touch target
            s.cells.push_back({marked ? LV_SYMBOL_OK : " ", marked ? kOnAccent : kText, 8, 28, LV_TEXT_ALIGN_CENTER, f_ui_, 8, marked ? kAccent : kSelected});
        }
        const int nx = select ? 44 : 12;
        s.cells.push_back({truncate_ellipsis(r.name, select ? 18 : 22), marked ? kAccent : kText, nx, select ? 164 : 196, LV_TEXT_ALIGN_LEFT, f_text_, y});
        s.cells.push_back({r.type_name, type_color, 212, 76, LV_TEXT_ALIGN_LEFT, f_ui_, y});
        s.cells.push_back({fmt_age_coarse(r.heard), kMuted, 292, 66, LV_TEXT_ALIGN_LEFT, f_ui_, y});
        s.cells.push_back({fmt_snr(r.has_snr, r.snr), kMuted, 360, 70, LV_TEXT_ALIGN_LEFT, f_ui_, y});
        s.cells.push_back({r.hops < 0 ? "-" : std::to_string(r.hops), kMuted, 432, 48, LV_TEXT_ALIGN_LEFT, f_ui_, y});
        s.cells.push_back({r.has_dist ? fmt_distance(r.dist_km) : "-", kMuted, 484, select ? 150 : 98, LV_TEXT_ALIGN_LEFT, f_ui_, y});
        if (!select) s.cells.push_back({"i", kText, 590, 44, LV_TEXT_ALIGN_CENTER, f_ui_, 4, kSelected});   // the details of this contact (a 44 px target)
        specs.push_back(std::move(s));
    }
    if (specs.empty()) {
        RowSpec s;
        s.h = 60;
        s.selectable = false;
        const bool filtered = (cview_.filter.active() || !cview_.group.empty()) && model_.contact_count() > 0;
        s.cells.push_back({filtered ? "No contact matches the filters." : client_->ready() ? "No contacts yet. Send an advert and wait for the others." : "No contacts cached. Connect the radio board.",
                           kMuted, 12, 616, LV_TEXT_ALIGN_LEFT, f_ui_, 20});
        specs.push_back(std::move(s));
    }
    int sel = csnap_.index_of(contact_sel_key_);
    if (sel < 0) sel = std::min(contact_sel_, static_cast<int>(csnap_.rows.size()) - 1);
    if (csnap_.rows.empty()) sel = -1;
    contact_sel_ = std::max(sel, 0);
    contact_sel_key_ = csnap_.key_at(sel);
    contact_list_.set(std::move(specs), sel, true);
}

void App::rebuild_contact_snapshot()
{
    const uint32_t rev = model_.revision();
    // a group that was deleted no longer filters
    if (!cview_.group.empty() && !model_.groups().has(cview_.group)) cview_.group.clear();
    csnap_ = make_contact_snapshot(model_, cview_);
    csnap_rev_ = rev;
    csnap_minute_ = now_unix() / 60;
    csnap_valid_ = true;
    if (nav_.select_mode) {                                  // marks of contacts that are gone are dropped
        std::set<std::string> alive;
        for (const ContactRec *c : model_.contacts_sorted()) alive.insert(c->key_hex());
        csel_.keep_only(alive);
    }
    build_contact_specs();
}

void App::render_contact_toolbar()
{
    const ContactView &v = view_pending_ ? view_next_ : cview_;
    static const SortKey sort_keys[6] = {SortKey::Name, SortKey::Type, SortKey::Heard, SortKey::Snr, SortKey::Hops, SortKey::Distance};
    static const char *const names[6] = {"Name", "Type", "Heard", "SNR", "Hops", "Distance"};
    for (int i = 0; i < 6; ++i) {
        const bool on = sort_keys[i] == v.sort;
        lv_label_set_text(col_hdr_[i], (std::string(names[i]) + (on ? (v.reverse ? " ^" : " v") : "")).c_str());
        set_color(col_hdr_[i], on ? kAccent : kMuted);
    }
    for (int i = 0; i < 5; ++i) {
        const bool on = static_cast<int>(v.filter.type) == i;
        lv_obj_set_style_bg_color(type_chip_[i], lv_color_hex(on ? kAccent : kSelected), 0);
        set_color(type_chip_label_[i], on ? kOnAccent : kText);
    }
    for (int i = 0; i < 4; ++i) {
        const bool on = static_cast<int>(v.filter.age) == i;
        lv_obj_set_style_bg_color(age_chip_[i], lv_color_hex(on ? kAccent : kSelected), 0);
        set_color(age_chip_label_[i], on ? kOnAccent : kText);
    }
    const size_t total = model_.contact_count();
    std::string cnt = csnap_.rows.size() == total ? std::to_string(total) + (total == 1 ? " contact" : " contacts")
                                                  : std::to_string(csnap_.rows.size()) + " of " + std::to_string(total);
    lv_label_set_text(contacts_count_, cnt.c_str());
    set_hidden(tb_normal_, nav_.select_mode);
    set_hidden(tb_select_, !nav_.select_mode);
    lv_label_set_text(group_btn_label_, v.group.empty() ? "Group: All" : ("Group: " + truncate_ellipsis(v.group, 8)).c_str());
    lv_obj_set_style_bg_color(lv_obj_get_parent(group_btn_label_), lv_color_hex(v.group.empty() ? kSelected : kAccent), 0);
    set_color(group_btn_label_, v.group.empty() ? kText : kOnAccent);
    const size_t pending = model_.pending_count();
    lv_label_set_text(nearby_btn_label_, pending ? ("Nearby " + std::to_string(pending)).c_str() : "Nearby");
    lv_obj_set_style_bg_color(nearby_btn_, lv_color_hex(pending ? kAccent : kSelected), 0);
    set_color(nearby_btn_label_, pending ? kOnAccent : kText);
    if (nav_.select_mode) {
        const size_t n = csel_.shown_selected(csnap_.rows).size();
        lv_label_set_text(del_btn_label_, n ? ("Delete " + std::to_string(n)).c_str() : "Delete");
        lv_obj_set_style_bg_color(del_btn_, lv_color_hex(n ? kDangerRed : kSelectedDim), 0);
        std::string c = std::to_string(n) + (n == 1 ? " contact marked" : " contacts marked");
        if (const size_t hidden = csel_.hidden_count(csnap_.rows)) c += " (+" + std::to_string(hidden) + " hidden)";
        lv_label_set_text(sel_count_, c.c_str());
    }
}

void App::render_contacts()
{
    // the rows are rebuilt only when something changed (the model, the sort or filter, the minute of the ages), never for a key press
    const uint32_t minute = now_unix() / 60;
    const bool stale = !csnap_valid_ || model_.revision() != csnap_rev_ || minute != csnap_minute_;
    if (stale && !view_pending_ && !plat_.touching()) rebuild_contact_snapshot();
    render_contact_toolbar();
    set_hidden(detail_, !nav_.detail_open);
    if (nav_.detail_open) render_detail();
    set_hidden(nearby_, !nav_.nearby_open);
    if (nav_.nearby_open) render_nearby();
}
void App::change_contact_view(const ContactView &next)
{
    if (next == (view_pending_ ? view_next_ : cview_)) return;
    view_next_ = next;
    view_pending_ = true;
    view_ms_ = mono_ms();
    show_loading(true);                         // drawn before the work starts (apply_contact_view runs a few ticks later)
    invalidate();
}

void App::apply_contact_view()
{
    view_pending_ = false;
    cview_ = view_next_;
    csnap_valid_ = false;
    contact_sel_ = 0;
    contact_sel_key_.clear();
    rebuild_contact_snapshot();
    contact_list_.scroll_to_top();
    show_loading(false);
    invalidate();
}

void App::show_loading(bool on)
{
    set_hidden(loading_, !on);
}

void App::move_contact_selection(int delta, bool absolute)
{
    if (csnap_.rows.empty()) return;
    const int n = static_cast<int>(csnap_.rows.size());
    contact_sel_ = std::clamp(absolute ? delta : contact_sel_ + delta, 0, n - 1);
    contact_sel_key_ = csnap_.key_at(contact_sel_);
    contact_list_.set_selected(contact_sel_, true, true);        // two rows re-bound and a scroll: no rebuild of the list
}

void App::render_detail()
{
    const ContactDetail d = build_contact_detail(model_, detail_key_);
    if (!d.found) {
        lv_label_set_text(detail_title_, "(gone)");
        for (auto *v : detail_val_) lv_label_set_text(v, "");
        set_hidden(detail_msg_btn_, true);
        return;
    }
    lv_label_set_text(detail_title_, d.title.c_str());
    lv_label_set_text(detail_val_[0], d.name.c_str());
    lv_label_set_text(detail_val_[1], d.type.c_str());
    lv_label_set_text(detail_val_[2], d.groups.c_str());
    lv_label_set_text(detail_val_[3], (d.key1 + "\n" + d.key2).c_str());
    lv_label_set_text(detail_val_[4], d.route.c_str());
    lv_label_set_text(detail_val_[5], d.last_advert.c_str());
    lv_label_set_text(detail_val_[6], d.heard.c_str());
    lv_label_set_text(detail_val_[7], d.position.c_str());
    lv_label_set_text(detail_val_[8], d.distance.c_str());
    lv_label_set_text(detail_val_[9], d.snr.c_str());
    set_hidden(detail_msg_btn_, !d.chat);
}
/* ---- settings */

void App::render_settings()
{
    const bool connected = client_->ready();
    const RadioSettings cur = model_.radio_settings();
    const DeviceInfo *dev = model_.device() ? &*model_.device() : nullptr;

    SettingsContext ctx;
    ctx.connected = connected;
    ctx.gps = feature_state(Feature::BoardGps, dev, model_.caps());
    ctx.channel_admin = feature_state(Feature::ChannelAdmin, dev, model_.caps());
    ctx.path_hash = feature_state(Feature::PathHashMode, dev, model_.caps());
    ctx.stats = feature_state(Feature::Stats, dev, model_.caps());
    ctx.autoadd = feature_state(Feature::AutoAdd, dev, model_.caps());
    ctx.repeat = repeat_state(dev, model_.caps(), model_.self() ? model_.self()->freq_mhz() : 0);
    ctx.have_self = model_.self().has_value();
    ctx.board_known = model_.board_known();
    refresh_other_boards(false);
    ctx.other_boards = other_boards_n_;
    const ChannelRec *ch0 = model_.find_channel(0);
    ctx.slot0_empty = !ch0 || ch0->empty;
    const std::vector<ChannelEntry> entries = build_channel_entries(model_);
    for (const ChannelEntry &e : entries) ctx.channels.push_back(e.idx);
    std::vector<SRowSpec> layout = settings_layout(ctx);

    std::vector<RowSpec> specs;
    auto id_of = [&](size_t i) { return std::to_string(i); };
    auto header = [&](int section) {
        RowSpec s;
        s.h = 28;
        s.header = true;
        s.selectable = false;
        std::string t = settings_section_title(section);
        if (section == 1 && dirty_radio_) t += "   (unsaved: see the end of the list)";
        s.cells.push_back({t, section == 1 && dirty_radio_ ? kWarn : kMuted, 12, 600, LV_TEXT_ALIGN_LEFT, f_small_, centre(28, f_small_)});
        return s;
    };
    auto info = [&](const char *label, const std::string &value, uint32_t color = kText) {
        RowSpec s;
        s.h = 30;
        s.selectable = false;
        s.cells.push_back({label, kMuted, 12, 200, LV_TEXT_ALIGN_LEFT, f_ui_, centre(30, f_ui_)});
        s.cells.push_back({value, color, 220, 408, LV_TEXT_ALIGN_LEFT, f_text_, centre(30, f_text_)});
        return s;
    };
    // a "firmware too old" or "not allowed" line: the reason in the small font, in gold
    auto notice_row = [&](const char *label, const std::string &text) {
        RowSpec s;
        s.h = 34;
        s.selectable = false;
        s.cells.push_back({label, kMuted, 12, 200, LV_TEXT_ALIGN_LEFT, f_ui_, centre(34, f_ui_)});
        s.cells.push_back({text, kWarn, 220, 408, LV_TEXT_ALIGN_LEFT, f_text_small_, centre(34, f_text_small_)});
        return s;
    };
    // a row that does something: label, value, and a ">" when it opens a popup
    auto row = [&](const std::string &label, const std::string &value, uint32_t value_color, uint32_t label_color, bool chevron, bool changed) {
        RowSpec s;
        s.h = 44;
        s.cells.push_back({std::string(changed ? "* " : "") + label, label_color, 12, 200, LV_TEXT_ALIGN_LEFT, f_ui_, centre(44, f_ui_)});
        s.cells.push_back({value, value_color, 220, chevron ? 360 : 400, LV_TEXT_ALIGN_LEFT, f_text_, centre(44, f_text_)});
        if (chevron) s.cells.push_back({">", kMuted, 596, 30, LV_TEXT_ALIGN_CENTER, f_big_, centre(44, f_big_)});
        return s;
    };

    const bool changed_freq = connected && std::fabs(edit_.freq_mhz - cur.freq_mhz) > 0.0004;
    const bool changed_bw = connected && std::fabs(edit_.bw_khz - cur.bw_khz) > 0.0004;
    const bool changed_sf = connected && edit_.sf != cur.sf, changed_cr = connected && edit_.cr != cur.cr;
    const bool changed_tx = connected && edit_.tx_power != cur.tx_power;
    const bool changed_name = connected && edit_.name != cur.name;
    const bool changed_radio = changed_freq || changed_bw || changed_sf || changed_cr;
    const RetrySettings retry = model_.retry();

    for (size_t i = 0; i < layout.size(); ++i) {
        const SRowSpec &r = layout[i];
        RowSpec s;
        switch (r.kind) {
        case SRowKind::Header: s = header(r.arg); break;
        case SRowKind::Info:
            if (r.arg == 0) {
                std::string v = client_->link_text();
                if (connected && dev) v += "   " + dev->model;
                else if (!client_->link_hint().empty()) v += "   " + client_->link_hint();
                s = info("Board", v, connected ? kGreen : kMuted);
            } else if (r.arg == 1) {
                s = info("Firmware", connected ? firmware_text(dev) : "-", connected ? kText : kMuted);
            } else if (r.arg == 2) {
                if (const auto &bat = model_.battery()) {
                    const int mv = bat->millivolts;
                    const int pct = std::clamp((mv - 3300) * 100 / 900, 0, 100);     // rough: 3.3 V empty, 4.2 V full
                    char b[48];
                    std::snprintf(b, sizeof(b), "%.2f V  (about %d%%)", mv / 1000.0, pct);
                    s = info("Battery", b);
                } else {
                    s = info("Battery", "-", kMuted);
                }
            } else {
                if (connected) {
                    const ClockInfo &ci = model_.clock();
                    s = info("Clock", std::string(clock_source_name(ci.source)) + "   " + format_local_datetime(now_unix()),
                             ci.source == ClockSource::NotSet ? kWarn : kText);
                } else {
                    s = info("Clock", "-", kMuted);
                }
            }
            break;
        case SRowKind::Position: {
            const auto &self = model_.self();
            const bool has = self && !(self->lat == 0 && self->lon == 0);
            s = row("Position", has ? fmt_position(true, self->lat, self->lon) : "not set", has ? kText : kMuted, kText, true, false);   // ">": opens the Position box
            break;
        }
        case SRowKind::Stats: s = row("Statistics", "packets, airtime, errors", kMuted, kText, true, false); break;
        case SRowKind::StatsNotice: s = notice_row("Statistics", ctx.stats.notice); break;
        case SRowKind::PacketLog: {
            const PacketLog &pl = client_->packet_log();
            s = row("Packet log", pl.capturing() ? "capturing, " + std::to_string(pl.size()) + " packets" : pl.size() ? "stopped, " + std::to_string(pl.size()) + " packets" : "off: the radio packets heard",
                    pl.capturing() ? kAccent : kMuted, kText, true, false);
            break;
        }
        case SRowKind::PathHash: {
            const int mode = dev ? dev->path_hash_mode : -1;
            s = row("Path hash size", mode >= 0 ? path_hash_text(mode) : "unknown", kText, kText, true, false);
            break;
        }
        case SRowKind::PathHashNotice: s = notice_row("Path hash size", ctx.path_hash.notice); break;
        case SRowKind::Repeat:
            s = row("Repeat", dev && dev->repeat ? "On: this board forwards packets" : "Off", dev && dev->repeat ? kAccent : kText, kText, false, false);
            s.cells.push_back({dev && dev->repeat ? "tap to switch off" : "tap to switch on", kMuted, 450, 170, LV_TEXT_ALIGN_RIGHT, f_small_, centre(44, f_small_)});
            break;
        case SRowKind::RepeatNotice: s = notice_row("Repeat", ctx.repeat.note); break;
        case SRowKind::AdvertEvery: {
            const AdvertSchedule sc = model_.advert_schedule();
            s = row("Advert every", sc.interval_hours ? std::to_string(sc.interval_hours) + " h" : "Off", sc.interval_hours ? kAccent : kText, kText, true, false);
            break;
        }
        case SRowKind::AdvertKind:
            s = row("Advert type", model_.advert_schedule().flood ? "Flood (whole mesh)" : "Zero-hop (neighbours)", kText, model_.advert_schedule().interval_hours ? kText : kMuted, true, false);
            break;
        case SRowKind::ManualAdd: {
            const bool manual = model_.self() && model_.self()->manual_add_contacts;
            s = row("Add nodes by itself", manual ? "Off: new nodes wait in Nearby" : "On: the board adds new nodes", manual ? kAccent : kText, kText, false, false);
            s.cells.push_back({"tap to switch", kMuted, 480, 140, LV_TEXT_ALIGN_RIGHT, f_small_, centre(44, f_small_)});
            break;
        }
        case SRowKind::AutoAddTypes:
            s = row("Still added by itself", model_.caps().autoadd_known ? autoadd_summary(model_.caps().autoadd_config) : "-", kText, kText, true, false);
            break;
        case SRowKind::AutoAddNotice: s = notice_row("Auto-add types", ctx.autoadd.notice); break;
        case SRowKind::Reboot: s = row("Reboot the board", "restarts it, nothing is erased", kMuted, kText, true, false); break;
        case SRowKind::FactoryReset: s = row("Factory reset the board", "erases its keys, contacts, channels", kRed, kRed, true, false); break;
        case SRowKind::BoardGps:
            s = row("Board GPS", model_.caps().gps_on ? "On" : "Off", model_.caps().gps_on ? kGreen : kText, kText, false, false);
            s.cells.push_back({model_.caps().gps_on ? "tap to switch off" : "tap to switch on", kMuted, 450, 170, LV_TEXT_ALIGN_RIGHT, f_small_, centre(44, f_small_)});
            break;
        case SRowKind::GpsNotice:
            s = info("Board GPS", "Firmware too old for this feature", kWarn);
            break;
        case SRowKind::Name: s = row("Node name", edit_.name.empty() ? "-" : edit_.name, changed_name ? kAccent : kText, kText, false, changed_name); break;
        case SRowKind::Preset: {
            const int p = connected ? find_preset(edit_.freq_mhz, edit_.bw_khz, edit_.sf, edit_.cr) : -1;
            std::string v = !connected ? "-" : p >= 0 ? radio_presets()[static_cast<size_t>(p)].name : "Custom";
            s = row("Preset", v, changed_radio ? kAccent : kText, connected ? kText : kMuted, true, false);
            break;
        }
        case SRowKind::Freq: s = row("Frequency", connected ? fmt_freq(edit_.freq_mhz) : "-", changed_freq ? kAccent : kText, kText, false, changed_freq); break;
        case SRowKind::Bw: s = row("Bandwidth", connected ? fmt_bw(edit_.bw_khz) : "-", changed_bw ? kAccent : kText, kText, true, changed_bw); break;
        case SRowKind::Sf: s = row("Spreading factor", connected ? "SF" + std::to_string(edit_.sf) : "-", changed_sf ? kAccent : kText, kText, true, changed_sf); break;
        case SRowKind::Cr: s = row("Coding rate", connected ? "4/" + std::to_string(edit_.cr) : "-", changed_cr ? kAccent : kText, kText, true, changed_cr); break;
        case SRowKind::Tx: s = row("TX power", connected ? std::to_string(edit_.tx_power) + " dBm" : "-", changed_tx ? kAccent : kText, kText, true, changed_tx); break;
        case SRowKind::RetryAttempts:
            s = row("Message tries", retry.attempts == 1 ? "1 (no retry)" : std::to_string(retry.attempts), kText, kText, true, false);
            break;
        case SRowKind::ResetAfter:
            s = row("Forget the route", retry.reset_after == 0 ? "Never" : "Before try " + std::to_string(retry.reset_after + 1), kText, kText, true, false);
            break;
        case SRowKind::SyncClock:
            s = row("Sync clock now", connected ? clock_source_name(model_.clock().source) : "", kMuted, connected ? kText : kMuted, false, false);
            break;
        case SRowKind::Channel: {
            const ChannelEntry *e = nullptr;
            for (const ChannelEntry &x : entries)
                if (x.idx == r.arg) e = &x;
            s.h = 48;
            if (!e) break;
            s.cells.push_back({truncate_ellipsis(e->name, 20), e->muted ? kMuted : kText, 12, 220, LV_TEXT_ALIGN_LEFT, f_text_, centre(48, f_text_)});
            s.cells.push_back({std::string(channel_kind_name(e->kind)) + (e->muted ? ", muted" : ""), kMuted, 236, 100, LV_TEXT_ALIGN_LEFT, f_small_, centre(48, f_small_)});
            s.cells.push_back({e->muted ? "Unmute" : "Mute", kText, 344, 90, LV_TEXT_ALIGN_CENTER, f_ui_, 5, kSelected});
            s.cells.push_back({"Key", e->has_key ? kText : kMuted, 440, 70, LV_TEXT_ALIGN_CENTER, f_ui_, 5, e->has_key ? kSelected : kSelectedDim});
            s.cells.push_back({"Remove", connected ? kRed : kMuted, 516, 104, LV_TEXT_ALIGN_CENTER, f_ui_, 5, connected ? kSelected : kSelectedDim});
            break;
        }
        case SRowKind::AddChannel: s = row("+ Add channel", "hashtag or private", kMuted, kBlue, true, false); break;
        case SRowKind::AddPublic: s = row("Add the Public channel", "slot 0 is empty", kMuted, kText, false, false); break;
        case SRowKind::ChannelsNotice: s = info("Channels", "Firmware too old for this feature", kWarn); break;
        case SRowKind::HistoryAll: {
            const size_t n = model_.message_count();
            s = row("Delete all messages", fmt_message_count(n), n ? kText : kMuted, kText, true, false);
            break;
        }
        case SRowKind::HistoryOlder: s = row("Delete older than", "7, 30 or 90 days", kMuted, kText, true, false); break;
        case SRowKind::HistoryNote: s = info("One conversation", "Chats: hold its name 3 s, or Ctrl+O", kMuted); break;
        case SRowKind::HistoryForget: s = row("Forget this board's data", "history, groups, mute flags", kMuted, kText, true, false); break;
        case SRowKind::HistoryOthers: s = row("Other boards' data", fmt_boards(other_boards_n_) + ", " + fmt_bytes(other_boards_bytes_), kMuted, kText, true, false); break;
        case SRowKind::Undo:
            s = row("Undo changes", dirty_radio_ ? "discard the unsaved edits" : "no changes", kMuted, dirty_radio_ ? kText : kMuted, false, false);
            break;
        case SRowKind::Save:
            s = row("Save to radio", dirty_radio_ ? "unsaved changes" : "no changes", dirty_radio_ ? kWarn : kMuted, dirty_radio_ && connected ? kGreen : kMuted, false, false);
            break;
        }
        s.id = id_of(i);
        specs.push_back(std::move(s));
    }

    // never replace the rows under the finger; the rows and the row list stay a matching pair
    uint32_t sig = 0;
    for (const RowSpec &s : specs) {
        for (const Cell &c : s.cells) sig = fnv(sig, c.text);
        sig = fnv_int(sig, static_cast<int>(s.h));
    }
    std::vector<bool> ok;
    for (const RowSpec &s : specs) ok.push_back(s.selectable);
    if (sig != settings_sig_ && !plat_.touching()) {
        settings_sig_ = sig;
        srows_ = layout;
        settings_channels_ = entries;
        settings_sel_ = step_selectable(ok, settings_sel_, 0);
        settings_list_.set(std::move(specs), settings_sel_, true);
    } else if (settings_list_.count() == static_cast<int>(srows_.size())) {
        settings_list_.set_selected(settings_sel_, true, false);
    }
}

/* The key hints of the screen (only shown while a keyboard is there). */
std::string App::hint_text() const
{
    if (nav_.popup_open) {
        switch (pop_.kind) {
        case Popup::Choice: return "Left/Right: change   Enter: accept   Esc: cancel";
        case Popup::Confirm: return "Y: yes   N: no   Left/Right: choose   Enter   Esc: no";
        case Popup::Menu:
        case Popup::List: return "Up/Down: choose   Enter: select   Esc: cancel";
        case Popup::Progress: return "Esc or Enter: stop after the contact in progress";
        default: return "Enter or Esc: close";
        }
    }
    if (editor_open_) return edit_kind_ == EditKind::Position ? "Tab: next field   Enter: OK   Esc: cancel" : "Type with the keyboard   Enter: OK   Esc: cancel";
    switch (nav_.tab) {
    case Tab::Chats:
        return nav_.search_open ? "Type  Up/Down  Enter: open  Ctrl+K D R: filters"
               : nav_.compose_focus ? "Enter: send  Esc: list  Up/Down: scroll  Ctrl+O: options"
                                    : "Enter: write  Ctrl+O: options  Ctrl+F: find";
    case Tab::Contacts:
        return nav_.detail_open ? "Enter: message   G: groups   D: delete   Esc: back"
               : nav_.nearby_open ? "Enter: add   I: ignore   S: scan   V: ignored   Esc: back"
               : nav_.select_mode ? "Space: mark  A all  N none  I invert  M menu  D delete  G group"
                                  : "Enter: open  I: info  X: select  N: nearby  G: group";
    case Tab::Settings:
        return nav_.stats_open ? "R: refresh   A: auto refresh   Esc: back"
               : nav_.packet_log_open ? "Up/Down: select  Space: start/stop  C: clear  Esc: back"
                                      : "Enter: change   Left/Right: step   S: save   V: undo";
    default: return "Tab: next tab   Esc: back";
    }
}

/* The caption line under the header: a notice for a few seconds, else the state of the board (the words say it, the colour only repeats it);
 * and the key hint line at the bottom of the screen. */
void App::render_footer()
{
    std::string text;
    cp0::ui::CaptionTone tone = cp0::ui::CaptionTone::Normal;
    if (!notice_text_.empty()) {
        text = notice_text_;
        tone = notice_ok_ ? cp0::ui::CaptionTone::Normal : cp0::ui::CaptionTone::Error;
    } else {
        const BoardLine bl = board_line(*client_, model_);
        text = bl.text;
        tone = bl.tone == Tone::Green ? cp0::ui::CaptionTone::Normal : bl.tone == Tone::Red ? cp0::ui::CaptionTone::Error : cp0::ui::CaptionTone::Warn;
    }
    if (text != cap_text_ || static_cast<int>(tone) != cap_tone_) {
        cap_text_ = text;
        cap_tone_ = static_cast<int>(tone);
        shell_->set_caption(text, tone);
        if (dialog_) dialog_->raise();                       // the caption is moved above the page: the dialogs stay over it
        if (ed_dialog_) ed_dialog_->raise();
    }
    if (kbd_ok_ && hint_label_) lv_label_set_text(hint_label_, hint_text().c_str());
}

/* ================================================================== popups (cp0::ui::Dialog) */

void App::popup_begin(Popup p)
{
    p.opened_ms = mono_ms();
    pop_ = std::move(p);
    nav_.popup_open = true;
    pop_dirty_ = true;                               // built in the next tick
    invalidate();
}

void App::close_popup()
{
    pop_.kind = Popup::None;
    pop_.on_accept = nullptr;
    pop_.on_yes = nullptr;
    pop_.on_cancel = nullptr;
    nav_.popup_open = false;
    pop_dirty_ = false;
    pop_items_.clear();
    pop_value_ = pop_detail_ = pop_pos_ = pop_left_ = pop_right_ = pop_yes_ = pop_no_ = nullptr;
    pop_bar_ = pop_bar_fill_ = pop_progress_ = nullptr;
    dialog_.reset();
    invalidate();
}

void App::open_choice(ChoiceField f)
{
    Popup p;
    p.kind = Popup::Choice;
    ChoiceExtra extra;
    extra.path_hash_mode = model_.device() ? model_.device()->path_hash_mode : -1;
    extra.schedule = model_.advert_schedule();
    p.choice = make_choice(f, edit_, model_.retry(), &extra);
    p.title = p.choice.title;
    choice_field_ = f;
    p.on_accept = [this, f](int idx) {
        if (f == ChoiceField::PathHash) {                                   // written to the board at once (the board keeps it), then read back
            const int mode = path_hash_mode_from_index(idx);
            if (model_.device() && mode == model_.device()->path_hash_mode) notice(std::string("Path hash size stays ") + path_hash_text(mode), true);
            else client_->set_path_hash_mode(mode);
            return;
        }
        if (f == ChoiceField::AdvertEvery || f == ChoiceField::AdvertKind) {   // a preference of the deck: the app sends the advert
            const AdvertSchedule next = apply_advert_choice(f, idx, model_.advert_schedule());
            model_.set_advert_schedule(next);
            notice(next.interval_hours ? "Advert every " + std::to_string(next.interval_hours) + " h (" + (next.flood ? "flood" : "zero-hop") + ") while the app runs"
                                       : std::string("Scheduled advert off"), true);
            invalidate();
            return;
        }
        RadioSettings s = edit_;
        RetrySettings r = model_.retry();
        apply_choice(f, idx, s, r);
        if (f == ChoiceField::RetryAttempts || f == ChoiceField::ResetAfter) {
            model_.set_retry(r);                      // a local preference: kept at once, nothing to save to the radio
            notice("Direct messages: " + std::to_string(model_.retry().attempts) + " tries" +
                       (model_.retry().reset_after ? ", route forgotten before try " + std::to_string(model_.retry().reset_after + 1) : ", route never forgotten"),
                   true);
        } else if (!s.same_radio(edit_)) {
            edit_ = s;
            dirty_radio_ = true;
        }
        invalidate();
    };
    popup_begin(std::move(p));
}

void App::open_confirm(const std::string &title, const std::string &body, std::function<void()> on_yes, const std::string &yes, const std::string &no,
                        uint32_t yes_delay_ms, bool danger)
{
    Popup p;
    p.kind = Popup::Confirm;
    p.title = title;
    p.body = body;
    p.yes_text = yes;
    p.no_text = no;
    p.focus = 0;                                      // the safe answer is the default
    p.on_yes = std::move(on_yes);
    p.yes_delay_ms = yes_delay_ms;
    p.danger = danger;
    popup_begin(std::move(p));
}

void App::open_list(const std::string &title, const std::vector<std::string> &items, int focus, std::function<void(int)> on_pick)
{
    Popup p;
    p.kind = Popup::List;
    p.title = title;
    p.items = items;
    p.focus = std::clamp(focus, 0, std::max(0, static_cast<int>(items.size()) - 1));
    p.on_accept = std::move(on_pick);
    popup_begin(std::move(p));
}

void App::open_progress(const std::string &title)
{
    Popup p;
    p.kind = Popup::Progress;
    p.title = title;
    popup_begin(std::move(p));
}

void App::open_menu(const std::string &title, const std::vector<std::string> &items, std::function<void(int)> on_pick)
{
    Popup p;
    p.kind = Popup::Menu;
    p.title = title;
    p.items = items;
    p.focus = 0;
    p.on_accept = std::move(on_pick);
    popup_begin(std::move(p));
}

void App::open_info(const std::string &title, const std::string &body, const std::string &small)
{
    Popup p;
    p.kind = Popup::Info;
    p.title = title;
    p.body = body;
    p.items = {small};
    popup_begin(std::move(p));
}

/* The buttons of a dialog (the toolkit does not hand them out): the buttons of its box, in order. */
lv_obj_t *App::dialog_button(cp0::ui::Dialog &d, size_t index) const
{
    lv_obj_t *backdrop = d.root();
    if (!backdrop || lv_obj_get_child_count(backdrop) < 2) return nullptr;
    lv_obj_t *box = lv_obj_get_child(backdrop, 1);
    size_t n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); ++i) {
        lv_obj_t *c = lv_obj_get_child(box, static_cast<int32_t>(i));
        if (!lv_obj_check_type(c, &lv_button_class)) continue;
        if (n++ == index) return c;
    }
    return nullptr;
}

/* A line of text in the body of a dialog: the BODY size, or the CAPTION size for a small muted line. */
lv_obj_t *App::dialog_label(cp0::ui::Dialog &d, const std::string &text, bool small, uint32_t color)
{
    lv_obj_t *l = lv_label_create(d.body());
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, d.geometry().content_w);
    lv_obj_set_style_text_font(l, small ? f_caption_ : f_body_, 0);
    set_color(l, color);
    lv_label_set_text(l, text.c_str());
    return l;
}

/* A row of a menu or a pick list in a dialog: the ROW token high, the label, and at the right the action dot (a tap picks it). */
lv_obj_t *App::dialog_row(cp0::ui::Dialog &d, const std::string &text, int index)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    const int w = d.geometry().content_w, h = layout_.row_h;
    cp0_ui_list_layout_t l{};
    cp0_ui_list_layout(m, CP0_UI_LIST_COMFORTABLE, 0, 0, w, h, &l);
    lv_obj_t *row = make_box(d.body(), 0, 0, w, h);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_radius(row, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(kPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    const int trail = cp0_ui_list_trail_w(&l, CP0_UI_LIST_AFFORDANCE_ACTION);
    const int lh = static_cast<int>(lv_font_get_line_height(f_body_));
    lv_obj_t *label = make_label(row, f_body_, kText, l.inset, (h - lh) / 2, std::max(10, w - 2 * l.inset - trail), lh);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(label, text.c_str());
    const int dpx = cp0_ui_list_dot_px(&l);
    lv_obj_t *dot = make_box(row, cp0_ui_list_dot_x(&l, w), (h - dpx) / 2, dpx, dpx);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(cp0_ui_list_dot_rgb(CP0_UI_LIST_AFFORDANCE_ACTION)), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(row, trampoline_popitem, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
    return row;
}

/* A one-line field in a dialog (the app types into it: the keys come from the app's bridge): the ROW token high, the DejaVu face when there is one. */
lv_obj_t *App::make_field(lv_obj_t *parent, int tag, size_t max_len)
{
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    lv_obj_t *ta = lv_textarea_create(parent);
    if (lv_obj_get_group(ta)) lv_group_remove_obj(ta);
    lv_textarea_set_one_line(ta, true);                       // (sets the height to its content: the height comes after)
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, layout_.row_h);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_text_font(ta, f_field_, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(kText), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, m->tok[CP0_TOK_RADIUS_S], 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(kAccent), 0);
    lv_obj_set_style_pad_hor(ta, m->tok[CP0_TOK_SPACE_S], 0);
    lv_obj_set_style_pad_ver(ta, std::max(0, (layout_.row_h - 4 - static_cast<int>(lv_font_get_line_height(f_field_))) / 2), 0);   // the text on the middle line
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_hex(kAccent), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_max_length(ta, static_cast<uint32_t>(max_len));
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    if (tag) lv_obj_add_event_cb(ta, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(tag)));
    return ta;
}

/* The widgets of the pending popup, built from the tick (never inside an event of a popup button): a cp0::ui::Dialog, which follows the
 * on-screen keyboard and keeps the header free. The buttons post their answer: the dialog is deleted after the event that pressed it. */
void App::popup_build()
{
    dialog_.reset();
    pop_items_.clear();
    pop_value_ = pop_detail_ = pop_pos_ = pop_left_ = pop_right_ = pop_yes_ = pop_no_ = nullptr;
    pop_bar_ = pop_bar_fill_ = pop_progress_ = nullptr;
    if (pop_.kind == Popup::None) return;
    using cp0::ui::ButtonRole;
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    auto tapped = [this](int tag) { return [this, tag] { post([this, tag] { if (nav_.popup_open) popup_click(tag); }); }; };
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
        spec.buttons.push_back({confirm_yes_text(pop_.yes_text, confirm_wait_left(pop_.opened_ms, mono_ms(), pop_.yes_delay_ms)), ButtonRole::Primary, tapped(kTagPopYes)});
        break;
    case Popup::List:
    case Popup::Menu: spec.buttons.push_back({"Cancel", ButtonRole::Cancel, tapped(kTagPopCancel)}); break;
    case Popup::Progress: spec.buttons.push_back({"Stop", ButtonRole::Cancel, tapped(kTagPopCancel)}); break;
    case Popup::Info: spec.buttons.push_back({pop_.ok_text, ButtonRole::Primary, tapped(kTagPopOk)}); break;
    case Popup::None: break;
    }
    dialog_ = std::make_unique<cp0::ui::Dialog>(shell_->root(), layout_, std::move(spec));
    cp0::ui::Dialog &d = *dialog_;
    const int cw = d.geometry().content_w;
    switch (pop_.kind) {
    case Popup::Choice: {
        // the value in the middle, a TARGET sized arrow on each side (touch), Left / Right keys do the same
        const int target = m->tok[CP0_TOK_TARGET], gap = m->tok[CP0_TOK_SPACE_M];
        lv_obj_t *row = lv_obj_create(d.body());
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row, cw, target);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto arrow = [&](const char *symbol, int tag) {
            lv_obj_t *b = lv_button_create(row);
            if (lv_obj_get_group(b)) lv_group_remove_obj(b);
            lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_size(b, target, target);
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
        lv_obj_set_width(pop_value_, std::max(10, cw - 2 * target - 2 * gap));
        lv_obj_set_style_text_align(pop_value_, LV_TEXT_ALIGN_CENTER, 0);
        set_color(pop_value_, kText);
        pop_right_ = arrow(LV_SYMBOL_RIGHT, kTagPopRight);
        pop_detail_ = dialog_label(d, "", false, kText);
        pop_pos_ = dialog_label(d, "", true, kMuted);
        lv_obj_set_style_text_align(pop_detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(pop_pos_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *note = dialog_label(d, pop_.choice.note, true, kMuted);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        break;
    }
    case Popup::Confirm:
        d.add_text(pop_.body);
        pop_no_ = dialog_button(d, 0);
        pop_yes_ = dialog_button(d, 1);
        break;
    case Popup::List:
    case Popup::Menu:
        for (size_t i = 0; i < pop_.items.size(); ++i) pop_items_.push_back(dialog_row(d, pop_.items[i], static_cast<int>(i)));
        break;
    case Popup::Progress: {
        pop_progress_ = dialog_label(d, "", false, kText);
        lv_obj_set_style_text_align(pop_progress_, LV_TEXT_ALIGN_CENTER, 0);
        const int bar_h = m->tok[CP0_TOK_SPACE_M];
        pop_bar_ = make_box(d.body(), 0, 0, cw, bar_h);
        lv_obj_set_style_bg_color(pop_bar_, lv_color_hex(kSelected), 0);
        lv_obj_set_style_bg_opa(pop_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pop_bar_, bar_h / 2, 0);
        pop_bar_fill_ = make_box(pop_bar_, 0, 0, 1, bar_h);
        lv_obj_set_style_bg_color(pop_bar_fill_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_bg_opa(pop_bar_fill_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pop_bar_fill_, bar_h / 2, 0);
        pop_bar_w_ = cw;
        break;
    }
    case Popup::Info: {
        d.add_text(pop_.body);
        if (!pop_.items.empty() && !pop_.items[0].empty()) dialog_label(d, pop_.items[0], true, kMuted);
        break;
    }
    case Popup::None: break;
    }
    d.relayout();
}

void App::popup_refresh()
{
    switch (pop_.kind) {
    case Popup::Choice: {
        if (!pop_value_) return;
        const std::string &label = pop_.choice.label();
        lv_obj_set_style_text_font(pop_value_, label.size() > 14 ? f_body_ : f_title_, 0);
        lv_label_set_text(pop_value_, label.c_str());
        lv_label_set_text(pop_detail_, pop_.choice.detail().c_str());
        lv_label_set_text(pop_pos_, pop_.choice.position().c_str());
        const bool prev = pop_.choice.can_prev(), next = pop_.choice.can_next();
        set_color(lv_obj_get_child(pop_left_, 0), prev ? kText : kPressed);
        set_color(lv_obj_get_child(pop_right_, 0), next ? kText : kPressed);
        lv_obj_set_style_bg_color(pop_left_, lv_color_hex(prev ? kSelected : kSelectedDim), 0);
        lv_obj_set_style_bg_color(pop_right_, lv_color_hex(next ? kSelected : kSelectedDim), 0);
        if (dialog_) dialog_->relayout();
        break;
    }
    case Popup::Confirm: {
        if (!pop_yes_ || !pop_no_) return;
        lv_obj_set_style_border_width(pop_yes_, pop_.focus == 1 ? 3 : 0, 0);
        lv_obj_set_style_border_width(pop_no_, pop_.focus == 0 ? 3 : 0, 0);
        lv_obj_set_style_border_color(pop_yes_, lv_color_hex(kText), 0);
        lv_obj_set_style_border_color(pop_no_, lv_color_hex(kText), 0);
        const bool red = pop_.yes_text == "Remove" || pop_.yes_text.rfind("Delete", 0) == 0 || pop_.yes_delay_ms > 0 || pop_.danger;
        const int left = confirm_wait_left(pop_.opened_ms, mono_ms(), pop_.yes_delay_ms);
        if (red || left) lv_obj_set_style_bg_color(pop_yes_, lv_color_hex(left ? kSelectedDim : kDangerRed), 0);
        break;
    }
    case Popup::Menu:
    case Popup::List:
        for (size_t i = 0; i < pop_items_.size(); ++i) {
            const bool sel = static_cast<int>(i) == pop_.focus;
            lv_obj_set_style_bg_color(pop_items_[i], lv_color_hex(kSelected), 0);
            lv_obj_set_style_bg_opa(pop_items_[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        if (pop_.focus >= 0 && pop_.focus < static_cast<int>(pop_items_.size())) lv_obj_scroll_to_view_recursive(pop_items_[static_cast<size_t>(pop_.focus)], LV_ANIM_OFF);
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
    if (pop_.kind == Popup::Progress) {                 // Stop: the job ends after the contact in flight; the box closes when it has
        client_->cancel_bulk();
        return;
    }
    auto cb = pop_.on_cancel;
    close_popup();
    if (cb) cb();
}

void App::popup_yes()
{
    auto cb = pop_.on_yes;
    close_popup();
    if (cb) cb();
}

void App::popup_key(const KeyEvent &e)
{
    if (mono_ms() - pop_.opened_ms < 300 && e.key == Key::Enter) return;      // the Enter that opened it must not answer it
    switch (pop_.kind) {
    case Popup::Choice: {
        const ChoiceResult r = choice_key(e, pop_.choice);
        if (r == ChoiceResult::Moved) popup_refresh();
        else if (r == ChoiceResult::Accept) popup_accept();
        else if (r == ChoiceResult::Cancel) popup_cancel();
        break;
    }
    case Popup::Confirm: {
        const ConfirmResult r = confirm_key(e, pop_.focus);
        if (r == ConfirmResult::Moved) popup_refresh();
        else if (r == ConfirmResult::Yes) {
            if (confirm_wait_left(pop_.opened_ms, mono_ms(), pop_.yes_delay_ms) == 0) popup_yes();     // a Yes that must wait does not answer early
        } else if (r == ConfirmResult::No) popup_cancel();
        break;
    }
    case Popup::List: {
        const ListResult r = list_key(e, pop_.focus, static_cast<int>(pop_.items.size()));
        if (r == ListResult::Moved) {
            popup_refresh();
        } else if (r == ListResult::Accept) {
            popup_accept();
        } else if (r == ListResult::Cancel) {
            popup_cancel();
        }
        break;
    }
    case Popup::Progress:
        if (!e.repeat && (e.key == Key::Esc || e.key == Key::Enter)) popup_cancel();                // Stop
        break;
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
        else popup_cancel();
        break;
    case kTagPopCancel:
    case kTagPopNo: popup_cancel(); break;
    case kTagPopYes:
        if (confirm_wait_left(pop_.opened_ms, mono_ms(), pop_.yes_delay_ms) == 0) popup_yes();
        break;
    default: break;
    }
}

/* A tap on a row of a menu or a pick list: it is the answer. */
void App::popup_pick(int index)
{
    if (!nav_.popup_open || pop_dirty_ || (pop_.kind != Popup::Menu && pop_.kind != Popup::List)) return;
    if (mono_ms() - pop_.opened_ms < 150 || index < 0 || index >= static_cast<int>(pop_.items.size())) return;
    pop_.focus = index;
    popup_accept();
}

/* ================================================================== editor */

void App::open_editor(EditKind kind, const std::string &title, const std::string &hint, const std::string &initial, int row)
{
    if (editor_open_) return;
    if (mono_ms() - start_ms_ < kStartGuardMs) return;
    if (!keyboard_ready()) {                       // no dead editor: say what is missing
        notice("Keyboard needed: wake the Bluetooth keyboard", false);
        if (kind == EditKind::Clock) client_->clock_skip();
        return;
    }
    edit_kind_ = kind;
    edit_row_ = row;
    edit_mode_ = kind == EditKind::Number ? EditMode::Number : kind == EditKind::Clock || kind == EditKind::DateRange ? EditMode::Clock
                 : kind == EditKind::Channel ? EditMode::Channel : kind == EditKind::PrivKey ? EditMode::HexKey : EditMode::Name;
    edit_limit_ = kind == EditKind::Name || kind == EditKind::PrivName || kind == EditKind::RandName ? 31 : kind == EditKind::Channel ? 30
                  : kind == EditKind::PrivKey ? 39 : kind == EditKind::GroupNew || kind == EditKind::GroupRename ? 24 : kind == EditKind::DateRange ? 21 : 16;
    {   // the dialog: title, the hint, the field and a status line (an error or a note), Cancel and OK
        cp0::ui::DialogSpec spec;
        spec.title = title;
        spec.page = shell_.get();
        spec.message_line = true;
        spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { if (editor_open_) close_editor(false); }); }});
        spec.buttons.push_back({"OK", cp0::ui::ButtonRole::Primary, [this] { post([this] { if (editor_open_) close_editor(true); }); }});
        ed_dialog_ = std::make_unique<cp0::ui::Dialog>(shell_->root(), layout_, std::move(spec));
        if (!hint.empty()) dialog_label(*ed_dialog_, hint, true, kMuted);
        ed_ta_ = make_field(ed_dialog_->body(), 0, 200);
        lv_textarea_set_text(ed_ta_, initial.c_str());
        lv_textarea_set_cursor_pos(ed_ta_, LV_TEXTAREA_CURSOR_LAST);
        ed_dialog_->relayout();
    }
    editor_open_ = true;
    nav_.editor_open = true;
    edit_open_ms_ = mono_ms();
    edit_fresh_ = kind == EditKind::Number || kind == EditKind::Clock;
    if (kind == EditKind::PrivKey) editor_note("0 of 32 hex characters");
    invalidate();
}

void App::editor_error(const std::string &text)
{
    if (ed_dialog_) ed_dialog_->set_message(text, true);
}

void App::editor_note(const std::string &text)
{
    if (ed_dialog_) ed_dialog_->set_message(text, false);
}

void App::close_editor(bool accept)
{
    if (!editor_open_) return;
    if (edit_kind_ == EditKind::Position) {
        close_position_box(accept);
        return;
    }
    const std::string text = lv_textarea_get_text(ed_ta_);
    bool chain_key = false;
    if (accept) {
        if (edit_kind_ == EditKind::Name) {
            const std::string bad = validate_name(text);
            if (!bad.empty()) { editor_error(bad); return; }
            edit_.name = text;
            dirty_radio_ = true;
        } else if (edit_kind_ == EditKind::Number) {
            const std::string bad = apply_number(edit_, edit_row_, text);
            if (!bad.empty()) { editor_error(bad); return; }
            dirty_radio_ = true;
        } else if (edit_kind_ == EditKind::Channel) {
            std::string name = text;
            if (name.empty() || name[0] != '#') name = "#" + name;
            std::string bad = validate_hashtag(name);
            if (bad.empty() && !client_->add_hashtag_channel(name)) bad = client_->notice().text;
            if (!bad.empty()) { editor_error(bad); return; }
        } else if (edit_kind_ == EditKind::PrivName) {
            const std::string bad = validate_private_name(text);
            if (!bad.empty()) { editor_error(bad); return; }
            pending_name_ = text;
            chain_key = true;                                  // the key is asked next
        } else if (edit_kind_ == EditKind::PrivKey) {
            ChannelSecret key{};
            std::string bad = parse_channel_key(text, key);
            if (bad.empty() && !client_->add_private_channel(pending_name_, key)) bad = client_->notice().text;
            if (!bad.empty()) { editor_error(bad); return; }
        } else if (edit_kind_ == EditKind::RandName) {
            std::string bad = validate_private_name(text);
            ChannelSecret key{};
            if (bad.empty() && !random_channel_key(key)) bad = "No random numbers available";
            if (bad.empty() && !client_->add_private_channel(text, key)) bad = client_->notice().text;
            if (!bad.empty()) { editor_error(bad); return; }
            share_pending_ = true;                             // the key is shown once the board has the channel
            share_name_ = text;
            share_key_ = key;
            share_deadline_ = mono_ms() + 8000;
        } else if (edit_kind_ == EditKind::Clock) {
            uint32_t ts = 0;
            if (!parse_local_datetime(text, ts)) { editor_error("Use YYYY-MM-DD HH:MM (2025 or later)"); return; }
            client_->clock_set_user(ts);
        } else if (edit_kind_ == EditKind::GroupNew) {
            const std::string bad = model_.groups_mut().add_group(text);
            if (!bad.empty()) { editor_error(bad); return; }
            if (!group_keys_.empty()) model_.groups_mut().add_members(text, group_keys_);
            model_.groups_changed();
            notice("Group " + text + " made" + (group_keys_.empty() ? "" : " with " + std::to_string(group_keys_.size()) + (group_keys_.size() == 1 ? " contact" : " contacts")), true);
            group_keys_.clear();
            if (nav_.select_mode) leave_select_mode();
            csnap_valid_ = false;
        } else if (edit_kind_ == EditKind::GroupRename) {
            const std::string bad = model_.groups_mut().rename_group(group_target_, text);
            if (!bad.empty()) { editor_error(bad); return; }
            if (cview_.group == group_target_) cview_.group = text;
            model_.groups_changed();
            notice("Group renamed to " + text, true);
            csnap_valid_ = false;
        } else if (edit_kind_ == EditKind::DateRange) {
            uint32_t from = 0, to = 0;
            const std::string bad = parse_date_range(text, from, to);
            if (!bad.empty()) { editor_error(bad); return; }
            search_.date = DatePreset::Custom;
            search_.from = from;
            search_.to = to;
            refresh_search_results();
        }
    } else if (edit_kind_ == EditKind::Clock) {
        client_->clock_skip();                      // Esc: the board clock stays as it is
    } else if (edit_kind_ == EditKind::GroupNew) {
        group_keys_.clear();
    }
    editor_open_ = false;
    nav_.editor_open = false;
    ed_ta_ = nullptr;
    ed_dialog_.reset();
    invalidate();
    if (chain_key) {
        open_editor(EditKind::PrivKey, "Key of " + pending_name_, "Type the 32 hex characters of the key (0-9, a-f). Spaces and dashes are ignored.", "");
    }
}

void App::editor_key(const KeyEvent &e)
{
    if (edit_kind_ == EditKind::Position) {
        position_key(e);
        return;
    }
    switch (e.key) {
    case Key::Esc:
        if (!e.repeat) close_editor(false);
        break;
    case Key::Enter:
        if (!e.repeat && mono_ms() - edit_open_ms_ >= 350) close_editor(true);     // an Enter already on its way must not accept
        break;
    case Key::Backspace: edit_fresh_ = false; lv_textarea_delete_char(ed_ta_); break;
    case Key::Delete: edit_fresh_ = false; lv_textarea_delete_char_forward(ed_ta_); break;
    case Key::Left: edit_fresh_ = false; lv_textarea_cursor_left(ed_ta_); break;
    case Key::Right: edit_fresh_ = false; lv_textarea_cursor_right(ed_ta_); break;
    case Key::Home: edit_fresh_ = false; lv_textarea_set_cursor_pos(ed_ta_, 0); break;
    case Key::End: edit_fresh_ = false; lv_textarea_set_cursor_pos(ed_ta_, LV_TEXTAREA_CURSOR_LAST); break;
    case Key::Char: {
        if (e.ctrl || e.alt) break;
        const char *cur = lv_textarea_get_text(ed_ta_);
        const std::string add = filter_typed(edit_mode_, edit_fresh_ ? "" : (cur ? cur : ""), e.text, edit_limit_);
        if (!add.empty()) {
            if (edit_fresh_) lv_textarea_set_text(ed_ta_, "");      // like a selected text: the first character replaces it
            edit_fresh_ = false;
            lv_textarea_add_text(ed_ta_, add.c_str());
        }
        if (ed_dialog_) ed_dialog_->set_message("", false);
        break;
    }
    default: break;
    }
    if (edit_kind_ == EditKind::PrivKey && (e.key == Key::Char || e.key == Key::Backspace || e.key == Key::Delete)) {
        const int n = channel_key_digits(lv_textarea_get_text(ed_ta_));
        editor_note(std::to_string(n) + " of 32 hex characters");
    }
}

/* ================================================================== the Position box (D6) */

void App::open_position_box()
{
    if (editor_open_) return;
    if (mono_ms() - start_ms_ < kStartGuardMs) return;
    if (!client_->ready()) { notice("Connect the radio first", false); return; }
    client_->refresh_self();                                  // the position as the board holds it now (the box shows it when it comes)
    const auto &self = model_.self();
    const bool has = self && position_is_set(self->lat, self->lon);
    const cp0_ui_metrics_t *m = cp0_ui_metrics_get();
    {   // the dialog: the position on the board, the GPS switch, the latitude and the longitude side by side, the privacy line, a status line
        cp0::ui::DialogSpec spec;
        spec.title = "Position";
        spec.page = shell_.get();
        spec.message_line = true;
        spec.buttons.push_back({"Cancel", cp0::ui::ButtonRole::Cancel, [this] { post([this] { if (editor_open_) close_position_box(false); }); }});
        spec.buttons.push_back({"Clear", cp0::ui::ButtonRole::Normal, [this] { post([this] { if (editor_open_ && edit_kind_ == EditKind::Position) position_clear(); }); }});
        spec.buttons.push_back({"OK", cp0::ui::ButtonRole::Primary, [this] { post([this] { if (editor_open_) close_position_box(true); }); }});
        ed_dialog_ = std::make_unique<cp0::ui::Dialog>(shell_->root(), layout_, std::move(spec));
        cp0::ui::Dialog &d = *ed_dialog_;
        const int cw = d.geometry().content_w, gap = m->tok[CP0_TOK_SPACE_M];
        pos_now_ = dialog_label(d, "", true, kMuted);
        pos_gps_ = lv_button_create(d.body());
        if (lv_obj_get_group(pos_gps_)) lv_group_remove_obj(pos_gps_);
        lv_obj_remove_flag(pos_gps_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_size(pos_gps_, cw, m->tok[CP0_TOK_TARGET]);
        lv_obj_set_style_radius(pos_gps_, layout_.esc_radius, 0);
        lv_obj_set_style_bg_color(pos_gps_, lv_color_hex(kSelected), 0);
        lv_obj_set_style_bg_opa(pos_gps_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(pos_gps_, lv_color_hex(kPressed), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(pos_gps_, 0, 0);
        lv_obj_add_event_cb(pos_gps_, trampoline_button, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(kTagPosGps)));
        pos_gps_label_ = lv_label_create(pos_gps_);
        lv_obj_set_style_text_font(pos_gps_label_, f_body_, 0);
        lv_label_set_text(pos_gps_label_, "");
        lv_obj_center(pos_gps_label_);
        lv_obj_t *cols = lv_obj_create(d.body());
        lv_obj_remove_style_all(cols);
        lv_obj_remove_flag(cols, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(cols, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(cols, cw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(cols, gap, 0);
        static const char *const names[2] = {"Latitude", "Longitude"};
        for (int i = 0; i < 2; ++i) {
            lv_obj_t *col = lv_obj_create(cols);
            lv_obj_remove_style_all(col);
            lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(col, (cw - gap) / 2, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(col, m->tok[CP0_TOK_SPACE_S], 0);
            pos_label_[i] = lv_label_create(col);
            lv_obj_set_style_text_font(pos_label_[i], f_caption_, 0);
            set_color(pos_label_[i], kMuted);
            lv_label_set_text(pos_label_[i], names[i]);
            pos_ta_[i] = make_field(col, i == 0 ? kTagPosLat : kTagPosLon, 24);
        }
        pos_hint_ = dialog_label(d, "", true, kMuted);
        pos_clear_ = dialog_button(d, 1);
    }
    lv_textarea_set_text(pos_ta_[0], has ? fmt_coordinate(self->lat).c_str() : "");
    lv_textarea_set_text(pos_ta_[1], has ? fmt_coordinate(self->lon).c_str() : "");
    for (lv_obj_t *ta : pos_ta_) lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    pos_fresh_[0] = pos_fresh_[1] = has;
    edit_kind_ = EditKind::Position;
    editor_open_ = true;
    nav_.editor_open = true;
    edit_open_ms_ = mono_ms();
    pos_gps_shown_ = position_box_has_gps(model_.device() ? &*model_.device() : nullptr, model_.caps());
    pos_err_text_.clear();
    position_layout(pos_gps_shown_);
    position_focus(kPosLat, false);
    // no keyboard: the box still opens (the GPS switch and Clear work by touch), and says what is missing to type
    position_error(keyboard_ready() ? "" : "Keyboard needed: wake the Bluetooth keyboard", true);
    position_refresh();
    invalidate();
}

/* The rows of the box; without the GPS switch its row is gone and the rest moves up. */
void App::position_layout(bool gps)
{
    set_hidden(pos_gps_, !gps);
    if (ed_dialog_) ed_dialog_->relayout();
    if (!gps && pos_focus_ == kPosGps) position_focus(kPosClear);
}

/* What the board reports (position, GPS on / off, location sharing) into the box; from render() while it is open. */
void App::position_refresh()
{
    if (!editor_open_ || edit_kind_ != EditKind::Position) return;
    const bool gps = position_box_has_gps(model_.device() ? &*model_.device() : nullptr, model_.caps());
    if (gps != pos_gps_shown_) {                              // the custom variables were read while the box is open
        pos_gps_shown_ = gps;
        position_layout(gps);
    }
    const auto &self = model_.self();
    const bool has = self && position_is_set(self->lat, self->lon);
    lv_label_set_text(pos_now_, ("On the board: " + (has ? fmt_position(true, self->lat, self->lon) : std::string("not set"))).c_str());
    const bool on = gps && model_.caps().gps_on;
    if (gps) lv_label_set_text(pos_gps_label_, on ? "Board GPS: On, tap to switch off" : "Board GPS: Off, tap to switch on");
    // the privacy line: setting the position sends no advert; the board puts it into its adverts only when its location policy shares it
    std::string hint = on ? "The board GPS is on: its fix replaces a typed position." : "Decimal degrees (48.8566 or 48,8566).";
    hint += self && self->adv_loc_policy == 0 ? " The board keeps its position out of its adverts (location sharing off)."
                                              : " Other nodes see this position when this node sends an advert.";
    lv_label_set_text(pos_hint_, hint.c_str());
    if (ed_dialog_) ed_dialog_->relayout();
}

/* Focus: a field (accent border and the cursor) or a button (a ring, as in the popups). */
void App::position_focus(int focus, bool scroll)
{
    pos_focus_ = focus;
    for (int i = 0; i < 2; ++i) {
        const bool f = focus == i;
        lv_obj_set_style_border_color(pos_ta_[i], lv_color_hex(f ? kAccent : kPressed), 0);
        if (f) lv_obj_add_state(pos_ta_[i], LV_STATE_FOCUSED);
        else lv_obj_remove_state(pos_ta_[i], LV_STATE_FOCUSED);
    }
    lv_obj_set_style_border_width(pos_gps_, focus == kPosGps ? 3 : 0, 0);
    lv_obj_set_style_border_color(pos_gps_, lv_color_hex(kText), 0);
    if (pos_clear_) {
        lv_obj_set_style_border_width(pos_clear_, focus == kPosClear ? 3 : 0, 0);
        lv_obj_set_style_border_color(pos_clear_, lv_color_hex(kText), 0);
    }
    lv_obj_t *target = focus == kPosLat ? pos_ta_[0] : focus == kPosLon ? pos_ta_[1] : focus == kPosGps ? pos_gps_ : nullptr;
    if (scroll && target && !lv_obj_has_flag(target, LV_OBJ_FLAG_HIDDEN)) lv_obj_scroll_to_view_recursive(target, LV_ANIM_OFF);
}

void App::position_error(const std::string &text, bool error)
{
    pos_err_text_ = text;
    if (ed_dialog_) ed_dialog_->set_message(text, error);
}

/* OK / Enter (apply: checked first, nothing is sent on a bad value), Cancel / Esc (apply false), or Clear (0, 0). */
void App::close_position_box(bool apply)
{
    if (!editor_open_ || edit_kind_ != EditKind::Position) return;
    if (apply) {
        double lat = 0, lon = 0;
        const std::string bad = parse_position(lv_textarea_get_text(pos_ta_[0]), lv_textarea_get_text(pos_ta_[1]), lat, lon);
        if (!bad.empty()) {
            position_error(bad, true);
            position_focus(bad.find("atitude") != std::string::npos ? kPosLat : kPosLon);
            return;
        }
        if (!client_->set_position(lat, lon)) { position_error(client_->notice().text, true); return; }
    }
    editor_open_ = false;
    nav_.editor_open = false;
    pos_now_ = pos_gps_ = pos_gps_label_ = pos_hint_ = pos_clear_ = nullptr;
    pos_label_[0] = pos_label_[1] = pos_ta_[0] = pos_ta_[1] = nullptr;
    ed_dialog_.reset();
    invalidate();
}

void App::position_clear()
{
    const auto &self = model_.self();
    if (!self || !position_is_set(self->lat, self->lon)) { position_error("No position to clear: the board has none", true); return; }
    if (!client_->set_position(0, 0)) { position_error(client_->notice().text, true); return; }
    close_position_box(false);
}

void App::position_key(const KeyEvent &e)
{
    switch (e.key) {
    case Key::Esc:
        if (!e.repeat) close_position_box(false);
        return;
    case Key::Enter:
        if (e.repeat || mono_ms() - edit_open_ms_ < 350) return;          // an Enter already on its way must not apply
        if (pos_focus_ == kPosGps) client_->set_board_gps(!model_.caps().gps_on);
        else if (pos_focus_ == kPosClear) position_clear();
        else close_position_box(true);
        return;
    case Key::Tab:
    case Key::BackTab: {
        if (e.repeat) return;
        std::vector<int> order = {kPosLat, kPosLon};
        if (pos_gps_shown_) order.push_back(kPosGps);
        order.push_back(kPosClear);
        const int n = static_cast<int>(order.size());
        int at = static_cast<int>(std::find(order.begin(), order.end(), pos_focus_) - order.begin());
        if (at >= n) at = 0;
        position_focus(order[static_cast<size_t>((at + (e.key == Key::Tab ? 1 : n - 1)) % n)]);
        return;
    }
    default: break;
    }
    if (pos_focus_ != kPosLat && pos_focus_ != kPosLon) return;
    const int i = pos_focus_;
    lv_obj_t *ta = pos_ta_[i];
    switch (e.key) {
    case Key::Backspace: pos_fresh_[i] = false; lv_textarea_delete_char(ta); break;
    case Key::Delete: pos_fresh_[i] = false; lv_textarea_delete_char_forward(ta); break;
    case Key::Left: pos_fresh_[i] = false; lv_textarea_cursor_left(ta); break;
    case Key::Right: pos_fresh_[i] = false; lv_textarea_cursor_right(ta); break;
    case Key::Home: pos_fresh_[i] = false; lv_textarea_set_cursor_pos(ta, 0); break;
    case Key::End: pos_fresh_[i] = false; lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); break;
    case Key::Char: {
        if (e.ctrl || e.alt) break;
        const char *cur = lv_textarea_get_text(ta);
        const std::string add = filter_typed(EditMode::Number, pos_fresh_[i] ? "" : (cur ? cur : ""), e.text, 16);     // digits, '.', '-'; ',' becomes '.'
        if (!add.empty()) {
            if (pos_fresh_[i]) lv_textarea_set_text(ta, "");    // like a selected text: the first character replaces it
            pos_fresh_[i] = false;
            lv_textarea_add_text(ta, add.c_str());
        }
        position_error("", true);
        break;
    }
    default: break;
    }
}

/* ================================================================== actions */

void App::select_chat_row(int index)
{
    if (index < 0 || index >= static_cast<int>(chat_rows_.size())) return;
    const ChatRow &r = chat_rows_[static_cast<size_t>(index)];
    if (r.kind == ChatRow::Header) return;
    chat_sel_ = index;
    chat_sel_key_ = r.key;
    if (r.kind != ChatRow::Add) {
        if (r.key != conv_) focus_seq_ = 0;
        conv_ = r.key;
        model_.mark_read(conv_);
    }
    invalidate();
}

void App::open_conversation(const std::string &conv, bool focus_compose)
{
    select_tab(Tab::Chats);
    chat_rows_ = build_chat_rows(model_, conv_.rfind("d:", 0) == 0 ? conv_ : std::string());
    int idx = find_chat_row(chat_rows_, conv);
    if (idx < 0) {                                  // a contact without messages yet: shown in the list once it has some
        conv_ = conv;
        chat_sel_key_ = conv;
        chat_sel_ = -1;
    } else {
        select_chat_row(idx);
    }
    conv_ = conv;
    nav_.compose_focus = focus_compose;
    invalidate();
}

void App::send_message()
{
    if (conv_.empty()) return;
    const std::string text = lv_textarea_get_text(compose_);
    if (text.empty()) return;
    uint32_t seq = 0;
    if (conv_.rfind("c:", 0) == 0) {
        seq = client_->send_channel(std::atoi(conv_.c_str() + 2), text);
    } else {
        KeyPrefix p{};
        const ContactRec *c = from_hex(conv_.substr(2), p.data(), 6) ? model_.find_by_prefix(p) : nullptr;
        seq = c ? client_->send_direct(c->key_hex(), text) : 0;
    }
    if (seq == 0) {                                 // refused: the text stays, the reason is in the notice
        if (notice_text_.empty()) notice("Message not sent", false);
        return;
    }
    lv_textarea_set_text(compose_, "");
    nav_.compose_focus = true;
    focus_seq_ = 0;                                  // a new message: the conversation shows its end again
    invalidate();
}

void App::open_detail(const std::string &key_hex)
{
    detail_key_ = key_hex;
    nav_.detail_open = true;
    invalidate();
}

void App::activate_contact(const std::string &key_hex)
{
    const ContactRec *rec = model_.find_contact(key_hex);
    if (!rec) return;
    const int idx = csnap_.index_of(key_hex);
    if (idx >= 0) {
        contact_sel_ = idx;
        contact_sel_key_ = key_hex;
    }
    if (opens_chat(rec->c.type)) open_conversation(Model::conv_direct(rec->prefix()), true);
    else open_detail(key_hex);
}

void App::settings_step(int index, int delta)
{
    if (index < 0 || index >= static_cast<int>(srows_.size()) || !client_->ready()) return;
    int row = -1;
    switch (srows_[static_cast<size_t>(index)].kind) {
    case SRowKind::Freq: row = kRowFreq; break;
    case SRowKind::Bw: row = kRowBw; break;
    case SRowKind::Sf: row = kRowSf; break;
    case SRowKind::Cr: row = kRowCr; break;
    case SRowKind::Tx: row = kRowTx; break;
    default: return;
    }
    adjust_setting(edit_, row, delta);
    dirty_radio_ = true;
    invalidate();
}

void App::settings_save()
{
    if (!dirty_radio_) return;
    if (client_->save_settings(edit_)) {
        dirty_radio_ = false;
        holding_ = true;                            // keep the edited values on screen until the radio confirmed
        hold_ms_ = mono_ms();
    }
    invalidate();
}

void App::settings_undo()
{
    edit_ = model_.radio_settings();
    dirty_radio_ = false;
    notice("Changes undone", true);
    invalidate();
}

void App::request_save()
{
    if (!client_->ready()) { notice("Connect the radio first", false); return; }
    if (!dirty_radio_) { notice("Nothing to save: no unsaved change", true); return; }
    const std::vector<std::string> lines = describe_radio_changes(model_.radio_settings(), edit_);
    open_confirm("Save to radio?", join_lines(lines) + "\n\nThe board applies these settings at once.", [this] { settings_save(); }, "Yes, save");
}

void App::request_undo()
{
    if (!dirty_radio_) { notice("Nothing to undo: no unsaved change", true); return; }
    const std::vector<std::string> lines = describe_radio_changes(model_.radio_settings(), edit_);
    open_confirm("Undo changes?", join_lines(lines) + "\n\nGo back to the settings the board reports?", [this] { settings_undo(); }, "Yes, undo");
}

void App::settings_choice(ChoiceField f)
{
    const bool local_field = f == ChoiceField::RetryAttempts || f == ChoiceField::ResetAfter || f == ChoiceField::AdvertEvery || f == ChoiceField::AdvertKind;
    if (!local_field && !client_->ready()) { notice("Connect the radio first", false); return; }
    open_choice(f);
}

void App::add_channel_menu()
{
    if (!client_->ready()) { notice("Connect the radio first", false); return; }
    const FeatureState fs = feature_state(Feature::ChannelAdmin, model_.device() ? &*model_.device() : nullptr, model_.caps());
    if (!fs.available) { notice(fs.notice, false); return; }
    open_menu("Add a channel", {"Hashtag channel (#name)", "Private channel: type its key", "Private channel: make a random key"},
              [this](int i) {
                  if (i == 0) open_editor(EditKind::Channel, "New hashtag channel", "Type the name, for example #hamradio (no spaces).", "#");
                  else if (i == 1) open_editor(EditKind::PrivName, "New private channel", "Name of the channel (up to 31 bytes). Its 32-hex key is asked next.", "");
                  else open_editor(EditKind::RandName, "New private channel", "Name of the channel. A random key is made and shown, to share as text.", "");
              });
}

void App::show_channel_key(int idx)
{
    const ChannelRec *c = model_.find_channel(idx);
    if (!c || c->empty) return;
    if (!c->has_secret) { notice("The key is read from the board: connect it first", false); return; }
    open_info("Key of " + c->name, channel_key_grouped(c->secret),
              std::string(channel_kind_name(c->kind())) + " channel in slot " + std::to_string(idx) +
                  ". Share the key as text: whoever adds a private channel with this key reads and writes in it. 32 hex characters: " + channel_key_hex(c->secret));
}

void App::confirm_remove_channel(int idx)
{
    const ChannelRec *c = model_.find_channel(idx);
    if (!c || c->empty || idx == 0) return;
    if (!client_->ready()) { notice("Connect the radio first", false); return; }
    const std::string name = c->name;
    open_confirm("Remove channel?", "Remove " + name + " from the board? The messages stay on the deck. Other people keep the channel.",
                 [this, idx] {
                     if (client_->remove_channel(idx) && conv_ == Model::conv_channel(idx)) {
                         conv_.clear();
                         chat_sel_key_.clear();
                         nav_.compose_focus = false;
                     }
                     invalidate();
                 },
                 "Remove");
}

void App::channel_menu(int idx)
{
    const ChannelRec *c = model_.find_channel(idx);
    if (!c || c->empty) return;
    const bool muted = model_.is_muted(Model::conv_channel(idx));
    open_menu(truncate_ellipsis(c->name, 24), {muted ? "Unmute" : "Mute", "Show the key", "Delete messages", "Remove"}, [this, idx](int i) {
        if (i == 0) toggle_mute(Model::conv_channel(idx));
        else if (i == 1) show_channel_key(idx);
        else if (i == 2) confirm_delete_conversation(Model::conv_channel(idx));
        else confirm_remove_channel(idx);
    });
}

void App::settings_activate(int index, int x)
{
    if (index < 0 || index >= static_cast<int>(srows_.size())) return;
    const SRowSpec r = srows_[static_cast<size_t>(index)];
    settings_sel_ = index;
    switch (r.kind) {
    case SRowKind::Header:
    case SRowKind::Info:
    case SRowKind::GpsNotice:
    case SRowKind::StatsNotice:
    case SRowKind::PathHashNotice:
    case SRowKind::RepeatNotice:
    case SRowKind::AutoAddNotice:
    case SRowKind::ChannelsNotice: break;
    case SRowKind::Position:
        self_refresh_ms_ = mono_ms();
        open_position_box();                                  // (it reads the position from the board again as it opens)
        break;
    case SRowKind::Stats: open_stats(); break;
    case SRowKind::PacketLog: open_packet_log(); break;
    case SRowKind::PathHash: settings_choice(ChoiceField::PathHash); break;
    case SRowKind::Repeat: ask_repeat(!(model_.device() && model_.device()->repeat)); break;
    case SRowKind::AdvertEvery: settings_choice(ChoiceField::AdvertEvery); break;
    case SRowKind::AdvertKind: settings_choice(ChoiceField::AdvertKind); break;
    case SRowKind::ManualAdd: client_->set_manual_add(!(model_.self() && model_.self()->manual_add_contacts)); break;
    case SRowKind::AutoAddTypes: autoadd_menu(); break;
    case SRowKind::Reboot: ask_reboot(); break;
    case SRowKind::FactoryReset: ask_factory_reset(); break;
    case SRowKind::Name: open_editor(EditKind::Name, "Node name", "The name other nodes see when you send an advert (up to 31 bytes).", edit_.name); break;
    case SRowKind::Preset: settings_choice(ChoiceField::Preset); break;
    case SRowKind::Bw: settings_choice(ChoiceField::Bandwidth); break;
    case SRowKind::Sf: settings_choice(ChoiceField::Sf); break;
    case SRowKind::Cr: settings_choice(ChoiceField::Cr); break;
    case SRowKind::Tx: settings_choice(ChoiceField::Tx); break;
    case SRowKind::RetryAttempts: settings_choice(ChoiceField::RetryAttempts); break;
    case SRowKind::ResetAfter: settings_choice(ChoiceField::ResetAfter); break;
    case SRowKind::Freq: {
        if (!client_->ready()) { notice("Connect the radio first", false); break; }
        char b[32];
        std::snprintf(b, sizeof(b), "%.3f", edit_.freq_mhz);
        open_editor(EditKind::Number, "Frequency (MHz)", "For example 869.525. The presets list the usual values; typed values from 137 to 2500 MHz are accepted.", b, kRowFreq);
        break;
    }
    case SRowKind::BoardGps: client_->set_board_gps(!model_.caps().gps_on); break;
    case SRowKind::SyncClock: client_->sync_clock(); break;
    case SRowKind::Channel: {
        // a tap on a button of the row acts at once; a tap on the name (or Enter) opens the menu of the channel
        if (x >= 344 && x < 434) toggle_mute(Model::conv_channel(r.arg));
        else if (x >= 440 && x < 510) show_channel_key(r.arg);
        else if (x >= 516) confirm_remove_channel(r.arg);
        else channel_menu(r.arg);
        break;
    }
    case SRowKind::AddChannel: add_channel_menu(); break;
    case SRowKind::AddPublic: client_->add_public_channel(); break;
    case SRowKind::HistoryAll: confirm_delete_all(); break;
    case SRowKind::HistoryOlder: choose_delete_older(); break;
    case SRowKind::HistoryNote: break;
    case SRowKind::HistoryForget: confirm_forget_board(); break;
    case SRowKind::HistoryOthers: confirm_forget_others(); break;
    case SRowKind::Undo: request_undo(); break;
    case SRowKind::Save: request_save(); break;
    }
}

/* ================================================================== phase 2 */

/* ---- contacts: select mode, bulk delete, groups, details */

void App::enter_select_mode()
{
    if (nav_.select_mode || nav_.detail_open || nav_.nearby_open || nav_.popup_open || editor_open_) return;
    nav_.select_mode = true;                                  // (groups work without the board; only the delete needs it)
    csel_.clear();
    if (csnap_valid_) build_contact_specs();
    invalidate();
}

void App::leave_select_mode()
{
    nav_.select_mode = false;
    csel_.clear();
    if (csnap_valid_) build_contact_specs();
    invalidate();
}

void App::toggle_selected(const std::string &key_hex)
{
    if (key_hex.empty() || !nav_.select_mode) return;
    csel_.toggle(key_hex);
    build_contact_specs();
    invalidate();
}

void App::open_select_menu()
{
    const SelectMenu m = select_menu_items();
    open_list("Select contacts", m.labels, 0, [this, m](int i) {
        if (i < 0 || i >= static_cast<int>(m.actions.size())) return;
        switch (m.actions[static_cast<size_t>(i)]) {
        case SelectAction::All: csel_.select_shown(csnap_.rows); break;
        case SelectAction::None: csel_.clear(); break;
        case SelectAction::Stale7: csel_.select_stale(csnap_.rows, 7); break;
        case SelectAction::Stale30: csel_.select_stale(csnap_.rows, 30); break;
        case SelectAction::NeverHeard: csel_.select_never_heard(csnap_.rows); break;
        case SelectAction::Invert: csel_.invert(csnap_.rows); break;
        }
        build_contact_specs();
        const size_t n = csel_.shown_selected(csnap_.rows).size();
        notice(std::to_string(n) + (n == 1 ? " contact marked" : " contacts marked"), true);
        invalidate();
    });
}

void App::confirm_bulk_delete()
{
    const BulkDeletePrompt p = bulk_delete_prompt(model_, csnap_.rows, csel_);
    if (p.empty) {
        notice("Mark the contacts to delete first", false);
        return;
    }
    if (!client_->ready()) {
        notice("Connect the radio first", false);
        return;
    }
    open_confirm(p.title, p.body,
                 [this] {
                     std::vector<PubKey> keys;
                     for (const std::string &k : csel_.shown_selected(csnap_.rows)) {
                         PubKey pk{};
                         if (from_hex(k, pk.data(), 32)) keys.push_back(pk);
                     }
                     if (log_) log_->line("contacts: bulk delete of " + std::to_string(keys.size()) + " asked");
                     if (client_->remove_contacts(keys)) open_progress("Deleting contacts");
                 },
                 "Delete " + std::to_string(p.count));
}

void App::open_group_filter()
{
    std::vector<std::string> labels = group_filter_labels(model_.groups());
    const size_t n_groups = model_.groups().groups().size();
    labels.push_back("+ New group...");
    if (n_groups) labels.push_back("Rename or delete a group...");
    int focus = 0;
    for (size_t i = 0; i < n_groups; ++i)
        if (model_.groups().groups()[i].name == cview_.group) focus = static_cast<int>(i) + 1;
    open_list("Show which group?", labels, focus, [this, n_groups](int i) {
        if (i == 0) {
            ContactView v = cview_;
            v.group.clear();
            change_contact_view(v);
        } else if (i >= 1 && i <= static_cast<int>(n_groups)) {
            ContactView v = cview_;
            v.group = model_.groups().groups()[static_cast<size_t>(i) - 1].name;
            change_contact_view(v);
        } else if (i == static_cast<int>(n_groups) + 1) {
            group_keys_.clear();
            open_editor(EditKind::GroupNew, "New group", "Name of the group (up to 24 characters). Add contacts to it with Select > Group.", "");
        } else {
            open_list("Which group?", group_pick_labels(model_.groups()), 0, [this](int k) {
                if (k < 0 || k >= static_cast<int>(model_.groups().groups().size())) return;
                const std::string name = model_.groups().groups()[static_cast<size_t>(k)].name;
                open_menu(truncate_ellipsis(name, 24), {"Rename", "Delete this group"}, [this, name](int j) {
                    if (j == 0) {
                        group_target_ = name;
                        open_editor(EditKind::GroupRename, "Rename group", "New name of the group (up to 24 characters).", name);
                    } else {
                        open_confirm("Delete group?", "Delete the group " + name + "? The contacts stay; only the group goes.", [this, name] {
                            if (model_.groups_mut().delete_group(name).empty()) {
                                model_.groups_changed();
                                if (cview_.group == name) {
                                    ContactView v = cview_;
                                    v.group.clear();
                                    change_contact_view(v);
                                }
                                notice("Group " + name + " deleted", true);
                            }
                        }, "Delete");
                    }
                });
            });
        }
    });
}

void App::ask_new_group(const std::vector<std::string> &keys)
{
    group_keys_ = keys;
    open_editor(EditKind::GroupNew, "New group", "Name of the group (up to 24 characters). The contacts you chose go into it.", "");
}

void App::open_group_assign()
{
    const std::vector<std::string> keys = csel_.shown_selected(csnap_.rows);
    if (keys.empty()) {
        notice("Mark the contacts first", false);
        return;
    }
    const GroupMenu m = group_assign_menu(model_.groups(), keys.size());
    open_menu("Groups", m.labels, [this, m](int i) {
        if (i >= 0 && i < static_cast<int>(m.actions.size())) group_pick(m.actions[static_cast<size_t>(i)]);
    });
}

void App::group_pick(GroupAction a)
{
    const std::vector<std::string> keys = csel_.shown_selected(csnap_.rows);
    if (keys.empty()) return;
    switch (a) {
    case GroupAction::NewWith: ask_new_group(keys); break;
    case GroupAction::RemoveAll: {
        const size_t n = model_.groups_mut().remove_everywhere(keys);
        if (n) model_.groups_changed();
        csnap_valid_ = false;
        notice(n ? "Taken out of all groups" : "They were in no group", true);
        break;
    }
    case GroupAction::AddTo:
    case GroupAction::RemoveFrom:
        open_list(a == GroupAction::AddTo ? "Add to which group?" : "Take out of which group?", group_pick_labels(model_.groups()), 0, [this, a, keys](int i) {
            if (i < 0 || i >= static_cast<int>(model_.groups().groups().size())) return;
            const std::string name = model_.groups().groups()[static_cast<size_t>(i)].name;
            const size_t n = a == GroupAction::AddTo ? model_.groups_mut().add_members(name, keys) : model_.groups_mut().remove_members(name, keys);
            if (n) model_.groups_changed();
            csnap_valid_ = false;
            notice(std::to_string(n) + (n == 1 ? " contact " : " contacts ") + (a == GroupAction::AddTo ? "added to " : "taken out of ") + name, true);
            leave_select_mode();
        });
        break;
    case GroupAction::Manage: break;
    }
}

/* The groups of one contact, from its detail panel: a tap flips the membership and the list opens again, so several can be set. */
void App::group_toggle_for(const std::string &key_hex)
{
    const ContactGroups &g = model_.groups();
    std::vector<std::string> labels;
    for (const ContactGroup &grp : g.groups())
        labels.push_back(std::string(g.is_member(grp.name, key_hex) ? "[x] " : "[ ] ") + truncate_ellipsis(grp.name, 24));
    labels.push_back("+ New group with this contact...");
    group_focus_ = std::clamp(group_focus_, 0, static_cast<int>(labels.size()) - 1);
    const size_t n = g.groups().size();
    open_list("Groups of this contact", labels, group_focus_, [this, key_hex, n](int i) {
        if (i >= 0 && i < static_cast<int>(n)) {
            const std::string name = model_.groups().groups()[static_cast<size_t>(i)].name;
            if (model_.groups().is_member(name, key_hex)) model_.groups_mut().remove_members(name, {key_hex});
            else model_.groups_mut().add_members(name, {key_hex});
            model_.groups_changed();
            csnap_valid_ = false;
            group_focus_ = i;
            group_toggle_for(key_hex);
        } else {
            ask_new_group({key_hex});
        }
    });
}

void App::open_contact_info(const std::string &key_hex)
{
    if (!model_.find_contact(key_hex)) return;
    open_detail(key_hex);
}

void App::delete_detail_contact()
{
    const ContactRec *r = model_.find_contact(detail_key_);
    if (!r) return;
    if (!client_->ready()) {
        notice("Connect the radio first", false);
        return;
    }
    const std::string name = r->c.name.empty() ? to_hex(r->prefix()) : r->c.name;
    PubKey key = r->c.key;
    open_confirm("Delete contact?", "Delete " + truncate_ellipsis(name, 30) + " from the radio board? It comes back only when its node adverts again. This cannot be undone.",
                 [this, key] {
                     if (client_->remove_contacts({key})) {
                         nav_.detail_open = false;
                         invalidate();
                     }
                 },
                 "Delete");
}

/* ---- nearby */

void App::open_nearby()
{
    if (nav_.popup_open || editor_open_ || nav_.select_mode) return;
    nav_.nearby_open = true;
    nearby_sel_ = 0;
    nearby_sel_key_.clear();
    nearby_sig_ = 0;
    invalidate();
}

void App::close_nearby()
{
    nav_.nearby_open = false;
    set_hidden(nearby_, true);
    invalidate();
}

void App::render_nearby()
{
    nearby_rows_ = build_nearby_rows(model_, nearby_show_ignored_);
    const size_t pending = model_.pending_count();
    std::string title = "Nearby nodes (" + std::to_string(nearby_rows_.size()) + ")";
    if (pending) title += "  " + std::to_string(pending) + " waiting";
    lv_label_set_text(nearby_title_, title.c_str());
    lv_label_set_text(nearby_scan_label_, client_->discover().active ? "Scanning" : "Scan");
    const size_t ign = ignored_nearby_count(model_);
    lv_label_set_text(nearby_ign_label_, nearby_show_ignored_ ? "Hide ign." : ign ? ("Ignored " + std::to_string(ign)).c_str() : "Ignored");
    // keep the selection on the same node while the list changes
    int sel = -1;
    for (size_t i = 0; i < nearby_rows_.size(); ++i)
        if (nearby_rows_[i].key_hex == nearby_sel_key_) sel = static_cast<int>(i);
    if (sel < 0) sel = nearby_rows_.empty() ? -1 : std::clamp(nearby_sel_, 0, static_cast<int>(nearby_rows_.size()) - 1);
    nearby_sel_ = std::max(sel, 0);
    nearby_sel_key_ = sel >= 0 ? nearby_rows_[static_cast<size_t>(sel)].key_hex : std::string();

    uint32_t sig = fnv_int(0, sel);
    sig = fnv_int(sig, nearby_show_ignored_ ? 1 : 0);
    for (const NearbyRow &r : nearby_rows_) {
        sig = fnv(sig, r.key_hex);
        sig = fnv(sig, r.title);
        sig = fnv(sig, r.detail);
        sig = fnv_int(sig, static_cast<int>(r.state));
        sig = fnv_int(sig, r.can_add ? 1 : 0);
    }
    if (sig != nearby_sig_ && !plat_.touching()) {
        nearby_sig_ = sig;
        std::vector<RowSpec> specs;
        for (const NearbyRow &r : nearby_rows_) {
            RowSpec s;
            s.h = 56;
            s.id = r.key_hex;
            const uint32_t state_color = r.state == NearbyState::Pending ? kAccent : r.state == NearbyState::New ? kBlue : kMuted;
            s.cells.push_back({r.title, r.state == NearbyState::Ignored ? kMuted : r.state == NearbyState::Pending ? kAccent : kText, 12, 290, LV_TEXT_ALIGN_LEFT, f_text_, 6});
            s.cells.push_back({r.type_name, r.type == advtype::kRepeater ? kBlue : kMuted, 306, 84, LV_TEXT_ALIGN_LEFT, f_small_, 10});
            s.cells.push_back({nearby_state_name(r.state), state_color, 392, 76, LV_TEXT_ALIGN_LEFT, f_small_, 10});
            s.cells.push_back({r.detail, kMuted, 12, 456, LV_TEXT_ALIGN_LEFT, f_small_, 34});
            if (r.state == NearbyState::Ignored) {
                s.cells.push_back({"Restore", kText, 472, 160, LV_TEXT_ALIGN_CENTER, f_ui_, 6, kSelected});
            } else if (r.state != NearbyState::Contact) {
                if (r.can_add) s.cells.push_back({"Add", kText, 472, 76, LV_TEXT_ALIGN_CENTER, f_ui_, 6, kOkGreen});
                s.cells.push_back({"Ignore", kText, 552, 80, LV_TEXT_ALIGN_CENTER, f_ui_, 6, kSelected});
            }
            specs.push_back(std::move(s));
        }
        nearby_list_.set(std::move(specs), sel, true);
    }
    if (nearby_rows_.empty()) {
        lv_label_set_text(nearby_empty_, !client_->ready() ? "Connect the radio board to hear nodes."
                          : nearby_show_ignored_ ? "Nothing here."
                          : "No node heard yet. Nodes that advertise in range show up here by themselves. Press Scan to ask the nodes in direct radio range to answer.");
        set_hidden(nearby_empty_, false);
    } else {
        set_hidden(nearby_empty_, true);
    }
}

void App::nearby_ignore(const std::string &key_hex)
{
    const bool now_ignored = !model_.is_ignored(key_hex);
    model_.set_ignored(key_hex, now_ignored);
    notice(now_ignored ? "Ignored: hidden from this list (the Ignored button shows them)" : "Restored", true);
    nearby_sig_ = 0;
    invalidate();
}

void App::nearby_activate(int index, int x)
{
    if (index < 0 || index >= static_cast<int>(nearby_rows_.size())) return;
    const NearbyRow &r = nearby_rows_[static_cast<size_t>(index)];
    nearby_sel_ = index;
    nearby_sel_key_ = r.key_hex;
    if (r.state == NearbyState::Ignored) {
        if (x >= 472) nearby_ignore(r.key_hex);
    } else if (r.state != NearbyState::Contact) {
        if (x >= 552) nearby_ignore(r.key_hex);
        else if (x >= 472 && r.can_add) client_->add_nearby(r.key_hex);
    }
    nearby_sig_ = 0;
    invalidate();
}

void App::key_nearby(const KeyEvent &e)
{
    const int n = static_cast<int>(nearby_rows_.size());
    auto move = [&](int to) {
        if (n == 0) return;
        nearby_sel_ = std::clamp(to, 0, n - 1);
        nearby_sel_key_ = nearby_rows_[static_cast<size_t>(nearby_sel_)].key_hex;
        nearby_list_.set_selected(nearby_sel_, true, true);
    };
    switch (e.key) {
    case Key::Up: move(nearby_sel_ - 1); break;
    case Key::Down: move(nearby_sel_ + 1); break;
    case Key::PageUp: move(nearby_sel_ - 5); break;
    case Key::PageDown: move(nearby_sel_ + 5); break;
    case Key::Home: move(0); break;
    case Key::End: move(n - 1); break;
    case Key::Enter:
        if (e.repeat || n == 0) break;
        {
            const NearbyRow &r = nearby_rows_[static_cast<size_t>(nearby_sel_)];
            if (r.state == NearbyState::Ignored) nearby_ignore(r.key_hex);
            else if (r.can_add) client_->add_nearby(r.key_hex);
            else notice(r.state == NearbyState::Contact ? "Already a contact" : "Cannot be added yet: wait for its next advert", r.state == NearbyState::Contact);
        }
        nearby_sig_ = 0;
        invalidate();
        break;
    case Key::Char: {
        if (e.ctrl || e.alt || e.repeat) break;
        const char c = e.text.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0]))) : 0;
        if (c == 'i' && n > 0) nearby_ignore(nearby_rows_[static_cast<size_t>(nearby_sel_)].key_hex);
        else if (c == 's') client_->start_discover();
        else if (c == 'v') {
            nearby_show_ignored_ = !nearby_show_ignored_;
            nearby_sig_ = 0;
            invalidate();
        }
        break;
    }
    default: break;
    }
}

void App::key_detail(const KeyEvent &e)
{
    if (e.repeat) return;
    if (e.key == Key::Enter) {
        const ContactRec *r = model_.find_contact(detail_key_);
        if (r && opens_chat(r->c.type)) open_conversation(Model::conv_direct(r->prefix()), true);
    } else if (e.key == Key::Delete) {
        delete_detail_contact();
    } else if (e.key == Key::Char && !e.ctrl && !e.alt && e.text.size() == 1) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0])));
        if (c == 'g') {
            group_focus_ = 0;
            group_toggle_for(detail_key_);
        } else if (c == 'd') {
            delete_detail_contact();
        }
    }
}

/* ---- message search */

void App::open_search()
{
    if (nav_.popup_open || editor_open_ || nav_.search_open || nav_.tab != Tab::Chats) return;
    nav_.search_open = true;
    nav_.compose_focus = false;
    lv_textarea_set_text(sr_query_, search_.text.c_str());
    search_sel_ = 0;
    refresh_search_results();
    invalidate();
}

void App::close_search()
{
    nav_.search_open = false;
    set_hidden(search_panel_, true);
    invalidate();
}

void App::refresh_search_results(bool to_top)
{
    search_rows_ = build_search_rows(model_, search_, &search_total_);
    search_sig_ = model_.next_seq() * 2654435761u ^ static_cast<uint32_t>(model_.message_count());
    std::vector<RowSpec> specs;
    for (const SearchRow &r : search_rows_) {
        RowSpec s;
        s.h = 56;
        s.id = std::to_string(r.seq);
        s.cells.push_back({r.head, r.outgoing ? kGreen : kBlue, 12, 610, LV_TEXT_ALIGN_LEFT, f_small_, 6});
        s.cells.push_back({r.snippet, kText, 12, 610, LV_TEXT_ALIGN_LEFT, f_text_, 26});
        specs.push_back(std::move(s));
    }
    search_sel_ = search_rows_.empty() ? -1 : std::clamp(search_sel_, 0, static_cast<int>(search_rows_.size()) - 1);
    search_list_.set(std::move(specs), search_sel_, true);
    if (to_top) search_list_.scroll_to_top();
}

void App::render_search()
{
    lv_label_set_text(sr_conv_label_, ("Chat: " + search_.conv_label(model_)).c_str());
    lv_label_set_text(sr_date_label_, search_.date_label().c_str());
    lv_label_set_text(sr_dir_label_, (std::string("Show: ") + search_dir_name(search_.dir)).c_str());
    if (search_sig_ != (model_.next_seq() * 2654435761u ^ static_cast<uint32_t>(model_.message_count()))) refresh_search_results(false);   // a message arrived or was deleted
    std::string c;
    if (search_.query(now_unix()).empty()) c = std::to_string(search_total_) + (search_total_ == 1 ? " message" : " messages") + " in the archive";
    else if (search_total_ == 0) c = "No message matches";
    else c = std::to_string(search_total_) + (search_total_ == 1 ? " message matches" : " messages match") +
             (search_total_ > search_rows_.size() ? ", the newest " + std::to_string(search_rows_.size()) + " are listed" : std::string());
    lv_label_set_text(sr_count_, c.c_str());
    // an active filter shows in gold
    auto tint = [&](lv_obj_t *label, bool on) {
        lv_obj_set_style_bg_color(lv_obj_get_parent(label), lv_color_hex(on ? kAccent : kSelected), 0);
        set_color(label, on ? kOnAccent : kText);
    };
    tint(sr_conv_label_, !search_.conv.empty());
    tint(sr_date_label_, search_.date != DatePreset::Any);
    tint(sr_dir_label_, search_.dir != SearchDir::Any);
}

void App::search_open_hit(int index)
{
    if (index < 0 || index >= static_cast<int>(search_rows_.size())) return;
    const SearchRow r = search_rows_[static_cast<size_t>(index)];
    close_search();
    open_conversation(r.conv, false);
    focus_conv_ = r.conv;
    focus_seq_ = r.seq;
    built_conv_.clear();
    built_sig_ = 0;
    invalidate();
}

void App::search_pick_conv()
{
    const auto convs = search_conversations(model_);
    std::vector<std::string> labels = {"All chats"};
    int focus = 0;
    for (size_t i = 0; i < convs.size(); ++i) {
        labels.push_back(truncate_ellipsis(convs[i].second, 30));
        if (convs[i].first == search_.conv) focus = static_cast<int>(i) + 1;
    }
    open_list("Search in which chat?", labels, focus, [this, convs](int i) {
        search_.conv = i <= 0 || i > static_cast<int>(convs.size()) ? std::string() : convs[static_cast<size_t>(i) - 1].first;
        refresh_search_results();
        invalidate();
    });
}

void App::search_pick_date()
{
    std::vector<std::string> labels;
    for (DatePreset p : {DatePreset::Any, DatePreset::Day, DatePreset::Week, DatePreset::Month, DatePreset::Quarter, DatePreset::Custom}) labels.push_back(date_preset_name(p));
    labels.back() = "Custom range...";
    open_list("Which dates?", labels, static_cast<int>(search_.date), [this](int i) {
        static const DatePreset presets[6] = {DatePreset::Any, DatePreset::Day, DatePreset::Week, DatePreset::Month, DatePreset::Quarter, DatePreset::Custom};
        if (i < 0 || i > 5) return;
        if (presets[i] != DatePreset::Any && now_unix() < kMinPlausibleTime) {
            notice("The clock is not set: dates cannot be told", false);
            return;
        }
        if (presets[i] == DatePreset::Custom) {
            open_editor(EditKind::DateRange, "Dates", "One day (2026-09-30) or a range (2026-09-01 2026-09-30), local time.", "");
            return;
        }
        search_.date = presets[i];
        refresh_search_results();
        invalidate();
    });
}

void App::search_edit(const KeyEvent &e)
{
    switch (e.key) {
    case Key::Backspace: lv_textarea_delete_char(sr_query_); break;
    case Key::Delete: lv_textarea_delete_char_forward(sr_query_); break;
    case Key::Left: lv_textarea_cursor_left(sr_query_); return;
    case Key::Right: lv_textarea_cursor_right(sr_query_); return;
    case Key::Char: {
        if (e.ctrl || e.alt) return;
        const char *cur = lv_textarea_get_text(sr_query_);
        const std::string add = filter_typed(EditMode::Free, cur ? cur : "", e.text, 60);
        if (add.empty()) return;
        lv_textarea_add_text(sr_query_, add.c_str());
        break;
    }
    default: return;
    }
    const char *now_text = lv_textarea_get_text(sr_query_);
    search_.text = now_text ? now_text : "";
    search_sel_ = 0;
    refresh_search_results();
    invalidate();
}

void App::key_search(const KeyEvent &e)
{
    const int n = static_cast<int>(search_rows_.size());
    auto move = [&](int to) {
        if (n == 0) return;
        search_sel_ = std::clamp(to, 0, n - 1);
        search_list_.set_selected(search_sel_, true, true);
    };
    if (e.key == Key::Char && e.ctrl && !e.alt && e.text.size() == 1) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0])));
        if (e.repeat) return;
        if (c == 'k') {
            search_pick_conv();
        } else if (c == 'd') {
            search_pick_date();
        } else if (c == 'r') {
            search_.cycle_dir();
            refresh_search_results();
            invalidate();
        }
        return;
    }
    switch (e.key) {
    case Key::Up: move(search_sel_ - 1); break;
    case Key::Down: move(search_sel_ + 1); break;
    case Key::PageUp: move(search_sel_ - 4); break;
    case Key::PageDown: move(search_sel_ + 4); break;
    case Key::Home: move(0); break;
    case Key::End: move(n - 1); break;
    case Key::Enter:
        if (!e.repeat) search_open_hit(search_sel_);
        break;
    default: search_edit(e); break;
    }
}

/* ---- statistics */

void App::open_stats()
{
    if (!client_->ready()) {
        notice("Connect the radio first", false);
        return;
    }
    if (model_.caps().stats == Cap::No) {
        notice("Firmware too old for this feature: Statistics", false);
        return;
    }
    nav_.stats_open = true;
    stats_ms_ = mono_ms();
    stats_sig_ = 0;
    client_->refresh_stats();
    invalidate();
}

void App::close_stats()
{
    nav_.stats_open = false;
    set_hidden(stats_panel_, true);
    invalidate();
}

void App::render_stats()
{
    const StatsView v = build_stats_view(model_.stats());
    for (int col = 0; col < 2; ++col) {
        const std::vector<StatLine> &lines = col == 0 ? v.left : v.right;
        for (int i = 0; i < 10; ++i) {
            const bool have = i < static_cast<int>(lines.size());
            lv_label_set_text(st_label_[col][i], have ? lines[static_cast<size_t>(i)].label.c_str() : "");
            lv_label_set_text(st_value_[col][i], have ? lines[static_cast<size_t>(i)].value.c_str() : "");
        }
    }
    lv_label_set_text(st_updated_, v.updated.c_str());
    lv_label_set_text(st_auto_label_, stats_auto_ ? "Auto: 5 s" : "Auto: off");
    lv_obj_set_style_bg_color(lv_obj_get_parent(st_auto_label_), lv_color_hex(stats_auto_ ? kOkGreen : kSelected), 0);
    std::string note;
    if (model_.caps().stats == Cap::No) note = "Firmware too old for this feature: Statistics";
    else if (!client_->ready()) note = "Not connected";
    else if (client_->stats_busy()) note = "Reading...";
    else if (!v.any) note = "No answer yet";
    lv_label_set_text(st_note_, note.c_str());
    set_color(st_note_, model_.caps().stats == Cap::No ? kWarn : kMuted);
}

void App::key_stats(const KeyEvent &e)
{
    if (e.repeat) return;
    const char c = e.key == Key::Char && !e.ctrl && !e.alt && e.text.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0]))) : 0;
    if (e.key == Key::Enter || c == 'r') {
        if (!client_->refresh_stats()) notice(client_->stats_busy() ? "Already reading..." : "Cannot read the statistics now", client_->stats_busy());
        stats_ms_ = mono_ms();
    } else if (c == 'a') {
        stats_auto_ = !stats_auto_;
        invalidate();
    }
}

/* ---- the board: auto-add types, reboot, factory reset, repeat */

void App::autoadd_menu()
{
    if (!client_->ready()) {
        notice("Connect the radio first", false);
        return;
    }
    if (!model_.caps().autoadd_known) {
        notice("The board has not told its auto-add types yet", false);
        return;
    }
    const AutoaddConfig cfg = model_.caps().autoadd_config;
    const std::vector<AutoAddItem> items = autoadd_items(cfg);
    std::vector<std::string> labels;
    for (const AutoAddItem &i : items) labels.push_back(std::string(i.on ? "[x] " : "[ ] ") + i.label);
    open_list("Added by itself in manual mode", labels, 0, [this, items, cfg](int i) {
        if (i < 0 || i >= static_cast<int>(items.size())) return;
        client_->set_autoadd_flags(autoadd_toggled(cfg.config, items[static_cast<size_t>(i)].flag));
    });
}

void App::ask_reboot()
{
    if (!client_->ready()) {
        notice("Connect the radio first", false);
        return;
    }
    open_confirm("Reboot the board?", "The board restarts. The USB link drops for a few seconds and Mesh Hop reconnects by itself. Nothing is erased.",
                 [this] { client_->reboot(); }, "Yes, reboot");
}

void App::ask_factory_reset()
{
    if (!client_->ready()) {
        notice("Connect the radio first", false);
        return;
    }
    open_confirm("Factory reset the board?",
                 "This erases the board's keys (its identity), its contacts and its channels and sets its radio back to the defaults. The messages, the groups and the settings of this deck stay. It cannot be undone.",
                 [this] {
                     open_confirm("Really erase the board?", "Last chance. The board loses its identity, its contacts and its channels now. Other people will see a new node.",
                                  [this] { client_->factory_reset(); }, "Yes, erase", "No, keep it", 3000, true);
                 },
                 "Continue", "No", 0, true);
}

void App::ask_repeat(bool on)
{
    if (!client_->ready()) {
        notice("Connect the radio first", false);
        return;
    }
    if (!on) {
        client_->set_client_repeat(false);
        return;
    }
    open_confirm("Switch repeat on?", "The board will forward the packets it hears for other nodes, like a repeater. That uses airtime of everyone on this frequency. Only the frequencies the board allows can use it.",
                 [this] { client_->set_client_repeat(true); }, "Yes, repeat");
}

/* Called every tick: the progress box of a bulk delete, the waiting Yes, the end of a scan, the automatic statistics. */
void App::tick_phase2(uint64_t now)
{
    if (pop_.kind == Popup::Progress && !pop_dirty_ && pop_progress_) {
        const Client::BulkProgress &b = client_->bulk();
        const size_t handled = b.done + b.failed;
        lv_label_set_text(pop_progress_, (std::to_string(handled) + " of " + std::to_string(b.total) + (b.cancelled && b.active ? "  (stopping)" : "")).c_str());
        lv_obj_set_width(pop_bar_fill_, b.total ? std::max<int>(1, static_cast<int>(pop_bar_w_ * handled / b.total)) : 1);
        if (!b.active) {                                                // done: the notice says what happened
            close_popup();
            if (nav_.select_mode) leave_select_mode();
            csnap_valid_ = false;
            invalidate();
        }
    }
    if (pop_.kind == Popup::Confirm && pop_.yes_delay_ms > 0 && !pop_dirty_ && pop_yes_) {
        const int left = confirm_wait_left(pop_.opened_ms, now, pop_.yes_delay_ms);
        lv_label_set_text(lv_obj_get_child(pop_yes_, 0), confirm_yes_text(pop_.yes_text, left).c_str());
        lv_obj_set_style_bg_color(pop_yes_, lv_color_hex(left ? kSelectedDim : kDangerRed), 0);
        set_color(lv_obj_get_child(pop_yes_, 0), left ? kMuted : kText);
    }
    if (client_->discover().finished_id != discover_seen_id_) {
        discover_seen_id_ = client_->discover().finished_id;
        nearby_sig_ = 0;
        invalidate();
    }
    if (nav_.stats_open && model_.caps().stats == Cap::No) {                // the board refused: back to Settings, where the row is now a notice
        close_stats();
        notice("Firmware too old for this feature: Statistics", false);
    }
    if (nav_.packet_log_open && client_->packet_log().revision() != log_rev_) invalidate();
    if (nav_.stats_open && client_->ready()) {
        if (stats_auto_ && now - stats_ms_ >= 5000 && !client_->stats_busy()) {
            stats_ms_ = now;
            client_->refresh_stats();
        }
        if (model_.revision() != stats_sig_) {
            stats_sig_ = model_.revision();
            invalidate();
        }
    }
}

/* ---- the packet log (D8) */

void App::open_packet_log()
{
    nav_.packet_log_open = true;
    log_rev_ = 0;
    log_built_sel_ = ~0ull;
    invalidate();
}

void App::close_packet_log()
{
    nav_.packet_log_open = false;
    set_hidden(log_panel_, true);
    invalidate();
}

void App::packet_log_toggle()
{
    PacketLog &pl = client_->packet_log();
    if (pl.capturing()) {
        pl.stop();
        notice("Packet capture stopped: " + std::to_string(pl.size()) + " packets kept", true);
    } else {
        pl.start();
        if (log_) log_->line("packet log: capture started");
        notice("Capturing the packets the board hears (memory only, the last 500)", true);
    }
    invalidate();
}

void App::packet_log_clear()
{
    client_->packet_log().clear();
    log_sel_abs_ = -1;
    notice("Packet log cleared", true);
    invalidate();
}

void App::packet_log_select(int64_t abs_index)
{
    const PacketLog &pl = client_->packet_log();
    if (pl.size() == 0) { log_sel_abs_ = -1; return; }
    const int64_t first = static_cast<int64_t>(pl.dropped());
    const int64_t last = first + static_cast<int64_t>(pl.size()) - 1;
    abs_index = std::clamp<int64_t>(abs_index, first, last);
    log_sel_abs_ = abs_index == last ? -1 : abs_index;          // on the newest row: keep following the new ones
    invalidate();
}

void App::render_packet_log()
{
    const PacketLog &pl = client_->packet_log();
    lv_label_set_text(log_status_, packet_log_status(pl).c_str());
    set_color(log_status_, pl.capturing() ? kAccent : kMuted);
    lv_label_set_text(log_toggle_label_, pl.capturing() ? "Stop" : "Start");
    lv_obj_set_style_bg_color(log_toggle_, lv_color_hex(pl.capturing() ? kOkGreen : kSelected), 0);

    const int64_t first = static_cast<int64_t>(pl.dropped());
    if (log_sel_abs_ >= 0 && (log_sel_abs_ < first || log_sel_abs_ >= first + static_cast<int64_t>(pl.size()))) log_sel_abs_ = -1;
    const int sel = pl.size() == 0 ? -1 : log_sel_abs_ < 0 ? static_cast<int>(pl.size()) - 1 : static_cast<int>(log_sel_abs_ - first);
    const uint64_t sel_key = static_cast<uint64_t>(sel + 1);
    if (pl.revision() != log_rev_) {
        if (plat_.touching()) return;                     // never rebuild a list under the finger
        log_rev_ = pl.revision();
        std::vector<RowSpec> specs;
        specs.reserve(pl.size());
        const int y = centre(kLogRowH, f_small_);
        for (size_t i = 0; i < pl.size(); ++i) {
            const PacketRow r = build_packet_row(model_, pl.at(i));
            RowSpec s;
            s.h = kLogRowH;
            s.id = std::to_string(first + static_cast<int64_t>(i));
            const std::string *v[8] = {&r.time, &r.route, &r.type, &r.hops, &r.snr, &r.rssi, &r.size, &r.channel};
            for (int c = 0; c < 8; ++c)
                s.cells.push_back({*v[c], c == 2 || c == 7 ? kText : kMuted, kLogCol[c][0], kLogCol[c][1], LV_TEXT_ALIGN_LEFT, f_small_, y});
            specs.push_back(std::move(s));
        }
        if (specs.empty()) {
            RowSpec s;
            s.h = 60;
            s.selectable = false;
            s.cells.push_back({pl.capturing() ? "Capturing: waiting for the board to hear a packet..." : "Not capturing. Space or Start records the packets the board hears.",
                               kMuted, 12, 616, LV_TEXT_ALIGN_LEFT, f_ui_, 8});
            s.cells.push_back({"Off at every start; kept in memory only, the last 500. Nothing is written to disk.", kMuted, 12, 616, LV_TEXT_ALIGN_LEFT, f_small_, 34});
            specs.push_back(std::move(s));
        }
        log_list_.set(std::move(specs), sel, true);
        log_built_sel_ = sel_key;
        log_list_.scroll_to_selected();
    } else if (sel_key != log_built_sel_) {
        log_built_sel_ = sel_key;
        log_list_.set_selected(sel, true, true);
    }
    if (sel >= 0) {
        const LogPacket &pk = pl.at(static_cast<size_t>(sel)).pkt;
        lv_label_set_text(log_detail_, packet_detail(pk).c_str());
        lv_label_set_text(log_hex_, packet_hex(pk.raw).c_str());
    } else {
        lv_label_set_text(log_detail_, "");
        lv_label_set_text(log_hex_, "");
    }
}

void App::key_packet_log(const KeyEvent &e)
{
    const PacketLog &pl = client_->packet_log();
    const int64_t first = static_cast<int64_t>(pl.dropped());
    const int64_t cur = log_sel_abs_ < 0 ? first + static_cast<int64_t>(pl.size()) - 1 : log_sel_abs_;
    switch (e.key) {
    case Key::Up: packet_log_select(cur - 1); break;
    case Key::Down: packet_log_select(cur + 1); break;
    case Key::PageUp: packet_log_select(cur - 7); break;
    case Key::PageDown: packet_log_select(cur + 7); break;
    case Key::Home: packet_log_select(first); break;
    case Key::End: packet_log_select(first + static_cast<int64_t>(pl.size())); break;
    case Key::Char: {
        if (e.ctrl || e.alt || e.repeat) break;
        const char c = e.text.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0]))) : 0;
        if (c == ' ' || c == 's') packet_log_toggle();
        else if (c == 'c') packet_log_clear();
        break;
    }
    default: break;
    }
}

/* ================================================================== events */

void App::button_clicked(int tag)
{
    if (mono_ms() - start_ms_ < kStartGuardMs) return;
    if (tag >= kTagPopLeft && tag < kTagPopItem + 10) {
        post([this, tag] { if (nav_.popup_open) popup_click(tag); });       // the dialog may be deleted by the answer: after this event
        return;
    }
    if (nav_.popup_open || view_pending_) return;
    if (tag >= kTagTab && tag < kTagTab + 5) {
        if (!editor_open_) select_tab(static_cast<Tab>(tag - kTagTab));
        return;
    }
    if (tag >= kTagHdr && tag < kTagHdr + 6) {
        static const SortKey keys[6] = {SortKey::Name, SortKey::Type, SortKey::Heard, SortKey::Snr, SortKey::Hops, SortKey::Distance};
        ContactView v = cview_;
        v.tap_column(keys[tag - kTagHdr]);
        change_contact_view(v);
        return;
    }
    if (tag >= kTagTypeChip && tag < kTagTypeChip + 5) {
        ContactView v = cview_;
        v.filter.type = static_cast<TypeFilter>(tag - kTagTypeChip);
        change_contact_view(v);
        return;
    }
    if (tag >= kTagAgeChip && tag < kTagAgeChip + 4) {
        ContactView v = cview_;
        v.filter.age = static_cast<AgeFilter>(tag - kTagAgeChip);
        change_contact_view(v);
        return;
    }
    switch (tag) {
    case kTagSend: send_message(); break;
    case kTagCompose:
        if (!conv_.empty()) {
            nav_.compose_focus = true;
            invalidate();
        }
        break;
    case kTagTitleHold: hold_.take_swallow(); break;       // a tap on the name does nothing
    case kTagAdvertFlood: client_->send_advert(true); break;
    case kTagAdvertZero: client_->send_advert(false); break;
    case kTagRefresh: client_->refresh_contacts(); break;
    case kTagSort: {
        ContactView v = cview_;
        v.cycle_sort();
        change_contact_view(v);
        break;
    }
    case kTagDetailMsg: {
        const ContactRec *r = model_.find_contact(detail_key_);
        if (r && opens_chat(r->c.type)) open_conversation(Model::conv_direct(r->prefix()), true);
        else notice("Repeaters and sensors take no text messages", false);
        break;
    }
    case kTagDetailClose:
        nav_.detail_open = false;
        invalidate();
        break;
    case kTagNearby: open_nearby(); break;
    case kTagSelect: enter_select_mode(); break;
    case kTagGroup: open_group_filter(); break;
    case kTagSelDone: leave_select_mode(); break;
    case kTagSelMenu: open_select_menu(); break;
    case kTagSelDelete: confirm_bulk_delete(); break;
    case kTagSelGroup: open_group_assign(); break;
    case kTagDetailGroups:
        group_focus_ = 0;
        group_toggle_for(detail_key_);
        break;
    case kTagDetailDelete: delete_detail_contact(); break;
    case kTagChatOptions:
        if (conv_.empty() || (chat_sel_ >= 0 && chat_sel_ < static_cast<int>(chat_rows_.size()) && chat_rows_[static_cast<size_t>(chat_sel_)].kind == ChatRow::Add))
            notice("Select a conversation first", false);
        else
            open_conversation_options(conv_);
        break;
    case kTagChatDetails: {
        KeyPrefix dp{};
        const ContactRec *dc = conv_.size() == 14 && from_hex(conv_.substr(2), dp.data(), 6) ? model_.find_by_prefix(dp) : nullptr;
        if (dc) {
            const std::string key = dc->key_hex();
            select_tab(Tab::Contacts);
            open_detail(key);
        }
        break;
    }
    case kTagNearbyClose: close_nearby(); break;
    case kTagNearbyScan: client_->start_discover(); break;
    case kTagNearbyIgnored:
        nearby_show_ignored_ = !nearby_show_ignored_;
        nearby_sig_ = 0;
        invalidate();
        break;
    case kTagSearchOpen: open_search(); break;
    case kTagSearchClose: close_search(); break;
    case kTagSearchConv: search_pick_conv(); break;
    case kTagSearchDate: search_pick_date(); break;
    case kTagSearchDir:
        search_.cycle_dir();
        refresh_search_results();
        invalidate();
        break;
    case kTagStatsClose: close_stats(); break;
    case kTagStatsRefresh:
        if (!client_->refresh_stats()) notice(client_->stats_busy() ? "Already reading..." : "Cannot read the statistics now", client_->stats_busy());
        stats_ms_ = mono_ms();
        break;
    case kTagStatsAuto:
        stats_auto_ = !stats_auto_;
        invalidate();
        break;
    case kTagLogToggle: packet_log_toggle(); break;
    case kTagLogClear: packet_log_clear(); break;
    case kTagLogClose: close_packet_log(); break;
    // the Position box (its widgets only act while it is open; its buttons are the dialog's)
    case kTagPosGps:
        if (editor_open_ && edit_kind_ == EditKind::Position) {
            position_focus(kPosGps);
            client_->set_board_gps(!model_.caps().gps_on);
        }
        break;
    case kTagPosLat:
    case kTagPosLon:
        if (editor_open_ && edit_kind_ == EditKind::Position) position_focus(tag == kTagPosLat ? kPosLat : kPosLon);
        break;
    default: break;
    }
}

void App::row_clicked(RowList *list, int index, int x)
{
    if (mono_ms() - start_ms_ < kStartGuardMs || editor_open_ || nav_.popup_open || view_pending_) return;
    if (list == &chat_list_) {
        if (hold_.take_swallow()) return;                 // the release of a completed 3 s hold is not a tap
        select_chat_row(index);
        nav_.compose_focus = false;
        if (index >= 0 && index < static_cast<int>(chat_rows_.size()) && chat_rows_[static_cast<size_t>(index)].kind == ChatRow::Add) add_channel_menu();
    } else if (list == &contact_list_) {
        // the row that was tapped, whatever the order it is shown in: its id is the contact key of THIS row
        const std::string key = contact_list_.id_at(index);
        if (!key.empty()) {
            if (nav_.select_mode) toggle_selected(key);                   // select mode: a tap marks the row
            else if (x >= 584) open_contact_info(key);                    // the "i" box at the right of the row: the details
            else activate_contact(key);
        }
    } else if (list == &nearby_list_) {
        nearby_activate(index, x);
    } else if (list == &search_list_) {
        search_open_hit(index);
    } else if (list == &settings_list_) {
        settings_activate(index, x);
        invalidate();
    } else if (list == &log_list_) {
        const std::string &id = log_list_.id_at(index);
        if (!id.empty()) packet_log_select(std::atoll(id.c_str()));
    }
}

void App::back()
{
    const BackAction a = back_action(nav_);
    switch (a) {
    case BackAction::ClosePopup: popup_cancel(); break;
    case BackAction::CancelEditor: close_editor(false); break;
    case BackAction::CloseSearch: close_search(); break;
    case BackAction::CloseStats: close_stats(); break;
    case BackAction::ClosePacketLog: close_packet_log(); break;
    case BackAction::CloseNearby: close_nearby(); break;
    case BackAction::ExitSelect: leave_select_mode(); break;
    case BackAction::CloseDetail:
        nav_.detail_open = false;
        invalidate();
        break;
    case BackAction::FocusList:
        nav_.compose_focus = false;
        invalidate();
        break;
    case BackAction::ExitHint: notice("Hold Esc 3 s to exit", true); break;
    }
}

/* The Esc button of the page header (touch): Back inside a sub-page, a detail, a box or an editor; at the root it asks first,
 * so that a touch-only board can leave the app, and a tap beside the button cannot close a running radio session. */
void App::header_back()
{
    post([this] {
        if (view_pending_) return;
        if (mono_ms() - start_ms_ < kStartGuardMs) return;
        if (back_action(nav_) == BackAction::ExitHint) ask_quit();
        else back();
    });
}

void App::ask_quit()
{
    if (nav_.popup_open || editor_open_) return;
    open_confirm("Quit Mesh Hop?", "The connection to the board closes and the scheduled advert stops. Messages and settings are kept.",
                 [] { cdk::request_quit(0); }, "Quit", "Cancel");
}

void App::scroll_messages(int dy)
{
    lv_obj_scroll_by(msgs_, 0, -dy, LV_ANIM_OFF);
}

void App::compose_edit(const KeyEvent &e)
{
    switch (e.key) {
    case Key::Backspace: lv_textarea_delete_char(compose_); break;
    case Key::Delete: lv_textarea_delete_char_forward(compose_); break;
    case Key::Left: lv_textarea_cursor_left(compose_); break;
    case Key::Right: lv_textarea_cursor_right(compose_); break;
    case Key::Home: lv_textarea_set_cursor_pos(compose_, 0); break;
    case Key::End: lv_textarea_set_cursor_pos(compose_, LV_TEXTAREA_CURSOR_LAST); break;
    case Key::Char: {
        if (e.ctrl || e.alt) break;
        const char *cur = lv_textarea_get_text(compose_);
        const std::string add = filter_typed(EditMode::Free, cur ? cur : "", e.text, static_cast<size_t>(max_compose_bytes()));
        if (!add.empty()) lv_textarea_add_text(compose_, add.c_str());
        break;
    }
    default: break;
    }
    invalidate();
}

void App::key_chats(const KeyEvent &e)
{
    const bool add_selected = chat_sel_ >= 0 && chat_sel_ < static_cast<int>(chat_rows_.size()) &&
                              chat_rows_[static_cast<size_t>(chat_sel_)].kind == ChatRow::Add;
    // Ctrl+M mutes / unmutes the channel (plain letters start a message, so the shortcut needs Ctrl)
    if (e.key == Key::Char && e.ctrl && !e.alt && (e.text == "m" || e.text == "M")) {
        if (!e.repeat && !add_selected && !conv_.empty()) toggle_mute(conv_);
        return;
    }
    // Ctrl+O opens the options of the conversation (mute, delete): the keyboard way of the 3 s hold on its name
    if (e.key == Key::Char && e.ctrl && !e.alt && (e.text == "o" || e.text == "O")) {
        if (e.repeat) return;
        if (add_selected || conv_.empty()) notice("Select a conversation first", false);
        else open_conversation_options(conv_);
        return;
    }
    // Ctrl+F opens the search in the message archive
    if (e.key == Key::Char && e.ctrl && !e.alt && (e.text == "f" || e.text == "F")) {
        if (!e.repeat) open_search();
        return;
    }
    if (nav_.compose_focus) {
        switch (e.key) {
        case Key::Enter:
            if (!e.repeat) send_message();
            return;
        case Key::Up: scroll_messages(40); return;
        case Key::Down: scroll_messages(-40); return;
        case Key::PageUp: scroll_messages(240); return;
        case Key::PageDown: scroll_messages(-240); return;
        default: compose_edit(e); return;
        }
    }
    switch (e.key) {
    case Key::Up:
    case Key::Down: {
        const int next = step_selection(chat_rows_, chat_sel_, e.key == Key::Up ? -1 : 1);
        if (next >= 0 && next != chat_sel_) select_chat_row(next);
        break;
    }
    case Key::PageUp: scroll_messages(240); break;
    case Key::PageDown: scroll_messages(-240); break;
    case Key::Enter:
    case Key::Right:
        if (e.repeat) break;
        if (add_selected) {
            add_channel_menu();
        } else if (!conv_.empty()) {
            nav_.compose_focus = true;
            invalidate();
        }
        break;
    case Key::Char: {
        if (e.ctrl || e.alt || add_selected || conv_.empty()) break;
        nav_.compose_focus = true;                  // typing starts the message
        compose_edit(e);
        break;
    }
    default: break;
    }
}

void App::key_contacts(const KeyEvent &e)
{
    if (nav_.detail_open) {
        key_detail(e);
        return;
    }
    if (nav_.nearby_open) {
        key_nearby(e);
        return;
    }
    if (view_pending_) return;
    const bool sel = nav_.select_mode;
    switch (e.key) {
    case Key::Up: move_contact_selection(-1, false); break;
    case Key::Down: move_contact_selection(1, false); break;
    case Key::PageUp: move_contact_selection(-6, false); break;
    case Key::PageDown: move_contact_selection(6, false); break;
    case Key::Home: move_contact_selection(0, true); break;
    case Key::End: move_contact_selection(static_cast<int>(csnap_.rows.size()) - 1, true); break;
    case Key::Enter:
        if (e.repeat) break;
        if (sel) toggle_selected(contact_sel_key_);
        else activate_contact(contact_sel_key_);
        break;
    case Key::Delete:
        if (sel && !e.repeat) confirm_bulk_delete();
        break;
    case Key::Char: {
        if (e.ctrl || e.alt || e.repeat) break;
        const char c = e.text.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0]))) : 0;
        ContactView v = cview_;
        if (sel && c == ' ') toggle_selected(contact_sel_key_);                    // select mode: Space (or Enter) marks the row
        else if (sel && c == 'a') { csel_.select_shown(csnap_.rows); build_contact_specs(); invalidate(); }
        else if (sel && c == 'n') { csel_.clear(); build_contact_specs(); invalidate(); }
        else if (sel && c == 'i') { csel_.invert(csnap_.rows); build_contact_specs(); invalidate(); }
        else if (sel && c == 'm') open_select_menu();
        else if (sel && c == 'd') confirm_bulk_delete();
        else if (sel && c == 'g') open_group_assign();
        else if (c == 's') { v.cycle_sort(); change_contact_view(v); }
        else if (c == 'r') { v.toggle_reverse(); change_contact_view(v); }
        else if (c == 't') { v.filter.type = next_type_filter(v.filter.type); change_contact_view(v); }
        else if (c == 'h') { v.filter.age = next_age_filter(v.filter.age); change_contact_view(v); }
        else if (!sel && c == 'a') client_->send_advert(true);
        else if (!sel && c == 'd') client_->send_advert(false);
        else if (!sel && c == 'u') client_->refresh_contacts();
        else if (!sel && c == 'x') enter_select_mode();
        else if (!sel && c == 'g') open_group_filter();
        else if (!sel && c == 'n') open_nearby();
        else if (!sel && c == 'i') open_contact_info(contact_sel_key_);
        break;
    }
    default: break;
    }
}
void App::key_settings(const KeyEvent &e)
{
    std::vector<bool> ok;
    for (const SRowSpec &r : srows_) ok.push_back(settings_row_selectable(r.kind));
    switch (e.key) {
    case Key::Up:
    case Key::Down:
    case Key::PageUp:
    case Key::PageDown:
    case Key::Home:
    case Key::End: {
        const int d = e.key == Key::Up ? -1 : e.key == Key::Down ? 1 : e.key == Key::PageUp ? -6 : e.key == Key::PageDown ? 6
                      : e.key == Key::Home ? -1000 : 1000;
        const int next = step_selectable(ok, settings_sel_, d);
        if (next >= 0) {
            settings_sel_ = next;
            settings_list_.set_selected(next, true, true);        // no rebuild of the list for a key press
        }
        break;
    }
    case Key::Left: settings_step(settings_sel_, -1); break;
    case Key::Right: settings_step(settings_sel_, 1); break;
    case Key::Enter:
        if (!e.repeat) settings_activate(settings_sel_, 0);
        break;
    case Key::Delete:
        if (!e.repeat && settings_sel_ >= 0 && settings_sel_ < static_cast<int>(srows_.size()) && srows_[static_cast<size_t>(settings_sel_)].kind == SRowKind::Channel)
            confirm_remove_channel(srows_[static_cast<size_t>(settings_sel_)].arg);
        break;
    case Key::Char: {
        if (e.ctrl || e.alt || e.repeat) break;
        const char c = e.text.size() == 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(e.text[0]))) : 0;
        const bool on_channel = settings_sel_ >= 0 && settings_sel_ < static_cast<int>(srows_.size()) && srows_[static_cast<size_t>(settings_sel_)].kind == SRowKind::Channel;
        if (c == 's') request_save();
        else if (c == 'v') request_undo();
        else if (c == 'a') client_->send_advert(true);
        else if (c == 'm' && on_channel) toggle_mute(Model::conv_channel(srows_[static_cast<size_t>(settings_sel_)].arg));
        else if (c == 'k' && on_channel) show_channel_key(srows_[static_cast<size_t>(settings_sel_)].arg);
        break;
    }
    default: break;
    }
}

void App::on_key(const KeyEvent &e)
{
    if (e.key == Key::None) return;
    if (view_pending_) return;                          // the "Loading..." popup is up for a moment
    if (nav_.popup_open) {
        popup_key(e);
        return;
    }
    if (editor_open_) {
        editor_key(e);
        return;
    }
    if (mono_ms() - start_ms_ < kStartGuardMs && (e.key == Key::Enter || e.key == Key::Esc)) return;
    // a short Esc of the keyboard is Back, never quit: the launcher ends the app itself after a 3 s hold (the header's Esc button
    // asks "Quit Mesh Hop?" at the root). The auto-repeat of a held Esc is ignored.
    if (e.key == Key::Esc) {
        if (!e.repeat) back();
        return;
    }
    // Tab walks the tabs in a loop (Shift+Tab backwards); touch goes straight to a tab. No other tab keys.
    if (e.key == Key::Tab || e.key == Key::BackTab) {
        if (!e.repeat) {
            const int dir = e.key == Key::Tab ? 1 : -1;
            Tab next = step_tab(nav_.tab, dir);
            while (next == Tab::Map || next == Tab::Terminal) next = step_tab(next, dir);       // not shown until they exist
            select_tab(next);
        }
        return;
    }
    switch (nav_.tab) {
    case Tab::Chats:
        if (nav_.search_open) key_search(e);
        else key_chats(e);
        break;
    case Tab::Contacts: key_contacts(e); break;
    case Tab::Settings:
        if (nav_.stats_open) key_stats(e);
        else if (nav_.packet_log_open) key_packet_log(e);
        else key_settings(e);
        break;
    default: break;
    }
}

} // namespace meshhop
