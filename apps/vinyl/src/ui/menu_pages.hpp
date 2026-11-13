/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: the Menu pages (P2a), drawn as the full-UX canvas draws them (Menu-480x320 / Menu-320x480,
 * SleepTimer-480x320, Playback-480x320, Keys-480x320 / Keys-320x480, About-480x320), in the list pages' look (the dark
 * panel of the Browser at the class's scale: core/layout menu_geom), under the launcher's header (the View's
 * cp0::ui::Page: the title, the Esc, the status strip):
 *
 *   Menu, Sleep timer, Playback, ReplayGain   rows of core/menu: section headings (grey capitals, never selected), Open
 *                                             (a chevron), Edit (the value, then a chevron), Choice (a white dot, the
 *                                             chosen one blue); the selected row on the selection grey; a tap acts on
 *                                             the first tap (as the Browser's and the Output page's rows); a strip at the
 *                                             bottom (the sleep timer's status, a line of help) that grows to its text
 *   Keys                                      two columns, the key on a key chip and what it does; it scrolls (a drag,
 *                                             Up / Down); nothing in it acts
 *   About                                     the name and version, a line, FORMATS, THIS OUTPUT, LIBRARIES, the licences'
 *                                             place; it scrolls
 *
 * Draws only: a tap goes to on_row(page, index); the controller decides. LVGL thread only.
 */
#pragma once

#include "layout/layout.hpp"
#include "view_model.hpp"

#include "lvgl/lvgl.h"

#include <functional>
#include <memory>

namespace vinyl {
namespace ui {

class MenuPages {
public:
    /* make_root: a page root of the View (full screen under the header, hidden at first) */
    MenuPages(std::function<lv_obj_t *()> make_root, int screen_w, int screen_h, int body_y, int list_row,
              std::function<void(Page, int)> on_row);
    ~MenuPages();
    MenuPages(const MenuPages &) = delete;
    MenuPages &operator=(const MenuPages &) = delete;

    static bool is_menu_page(Page p);
    lv_obj_t *root(Page p) const;
    void update(Page p, const MenuView &v);
    void set_keys(const std::vector<menu::KeyHelp> &rows);
    void set_about(const menu::About &a);
    /* Keys and About: scroll by `rows` rows (negative: up) */
    void scroll(Page p, int rows);
    /* where a notice may sit on the page: over its strip, or over the panel's bottom (screen px) */
    layout::R notice_room(Page p) const;
    int rows(Page p) const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace ui
}  // namespace vinyl
