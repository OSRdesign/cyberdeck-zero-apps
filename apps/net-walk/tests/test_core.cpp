/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk unit tests: main() and the generator's tests that run over thousands of seeds (no LVGL).
 *
 *   per size and mode (Easy 5x5, Normal 7x7, Hard 9x9, Expert 9x9 wrap), seeds 1 .. kSeeds:
 *     - the solved orientation is a win;
 *     - undoing the scramble (the recorded quarter turns, reversed) is a win;
 *     - the scrambled board never starts as a win;
 *     - every tile is used (a cable end on each, powered in the solved layout) and the layout is a spanning tree;
 *     - the types never change under the scramble, the server is a tee or a cross, nobody else is a cross;
 *     - wrap: cables cross the edge; no wrap: no end ever points off the board;
 *     - the same seed gives the same board.
 */
#include "test_util.hpp"

#include "board.hpp"

using namespace netwalk;

namespace {

constexpr int kSeeds = 2500;

struct Mode {
    int n;
    bool wrap;
};
const Mode kModes[4] = {{5, false}, {7, false}, {9, false}, {9, true}};

int edges_of(const Board &b)
{
    int ends = 0;
    for (int i = 0; i < b.size(); ++i) ends += popcount4(b.solved_mask(i));
    return ends / 2;
}

bool solved_layout_won(const Board &src)
{
    Board b = src;
    for (int i = 0; i < b.size(); ++i) b.solve_tile(i);
    return b.is_won();
}

bool undone_scramble_won(const Board &src)
{
    Board b = src;
    for (int i = 0; i < b.size(); ++i)
        for (int k = 0; k < (4 - b.scramble(i)) % 4; ++k) b.rotate(i, true);
    return b.is_won();
}

void test_seeds()
{
    long wrapped_edges = 0, straights = 0, tiles = 0;
    long server_tee = 0, server_cross = 0;
    for (const Mode &m : kModes) {
        for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
            const Board b = Board::generate(m.n, m.n, m.wrap, seed);
            CHECK_AT(b.valid(), seed, m.n, m.wrap);
            if (!b.valid()) continue;
            CHECK_AT(b.size() == m.n * m.n, seed, m.n, m.wrap);
            CHECK_AT(b.server() == (m.n / 2) * m.n + m.n / 2, seed, m.n, m.wrap);
            CHECK_AT(solved_layout_won(b), seed, m.n, m.wrap);
            CHECK_AT(undone_scramble_won(b), seed, m.n, m.wrap);
            CHECK_AT(!b.is_won(), seed, m.n, m.wrap);
            /* all tiles used, and a spanning tree: n*n - 1 cables */
            bool all_used = true, types_kept = true, no_stray_cross = true;
            for (int i = 0; i < b.size(); ++i) {
                if (b.solved_mask(i) == 0) all_used = false;
                if (kind_of(b.mask(i)) != b.kind(i)) types_kept = false;
                if (i != b.server() && b.kind(i) == Kind::Cross) no_stray_cross = false;
                ++tiles;
                if (b.kind(i) == Kind::Straight) ++straights;
            }
            CHECK_AT(all_used, seed, m.n, m.wrap);
            CHECK_AT(types_kept, seed, m.n, m.wrap);
            CHECK_AT(no_stray_cross, seed, m.n, m.wrap);
            CHECK_AT(edges_of(b) == b.size() - 1, seed, m.n, m.wrap);
            const Kind sk = b.kind(b.server());
            CHECK_AT(sk == Kind::Tee || sk == Kind::Cross, seed, m.n, m.wrap);
            (sk == Kind::Tee ? server_tee : server_cross)++;
            /* the solved layout is fully powered; every tile of the scrambled board is a rotation of its solved one */
            Board s = b;
            for (int i = 0; i < s.size(); ++i) s.solve_tile(i);
            CHECK_AT(s.powered_count() == s.size(), seed, m.n, m.wrap);
            bool rotations_only = true;
            for (int i = 0; i < b.size(); ++i) {
                std::uint8_t r = b.solved_mask(i);
                bool hit = false;
                for (int k = 0; k < 4; ++k, r = rot_cw(r)) hit = hit || r == b.mask(i);
                rotations_only = rotations_only && hit;
            }
            CHECK_AT(rotations_only, seed, m.n, m.wrap);
            /* edges: wrap crosses, no wrap never points off the board */
            for (int i = 0; i < b.size(); ++i) {
                const int x = i % b.w(), y = i / b.w();
                for (std::uint8_t d : {kN, kE, kS, kW}) {
                    if (!(b.solved_mask(i) & d)) continue;
                    const int nb = b.neighbor(i, d);
                    CHECK_AT(nb >= 0, seed, m.n, m.wrap);
                    if (nb < 0) continue;
                    CHECK_AT((b.solved_mask(nb) & opposite(d)) != 0, seed, m.n, m.wrap);
                    const bool off = (d == kN && y == 0) || (d == kS && y == b.h() - 1) || (d == kW && x == 0) ||
                                     (d == kE && x == b.w() - 1);
                    if (off) {
                        CHECK_AT(m.wrap, seed, m.n, m.wrap);
                        ++wrapped_edges;
                    }
                }
            }
            /* the same seed, the same board */
            if (seed % 50 == 0) {
                const Board again = Board::generate(m.n, m.n, m.wrap, seed);
                bool same = true;
                for (int i = 0; i < b.size(); ++i)
                    same = same && again.mask(i) == b.mask(i) && again.solved_mask(i) == b.solved_mask(i);
                CHECK_AT(same, seed, m.n, m.wrap);
            }
        }
    }
    CHECK(wrapped_edges > 0); /* the wrap mode really uses its edges */
    CHECK(server_tee > 0 && server_cross > 0);
    std::fprintf(stdout, "  generator: %d seeds x 4 modes, %ld tiles, straights %.1f%%, server tee %ld cross %ld, wrapped cables %ld\n",
                 kSeeds, tiles, 100.0 * static_cast<double>(straights) / static_cast<double>(tiles), server_tee,
                 server_cross, wrapped_edges);
    /* the bias against long straight runs (a plain random spanning tree has about 4 straights in 10 tiles) */
    CHECK(static_cast<double>(straights) / static_cast<double>(tiles) < 0.30);
}

void test_seeds_differ()
{
    int differ = 0;
    const Board a = Board::generate(7, 7, false, 1);
    for (std::uint64_t s = 2; s <= 20; ++s) {
        const Board b = Board::generate(7, 7, false, s);
        bool same = true;
        for (int i = 0; i < b.size(); ++i) same = same && a.mask(i) == b.mask(i);
        if (!same) ++differ;
    }
    CHECK(differ >= 18);
}

} // namespace

int main()
{
    test_seeds();
    test_seeds_differ();
    test_board_group();
    test_game_group();
    test_scores_group();
    test_layout_group();
    test_font_group();
    test_package_group();
    std::printf("test-net-walk: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
