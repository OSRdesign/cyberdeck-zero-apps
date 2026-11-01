/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk core: the seeded generator (SplitMix64, Sebastiano Vigna, public domain algorithm, written here from its
 * published description). Same seed, same sequence on every board and architecture.
 */
#pragma once

#include <cstdint>

namespace netwalk {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 1) : s_(seed) {}
    std::uint64_t next()
    {
        std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    /* 0 .. n-1 (0 for n == 0) */
    std::uint32_t below(std::uint32_t n) { return n ? static_cast<std::uint32_t>(next() % n) : 0u; }

private:
    std::uint64_t s_;
};

} // namespace netwalk
