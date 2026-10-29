/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of how the boards fit the screen (core/view_layout.hpp): the Expert grid from the viewport, Intermediate and
 * Beginner as shares of it, the mine densities, the square cells, and the play rules on the derived boards (first-click
 * safety, the solver and the fair generator on non-square grids).
 */
#include "test_util.hpp"

#include "generator.hpp"
#include "solver.hpp"
#include "view_layout.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>

using namespace minefield;

namespace {

struct Screen {
    int w, h;
    double ppmm;
};

/* An estimate of the playable area: a landscape canvas loses a header (5.5 mm) and a dock column (21 mm); a portrait one a
 * header, the strip (7 mm) and the three-button dock (11 mm + gaps). The app's own numbers are measured by its scenes. */
void viewport_of(const Screen &s, int *vw, int *vh)
{
    const int header = static_cast<int>(std::lround(5.5 * s.ppmm));
    if (s.w > s.h) {
        *vw = s.w - static_cast<int>(std::lround(21 * s.ppmm)) - 8;
        *vh = s.h - header - 8;
    } else {
        *vw = s.w - 8;
        *vh = s.h - header - static_cast<int>(std::lround(7 * s.ppmm)) - static_cast<int>(std::lround(11 * s.ppmm)) - 16;
    }
}

const Screen kScreens[] = {
    {640, 480, 7.4}, {640, 480, 8.5}, {640, 480, 9.2}, {640, 480, 11.3},
    {800, 480, 7.4}, {800, 480, 8.5}, {800, 480, 9.2}, {800, 480, 11.3},
    {720, 720, 7.4}, {720, 720, 8.5}, {720, 720, 9.2}, {720, 720, 11.3},
    {1024, 600, 7.4}, {1024, 600, 8.5}, {1024, 600, 9.2}, {1024, 600, 11.3},
    {1280, 720, 7.4}, {1280, 720, 8.5}, {1280, 720, 9.2}, {1280, 720, 11.3},
    {480, 800, 7.4}, {480, 800, 8.5}, {480, 800, 9.2}, {480, 800, 11.3},
    {480, 640, 7.4}, {480, 640, 8.5}, {480, 640, 9.2}, {480, 640, 11.3},
    {800, 1280, 7.4}, {800, 1280, 8.5}, {800, 1280, 9.2}, {800, 1280, 11.3},
    /* the real boards: the deck (about 11.3), the Pi 4B 5 inch (7.41), the 4.3 inch DSI (8.5), the tablet (8.6) */
    {640, 480, 11.3}, {800, 480, 7.41}, {480, 800, 7.41}, {800, 480, 8.5}, {480, 800, 8.5}, {800, 1280, 8.6},
};

void test_basics()
{
    CHECK(hit_cell(5, 5, 0, 0, 10, 4, 3) == 0 && hit_cell(35, 25, 0, 0, 10, 4, 3) == 11);
    CHECK(hit_cell(40, 5, 0, 0, 10, 4, 3) == -1 && hit_cell(5, 30, 0, 0, 10, 4, 3) == -1 && hit_cell(-1, 5, 0, 0, 10, 4, 3) == -1);
    CHECK(hit_cell(15, 15, 10, 10, 10, 4, 3) == 0 && hit_cell(5, 15, 10, 10, 10, 4, 3) == -1 && hit_cell(5, 5, 0, 0, 0, 4, 3) == -1);
    /* a grid in a viewport: the cell is the largest square, capped at 12 mm; the grid is centred */
    BoardPlan p = fit_board(400, 300, 10.0, 8, 6, 7);
    CHECK(p.valid && p.fits && p.cell_px == 50 && p.board_w == 400 && p.board_h == 300 && p.origin_x == 0 && p.origin_y == 0);
    p = fit_board(400, 300, 10.0, 6, 6, 5);
    CHECK(p.cell_px == 50 && p.board_w == 300 && p.origin_x == 50 && p.origin_y == 0);
    p = fit_board(1000, 1000, 10.0, 6, 6, 5);   /* 12 mm = 120 px */
    CHECK(p.cell_px == 120 && p.origin_x == 140 && p.origin_y == 140 && std::fabs(p.cell_mm - 12.0) < 1e-9);
    CHECK(!fit_board(0, 100, 10.0, 8, 6, 7).valid && !fit_board(100, 100, 0, 8, 6, 7).valid && !fit_board(100, 100, 10, 0, 6, 7).valid);
    /* the Expert grid: 5 mm cells, at least 6, at most 30 */
    CHECK((expert_grid(400, 300, 10.0).w == 8 && expert_grid(400, 300, 10.0).h == 6));
    CHECK((expert_grid(50, 50, 10.0).w == 6 && expert_grid(50, 50, 10.0).h == 6));
    CHECK((expert_grid(5000, 5000, 10.0).w == 30 && expert_grid(5000, 5000, 10.0).h == 30));
    /* mines: 12 %, 16 %, 20.5 %, at least 1, at most cells - 9 */
    CHECK(mines_for(10, 10, Preset::Beginner) == 12 && mines_for(10, 10, Preset::Intermediate) == 16 && mines_for(10, 10, Preset::Expert) == 21);
    CHECK(mines_for(6, 6, Preset::Expert) == 7 && mines_for(5, 5, Preset::Expert) == 5 && mines_for(30, 30, Preset::Expert) == 185);
}

void test_every_screen()
{
    for (const Screen &s : kScreens) {
        int vw = 0, vh = 0;
        viewport_of(s, &vw, &vh);
        const BoardPlan b = derive_board(vw, vh, s.ppmm, Preset::Beginner);
        const BoardPlan i = derive_board(vw, vh, s.ppmm, Preset::Intermediate);
        const BoardPlan e = derive_board(vw, vh, s.ppmm, Preset::Expert);
        const BoardPlan *all[3] = {&b, &i, &e};
        for (const BoardPlan *p : all) {
            CHECK(p->valid && p->fits);
            CHECK(p->dims.w >= kMinDim && p->dims.w <= kMaxDim && p->dims.h >= kMinDim && p->dims.h <= kMaxDim);
            CHECK(p->dims.mines >= 1 && p->dims.mines <= p->dims.w * p->dims.h - 9);
            CHECK(p->origin_x >= 0 && p->origin_y >= 0 && p->origin_x + p->board_w <= vw && p->origin_y + p->board_h <= vh);
            CHECK(p->board_w == p->cell_px * p->dims.w && p->board_h == p->cell_px * p->dims.h);
            CHECK(p->cell_mm <= kMaxCellMm + 1e-9);
            CHECK(p->cell_mm >= 4.4);   /* 5 mm by design; a very small viewport keeps the 6 x 6 floor and a smaller cell */
        }
        const int cb = b.dims.w * b.dims.h, ci = i.dims.w * i.dims.h, ce = e.dims.w * e.dims.h;
        CHECK(cb < ci && ci < ce);
        const double db = static_cast<double>(b.dims.mines) / cb, di = static_cast<double>(i.dims.mines) / ci,
                     de = static_cast<double>(e.dims.mines) / ce;
        CHECK(db < di && di < de);
        CHECK(e.dims.w >= 6 && e.dims.h >= 6);
        CHECK(b.cell_px >= i.cell_px && i.cell_px >= e.cell_px);   /* easier levels get bigger (or equal) cells */
        /* the grid is not forced square: a wide viewport gives a wide grid */
        if (vw >= 2 * vh && vh > 0) CHECK(e.dims.w > e.dims.h);
        if (vh >= 2 * vw) CHECK(e.dims.h > e.dims.w);
        /* the same inputs, the same boards */
        CHECK(derive_board(vw, vh, s.ppmm, Preset::Expert).dims == e.dims);
        const std::array<Dims, kRecordedPresets> three = derive_boards(vw, vh, s.ppmm);
        CHECK(three[0] == b.dims && three[1] == i.dims && three[2] == e.dims);
        /* a larger viewport never gives a smaller Expert grid */
        const Dims e2 = expert_grid(vw + 100, vh + 100, s.ppmm);
        CHECK(e2.w >= e.dims.w && e2.h >= e.dims.h);
    }
}

/* first-click safety, the solver and the fair generator on the derived boards (non-square, any size) */
void test_play_on_derived()
{
    const Screen picks[] = {{640, 480, 11.3}, {800, 480, 7.41}, {480, 800, 8.5}, {800, 1280, 8.6}, {1280, 720, 7.4}, {720, 720, 9.2}};
    for (const Screen &s : picks) {
        int vw = 0, vh = 0;
        viewport_of(s, &vw, &vh);
        for (int pi = 0; pi < kRecordedPresets; ++pi) {
            const BoardPlan p = derive_board(vw, vh, s.ppmm, static_cast<Preset>(pi));
            const int w = p.dims.w, h = p.dims.h, cells = w * h;
            const int firsts[] = {0, w - 1, (h - 1) * w, cells - 1, (h / 2) * w + w / 2, w / 2};
            for (int first : firsts)
                for (std::uint64_t seed = 1; seed <= 6; ++seed) {
                    const std::vector<std::uint8_t> m = place_mines(w, h, p.dims.mines, seed, first);
                    int n = 0;
                    for (std::uint8_t v : m) n += v;
                    CHECK(n == p.dims.mines && m[static_cast<std::size_t>(first)] == 0);
                    Board bd(w, h, p.dims.mines);
                    int nb[8];
                    const int c = bd.neighbours(first, nb);
                    for (int k = 0; k < c; ++k) CHECK(m[static_cast<std::size_t>(nb[k])] == 0);
                }
            /* a game on it: the first dig opens an area, a loss and a win are reachable */
            GameConfig cfg;
            cfg.dims = p.dims;
            cfg.seed = 5;
            Game g(cfg);
            CHECK(g.reveal(w / 2, h / 2) == Game::Outcome::Opened && g.board().revealed_safe() >= 9);
            CHECK(g.width() == w && g.height() == h);
            /* the solver plays whatever it can, never opening a mine */
            for (int guard = 0; guard < 2000 && g.status() == Game::Status::Playing; ++guard) {
                const int c = find_safe_cell(g.view());
                if (c < 0) break;
                CHECK(g.reveal(c % w, c / w) != Game::Outcome::Exploded);
            }
        }
    }
}

void test_fair_on_largest()
{
    /* the largest derived grids (a big, dense screen): the fair search stays inside its budget and the layout it returns plays */
    const Screen big[] = {{1280, 720, 7.4}, {800, 1280, 7.4}, {1024, 600, 7.4}};
    for (const Screen &s : big) {
        int vw = 0, vh = 0;
        viewport_of(s, &vw, &vh);
        for (int pi = 0; pi < kRecordedPresets; ++pi) {
            const BoardPlan p = derive_board(vw, vh, s.ppmm, static_cast<Preset>(pi));
            const auto t0 = std::chrono::steady_clock::now();
            const GenResult r = generate(p.dims.w, p.dims.h, p.dims.mines, 12345, (p.dims.h / 2) * p.dims.w + p.dims.w / 2, true);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            int n = 0;
            for (std::uint8_t v : r.mines) n += v;
            CHECK(n == p.dims.mines && r.attempts >= 1 && r.attempts <= kFairMaxAttempts);
            CHECK(ms < 20000);   /* a desktop core: the budget is about a second; the slow boards are measured on the boards */
            if (std::getenv("MINEFIELD_TEST_VERBOSE"))
                std::printf("fair %dx%d/%d: %d attempts, failed=%d, %.0f ms\n", p.dims.w, p.dims.h, p.dims.mines, r.attempts,
                            r.fair_failed ? 1 : 0, ms);
        }
    }
}

} // namespace

void test_view_group()
{
    test_basics();
    test_every_screen();
    test_play_on_derived();
    test_fair_on_largest();
}
