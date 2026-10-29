/*
 * SPDX-License-Identifier: MIT
 */
#include "win_sim.hpp"

#include <algorithm>

namespace klondike {

namespace {
/* Speeds as parts per 10000 of the play area's width per step; kicks and gravity of its height. */
constexpr int kVxMin = 80, kVxMax = 210;
constexpr int kKickMin = 40, kKickMax = 340;
constexpr int kGravity = 30;
constexpr int kRestMin = 194, kRestMax = 214;   /* /256: 0.76 .. 0.84 */
} // namespace

std::uint32_t WinSim::rnd()
{
    std::uint32_t x = rng_; /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x ? x : 0x9E3779B9u;
    return rng_;
}

int WinSim::rnd_range(int lo, int hi)
{
    return lo + static_cast<int>(rnd() % static_cast<std::uint32_t>(hi - lo + 1));
}

void WinSim::init(const WinParams &p)
{
    p_ = p;
    if (p_.count > kCards) p_.count = kCards;
    rng_ = p_.seed * 2654435761u + 0x7F4A7C15u;
    if (!rng_) rng_ = 1;
    for (int i = 0; i < 4; ++i) rnd();
    frame_ = launched_ = exited_ = next_launch_ = 0;
    nactive_ = nleft_ = 0;
    const int h = std::max(1, p_.floor - p_.top);
    const int ts = p_.reduced ? 2 : 1; /* a reduced step is twice the time: speeds x2, gravity x4 per step */
    g_ = std::max<std::int32_t>(1, static_cast<std::int32_t>(static_cast<std::int64_t>(h) * 256 * kGravity * ts * ts / 10000));
    ymax_ = (p_.floor - p_.card_h) * 256;
    ceil_ = p_.top * 256;
}

void WinSim::launch_next()
{
    const WinLaunch &l = p_.launch[launched_++];
    Body &b = body_[nactive_];
    b = Body();
    b.card = l.card;
    b.x = l.x * 256;
    b.y = std::min(l.y * 256, ymax_);
    const int w = std::max(1, p_.x1 - p_.x0);
    const int h = std::max(1, p_.floor - p_.top);
    int speed = rnd_range(kVxMin, kVxMax);
    if (p_.reduced) speed = speed * 2;
    const int ts = p_.reduced ? 2 : 1;
    const std::int32_t vx = static_cast<std::int32_t>(static_cast<std::int64_t>(w) * 256 * speed * ts / 10000);
    /* mostly towards the side with more room (a long trail across the board), sometimes the other way */
    const int room_r = p_.x1 - (l.x + p_.card_w), room_l = l.x - p_.x0;
    const bool towards_more = rnd() % 10 < 7;
    const bool right = (room_r >= room_l) == towards_more;
    b.vx = right ? vx : -vx;
    b.vy = -static_cast<std::int32_t>(static_cast<std::int64_t>(h) * 256 * rnd_range(kKickMin, kKickMax) * ts / 10000);
    b.restitution = rnd_range(kRestMin, kRestMax);
    view_[nactive_] = Flyer{b.card, l.x, l.y, true, true};
    ++nactive_;
}

bool WinSim::step()
{
    nleft_ = 0;
    if (done()) return false;
    ++frame_;
    const int every = stamp_every(p_.reduced);
    for (int i = 0; i < nactive_;) {
        Body &b = body_[i];
        b.vy += g_;
        b.x += b.vx;
        b.y += b.vy;
        if (b.y > ymax_ && b.vy > 0) {
            b.y = 2 * ymax_ - b.y;
            b.vy = -static_cast<std::int32_t>(static_cast<std::int64_t>(b.vy) * b.restitution / 256);
            if (b.vy > -g_) { /* too little left to bounce: it rolls on the floor */
                b.y = ymax_;
                b.vy = 0;
            }
        }
        if (b.y < ceil_ && b.vy < 0) { /* the top of the play area (under the HUD): bounce back down */
            b.y = 2 * ceil_ - b.y;
            b.vy = -static_cast<std::int32_t>(static_cast<std::int64_t>(b.vy) * b.restitution / 256);
        }
        ++b.age;
        const int px = b.x >> 8;
        const bool gone = px >= p_.x1 || px + p_.card_w <= p_.x0 || frame_ * (p_.reduced ? 2 : 1) >= kWinMaxSteps;
        if (gone) {
            left_[nleft_++] = b.card;
            ++exited_;
            body_[i] = body_[nactive_ - 1];
            view_[i] = view_[nactive_ - 1];
            --nactive_;
            continue;
        }
        view_[i] = Flyer{b.card, px, b.y >> 8, b.age % every == 0, false};
        ++i;
    }
    if (launched_ < p_.count && nactive_ < max_active(p_.reduced) && frame_ >= next_launch_) {
        launch_next();
        next_launch_ = frame_ + cadence(p_.reduced);
    }
    return true;
}

} // namespace klondike
