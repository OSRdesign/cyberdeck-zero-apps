/*
 * SPDX-License-Identifier: MIT
 *
 * When a list scrolls to its highlighted row by itself (owner's board test on the HackBerry, 2026-11-15: "while scrolling on a
 * long queue, when I stop the list reverts to the selected line, so I don't have time to read all the song names"). The cause:
 * every refresh that rebuilt the rows (a track change, the periodic snapshot) put the highlight back in view. The rule, shared by
 * every list page (ui/row_list.hpp):
 *   - the user is BROWSING from his last scroll (a drag, its momentum) until his next explicit action (a key, a tap on a row) or
 *     the page being opened again: the last user scroll is later than the last action or open;
 *   - the list follows the selection when the page opens, and on an explicit action (a key moved the highlight, a row was
 *     tapped), always; on a refresh (rows rebuilt by a track change, a rescan ...) only while the user is not browsing. A
 *     browsing user's list stays exactly where he left it; the highlight may change, the list does not move.
 * Pure: the caller passes a monotonic clock in ms (the LVGL tick; the tests a fake clock). LVGL-free.
 */
#pragma once

#include <cstdint>

namespace vinyl {

enum class FollowCause { Open, Action, Refresh };

class ScrollFollow {
public:
    /* the user scrolled the list (a drag or its momentum; never a scroll the app made) */
    void user_scrolled(uint64_t now_ms) { scrolled_ms_ = now_ms; seen_scroll_ = true; }
    /* an explicit action (a key moved the selection, a row was tapped) */
    void action(uint64_t now_ms) { acted_ms_ = now_ms; }
    /* the page was opened or its list replaced */
    void open(uint64_t now_ms) { acted_ms_ = now_ms; }
    bool browsing() const { return seen_scroll_ && scrolled_ms_ > acted_ms_; }
    /* does the list scroll to the selection now? (selection_changed: the highlighted row differs from the last refresh) */
    bool follow(FollowCause cause, bool selection_changed) const {
        (void)selection_changed;
        switch (cause) {
        case FollowCause::Open:
        case FollowCause::Action: return true;
        default: return !browsing();
        }
    }

private:
    uint64_t scrolled_ms_ = 0, acted_ms_ = 0;
    bool seen_scroll_ = false;
};

}  // namespace vinyl
