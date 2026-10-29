/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the statistics, the settings, the saved game, the store (atomic files, corrupt files, versions) and the
 * session's rules about when a game counts (core/stats.hpp, settings.hpp, game.hpp save/load, store.hpp, session.hpp).
 */
#include "test_util.hpp"

#include "fsutil.hpp"
#include "generator.hpp"
#include "session.hpp"
#include "solver.hpp"

#include <algorithm>
#include <cstdlib>
#include <unistd.h>

using namespace minefield;

namespace {

struct TempDir {
    std::string path;
    TempDir()
    {
        char tpl[] = "/tmp/minefield-test-XXXXXX";
        const char *p = mkdtemp(tpl);
        path = p ? p : "/tmp/minefield-test-fallback";
    }
    ~TempDir()
    {
        const std::string cmd = "rm -rf '" + path + "'";
        if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup of %s failed\n", path.c_str());
    }
    std::string data() const { return path + "/data"; }
    std::string conf() const { return path + "/conf"; }
};

GameRecord rec(Preset p, bool won, std::uint32_t ms, std::uint64_t seed = 1, bool fair = false, bool assisted = false,
               bool started = true)
{
    GameRecord r;
    r.preset = p;
    r.dims = p == Preset::Beginner ? Dims{11, 8, 11} : (p == Preset::Intermediate ? Dims{14, 10, 22} : Dims{18, 12, 44});
    r.won = won;
    r.time_ms = ms;
    r.seed = seed;
    r.fair = fair;
    r.assisted = assisted;
    r.started = started;
    return r;
}

std::string slurp(const std::string &path)
{
    std::string s;
    read_file(path, s);
    return s;
}

/* ------------------------------------------------------------------------------------------------ stats */

void test_stats()
{
    Stats st;
    CHECK(st.get(Preset::Expert).played == 0);
    /* never recorded: a game never started */
    CHECK(!st.record(rec(Preset::Beginner, false, 0, 1, false, false, false)));
    CHECK(st.get(Preset::Beginner).played == 0 && st == Stats());
    /* best times: ascending, an equal time ranks after the older one, five kept */
    CHECK(st.record(rec(Preset::Beginner, true, 50000, 1)));
    CHECK(st.record(rec(Preset::Beginner, true, 30000, 2, true)));
    CHECK(st.record(rec(Preset::Beginner, true, 40000, 3)));
    CHECK(st.record(rec(Preset::Beginner, true, 30000, 4)));
    CHECK(st.record(rec(Preset::Beginner, true, 60000, 5)));
    CHECK(st.record(rec(Preset::Beginner, true, 20000, 6)));
    PresetStats b = st.get(Preset::Beginner);
    CHECK(b.played == 6 && b.won == 6 && b.streak == 6 && b.best_streak == 6);
    CHECK(b.best.size() == 5);
    const std::uint32_t times[] = {20000, 30000, 30000, 40000, 50000};
    const std::uint64_t seeds[] = {6, 2, 4, 3, 1};
    for (int i = 0; i < 5; ++i) CHECK(b.best[static_cast<std::size_t>(i)].time_ms == times[i] && b.best[static_cast<std::size_t>(i)].seed == seeds[i]);
    CHECK(b.best[1].fair && !b.best[2].fair);   /* the flag is kept per entry, one table */
    CHECK(b.total_time_ms == 230000);
    CHECK(st.rank_of(Preset::Beginner, 10000) == 0 && st.rank_of(Preset::Beginner, 30000) == 3 &&
          st.rank_of(Preset::Beginner, 50000) == -1 && st.rank_of(Preset::Beginner, 49999) == 4);
    CHECK(st.rank_of(Preset::Intermediate, 999999) == 0);
    /* a best entry keeps the board it was won on */
    CHECK(b.best[0].w == 11 && b.best[0].h == 8 && b.best[0].mines == 11);
    /* a loss resets the streak, not the best streak; its time counts in the total */
    CHECK(st.record(rec(Preset::Beginner, false, 5000)));
    b = st.get(Preset::Beginner);
    CHECK(b.played == 7 && b.won == 6 && b.streak == 0 && b.best_streak == 6 && b.total_time_ms == 235000);
    /* a win with hints counts as a win but is never a best time */
    CHECK(st.record(rec(Preset::Beginner, true, 1000, 9, false, true)));
    b = st.get(Preset::Beginner);
    CHECK(b.won == 7 && b.streak == 1 && b.best[0].time_ms == 20000);
    /* presets are separate */
    CHECK(st.record(rec(Preset::Expert, true, 300000, 11)));
    CHECK(st.get(Preset::Expert).best.size() == 1 && st.get(Preset::Intermediate).played == 0);
    /* reset */
    st.reset(Preset::Beginner);
    CHECK(st.get(Preset::Beginner).played == 0 && st.get(Preset::Expert).played == 1);
    st.reset_all();
    CHECK(st == Stats());
}

void test_stats_json()
{
    Stats st;
    st.record(rec(Preset::Beginner, true, 31000, 0xFFFFFFFFFFFFFFFFull, true));
    st.record(rec(Preset::Beginner, false, 4000));
    st.record(rec(Preset::Expert, true, 123456, 42));
    Stats back;
    std::string why;
    CHECK(Stats::from_json(st.to_json(), back, &why));
    CHECK(back == st);
    CHECK(back.get(Preset::Beginner).best[0].seed == 0xFFFFFFFFFFFFFFFFull);   /* 64 bit survives */
    CHECK(Stats::from_json(Stats().to_json(), back) && back == Stats());
    /* damaged */
    const std::string good = st.to_json();
    Stats out;
    for (const std::string &bad : {std::string(""), std::string("garbage"), good.substr(0, good.size() / 2), std::string("[]"),
                                   std::string("{\"version\":1}"), std::string("{\"version\":0,\"presets\":{}}"),
                                   std::string("{\"version\":3,\"presets\":{}}"),
                                   std::string("{\"version\":1,\"presets\":{}}"),   /* version 1 (fixed boards) is set aside */
                                   std::string("{\"version\":2,\"presets\":{\"beginner\":{\"played\":-1}}}"),
                                   std::string("{\"version\":2,\"presets\":{\"beginner\":{\"played\":1,\"won\":2,\"streak\":0,"
                                               "\"best_streak\":0,\"total_time_ms\":0,\"best\":[]}}}")})
        CHECK(!Stats::from_json(bad, out, &why) && !why.empty());
    /* unknown names are skipped, an unsorted best list is sorted */
    const std::string odd = "{\"version\":2,\"presets\":{\"mega\":{\"played\":1},\"beginner\":{\"played\":2,\"won\":2,\"streak\":2,"
                            "\"best_streak\":2,\"total_time_ms\":9,\"best\":[{\"time_ms\":9,\"seed\":\"1\",\"fair\":false,\"w\":9,\"h\":7,\"mines\":8},"
                            "{\"time_ms\":3,\"seed\":\"2\",\"fair\":true,\"w\":9,\"h\":7,\"mines\":8}]}}}";
    CHECK(Stats::from_json(odd, out, &why));
    CHECK(out.get(Preset::Beginner).best.size() == 2 && out.get(Preset::Beginner).best[0].time_ms == 3);
}

/* ------------------------------------------------------------------------------------------------ settings */

void test_settings()
{
    Settings s;
    CHECK(s.preset == Preset::Beginner && !s.fair && s.animations == Animations::On && !s.flag_mode && s.show_timer);
    CHECK(Settings::first_tap_opens);
    s.preset = Preset::Intermediate;
    s.fair = true;
    s.animations = Animations::Reduced;
    s.flag_mode = true;
    s.show_timer = false;
    Settings back;
    std::string why;
    CHECK(Settings::from_json(s.to_json(), back, &why) && back == s);
    s.animations = Animations::Off;
    s.preset = Preset::Expert;
    CHECK(Settings::from_json(s.to_json(), back) && back == s && back.preset == Preset::Expert);
    /* version 1 files (with a custom board) are read: the custom board is dropped, preset custom becomes beginner */
    CHECK(Settings::from_json("{\"version\":1,\"preset\":\"custom\",\"custom_w\":99,\"custom_h\":1,\"custom_mines\":5000,\"x\":1}", back));
    CHECK(back.preset == Preset::Beginner && back.fair == false && back.show_timer);
    CHECK(Settings::from_json("{\"version\":1,\"preset\":\"expert\",\"fair\":true}", back) && back.preset == Preset::Expert && back.fair);
    CHECK(s.to_json().find("custom") == std::string::npos);
    CHECK(Settings::from_json("{\"version\":1}", back) && back == Settings());
    /* wrong */
    for (const char *bad : {"", "nope", "{\"version\":0}", "{\"version\":3}", "{}", "{\"version\":1,\"preset\":\"huge\"}",
                            "{\"version\":1,\"fair\":1}", "{\"version\":1,\"animations\":\"wild\"}"})
        CHECK(!Settings::from_json(bad, back, &why));
}

/* ------------------------------------------------------------------------------------------------ saved game */

void test_game_save()
{
    GameConfig gc{Dims{16, 16, 40}, 99, true};
    gc.preset = Preset::Intermediate;
    Game g(gc);
    g.reveal(8, 8);
    g.toggle_flag(0, 0);
    g.tick(12345);
    g.hint();
    const std::string text = g.save();
    Game back;
    std::string why;
    CHECK(Game::load(text, back, &why));
    CHECK(back.save() == text);
    CHECK(back.board().mine_map() == g.board().mine_map() && back.mines_left() == g.mines_left());
    CHECK(back.status() == Game::Status::Playing && back.first_click() == g.first_click() && back.config().fair);
    CHECK(back.attempts() == g.attempts() && back.seed_used() == g.seed_used() && back.fair_failed() == g.fair_failed());
    CHECK(back.time_ms() == g.time_ms() && back.hints_used() == g.hints_used() && back.config().preset == Preset::Intermediate);
    /* version 1 (no preset, the fixed boards): read, the preset recognised from the dims */
    {
        std::string v1 = text;
        const std::size_t pv = v1.find("\"version\": 2");
        const std::size_t pp = v1.find("  \"preset\"");
        CHECK(pv != std::string::npos && pp != std::string::npos);
        v1.erase(pp, v1.find('\n', pp) - pp + 1);
        v1.replace(v1.find("\"version\": 2"), 12, "\"version\": 1");
        Game old;
        CHECK(Game::load(v1, old, &why) && old.config().preset == Preset::Intermediate && old.config().dims == (Dims{16, 16, 40}));
    }
    /* the loaded game plays on */
    CHECK(back.reveal(1, 1) != Game::Outcome::Ignored || back.board().state(17) != CellState::Hidden);
    /* inconsistent saves are refused */
    const auto replace = [&](const std::string &from, const std::string &to) {
        std::string t = text;
        const std::size_t p = t.find(from);
        CHECK(p != std::string::npos);
        if (p != std::string::npos) t.replace(p, from.size(), to);
        return t;
    };
    Game tmp;
    CHECK(!Game::load("", tmp, &why) && !Game::load("{}", tmp, &why) && !Game::load(text.substr(0, text.size() / 2), tmp, &why));
    CHECK(!Game::load(replace("\"version\": 2", "\"version\": 0"), tmp, &why));
    CHECK(!Game::load(replace("\"version\": 2", "\"version\": 3"), tmp, &why));
    CHECK(!Game::load(replace("\"preset\": \"intermediate\"", "\"preset\": \"huge\""), tmp, &why));
    CHECK(!Game::load(replace("\"w\": 16", "\"w\": 4"), tmp, &why));
    CHECK(!Game::load(replace("\"mines\": 40", "\"mines\": 41"), tmp, &why));
    CHECK(!Game::load(replace("\"mines\": 40", "\"mines\": 900"), tmp, &why));
    CHECK(!Game::load(replace("\"first_click\": " + std::to_string(g.first_click()), "\"first_click\": 1"), tmp, &why));
    CHECK(!Game::load(replace("\"seed\": \"99\"", "\"seed\": \"9x\""), tmp, &why));
    CHECK(!Game::load(replace("\"fair\": true", "\"fair\": 1"), tmp, &why));
    {
        /* a revealed mine, a mine under the first click, a won game, a short mine map */
        std::string t = text;
        const std::size_t mp = t.find("\"mine_map\": \"") + 13, cp = t.find("\"cells\": \"") + 10;
        const int mine = static_cast<int>(std::find(g.board().mine_map().begin(), g.board().mine_map().end(), 1) - g.board().mine_map().begin());
        std::string t2 = t;
        t2[cp + static_cast<std::size_t>(mine)] = 'r';
        CHECK(!Game::load(t2, tmp, &why));
        std::string t3 = t;
        t3[mp + static_cast<std::size_t>(g.first_click())] = '1';
        CHECK(!Game::load(t3, tmp, &why));
        std::string t4 = t;
        for (int i = 0; i < 256; ++i)
            if (!g.board().is_mine(i)) t4[cp + static_cast<std::size_t>(i)] = 'r';
        CHECK(!Game::load(t4, tmp, &why));
        std::string t5 = t;
        t5.erase(mp, 3);
        CHECK(!Game::load(t5, tmp, &why));
    }
    /* a finished game is not a save (load refuses a won state; the session never writes one) */
}

/* ------------------------------------------------------------------------------------------------ store */

void test_store()
{
    TempDir td;
    Store store(td.data(), td.conf());
    Stats st;
    Settings se;
    Game g;
    CHECK(store.load_stats(st) == Store::Load::Missing && store.load_settings(se) == Store::Load::Missing &&
          store.load_game(g) == Store::Load::Missing);
    /* round trips */
    st.record(rec(Preset::Intermediate, true, 77000, 5, true));
    CHECK(store.save_stats(st));
    Stats st2;
    CHECK(store.load_stats(st2) == Store::Load::Ok && st2 == st);
    se.preset = Preset::Expert;
    se.fair = true;
    CHECK(store.save_settings(se));
    Settings se2;
    CHECK(store.load_settings(se2) == Store::Load::Ok && se2 == se);
    Game live(GameConfig{Dims{9, 9, 10}, 5, false});
    live.reveal(4, 4);
    CHECK(store.save_game(live));
    Game live2;
    CHECK(store.load_game(live2) == Store::Load::Ok && live2.save() == live.save());
    CHECK(store.clear_game() && store.load_game(live2) == Store::Load::Missing);
    /* atomic writes leave no temp file behind */
    CHECK(!path_exists(td.data() + "/stats.json.tmp") && !path_exists(td.conf() + "/settings.json.tmp"));
    /* the folders and files are private enough to exist at the documented places */
    CHECK(path_exists(td.data() + "/stats.json") && path_exists(td.conf() + "/settings.json"));

    /* corrupt files: set aside as .bad, the part starts afresh, the reason is reported */
    const std::string sp = td.data() + "/stats.json";
    CHECK(write_file_atomic(sp, "{\"version\":1,\"presets\":{\"beginner\":{\"played\":"));   /* cut */
    CHECK(store.load_stats(st2) == Store::Load::Bad && st2 == Stats() && !store.last_error().empty());
    CHECK(path_exists(sp + ".bad") && !path_exists(sp));
    CHECK(store.load_stats(st2) == Store::Load::Missing);
    CHECK(write_file_atomic(sp, "\x01\x02 not json at all"));
    CHECK(store.load_stats(st2) == Store::Load::Bad && path_exists(sp + ".bad"));
    CHECK(write_file_atomic(sp, "{\"version\":0,\"presets\":{}}"));   /* an old / unknown version */
    CHECK(store.load_stats(st2) == Store::Load::Bad);
    CHECK(write_file_atomic(sp, "{\"version\":99,\"presets\":{}}"));  /* a newer one */
    CHECK(store.load_stats(st2) == Store::Load::Bad && st2 == Stats());
    const std::string cp = td.conf() + "/settings.json";
    CHECK(write_file_atomic(cp, "{\"version\":0}"));
    CHECK(store.load_settings(se2) == Store::Load::Bad && se2 == Settings() && path_exists(cp + ".bad"));
    CHECK(write_file_atomic(cp, "[1,2]"));
    CHECK(store.load_settings(se2) == Store::Load::Bad);
    const std::string gp = td.data() + "/game.json";
    CHECK(write_file_atomic(gp, "{\"version\":1,\"w\":9}"));
    CHECK(store.load_game(live2) == Store::Load::Bad && path_exists(gp + ".bad"));
    /* an unwritable place reports the failure instead of throwing */
    Store nowhere("/proc/nonexistent/x", "/proc/nonexistent/y");
    CHECK(!nowhere.save_stats(st) && !nowhere.last_error().empty());
}

/* ------------------------------------------------------------------------------------------------ session */

void test_session()
{
    TempDir td;
    {
        Session s(Store(td.data(), td.conf()), 1234);
        CHECK(!s.load());   /* nothing saved: a fresh game */
        CHECK(s.game().status() == Game::Status::Ready && !s.in_progress());
        /* a new game that was never started costs nothing */
        s.new_game(5);
        s.new_game(6);
        CHECK(s.stats().get(Preset::Beginner).played == 0 && !path_exists(td.data() + "/stats.json"));
        CHECK(s.game().config().seed == 6);
        /* a flag before the first reveal is not a game yet */
        CHECK(s.toggle_flag(0, 0) && !path_exists(td.data() + "/game.json"));
        s.toggle_flag(0, 0);
        CHECK(s.reveal(4, 4) == Game::Outcome::Opened);
        CHECK(s.in_progress() && path_exists(td.data() + "/game.json"));
        s.tick(2500);
        s.save_game();
    }
    {
        /* resumed from the saved game: the same board, the same clock */
        Session s(Store(td.data(), td.conf()), 99);
        CHECK(s.load());
        CHECK(s.game().status() == Game::Status::Playing && s.game().clock_ms() == 2500 && s.game().config().seed == 6);
        const std::uint64_t before = s.game().config().seed;
        (void)before;
        /* a new game while one is in progress: the old one counts as a loss */
        s.new_game();
        CHECK(s.stats().get(Preset::Beginner).played == 1 && s.stats().get(Preset::Beginner).won == 0);
        CHECK(!path_exists(td.data() + "/game.json"));
        CHECK(s.game().status() == Game::Status::Ready);
    }
    {
        Session s(Store(td.data(), td.conf()), 7);
        CHECK(!s.load());   /* the abandoned game's file is gone */
        CHECK(s.stats().get(Preset::Beginner).played == 1);   /* and its loss was written */
        /* a loss by a mine */
        s.new_game(11);
        s.reveal(4, 4);
        int mine = 0;
        while (!s.game().board().is_mine(mine)) ++mine;
        CHECK(s.reveal(mine % 9, mine / 9) == Game::Outcome::Exploded);
        CHECK(s.stats().get(Preset::Beginner).played == 2 && s.stats().get(Preset::Beginner).streak == 0);
        CHECK(!path_exists(td.data() + "/game.json"));
        /* the finished game is not recorded again by the next new_game */
        s.new_game(12);
        CHECK(s.stats().get(Preset::Beginner).played == 2);
        /* a win: fair board played by the solver, clock fed */
        Settings se = s.settings();
        se.fair = true;
        s.set_settings(se);
        CHECK(s.settings_pending());
        s.new_game(13);
        CHECK(!s.settings_pending());
        s.reveal(4, 4);
        s.tick(65000);
        int guard = 0;
        while (s.game().status() == Game::Status::Playing && guard++ < 500) {
            const int c = find_safe_cell(s.game().view());
            if (c < 0) break;
            s.reveal(c % 9, c / 9);
        }
        CHECK(s.game().status() == Game::Status::Won);
        const PresetStats b = s.stats().get(Preset::Beginner);
        CHECK(b.played == 3 && b.won == 1 && b.streak == 1 && b.best.size() == 1 && b.best[0].time_ms == 65000);
        CHECK(b.best[0].fair && b.best[0].seed == 13);
        CHECK(!path_exists(td.data() + "/game.json"));
        s.new_game(14);
        CHECK(s.stats().get(Preset::Beginner).played == 3);   /* the win was recorded once */
        /* a hinted win counts but is no best time */
        s.reveal(4, 4);
        const Game::Hint h = s.hint();
        CHECK(h.kind == Game::HintKind::Safe || h.kind == Game::HintKind::Mine || h.kind == Game::HintKind::None);
        if (h.kind != Game::HintKind::None) CHECK(s.game().assisted() && s.game().penalty_ms() == 10000);
        /* the screen decides the board: Expert on this screen, abandoned, is recorded on Expert only */
        se.preset = Preset::Expert;
        s.set_settings(se);
        s.new_game(15);   /* abandons the hinted game: a loss on Beginner */
        const std::uint32_t played = s.stats().get(Preset::Beginner).played;
        CHECK(played == 4);
        CHECK(!s.set_screen_boards({Dims{8, 6, 6}, Dims{11, 8, 14}, Dims{14, 10, 29}}));   /* a game not dug is replaced silently */
        CHECK(s.game().config().dims == (Dims{14, 10, 29}) && s.game().config().preset == Preset::Expert);
        s.reveal(3, 3);
        s.new_game(16);
        CHECK(s.stats().get(Preset::Beginner).played == 4 && s.stats().get(Preset::Intermediate).played == 0 &&
              s.stats().get(Preset::Expert).played == 1);
        CHECK(s.stats().get(Preset::Expert).won == 0);
        s.reset_stats(Preset::Beginner);
        CHECK(s.stats().get(Preset::Beginner).played == 0);
    }
    {
        /* what the second session saved is what a third reads */
        Session s(Store(td.data(), td.conf()), 8);
        CHECK(!s.load());
        CHECK(s.settings().preset == Preset::Expert && s.settings().fair && s.stats().get(Preset::Beginner).played == 0);
    }
    /* seeds from the session's generator are deterministic from its entropy and differ from call to call */
    Session a(Store(td.data(), td.conf()), 5), b(Store(td.data(), td.conf()), 5);
    const std::uint64_t a1 = a.random_seed(), a2 = a.random_seed();
    CHECK(a1 != 0 && a1 != a2 && b.random_seed() == a1);
}

} // namespace

/* prepare() / supply() (the UI lays the board off the UI thread) and replay() ("same board", "try again") */
void test_prepare_replay()
{
    GameConfig cfg;
    cfg.dims = Dims{16, 16, 40};
    cfg.seed = 4242;
    cfg.fair = true;
    cfg.preset = Preset::Intermediate;
    const int first = 8 * 16 + 8;
    Game direct(cfg);
    CHECK(direct.reveal(8, 8) != Game::Outcome::Ignored);
    const Game::Prepared p = Game::prepare(cfg, first);
    CHECK(p.valid && p.first == first);
    Game viaprep(cfg);
    viaprep.supply(p);
    CHECK(viaprep.status() == Game::Status::Ready);
    CHECK(viaprep.reveal(8, 8) != Game::Outcome::Ignored);
    CHECK(viaprep.board().mine_map() == direct.board().mine_map());
    CHECK(viaprep.seed_used() == direct.seed_used() && viaprep.attempts() == direct.attempts());
    /* a layout for another cell is not used; an out of range cell makes nothing; a started game ignores supply() */
    Game other(cfg);
    other.supply(Game::prepare(cfg, 0));
    other.reveal(8, 8);
    CHECK(other.board().mine_map() == direct.board().mine_map());
    CHECK(!Game::prepare(cfg, -1).valid && !Game::prepare(cfg, 256).valid);
    Game::Prepared q = Game::prepare(cfg, first);
    q.gen.mines.assign(256, 0);
    viaprep.supply(q);   /* already started: ignored */
    CHECK(viaprep.board().mine_map() == direct.board().mine_map());

    /* replay: the same board and first click give the same layout, whatever the settings say now */
    TempDir td;
    Session s(Store(td.data(), td.conf()), 7);
    s.load();
    Settings st;
    st.preset = Preset::Expert;
    s.set_settings(st);
    s.replay(cfg);
    CHECK(s.game().config().dims == cfg.dims && s.game().config().seed == 4242 && s.game().config().fair);
    CHECK(s.reveal(8, 8) != Game::Outcome::Ignored);
    CHECK(s.game().board().mine_map() == direct.board().mine_map());
    const int played = static_cast<int>(s.stats().get(Preset::Intermediate).played);
    s.replay(cfg);   /* the game in progress counts as a loss */
    CHECK(static_cast<int>(s.stats().get(Preset::Intermediate).played) == played + 1);
    CHECK(s.game().status() == Game::Status::Ready);
}

/* a saved game keeps its own dims; a screen that gives its difficulty other dims drops it without a loss record */
void test_screen_rule()
{
    TempDir td;
    const std::array<Dims, kRecordedPresets> a{Dims{9, 7, 8}, Dims{12, 9, 17}, Dims{16, 11, 36}};
    const std::array<Dims, kRecordedPresets> b{Dims{7, 9, 8}, Dims{9, 12, 17}, Dims{11, 16, 36}};   /* the screen turned */
    {
        Session s(Store(td.data(), td.conf()), 1);
        s.set_screen_boards(a);
        CHECK(!s.load() && !s.take_dropped_notice());
        CHECK(s.game().config().dims == a[0] && s.game().config().preset == Preset::Beginner);
        s.reveal(3, 3);
        CHECK(s.in_progress() && path_exists(td.data() + "/game.json"));
    }
    {
        Session s(Store(td.data(), td.conf()), 2);   /* the same screen: resumed */
        s.set_screen_boards(a);
        CHECK(s.load() && s.game().config().dims == a[0] && !s.take_dropped_notice());
    }
    {
        Session s(Store(td.data(), td.conf()), 3);   /* another shape: dropped, no loss, the note once */
        s.set_screen_boards(b);
        CHECK(!s.load());
        CHECK(s.take_dropped_notice() && !s.take_dropped_notice());
        CHECK(s.stats().get(Preset::Beginner).played == 0 && !path_exists(td.data() + "/game.json"));
        CHECK(s.game().config().dims == b[0] && s.game().status() == Game::Status::Ready);
        /* a change inside a session: a game in progress is dropped (no record), an idle one is replaced */
        s.set_screen_boards(a);
        CHECK(s.game().config().dims == a[0] && !s.take_dropped_notice());   /* Ready: replaced silently */
        s.reveal(3, 3);
        CHECK(s.in_progress());
        CHECK(s.set_screen_boards(b) && s.take_dropped_notice());
        CHECK(s.game().config().dims == b[0] && s.game().status() == Game::Status::Ready && s.stats().get(Preset::Beginner).played == 0);
        CHECK(!s.set_screen_boards(b));   /* nothing changed */
    }
    {
        /* an old saved game (version 1: fixed 9x9, no preset) on a screen whose Beginner is another size: dropped, not a crash */
        Game old(GameConfig{Dims{9, 9, 10}, 3, false});
        old.reveal(4, 4);
        std::string text = old.save();
        const std::size_t pp = text.find("  \"preset\"");
        text.erase(pp, text.find('\n', pp) - pp + 1);
        text.replace(text.find("\"version\": 2"), 12, "\"version\": 1");
        Store store(td.data(), td.conf());
        CHECK(write_file_atomic(td.data() + "/game.json", text));
        Session s(Store(td.data(), td.conf()), 4);
        s.set_screen_boards(a);
        CHECK(!s.load() && s.take_dropped_notice() && s.stats().get(Preset::Beginner).played == 0);
        /* the same file on a screen whose Beginner IS 9x9/10 resumes */
        CHECK(write_file_atomic(td.data() + "/game.json", text));
        Session s2(Store(td.data(), td.conf()), 5);
        s2.set_screen_boards({Dims{9, 9, 10}, Dims{12, 9, 17}, Dims{16, 11, 36}});
        CHECK(s2.load() && s2.game().config().dims == (Dims{9, 9, 10}));
        /* an old statistics file (version 1) is set aside, the app starts clean */
        CHECK(write_file_atomic(td.data() + "/stats.json", "{\"version\":1,\"presets\":{}}"));
        Session s3(Store(td.data(), td.conf()), 6);
        s3.load();
        CHECK(s3.stats() == Stats() && path_exists(td.data() + "/stats.json.bad"));
    }
}

void test_store_group()
{
    test_screen_rule();
    test_prepare_replay();
    test_stats();
    test_stats_json();
    test_settings();
    test_game_save();
    test_store();
    test_session();
}
