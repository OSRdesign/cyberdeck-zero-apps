/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the physics of the win cascade (pure, no LVGL; tests/test_winsim.cpp). Own design, in the spirit of the
 * classic: the cards leave the foundations one by one (the caller gives the order and the launch points), each is thrown
 * sideways with its own speed, falls with gravity, bounces on the floor losing energy and flies on until it has left the
 * play area sideways. The UI draws the cards where step() says and stamps the card image into a trail layer at every
 * `stamp` flag, so the screen fills with the arcs of the cascade.
 *
 *   Fixed point: positions and speeds are integers in 1/256 px, the steps are fixed (kWinStepMs), so a deal always
 *   gives the same scene, on every CPU, whatever the frame rate: the UI runs as many steps as time has passed.
 *   Scaling: horizontal speeds are fractions of the play area's width, gravity and the upward kick fractions of its
 *   height (a drop from the foundations to the floor takes about 0.65 s), so every layout (landscape, portrait, a tablet)
 *   gets the same pace; the card size only matters for the exit test and the floor.
 *   Bounded: every card is at least kVxMin fast, so one crosses the area in a few hundred steps at most; the whole
 *   cascade (52 cards, at most kWinMaxActive in flight, a new one every cadence() steps) is over in well under
 *   kWinMaxSteps, which is also a hard stop (what is still flying then leaves).
 *   Reduced (the "reduced" animation setting): a step is twice as long (66 ms, about 15 pictures a second: half the
 *   drawing work), the same pace on the clock, at most kReducedActive cards in flight, throws 2x faster (a shorter run),
 *   and a stamp only every 2nd step (a quarter of the stamps).
 */
#pragma once

#include "card.hpp"

#include <cstdint>

namespace klondike {

constexpr int kWinStepMs = 33;             /* one simulation step (about 30 per second) */
constexpr int kWinMaxActive = 8;           /* cards in flight at once */
constexpr int kReducedActive = 6;
constexpr int kWinMaxSteps = 1500;         /* hard stop: 50 s */

struct WinLaunch {
    Card card = kNoCard;
    int x = 0, y = 0;                      /* top left of the card when it leaves (the foundation's place) */
};

struct WinParams {
    int x0 = 0, x1 = 0;                    /* the play area, horizontally: a card is gone when it is fully outside */
    int top = 0;                           /* top of the play area: the cards bounce off it too (the HUD stays clear) */
    int floor = 0;                         /* bottom of the play area: the cards bounce on it */
    int card_w = 0, card_h = 0;
    std::uint32_t seed = 1;                /* the deal number */
    bool reduced = false;
    int count = 0;                         /* launches used */
    WinLaunch launch[kCards];
};

struct Flyer {
    Card card = kNoCard;
    int x = 0, y = 0;                      /* top left, px */
    bool stamp = false;                    /* leave an image of the card here */
    bool fresh = false;                    /* launched in this step */
};

class WinSim {
public:
    void init(const WinParams &p);
    /* One fixed step. Returns false when everything is gone (nothing moved). */
    bool step();

    int step_count() const { return frame_; }
    bool done() const { return exited_ >= p_.count; }
    int launched() const { return launched_; }
    int exited() const { return exited_; }
    int active() const { return nactive_; }
    const Flyer &flyer(int i) const { return view_[i]; }
    /* what the last step() retired: the cards that left (the UI frees their sprites) */
    int left_count() const { return nleft_; }
    Card left_card(int i) const { return left_[i]; }

    static int cadence(bool reduced) { return reduced ? 2 : 8; }   /* steps between two launches */
    static int stamp_every(bool reduced) { return reduced ? 2 : 1; }
    static int step_ms(bool reduced) { return reduced ? 2 * kWinStepMs : kWinStepMs; }
    static int max_active(bool reduced) { return reduced ? kReducedActive : kWinMaxActive; }
    int step_ms() const { return step_ms(p_.reduced); }

private:
    struct Body {
        Card card = kNoCard;
        std::int32_t x = 0, y = 0;         /* 1/256 px */
        std::int32_t vx = 0, vy = 0;
        std::int32_t restitution = 0;      /* /256 */
        int age = 0;
    };
    std::uint32_t rnd();
    int rnd_range(int lo, int hi);         /* inclusive */
    void launch_next();

    WinParams p_;
    std::uint32_t rng_ = 1;
    int frame_ = 0, launched_ = 0, exited_ = 0, next_launch_ = 0;
    std::int32_t g_ = 1, ymax_ = 0, ceil_ = 0;
    int nactive_ = 0, nleft_ = 0;
    Body body_[kWinMaxActive];
    Flyer view_[kWinMaxActive];
    Card left_[kWinMaxActive];
};

} // namespace klondike
