/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader: the colours (the launcher's: cp0_ui_list.h and cp0_ui_osk.h, not an own palette), the button tags and the
 * small LVGL helpers the page files share. Internal to src/ui.
 */
#pragma once

#include "cp0_ui_list.h"
#include "cp0_ui_osk.h"
#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>

namespace rssui {

constexpr std::uint32_t kBackground = 0x000000;
constexpr std::uint32_t kPanel = CP0_UI_OSK_KEY_RGB;
constexpr std::uint32_t kSelected = CP0_UI_LIST_BAR_RGB;
constexpr std::uint32_t kPressed = CP0_UI_OSK_KEY_PRESSED_RGB;
constexpr std::uint32_t kText = CP0_UI_LIST_TEXT_RGB;
constexpr std::uint32_t kMuted = CP0_UI_LIST_MUTED_RGB;
constexpr std::uint32_t kAccent = CP0_UI_LIST_ACCENT_RGB;      /* unread, "N new", in use */
constexpr std::uint32_t kWarn = CP0_UI_LIST_WARN_RGB;          /* a feed that could not be updated */
constexpr std::uint32_t kOk = CP0_UI_LIST_OK_RGB;              /* "Found: ..." */
constexpr std::uint32_t kValue = CP0_UI_LIST_VALUE_RGB;        /* a setting's value (the roller's green) */
constexpr std::uint32_t kPrimary = CP0_UI_OSK_ENTER_RGB;       /* the primary button: the keyboard's Enter blue */
constexpr std::uint32_t kDangerRed = 0x7A2E24;
constexpr std::uint32_t kReadTitle = 0xB4B4B4;                 /* a read article / an all-read feed: grey, still readable */

enum Tag {
    kTagNone = 0,
    kTagPopLeft = 10, kTagPopRight, kTagPopOk, kTagPopCancel, kTagPopYes, kTagPopNo,
    kTagFavBar = 30, kTagSearchField, kTagAddField, kTagAddFind, kTagAddPin, kTagAddCancel,
};

inline void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

inline bool is_hidden(lv_obj_t *obj)
{
    return !obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

inline lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    return b;
}

inline int line_h(const lv_font_t *f)
{
    return f ? static_cast<int>(lv_font_get_line_height(f)) : 0;
}

inline int text_w(const std::string &text, const lv_font_t *f)
{
    if (text.empty() || !f) return 0;
    lv_point_t sz{};
    lv_text_get_size(&sz, text.c_str(), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

inline void set_color(lv_obj_t *obj, std::uint32_t color)
{
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

} // namespace rssui
