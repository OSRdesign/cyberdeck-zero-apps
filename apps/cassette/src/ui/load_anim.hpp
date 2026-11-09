/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette UI: the cassette LOAD animation as pure functions of time (no LVGL; tests/ui/test_load_anim.cpp), and the size
 * ladder of the handwriting font. The window (cassette_window.cpp) applies a pose to the one object it moves.
 *
 * The owner's boards "Cassette A: load and unload, looping" and "Cassette A: load frames" (draw units: the cassette is
 * 200 x 104 units; 130 units is further than the window is high, so a cassette at +130 or -130 is out of the window):
 *
 *   a take is loaded over another one (900 ms)
 *     0..140 ms     the old cassette drops out through the bottom of the window: 0 -> +130 units, ease in
 *                   (cubic-bezier .42 0 1 1). It keeps its own name on the label.
 *     140..240 ms   the window is empty (only the OPEN word shows)
 *     240..600 ms   the new cassette falls in from above: -130 -> +4 units over the first 300 ms (cubic-bezier .2 .7 .3 1,
 *                   fast, slowing down), then settles back from +4 to 0 over 60 ms (ease in-out): it overshoots 4 units
 *                   at 540 ms, 83 % of the way, and lands at 600 ms
 *     600..900 ms   the take name is written onto the label from left to right (the reveal grows 0 -> 1)
 *   a LOAD while the BLANK cassette (no name on the label, shown when no take is loaded) is in the deck is this same
 *   900 ms swap: the blank one drops out and the loaded one falls in; there is no cassette-less deck any more.
 *   the first load (no cassette in view at all, 660 ms; only without a sound input, where LOAD is not reachable): no drop-out and no gap: the new cassette falls in at 0..360 ms, the name
 *     follows at 360..660 ms.
 *   The label shows no name until the cassette has landed; the strip text and the counter update at the landing; the
 *   hubs do not turn (the transport is idle) until PLAY.
 *
 * Input: keys, touches on the keys and on the list and settings buttons are ignored while the animation runs, except Esc,
 * which finishes it at once and then acts as usual (LoadAnim::accepts).
 *
 * Test hook (the scenes): CASSETTE_ANIM_TICKS=<n> with CASSETTE_LOAD_STEP_MS=<ms> (default 10): every timer tick advances
 * the animation by that step instead of the real time and it stops after n ticks, so a shot shows a fixed frame.
 */
#pragma once

namespace cassette {
namespace ui {

/* ---- the handwriting font's size ladder (ui/fonts/caveat_semibold_<px>.c): the label picks the largest rung that is at
 * most the wanted size and fits the label's width, then smaller rungs */
constexpr int kHandRungs[4] = {15, 22, 29, 31};
constexpr int kHandRungCount = 4;
/* The largest rung <= px; the smallest rung when px is under all of them. */
int hand_rung(int px);
/* The next smaller rung than `rung`, or 0 when it is the smallest. */
int hand_rung_below(int rung);

/* ---- the timeline ---- */
constexpr int kLoadOutMs = 140, kLoadGapMs = 100, kLoadInMs = 360, kLoadWriteMs = 300;
constexpr double kLoadFar = 130.0;    /* draw units: out of the window above or below */
constexpr double kLoadBounce = 4.0;   /* draw units the landing overshoots */
constexpr int kLoadSettleMs = 60;     /* the last part of the fall-in: +4 -> 0 */

/* CSS cubic-bezier(x1, y1, x2, y2) at x in 0..1 (the progress at time x). */
double bezier_at(double x1, double y1, double x2, double y2, double x);

struct LoadPose {
    enum class Phase { Out, Gap, In, Write, Done };
    Phase phase = Phase::Done;
    bool old_shown = false;   /* the old cassette is in view (Out) */
    bool new_shown = false;   /* the new cassette is in view or on its way (In, Write, Done) */
    double old_y = 0;         /* draw units down from rest while the old cassette drops: 0..130 */
    double new_y = 0;         /* draw units from rest: -130 above .. +4 .. 0 */
    double name = 0;          /* the reveal of the name: 0 nothing .. 1 all */
    bool landed = false;      /* t >= the landing: the strip text and the counter update now */
};

int load_total_ms(bool had_old);      /* 900, 660 without an old cassette */
int load_landing_ms(bool had_old);    /* 600, 360 */
LoadPose load_pose(int t_ms, bool had_old);

/* The running animation: advance() with the elapsed time, finish() ends it at once (Esc). */
class LoadAnim {
public:
    void start(bool had_old)
    {
        had_old_ = had_old;
        t_ = 0;
        active_ = true;
    }
    void advance(int dt_ms)
    {
        if (!active_) return;
        t_ += dt_ms < 0 ? 0 : dt_ms;
        if (t_ >= load_total_ms(had_old_)) finish();
    }
    void finish()
    {
        t_ = load_total_ms(had_old_);
        active_ = false;
    }
    bool active() const { return active_; }
    int t_ms() const { return t_; }
    bool had_old() const { return had_old_; }
    LoadPose pose() const { return load_pose(t_, had_old_); }
    /* While it runs only Esc is let through. */
    bool accepts(bool is_esc) const { return !active_ || is_esc; }

private:
    bool had_old_ = true, active_ = false;
    int t_ = 0;
};

} // namespace ui
} // namespace cassette
