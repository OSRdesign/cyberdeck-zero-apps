/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the board (see board.hpp).
 */
#include "board.hpp"

#include "rng.hpp"

namespace netwalk {

namespace {

constexpr std::uint8_t kDirs[4] = {kN, kE, kS, kW};

} // namespace

int popcount4(std::uint8_t m)
{
    return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1);
}

Kind kind_of(std::uint8_t mask)
{
    switch (popcount4(mask & 15)) {
    case 1: return Kind::End;
    case 2: return (mask == (kN | kS) || mask == (kE | kW)) ? Kind::Straight : Kind::Corner;
    case 3: return Kind::Tee;
    case 4: return Kind::Cross;
    default: return Kind::None;
    }
}

int Board::neighbor(int i, std::uint8_t d) const
{
    int x = i % w_, y = i / w_;
    switch (d) {
    case kN: --y; break;
    case kS: ++y; break;
    case kE: ++x; break;
    case kW: --x; break;
    default: return -1;
    }
    if (wrap_) {
        x = (x + w_) % w_;
        y = (y + h_) % h_;
    } else if (x < 0 || y < 0 || x >= w_ || y >= h_) {
        return -1;
    }
    return y * w_ + x;
}

bool Board::rotate(int i, bool clockwise)
{
    const std::uint8_t old = mask_[static_cast<std::size_t>(i)];
    const std::uint8_t now = clockwise ? rot_cw(old) : rot_ccw(old);
    mask_[static_cast<std::size_t>(i)] = now;
    return now != old;
}

Board Board::from_masks(int w, int h, bool wrap, int server, const std::vector<std::uint8_t> &masks)
{
    Board b;
    b.w_ = w;
    b.h_ = h;
    b.wrap_ = wrap;
    b.server_ = server;
    b.mask_ = masks;
    b.solved_ = masks;
    b.scramble_.assign(masks.size(), 0);
    b.locked_.assign(masks.size(), 0);
    return b;
}

std::vector<std::uint8_t> Board::powered() const
{
    std::vector<std::uint8_t> on(static_cast<std::size_t>(size()), 0);
    if (!valid()) return on;
    std::vector<int> stack{server_};
    on[static_cast<std::size_t>(server_)] = 1;
    while (!stack.empty()) {
        const int c = stack.back();
        stack.pop_back();
        for (std::uint8_t d : kDirs) {
            if (!(mask(c) & d)) continue;
            const int n = neighbor(c, d);
            if (n < 0 || on[static_cast<std::size_t>(n)] || !(mask(n) & opposite(d))) continue;
            on[static_cast<std::size_t>(n)] = 1;
            stack.push_back(n);
        }
    }
    return on;
}

int Board::powered_count() const
{
    int n = 0;
    for (std::uint8_t v : powered()) n += v;
    return n;
}

bool Board::has_loose_end(int i) const
{
    for (std::uint8_t d : kDirs) {
        if (!(mask(i) & d)) continue;
        const int n = neighbor(i, d);
        if (n < 0 || !(mask(n) & opposite(d))) return true;
    }
    return false;
}

bool Board::is_won() const
{
    if (!valid()) return false;
    if (powered_count() != size()) return false;
    for (int i = 0; i < size(); ++i)
        if (has_loose_end(i)) return false;
    return true;
}

namespace {

/* One try at the tree; false when growing got stuck (a tile with three cables walled in): the caller tries again. */
bool grow(int w, int h, bool wrap, int server, Rng &rng, std::vector<std::uint8_t> &out)
{
    const int n = w * h;
    const Board geom = Board::from_masks(w, h, wrap, server, std::vector<std::uint8_t>(static_cast<std::size_t>(n), 0));
    std::vector<std::uint8_t> m(static_cast<std::size_t>(n), 0), seen(static_cast<std::size_t>(n), 0),
        arrive(static_cast<std::size_t>(n), 0), cap(static_cast<std::size_t>(n), 3);
    seen[static_cast<std::size_t>(server)] = 1;
    std::vector<int> active;

    /* the server is a tee (two in three) or a cross: it takes 3 or 4 cables at once */
    const int first = rng.below(3) == 0 ? 4 : 3;
    cap[static_cast<std::size_t>(server)] = static_cast<std::uint8_t>(first);
    const std::uint8_t skip = kDirs[rng.below(4)];
    for (std::uint8_t d : kDirs) {
        if (first == 3 && d == skip) continue;
        const int nb = geom.neighbor(server, d);
        if (nb < 0 || seen[static_cast<std::size_t>(nb)]) continue;
        m[static_cast<std::size_t>(server)] |= d;
        m[static_cast<std::size_t>(nb)] |= opposite(d);
        seen[static_cast<std::size_t>(nb)] = 1;
        arrive[static_cast<std::size_t>(nb)] = d;
        active.push_back(nb);
    }
    cap[static_cast<std::size_t>(server)] = static_cast<std::uint8_t>(popcount4(m[static_cast<std::size_t>(server)]));

    while (!active.empty()) {
        const std::size_t k =
            (rng.below(2) == 0) ? active.size() - 1 : rng.below(static_cast<std::uint32_t>(active.size()));
        const int c = active[k];
        const int deg = popcount4(m[static_cast<std::size_t>(c)]);
        std::vector<std::uint8_t> free_dirs, bent;
        if (deg < cap[static_cast<std::size_t>(c)]) {
            for (std::uint8_t d : kDirs) {
                const int nb = geom.neighbor(c, d);
                if (nb < 0 || seen[static_cast<std::size_t>(nb)]) continue;
                free_dirs.push_back(d);
                /* continuing the way the cable came in would make a straight run */
                if (!(deg == 1 && d == arrive[static_cast<std::size_t>(c)])) bent.push_back(d);
            }
        }
        if (free_dirs.empty()) {
            active[k] = active.back();
            active.pop_back();
            continue;
        }
        const std::vector<std::uint8_t> &pool = (!bent.empty() && rng.below(4) != 0) ? bent : free_dirs;
        const std::uint8_t d = pool[rng.below(static_cast<std::uint32_t>(pool.size()))];
        const int nb = geom.neighbor(c, d);
        m[static_cast<std::size_t>(c)] |= d;
        m[static_cast<std::size_t>(nb)] |= opposite(d);
        seen[static_cast<std::size_t>(nb)] = 1;
        arrive[static_cast<std::size_t>(nb)] = d;
        active.push_back(nb);
    }
    for (std::uint8_t s : seen)
        if (!s) return false;
    out = m;
    return true;
}

} // namespace

Board Board::generate(int w, int h, bool wrap, std::uint64_t seed)
{
    const int server = (h / 2) * w + w / 2;
    Rng rng(seed * 0x2545F4914F6CDD1Dull + 0x51ED27ull);
    std::vector<std::uint8_t> tree;
    for (int attempt = 0; attempt < 1000; ++attempt)
        if (grow(w, h, wrap, server, rng, tree)) break;
    Board b = from_masks(w, h, wrap, server, tree);
    /* scramble by rotation only; a board that is already a win is scrambled again */
    for (int guard = 0; guard < 1000; ++guard) {
        for (int i = 0; i < b.size(); ++i) {
            const int r = static_cast<int>(rng.below(4));
            std::uint8_t m = b.solved_[static_cast<std::size_t>(i)];
            for (int k = 0; k < r; ++k) m = rot_cw(m);
            b.mask_[static_cast<std::size_t>(i)] = m;
            b.scramble_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(r);
        }
        if (!b.is_won()) break;
    }
    return b;
}

} // namespace netwalk
