/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the scroll-follow policy of the list pages (core/nav/scroll_follow.hpp): a list never jumps while the user is
 * browsing it (owner's board test on the HackBerry, 2026-11-15: "while scrolling on a long queue, when I stop the list reverts
 * to the selected line"). A fake clock in ms.
 */
#include "check.hpp"

#include "nav/scroll_follow.hpp"

using namespace vinyl;

namespace {

void test_policy() {
    ScrollFollow f;
    uint64_t t = 1000;
    /* nobody scrolled: the page opens on the selection, refreshes keep the highlight in view as before */
    f.open(t);
    CHECK(!f.browsing());
    CHECK(f.follow(FollowCause::Open, true));
    CHECK(f.follow(FollowCause::Refresh, false));
    CHECK(f.follow(FollowCause::Refresh, true));
    CHECK(f.follow(FollowCause::Action, true));
    /* the user drags: from then on he is browsing, and the list does not follow a refresh (a track change, a rescan, the
     * periodic snapshot), changed selection or not, whatever time has passed */
    t += 5000;
    f.user_scrolled(t);
    CHECK(f.browsing());
    for (uint64_t later : {0ull, 100ull, 1500ull, 60000ull, 3600000ull}) {
        (void)later;
        CHECK(!f.follow(FollowCause::Refresh, false));
        CHECK(!f.follow(FollowCause::Refresh, true));
    }
    t += 3600000; /* an hour later: still where he left it */
    CHECK(f.browsing());
    CHECK(!f.follow(FollowCause::Refresh, true));
    /* more scrolling (its momentum) keeps it so */
    t += 40;
    f.user_scrolled(t);
    CHECK(f.browsing());
    /* an explicit action (a key moved the highlight, a tap on a row): the list follows, the user is not browsing any more */
    t += 2000;
    CHECK(f.follow(FollowCause::Action, true));
    f.action(t);
    CHECK(!f.browsing());
    CHECK(f.follow(FollowCause::Refresh, true)); /* a track change moves the highlight into view again */
    /* he scrolls again, then the page is opened again (Esc and back in): the page follows the selection */
    t += 10;
    f.user_scrolled(t + 5);
    CHECK(f.browsing());
    t += 100;
    f.open(t);
    CHECK(!f.browsing());
    CHECK(f.follow(FollowCause::Open, false));
    CHECK(f.follow(FollowCause::Refresh, true));
    /* the order decides: an action after the scroll ends the browsing, a scroll after the action starts it */
    ScrollFollow g;
    g.user_scrolled(100);
    g.action(200);
    CHECK(!g.browsing());
    g.user_scrolled(300);
    CHECK(g.browsing());
    /* a tap on a row and the track change after it: the list is not browsed, the refresh may follow */
    g.action(400);
    CHECK(g.follow(FollowCause::Refresh, true));
    /* a fresh list that was never scrolled by a user is not browsing */
    CHECK(!ScrollFollow().browsing());
    CHECK(ScrollFollow().follow(FollowCause::Refresh, true));
}

}  // namespace

void test_scroll_follow() { test_policy(); }
