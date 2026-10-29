/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: a small deterministic generator (splitmix64) for the mine layouts. The core never reads the
 * system's entropy: a game is a function of its 64-bit seed (the UI or the session supplies the seed).
 */
#pragma once

#include <cstdint>

namespace minefield {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next()
    {
        s += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    /* 0 .. n-1 (n > 0); the modulo bias is far below anything a board can show */
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>((next() >> 11) % n); }
};

/* The seed of attempt `k` of a fair-board search: attempt 0 is the seed itself (so a plain board that happens to be
 * solvable is also the fair board), the others are well mixed derivations of it. */
inline std::uint64_t derive_seed(std::uint64_t seed, int attempt)
{
    if (attempt <= 0) return seed;
    Rng r(seed ^ (static_cast<std::uint64_t>(attempt) * 0xD1B54A32D192ED03ull));
    return r.next();
}

} // namespace minefield
