/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: follow mode, as pure logic. On: the map recentres on every fresh fix (the zoom stays), and a pan by the
 * user turns it off. The engine reports every move of the view (cp0_map_on_view_change), the app's own recentring
 * included; the logic tells them apart by where the view is: it is "where the app put it" (the last fix, or the centre
 * when follow was switched on) within a few pixels, or it was moved by hand. A zoom with the buttons or keys keeps
 * the centre and so keeps follow; a drag, an arrow key or a double tap away from the centre moves it and ends follow.
 */
#pragma once

namespace atlas {

class FollowLogic {
public:
    static constexpr double kPanSlopPx = 3.0;

    bool on() const { return on_; }
    /* Switches follow on or off; `lat`/`lon`: where the view is now. */
    void set(bool on, double lat, double lon);
    /* A fresh fix: true when the view should be moved to it (follow on). The view is then expected there. */
    bool on_fix(double lat, double lon);
    /* The engine reports the view: the centre, the zoom and the display scale (1, 1.5, 2). True when this was a pan by
     * the user and follow just went off. */
    bool on_view_change(double lat, double lon, int zoom, double scale);

private:
    bool on_ = false;
    double lat_ = 0, lon_ = 0;   /* where the app last put the view */
};

} // namespace atlas
