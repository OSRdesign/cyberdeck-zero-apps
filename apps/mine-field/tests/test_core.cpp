/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the board and the game (core/board.hpp, core/game.hpp, core/generator.hpp's first-click rules). No LVGL.
 * main() runs the groups of every test file.
 */
#include "test_util.hpp"

#include "generator.hpp"

#include <algorithm>
#include <set>

using namespace minefield;

/* ------------------------------------------------------------------------------------------------ helpers */

Game make_game(const std::vector<std::string> &mines, int fx, int fy, const std::vector<std::string> &states,
               std::uint32_t clock_ms)
{
    const int h = static_cast<int>(mines.size());
    const int w = static_cast<int>(mines[0].size());
    std::string mm, cells;
    int count = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool m = mines[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == '*';
            count += m ? 1 : 0;
            mm += m ? '1' : '0';
            char c = states.empty() ? 'h' : states[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            if (x == fx && y == fy) c = 'r';
            cells += c;
        }
    const std::string json = "{\"version\":1,\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) +
                             ",\"mines\":" + std::to_string(count) + ",\"seed\":\"1\",\"seed_used\":\"1\",\"fair\":false," +
                             "\"fair_failed\":false,\"attempts\":1,\"first_click\":" + std::to_string(fy * w + fx) +
                             ",\"clock_ms\":" + std::to_string(clock_ms) + ",\"penalty_ms\":0,\"hints\":0,\"mine_map\":\"" + mm +
                             "\",\"cells\":\"" + cells + "\"}";
    Game g;
    std::string why;
    const bool ok = Game::load(json, g, &why);
    if (!ok) std::fprintf(stderr, "make_game: %s\n", why.c_str());
    CHECK(ok);
    return g;
}

namespace {

std::vector<std::string> grid(int w, int h, char c)
{
    return std::vector<std::string>(static_cast<std::size_t>(h), std::string(static_cast<std::size_t>(w), c));
}

const std::vector<std::string> kSix = {"......", ".*....", "......", "......", "......", ".....*"};

/* ------------------------------------------------------------------------------------------------ board */

void test_dims()
{
    /* the fixed boards of version 1 are only known to recognise an old saved game */
    CHECK((legacy_preset_dims(Preset::Beginner) == Dims{9, 9, 10}));
    CHECK((legacy_preset_dims(Preset::Intermediate) == Dims{16, 16, 40}));
    CHECK((legacy_preset_dims(Preset::Expert) == Dims{30, 16, 99}));
    Preset lp = Preset::Beginner;
    CHECK(legacy_preset_of(Dims{30, 16, 99}, lp) && lp == Preset::Expert);
    CHECK(!legacy_preset_of(Dims{30, 16, 98}, lp) && !legacy_preset_of(Dims{16, 30, 99}, lp));
    CHECK(parse_preset("expert", lp) && lp == Preset::Expert && !parse_preset("custom", lp));
    CHECK((clamp_dims(1, 99, 0) == Dims{5, 30, 1}));
    CHECK((clamp_dims(5, 5, 100) == Dims{5, 5, 16}));
    CHECK((clamp_dims(30, 30, 891) == Dims{30, 30, 891}));
    CHECK((clamp_dims(30, 30, 892) == Dims{30, 30, 891}));
    CHECK(clamp_dims(-5, -5, -5).mines == 1);
    for (int w = 0; w <= 40; w += 5)
        for (int h = 0; h <= 40; h += 7)
            for (int m = -3; m < 1000; m += 37) {
                const Dims d = clamp_dims(w, h, m);
                CHECK(d.w >= 5 && d.w <= 30 && d.h >= 5 && d.h <= 30 && d.mines >= 1 && d.mines <= d.w * d.h - 9);
            }
    Preset p;
    CHECK(parse_preset("expert", p) && p == Preset::Expert);
    CHECK(!parse_preset("Expert", p));
    Game g(GameConfig{Dims{3, 99, 500}, 5, false});   /* the game clamps its board too */
    CHECK(g.width() == 5 && g.height() == 30 && g.board().mines() == 141);
}

void test_flood_fill()
{
    /* one mine in the corner of an open board: a click at the far corner opens everything else */
    for (const Dims d : {Dims{9, 9, 1}, Dims{30, 30, 1}, Dims{30, 16, 1}, Dims{5, 30, 1}}) {
        Board b(d.w, d.h, d.mines);
        std::vector<std::uint8_t> map(static_cast<std::size_t>(d.w * d.h), 0);
        map[static_cast<std::size_t>(d.w * d.h - 1)] = 1;
        CHECK(b.place(map));
        std::vector<int> opened;
        CHECK(b.reveal(0, &opened) == RevealResult::Opened);
        CHECK(static_cast<int>(opened.size()) == d.w * d.h - 1);   /* every safe cell (30x30: 899, no stack overflow) */
        CHECK(opened.front() == 0);
        CHECK(b.all_safe_revealed());
        CHECK(std::set<int>(opened.begin(), opened.end()).size() == opened.size());
        CHECK(b.state(d.w * d.h - 1) == CellState::Hidden && b.adjacent(d.w * d.h - 2) == 1);
    }
    /* zero regions reach every edge: a wall of mines in column 3 splits a 7x5 board */
    {
        Board b(7, 5, 5);
        std::vector<std::uint8_t> map(35, 0);
        for (int y = 0; y < 5; ++y) map[static_cast<std::size_t>(y * 7 + 3)] = 1;
        b.place(map);
        std::vector<int> opened;
        b.reveal(0, &opened);
        /* columns 0..2: column 2 has the number 5/3 next to the wall, columns 0..1 are zeros */
        CHECK(opened.size() == 15);
        for (int y = 0; y < 5; ++y) {
            CHECK(b.state(y * 7 + 0) == CellState::Revealed);
            CHECK(b.state(y * 7 + 2) == CellState::Revealed);
            CHECK(b.state(y * 7 + 4) == CellState::Hidden);
        }
        CHECK(b.adjacent(2) == 2 && b.adjacent(2 * 7 + 2) == 3);
    }
    /* a flag stops the flood and stays; revealed / flagged / out of range cells are ignored */
    {
        Board b(6, 6, 1);
        std::vector<std::uint8_t> map(36, 0);
        map[35] = 1;
        b.place(map);
        CHECK(b.toggle_flag(7) && b.state(7) == CellState::Flagged && b.flag_count() == 1);
        std::vector<int> opened;
        b.reveal(0, &opened);
        CHECK(b.state(7) == CellState::Flagged);
        CHECK(b.safe_left() == 1);   /* only the flagged safe cell is left */
        CHECK(b.reveal(7) == RevealResult::NoOp);
        CHECK(b.reveal(0) == RevealResult::NoOp);
        CHECK(b.reveal(-1) == RevealResult::NoOp && b.reveal(36) == RevealResult::NoOp);
        CHECK(!b.toggle_flag(0));   /* revealed */
        CHECK(b.toggle_flag(7) && b.state(7) == CellState::Hidden && b.flag_count() == 0);
        CHECK(b.reveal(7) == RevealResult::Opened && b.all_safe_revealed());
    }
    /* place() refuses a layout with the wrong number of mines or size */
    {
        Board b(5, 5, 3);
        CHECK(!b.place(std::vector<std::uint8_t>(25, 0)));
        CHECK(!b.place(std::vector<std::uint8_t>(24, 1)));
        CHECK(!b.placed());
        CHECK(b.reveal(0) == RevealResult::NoOp);   /* no layout yet */
    }
}

/* ------------------------------------------------------------------------------------------------ first click */

int mines_in(const std::vector<std::uint8_t> &m)
{
    int n = 0;
    for (auto v : m) n += v ? 1 : 0;
    return n;
}

void test_first_click_safety()
{
    int checked = 0;
    for (const Dims d : {Dims{9, 9, 10}, Dims{16, 16, 40}, Dims{30, 16, 99}, Dims{5, 5, 16}, Dims{30, 30, 891}, Dims{7, 30, 198}}) {
        const int n = d.w * d.h;
        TestRng rng(static_cast<std::uint64_t>(d.w * 1000 + d.mines));
        for (int s = 0; s < 3000; ++s) {
            /* corners, edges and the middle are all tried */
            int first;
            switch (s % 5) {
            case 0: first = 0; break;
            case 1: first = n - 1; break;
            case 2: first = d.w - 1; break;
            default: first = static_cast<int>(rng.below(static_cast<std::uint32_t>(n))); break;
            }
            const auto map = place_mines(d.w, d.h, d.mines, rng.next(), first);
            bool ok = static_cast<int>(map.size()) == n && mines_in(map) == d.mines;
            const int fx = first % d.w, fy = first / d.w;
            for (int y = std::max(0, fy - 1); y <= std::min(d.h - 1, fy + 1); ++y)
                for (int x = std::max(0, fx - 1); x <= std::min(d.w - 1, fx + 1); ++x)
                    if (map[static_cast<std::size_t>(y * d.w + x)]) ok = false;
            CHECK(ok);
            ++checked;
        }
    }
    CHECK(checked == 18000);
    /* through the Game: the first reveal never loses and always opens an area (the first cell is a zero) */
    for (const Dims d : {Dims{9, 9, 10}, Dims{16, 16, 40}, Dims{30, 16, 99}}) {
        for (std::uint64_t seed = 1; seed <= 400; ++seed) {
            Game g(GameConfig{d, seed, false});
            const int x = static_cast<int>(seed * 7 % static_cast<std::uint64_t>(d.w));
            const int y = static_cast<int>(seed * 13 % static_cast<std::uint64_t>(d.h));
            const Game::Outcome o = g.reveal(x, y);
            CHECK(o == Game::Outcome::Opened);
            CHECK(g.board().adjacent(g.board().index(x, y)) == 0);
            CHECK(g.last_opened().size() >= 4 && g.last_opened().front() == g.board().index(x, y));
            CHECK(g.status() == Game::Status::Playing);
        }
    }
}

void test_determinism()
{
    const Dims d{16, 16, 40};
    const auto a = place_mines(d.w, d.h, d.mines, 12345, 100);
    const auto b = place_mines(d.w, d.h, d.mines, 12345, 100);
    const auto c = place_mines(d.w, d.h, d.mines, 12346, 100);
    CHECK(a == b);
    CHECK(a != c);
    /* two games with the same seed and the same first cell have the same board, plain and fair */
    for (const bool fair : {false, true}) {
        Game g1(GameConfig{d, 777, fair}), g2(GameConfig{d, 777, fair});
        g1.reveal(3, 4);
        g2.reveal(3, 4);
        CHECK(g1.board().mine_map() == g2.board().mine_map());
        CHECK(g1.seed_used() == g2.seed_used() && g1.attempts() == g2.attempts());
        CHECK(g1.board().mine_map().size() == 256);
    }
    const GenResult r1 = generate(30, 16, 99, 4242, 77, true), r2 = generate(30, 16, 99, 4242, 77, true);
    CHECK(r1.mines == r2.mines && r1.attempts == r2.attempts && r1.fair_failed == r2.fair_failed);
    /* a plain game is the seed's own layout */
    Game g(GameConfig{d, 777, false});
    g.reveal(3, 4);
    CHECK(g.board().mine_map() == place_mines(d.w, d.h, d.mines, 777, 4 * 16 + 3));
}

/* ------------------------------------------------------------------------------------------------ game */

void test_chord()
{
    const int w = 6;
    /* right count: the number 1 at (0,0) with the mine at (1,1) flagged opens (1,0) and (0,1) */
    {
        auto st = grid(6, 6, 'h');
        st[0][0] = 'r';
        st[1][1] = 'f';
        Game g = make_game(kSix, 4, 0, st);
        CHECK(g.mines_left() == 1);
        CHECK(g.chord(0, 0) == Game::ChordOutcome::Opened);
        CHECK(g.board().state(1) == CellState::Revealed && g.board().state(w) == CellState::Revealed);
        CHECK(g.board().state(w + 1) == CellState::Flagged);
        CHECK(g.last_opened().size() == 2 && g.status() == Game::Status::Playing);
    }
    /* wrong count (no flag, then two flags): nothing happens and it is reported */
    {
        auto st = grid(6, 6, 'h');
        st[0][0] = 'r';
        Game g = make_game(kSix, 4, 0, st);
        const std::string before = g.save();
        CHECK(g.chord(0, 0) == Game::ChordOutcome::FlagMismatch);
        CHECK(g.save() == before && g.last_opened().empty());
        g.toggle_flag(1, 0);
        g.toggle_flag(1, 1);
        const std::string before2 = g.save();
        CHECK(g.chord(0, 0) == Game::ChordOutcome::FlagMismatch);
        CHECK(g.save() == before2);
    }
    /* a wrong flag: the count matches but a mine is opened */
    {
        auto st = grid(6, 6, 'h');
        st[0][0] = 'r';
        st[0][1] = 'f';   /* (1,0) is flagged, the mine at (1,1) is not */
        Game g = make_game(kSix, 4, 0, st);
        CHECK(g.chord(0, 0) == Game::ChordOutcome::Exploded);
        CHECK(g.status() == Game::Status::Lost);
        CHECK(g.exploded().size() == 1 && g.exploded()[0] == w + 1);
        CHECK(g.wrong_flags() == std::vector<int>{1});
    }
    /* chord on a zero, a hidden cell, a flag, outside, before the start: ignored */
    {
        Game g = make_game(kSix, 5, 0);   /* (5,0) revealed, hidden around */
        CHECK(g.chord(0, 0) == Game::ChordOutcome::Ignored);
        CHECK(g.chord(-1, 0) == Game::ChordOutcome::Ignored);
        auto st = grid(6, 6, 'r');
        st[1][1] = 'f';
        st[5][5] = 'h';
        st[5][0] = 'h';   /* a safe cell still hidden, away from the chord */
        Game g2 = make_game(kSix, 4, 0, st);
        CHECK(g2.chord(3, 3) == Game::ChordOutcome::Ignored);   /* a zero */
        CHECK(g2.chord(1, 1) == Game::ChordOutcome::Ignored);   /* a flag */
        CHECK(g2.chord(0, 0) == Game::ChordOutcome::NothingToOpen);   /* flagged, nothing hidden around */
        Game fresh(GameConfig{Dims{9, 9, 10}, 3, false});
        CHECK(fresh.chord(0, 0) == Game::ChordOutcome::Ignored);
    }
    /* a chord that opens the last safe cells wins */
    {
        auto st = grid(6, 6, 'r');
        st[1][1] = 'f';
        st[5][5] = 'h';
        st[0][1] = 'h';
        st[1][0] = 'h';
        Game g = make_game(kSix, 4, 0, st);
        CHECK(g.chord(0, 0) == Game::ChordOutcome::Won);
        CHECK(g.status() == Game::Status::Won && g.finished());
        CHECK(g.mines_left() == 0 && g.board().state(5 * 6 + 5) == CellState::Flagged);   /* the other mine got its flag */
    }
    /* a chord opening a zero floods */
    {
        const std::vector<std::string> m = {"*.....", "......", "......", "......", "......", "......"};
        auto st = grid(6, 6, 'h');
        st[0][1] = 'r';
        st[0][0] = 'f';
        Game g = make_game(m, 5, 5, st);   /* (1,0) shows 1; flag (0,0) matches */
        CHECK(g.chord(1, 0) == Game::ChordOutcome::Won || g.chord(1, 0) == Game::ChordOutcome::Opened);
        CHECK(g.status() == Game::Status::Won);   /* everything else flooded open */
    }
}

void test_win_loss()
{
    /* loss: the exploded cell, the mines still hidden nearest first, the flagged ones left alone, wrong flags */
    const std::vector<std::string> m = {"*.....*", ".......", ".......", "...*...", ".......", ".*.....", "......."};
    {
        Game g = make_game(m, 6, 6);
        CHECK(g.mines_left() == 4);
        g.toggle_flag(0, 0);            /* right */
        g.toggle_flag(2, 2);            /* wrong */
        g.toggle_flag(4, 4);            /* wrong */
        CHECK(g.mines_left() == 1);
        CHECK(g.reveal(3, 3) == Game::Outcome::Exploded);
        CHECK(g.status() == Game::Status::Lost);
        CHECK(g.exploded() == std::vector<int>{3 * 7 + 3});
        CHECK((g.wrong_flags() == std::vector<int>{2 * 7 + 2, 4 * 7 + 4}));
        const auto order = g.mine_reveal_order();
        /* (1,5) at distance^2 4+4 = 8, then (6,0) at 9+9 = 18; (0,0) is flagged so it is not in the list */
        CHECK((order == std::vector<int>{5 * 7 + 1, 6}));
        /* nothing works any more */
        CHECK(!g.toggle_flag(1, 1));
        CHECK(g.reveal(1, 1) == Game::Outcome::Ignored);
        CHECK(g.chord(6, 6) == Game::ChordOutcome::Ignored);
        CHECK(g.hint().kind == Game::HintKind::Unavailable);
        CHECK(!g.record().won && g.record().started);
    }
    /* a flagged mine cannot be dug by mistake */
    {
        Game g = make_game(m, 6, 6);
        g.toggle_flag(3, 3);
        CHECK(g.reveal(3, 3) == Game::Outcome::Ignored && g.status() == Game::Status::Playing);
    }
    /* win: open every safe cell; the mines get flags; the counter reads 0 */
    {
        Game g = make_game(m, 6, 6);
        int opened_total = 0;
        Game::Outcome last = Game::Outcome::Ignored;
        for (int i = 0; i < 49; ++i)
            if (!g.board().is_mine(i)) {
                last = g.reveal(i % 7, i / 7);
                if (last != Game::Outcome::Ignored) opened_total += static_cast<int>(g.last_opened().size());
                if (g.status() == Game::Status::Won) break;
            }
        CHECK(last == Game::Outcome::Won && g.status() == Game::Status::Won);
        CHECK(opened_total >= 44 - 1);   /* every safe cell was reported by some reveal (the first click is counted too) */
        CHECK(g.mines_left() == 0 && g.board().flag_count() == 4);
        CHECK(g.record().won && g.wrong_flags().empty());
        CHECK(!g.toggle_flag(0, 0));
    }
    /* mines left goes negative */
    {
        Game g = make_game(kSix, 5, 0);
        for (int i = 0; i < 4; ++i) CHECK(g.toggle_flag(i, 3));
        CHECK(g.mines_left() == -2);
        CHECK(g.toggle_flag(0, 3) && g.mines_left() == -1);
    }
    /* flags before the first reveal; a flagged first cell is not dug */
    {
        Game g(GameConfig{Dims{9, 9, 10}, 9, false});
        CHECK(g.toggle_flag(4, 4) && g.mines_left() == 9);
        CHECK(g.reveal(4, 4) == Game::Outcome::Ignored && g.status() == Game::Status::Ready);
        CHECK(g.toggle_flag(4, 4));
        CHECK(g.reveal(4, 4) == Game::Outcome::Opened && g.status() == Game::Status::Playing);
        CHECK(!g.toggle_flag(4, 4));   /* revealed */
        CHECK(g.reveal(99, 99) == Game::Outcome::Ignored);
    }
}

void test_timer()
{
    Game g(GameConfig{Dims{9, 9, 10}, 11, false});
    g.tick(5000);
    CHECK(g.clock_ms() == 0 && !g.clock_running());
    g.toggle_flag(0, 0);
    g.tick(1000);
    CHECK(g.clock_ms() == 0);   /* a flag does not start it */
    g.reveal(4, 4);
    CHECK(g.clock_ms() == 0 && g.clock_running());   /* it starts from here */
    g.tick(1500);
    g.tick(600);
    CHECK(g.clock_ms() == 2100 && g.time_ms() == 2100 && g.display_seconds() == 2);
    g.set_paused(true);
    CHECK(g.paused() && !g.clock_running());
    g.tick(60000);
    CHECK(g.clock_ms() == 2100);
    g.set_paused(false);
    g.tick(900);
    CHECK(g.clock_ms() == 3000 && g.display_seconds() == 3);
    /* the cap */
    g.tick(5000000);
    CHECK(g.display_seconds() == 999 && g.clock_ms() == 5003000);
    CHECK(display_seconds(0) == 0 && display_seconds(999) == 0 && display_seconds(1000) == 1 &&
          display_seconds(998999) == 998 && display_seconds(999000) == 999 && display_seconds(5000000000ull) == 999);
    /* it stops at a loss and at a win */
    Game lost = make_game(kSix, 5, 0, {}, 7000);
    lost.tick(1000);
    CHECK(lost.clock_ms() == 8000);
    CHECK(lost.reveal(1, 1) == Game::Outcome::Exploded);
    lost.tick(1000);
    CHECK(lost.clock_ms() == 8000 && !lost.clock_running());
    auto st = grid(6, 6, 'r');
    st[1][1] = 'h';
    st[5][5] = 'h';
    st[2][2] = 'h';
    Game won = make_game(kSix, 4, 0, st, 1234);
    CHECK(won.reveal(2, 2) == Game::Outcome::Won);
    won.tick(1000);
    CHECK(won.clock_ms() == 1234);
    /* a time survives a save */
    Game g2 = make_game(kSix, 5, 0, {}, 4321);
    Game g3;
    CHECK(Game::load(g2.save(), g3) && g3.clock_ms() == 4321 && g3.clock_running());
    CHECK(!g3.paused());
}

void test_hint_rules()
{
    const std::vector<std::string> m = {".*...", ".....", ".....", ".....", "....."};
    auto st = grid(5, 5, 'r');
    for (int x = 0; x < 5; ++x) st[0][static_cast<std::size_t>(x)] = 'h';
    Game g = make_game(m, 4, 4, st, 1000);
    CHECK(g.hints_used() == 0 && !g.assisted() && g.penalty_ms() == 0);
    const Game::Hint h = g.hint();
    CHECK(h.kind == Game::HintKind::Safe && h.cell >= 0 && !g.board().is_mine(h.cell));
    CHECK(g.hints_used() == 1 && g.penalty_ms() == 10000 && g.assisted());
    CHECK(g.time_ms() == 11000 && g.display_seconds() == 11);
    g.tick(500);
    CHECK(g.time_ms() == 11500);
    const Game::Hint h2 = g.hint();
    CHECK(h2.kind == Game::HintKind::Safe && g.hints_used() == 2 && g.penalty_ms() == 20000);
    /* the hint points only: nothing was revealed */
    CHECK(g.board().safe_left() == 4);
    /* a mine hint once the safe cells are open */
    for (int x : {0, 2, 3, 4}) g.reveal(x, 0);
    CHECK(g.status() == Game::Status::Won);
    const GameRecord r = g.record();
    CHECK(r.won && r.assisted && r.hints_used == 2 && r.time_ms == 20000 + 1500);
    /* the assisted flag and penalty survive a save */
    Game g2 = make_game(m, 4, 4, st);
    g2.hint();
    Game g3;
    CHECK(Game::load(g2.save(), g3) && g3.hints_used() == 1 && g3.penalty_ms() == 10000 && g3.assisted());
    /* no hint before the first reveal, none after the end, and none costs anything when nothing is provable */
    Game fresh(GameConfig{Dims{9, 9, 10}, 1, false});
    CHECK(fresh.hint().kind == Game::HintKind::Unavailable && fresh.hints_used() == 0 && fresh.penalty_ms() == 0);
    CHECK(g.hint().kind == Game::HintKind::Unavailable);
    /* two hidden cells at the corner: (0,0) is the safe one (the number at (0,1) sees the mine at (1,0) via (2,0)) */
    auto st2 = grid(5, 5, 'r');
    st2[0][0] = 'h';
    st2[0][1] = 'h';
    Game g4 = make_game(m, 4, 4, st2);
    const Game::Hint h4 = g4.hint();
    CHECK(h4.kind == Game::HintKind::Safe && h4.cell == 0);
}

} // namespace

void test_package_group();

int main()
{
    test_dims();
    test_flood_fill();
    test_first_click_safety();
    test_determinism();
    test_chord();
    test_win_loss();
    test_timer();
    test_hint_rules();
    test_solver_group();
    test_store_group();
    test_view_group();
    test_package_group();
    std::printf("test-mine-field: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
