/*
 * SPDX-License-Identifier: MIT
 *
 * Net Walk unit tests: one game (moves, locks, the clock, the win), the scores book and its file.
 */
#include "test_util.hpp"

#include "fsutil.hpp"
#include "scores.hpp"

#include <cstdlib>
#include <unistd.h>

using namespace netwalk;

namespace {

/* turn tile i clockwise until it is in its solved orientation; the moves it took */
int solve_by_turns(Game &g, int i)
{
    int n = 0;
    while (g.board().mask(i) != g.board().solved_mask(i) && n < 4) {
        if (g.turn(i, true)) ++n;
        else break;
    }
    return n;
}

void test_play()
{
    Game g;
    g.start(Level::Easy, 42);
    CHECK(g.board().w() == 5 && g.board().h() == 5 && !g.board().wrap());
    CHECK(g.moves() == 0 && !g.started() && !g.won() && g.elapsed_ms() == 0);
    g.tick(5000); /* the clock waits for the first move */
    CHECK(g.elapsed_ms() == 0);

    /* a cross does not turn and is not a move; a locked tile does not turn and is not a move */
    int first = -1, cross = -1;
    for (int i = 0; i < g.board().size(); ++i) {
        if (g.board().kind(i) == Kind::Cross) cross = i;
        if (first < 0 && g.board().mask(i) != g.board().solved_mask(i) && g.board().kind(i) != Kind::Cross) first = i;
    }
    if (cross >= 0) CHECK(!g.turn(cross, true) && g.moves() == 0);
    CHECK(first >= 0);
    CHECK(g.toggle_lock(first) && g.board().locked(first));
    const std::uint8_t before = g.board().mask(first);
    CHECK(!g.turn(first, true) && !g.turn(first, false));
    CHECK(g.board().mask(first) == before && g.moves() == 0);
    CHECK(g.toggle_lock(first) && !g.board().locked(first));

    /* a turn is a move, either way; the clock starts with it */
    CHECK(g.turn(first, true) && g.moves() == 1);
    CHECK(g.turn(first, false) && g.moves() == 2 && g.board().mask(first) == before);
    g.tick(1500);
    CHECK(g.elapsed_ms() == 1500 && g.elapsed_seconds() == 1);

    /* solve the rest: the win is a property of the state, so it comes when the last wrong tile is put right */
    int total = g.moves();
    for (int i = 0; i < g.board().size(); ++i) {
        total += solve_by_turns(g, i);
        if (g.won()) CHECK(g.board().is_won());
    }
    CHECK(g.won() && g.board().is_won());
    CHECK(g.moves() == total);
    /* the board is frozen after the win: no turns, no locks, no clock */
    const int moves = g.moves();
    const std::uint32_t t = g.elapsed_ms();
    CHECK(!g.turn(first, true) && !g.toggle_lock(first));
    g.tick(9000);
    CHECK(g.moves() == moves && g.elapsed_ms() == t);
    /* a new game resets everything */
    g.start(Level::Hard, 5);
    CHECK(g.moves() == 0 && !g.won() && g.elapsed_ms() == 0 && g.board().w() == 9);
    CHECK(g.powered().size() == 81);
}

void test_power_is_live()
{
    Game g;
    g.start(Level::Normal, 9);
    const int server = g.board().server();
    CHECK(g.powered()[static_cast<std::size_t>(server)] == 1);
    /* the cached power always equals a fresh computation, move after move */
    std::uint64_t s = 1;
    for (int k = 0; k < 300; ++k) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        g.turn(static_cast<int>((s >> 33) % static_cast<std::uint64_t>(g.board().size())), ((s >> 20) & 1) != 0);
        CHECK(g.powered() == g.board().powered());
        if (g.won()) break;
    }
}

void test_levels()
{
    CHECK(level_info(Level::Easy).w == 5 && !level_info(Level::Easy).wrap);
    CHECK(level_info(Level::Normal).w == 7 && !level_info(Level::Normal).wrap);
    CHECK(level_info(Level::Hard).w == 9 && !level_info(Level::Hard).wrap);
    CHECK(level_info(Level::Expert).w == 9 && level_info(Level::Expert).wrap);
    bool found = false;
    CHECK(level_from_key("expert", &found) == Level::Expert && found);
    CHECK(level_from_key("nonsense", &found) == Level::Normal && !found);
    Game g;
    g.start(Level::Expert, 3);
    CHECK(g.board().wrap() && g.level() == Level::Expert);
    CHECK(format_time(0) == "00:00" && format_time(97) == "01:37" && format_time(6005) == "100:05" && format_time(-4) == "00:00");
}

void test_scores()
{
    Scores s;
    for (int i = 0; i < kLevelCount; ++i)
        CHECK(s.best(static_cast<Level>(i)).moves == 0 && s.best(static_cast<Level>(i)).seconds == 0);
    Record r = s.record(Level::Hard, 80, 200);
    CHECK(r.new_moves && r.new_time && s.best(Level::Hard).moves == 80 && s.best(Level::Hard).seconds == 200);
    r = s.record(Level::Hard, 90, 150); /* more moves, less time: only the time improves */
    CHECK(!r.new_moves && r.new_time && s.best(Level::Hard).moves == 80 && s.best(Level::Hard).seconds == 150);
    r = s.record(Level::Hard, 70, 400);
    CHECK(r.new_moves && !r.new_time && s.best(Level::Hard).moves == 70 && s.best(Level::Hard).seconds == 150);
    r = s.record(Level::Hard, 70, 150); /* a tie is not a record */
    CHECK(!r.new_moves && !r.new_time);
    CHECK(s.best(Level::Easy).moves == 0); /* the levels are separate */
    r = s.record(Level::Easy, 5, 0); /* 0 seconds is stored as 1: 0 means none */
    CHECK(s.best(Level::Easy).seconds == 1 && r.new_time);
    s.set_last(Level::Expert);

    const std::string js = s.to_json();
    Scores t;
    CHECK(Scores::from_json(js, t));
    for (int i = 0; i < kLevelCount; ++i) {
        const Level l = static_cast<Level>(i);
        CHECK(t.best(l).moves == s.best(l).moves && t.best(l).seconds == s.best(l).seconds);
    }
    CHECK(t.last() == Level::Expert);
    CHECK(js.find("\"hard\":{\"moves\":70,\"seconds\":150}") != std::string::npos);

    /* damaged or foreign text: no scores, never a crash */
    Scores u;
    u.record(Level::Easy, 1, 1);
    CHECK(!Scores::from_json("", u) && u.best(Level::Easy).moves == 0);
    CHECK(!Scores::from_json("not json at all", u));
    CHECK(Scores::from_json("{\"best\":{\"easy\":{\"moves\":-3,\"seconds\":\"x\"}}}", u) && u.best(Level::Easy).moves == 0 &&
          u.best(Level::Easy).seconds == 0);
    CHECK(Scores::from_json("{\"best\":{\"normal\":{\"moves\":12,\"seconds\":34}}}", u) && u.best(Level::Normal).moves == 12 &&
          u.best(Level::Normal).seconds == 34);
    Scores v;
    CHECK(Scores::from_json("{\"version\":1,\"last\":\"bogus\",\"best\":{}}", v) && v.last() == Level::Normal);
}

void test_store()
{
    char tmpl[] = "/tmp/netwalk-test-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    const std::string path = std::string(dir) + "/data";
    ScoreStore store(path);
    CHECK(store.load().best(Level::Easy).moves == 0); /* no file yet */
    Scores s;
    s.record(Level::Normal, 33, 61);
    s.set_last(Level::Normal);
    CHECK(store.save(s)); /* makes the folder */
    const Scores back = store.load();
    CHECK(back.best(Level::Normal).moves == 33 && back.best(Level::Normal).seconds == 61);
    /* a damaged file reads as empty and the next save replaces it */
    CHECK(write_file_atomic(path + "/scores.json", "{{{{ broken"));
    CHECK(store.load().best(Level::Normal).moves == 0);
    CHECK(store.save(s) && store.load().best(Level::Normal).moves == 33);
    /* the data folder rule */
    setenv("NETWALK_DATA", "/tmp/nw-data-x", 1);
    CHECK(ScoreStore::default_dir() == "/tmp/nw-data-x");
    unsetenv("NETWALK_DATA");
    setenv("XDG_DATA_HOME", "/tmp/xdg-x", 1);
    CHECK(ScoreStore::default_dir() == "/tmp/xdg-x/net-walk");
    unsetenv("XDG_DATA_HOME");
    CHECK(ScoreStore::default_dir().find("/net-walk") != std::string::npos);
    remove_file(path + "/scores.json");
    rmdir(path.c_str());
    rmdir(dir);
}

} // namespace

void test_game_group()
{
    test_play();
    test_power_is_live();
    test_levels();
}

void test_scores_group()
{
    test_scores();
    test_store();
}
