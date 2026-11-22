/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl UI: "Add a network folder" (0.8.0; the design boards Add-480x320 and Add-320x480, the 480x480 and 800x480 drawn as
 * the 480x320 one at their scale): on the dark panel of the list pages,
 *
 *   SERVER (IP OR NAME)    SHARE NAME            two columns on a landscape panel, one under the other in portrait
 *   FOLDER IN THE SHARE    NAME IN VINYL         (optional; the hint text in the field says what an empty one means)
 *   SIGN IN  [Guest] [User name and password]    two pills; the second one adds USER NAME and PASSWORD (masked)
 *   the status line                              "Read only. Vinyl checks the share before it adds it.", the progress,
 *                                                or why it was refused (amber)
 *   Enter: next field        [Cancel] [Check and add]
 *
 * The fields are the toolkit's TextField, as the Library's search field and the name dialog (its on-screen keyboard and the
 * writing band are the toolkit's and _common/osk_policy's: no change to the keyboard policy). Enter moves to the next
 * field; on the last one it ends the typing and the cursor sits on "Check and add". Esc while typing ends the typing; Esc
 * again leaves the page (the controller). Without typing, Up / Down move the cursor over the fields, the pills and the
 * buttons, Left / Right choose the sign-in, Enter acts. The fields and pills scroll in their area while the on-screen keyboard
 * is up. Draws only; LVGL thread only.
 */
#pragma once

#include "layout/layout.hpp"
#include "shares/share_helper.hpp"

#include "cp0_ui_roller.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <string>

namespace vinyl {
namespace ui {

struct ShareFormView {
    bool guest = true;
    std::string status;          /* "" : the page's own line */
    bool status_error = false;   /* amber */
    bool busy = false;           /* the helper runs: Check and add dimmed, nothing types */
};

struct ShareFormEvents {
    std::function<void(bool guest)> sign_in;
    std::function<void()> submit, cancel;
};

class ShareForm {
public:
    ShareForm(std::function<lv_obj_t *()> make_root, const cp0_ui_roller_page_t &tk, int screen_w, int screen_h, int body_y, int target,
              ShareFormEvents ev);
    ~ShareForm();
    ShareForm(const ShareForm &) = delete;
    ShareForm &operator=(const ShareForm &) = delete;

    lv_obj_t *root() const;
    /* a new form: every field empty, Guest, the server's field typing (the scenes: `prefill` for the screenshots) */
    void open(const shares::AddForm &prefill = shares::AddForm(), bool edit = true);
    void update(const ShareFormView &v);
    /* what was typed (the password copied: the caller wipes its copy) */
    shares::AddForm values() const;
    /* a field to fix: the cursor there, typing */
    void focus(shares::Field f);
    void end_edit();
    bool editing() const;
    /* keypad keys while nothing types (true: used) */
    bool key(uint32_t lv_key);
    /* the password field emptied (the form left, the share added) */
    void wipe();

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace ui
} // namespace vinyl
