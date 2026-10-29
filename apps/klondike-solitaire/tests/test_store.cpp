/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the JSON reader, the rules line, the statistics, the saved game and the files (see test_core.cpp
 * for the rest). Files go to a fresh folder under /tmp, removed at the end.
 */
#include "test_util.hpp"

#include "fsutil.hpp"
#include "json.hpp"
#include "stats.hpp"
#include "store.hpp"

#include <cstdlib>
#include <dirent.h>
#include <unistd.h>

using namespace klondike;

static std::string g_tmp;

static void remove_tree(const std::string &dir)
{
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string p = dir + "/" + n;
            if (e->d_type == DT_DIR) remove_tree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

/* ------------------------------------------------------------------------------------------------- json */

static void test_json()
{
    json::Value v;
    CHECK(json::parse("{\"a\": [1, -2, 3.5e2, true, false, null], \"b\": {\"c\": \"x\\ny\\u00e9\\ud83d\\ude00\"}}", v));
    CHECK(v.kind == json::Value::Kind::Object && v.get("a") && v.get("a")->items.size() == 6);
    CHECK(v.get("a")->items[1].as_int() == -2 && !v.get("a")->items[2].is_int());
    CHECK(v.get("a")->items[3].as_bool() && !v.get("a")->items[4].as_bool(true));
    CHECK(v.get("b")->get("c")->as_string() == "x\ny\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(json::parse("9007199254740993", v) && v.as_int() == 9007199254740993ll); /* 64-bit exact */
    CHECK(json::parse("-9223372036854775808", v) && v.as_int() == (-9223372036854775807ll - 1));
    CHECK(json::parse("99999999999999999999", v) && !v.is_int()); /* too big for 64 bits */
    std::string why;
    CHECK(!json::parse("{\"a\": [1, 2", v, &why) && !why.empty());   /* cut */
    CHECK(!json::parse("{} x", v));                                  /* text after */
    CHECK(!json::parse("\"\\q\"", v));                               /* bad escape */
    CHECK(!json::parse("\"\\ud800\"", v));                           /* lone surrogate */
    CHECK(!json::parse("[01x]", v));
    CHECK(!json::parse("", v));
    CHECK(!json::parse(std::string(40, '[') + std::string(40, ']'), v)); /* too deep */
    CHECK(json::parse(std::string(20, '[') + std::string(20, ']'), v));
    const std::string odd = "quote \" back \\ tab \t nl \n ctrl \x01 utf8 \xC3\xA9";
    CHECK(json::parse(json::quote(odd), v) && v.as_string() == odd);
}

/* ------------------------------------------------------------------------------------------------- rules */

static void test_rules_line()
{
    Rules d;
    CHECK(d.serialize() == "draw=1,passes=0,scoring=standard,timed=1,auto=1,fback=1,fill=king");
    CHECK(d.variation_key() == "d1-unl-standard");
    int combos = 0;
    for (int draw : {1, 3})
        for (int passes : {0, 3, 1})
            for (int sc = 0; sc < 3; ++sc)
                for (int flags = 0; flags < 16; ++flags) {
                    Rules r;
                    r.draw = draw;
                    r.passes = passes;
                    r.scoring = static_cast<Scoring>(sc);
                    r.timed = flags & 1;
                    r.auto_foundation = flags & 2;
                    r.foundation_back = flags & 4;
                    r.fill = flags & 8 ? TableauFill::AnyCard : TableauFill::KingOnly;
                    Rules back;
                    CHECK(Rules::parse(r.serialize(), back) && back == r);
                    ++combos;
                }
    CHECK(combos == 288);
    Rules v;
    v.draw = 3;
    v.passes = 3;
    v.scoring = Scoring::Vegas;
    CHECK(v.variation_key() == "d3-p3-vegas");
    v.passes = 1;
    v.scoring = Scoring::None;
    CHECK(v.variation_key() == "d3-p1-none");
    Rules p;
    CHECK(Rules::parse("draw=3,colour=blue,passes=7", p) && p.draw == 3 && p.passes == 0); /* unknown and bad keys */
    CHECK(!Rules::parse("nonsense", p) && p == Rules());
    CHECK(Rules::parse("fill=any", p) && p.fill == TableauFill::AnyCard && p.draw == 1);
}

/* ------------------------------------------------------------------------------------------------- stats */

static GameRecord rec(bool won, int score, unsigned seconds, unsigned moves)
{
    GameRecord g;
    g.won = won;
    g.score = score;
    g.seconds = seconds;
    g.moves = moves;
    return g;
}

static void test_stats()
{
    Rules std_rules;
    Stats s;
    s.record(std_rules, rec(true, 600, 300, 140));
    s.record(std_rules, rec(false, 900, 100, 80));
    s.record(std_rules, rec(true, 500, 250, 160));
    s.record(std_rules, rec(true, 700, 400, 120));
    VariationStats v = s.get("d1-unl-standard");
    CHECK(v.played == 4 && v.won == 3 && v.streak == 2 && v.best_streak == 2);
    CHECK(v.has_best_score && v.best_score == 900);
    CHECK(v.best_time_s == 250 && v.fewest_moves == 120 && v.total_time_s == 1050 && v.vegas_bank == 0);
    s.record(std_rules, rec(false, 0, 10, 5));
    CHECK(s.get("d1-unl-standard").streak == 0 && s.get("d1-unl-standard").best_streak == 2);
    CHECK(s.get("d3-p3-vegas").played == 0);

    /* Vegas: the bank carries over, per variation, and can go below zero */
    Rules vegas;
    vegas.draw = 3;
    vegas.passes = 3;
    vegas.scoring = Scoring::Vegas;
    s.record(vegas, rec(false, score::vegas(10), 200, 90));  /* -2 */
    s.record(vegas, rec(true, score::vegas(52), 300, 150));  /* +208 */
    s.record(vegas, rec(false, score::vegas(0), 20, 3));     /* -52 */
    v = s.get("d3-p3-vegas");
    CHECK(v.vegas_bank == -2 + 208 - 52 && v.played == 3 && v.won == 1 && v.best_score == 208);
    Rules vegas1 = vegas;
    vegas1.draw = 1;
    vegas1.passes = 1;
    s.record(vegas1, rec(false, score::vegas(3), 60, 30));
    CHECK(s.get("d1-p1-vegas").vegas_bank == -37 && s.get("d3-p3-vegas").vegas_bank == 154);
    /* scoring None keeps no best score */
    Rules none;
    none.scoring = Scoring::None;
    s.record(none, rec(true, 0, 0, 100));
    CHECK(!s.get("d1-unl-none").has_best_score && s.get("d1-unl-none").best_time_s == 1);

    /* JSON round trip */
    Stats back;
    std::string why;
    CHECK(Stats::from_json(s.to_json(), back, &why) && back == s);
    CHECK(Stats::from_json(Stats().to_json(), back) && back.all().empty());
    CHECK(!Stats::from_json("{\"version\": 2, \"variations\": {}}", back, &why)); /* newer */
    CHECK(!Stats::from_json("{\"version\": 1, \"variations\": {\"d1-unl-standard\": {\"played\": 1, \"won\": 2}}}", back));
    CHECK(!Stats::from_json("{\"version\": 1, \"variations\": {\"k\": {\"played\": -1}}}", back));
    CHECK(!Stats::from_json("[]", back));
    const std::string full = s.to_json();
    CHECK(!Stats::from_json(full.substr(0, full.size() / 2), back)); /* cut in half */
    s.reset("d1-unl-none");
    CHECK(s.get("d1-unl-none").played == 0);
    s.reset_all();
    CHECK(s.all().empty());
}

/* ------------------------------------------------------------------------------------------------- saved game */

static Game played_game(std::uint32_t deal, const Rules &r, int steps, std::uint64_t seed)
{
    TestRng rng(seed);
    Game g;
    g.new_game(deal, r);
    for (int i = 0; i < steps; ++i) {
        const std::vector<Move> moves = g.legal_moves();
        if (moves.empty() || g.won()) break;
        g.apply(moves[rng.below(static_cast<std::uint32_t>(moves.size()))]);
        for (Move a = g.safe_auto_move(); a.count; a = g.safe_auto_move()) g.apply(a, true);
        g.add_time(1000 + rng.below(5000));
    }
    for (int i = 0; i < 6; ++i) g.undo(); /* leave a redo list */
    return g;
}

static bool same_game(const Game &a, const Game &b)
{
    if (a.board() != b.board() || a.start() != b.start() || a.rules() != b.rules()) return false;
    if (a.deal_number() != b.deal_number() || a.user_number() != b.user_number() || a.winnable() != b.winnable())
        return false;
    if (a.score() != b.score() || a.elapsed_ms() != b.elapsed_ms() || a.moves_made() != b.moves_made()) return false;
    if (a.history().size() != b.history().size() || a.redo_list().size() != b.redo_list().size()) return false;
    for (std::size_t i = 0; i < a.history().size(); ++i) {
        const Step &x = a.history()[i], &y = b.history()[i];
        if (x.move != y.move || x.chained != y.chained || x.flipped != y.flipped || x.score_before != y.score_before)
            return false;
    }
    for (std::size_t i = 0; i < a.redo_list().size(); ++i)
        if (a.redo_list()[i].move != b.redo_list()[i].move || a.redo_list()[i].chained != b.redo_list()[i].chained)
            return false;
    return true;
}

static void test_save_resume()
{
    for (std::uint32_t d = 0; d < 40; ++d) {
        Rules r;
        r.draw = d % 2 ? 3 : 1;
        r.passes = d % 3 == 2 ? 3 : 0;
        r.scoring = static_cast<Scoring>(d % 3);
        r.timed = d % 4 != 0;
        Game g = played_game(d * 31 + 5, r, 60 + static_cast<int>(d), d);
        g.set_labels(d, d % 2 == 0);
        Game back;
        std::string why;
        const bool ok = Game::load(g.save(), back, &why);
        if (!ok) std::fprintf(stderr, "load: %s\n", why.c_str());
        CHECK(ok && same_game(g, back));
        /* both go on the same way */
        bool same = true;
        while (g.can_redo()) same = same && g.redo() && back.redo() && same_game(g, back);
        CHECK(same && !back.can_redo());
    }
    /* a game started from a constructed position (not a deal) */
    Game c;
    CHECK(c.new_game(position("C1 - - -", {"3D", "kh 2C", "", "", "", "", ""}, "", "", true), Rules()));
    CHECK(c.apply(mv(T(2), F(1), 1)));
    Game cb;
    CHECK(Game::load(c.save(), cb) && same_game(c, cb));

    /* refused: cut, not JSON, newer, a move that does not replay, a position that does not match */
    const Game g = played_game(99, Rules(), 80, 3);
    const std::string text = g.save();
    Game out;
    std::string why;
    CHECK(!Game::load(text.substr(0, text.size() * 2 / 3), out, &why));
    CHECK(!Game::load("garbage", out));
    std::string newer = text;
    newer.replace(newer.find("\"version\": 1"), 12, "\"version\": 9");
    CHECK(!Game::load(newer, out, &why) && why.find("newer") != std::string::npos);
    std::string tampered = text;
    const std::size_t h = tampered.find("\"history\": [\"");
    CHECK(h != std::string::npos);
    tampered.replace(h + 13, 0, "1-6-1\", \""); /* from the empty waste, as the first move */
    CHECK(!Game::load(tampered, out, &why) && why.find("does not replay") != std::string::npos);
    std::string moved = text;
    const std::size_t p = moved.find("\"position\": \"");
    moved[p + 13] = moved[p + 13] == '0' ? '1' : '0';
    CHECK(!Game::load(moved, out, &why) && why.find("position") != std::string::npos);
    std::string bad_rules = text;
    const std::size_t rr = bad_rules.find("\"rules\": \"");
    bad_rules.replace(rr + 10, 0, "x");
    CHECK(Game::load(bad_rules, out)); /* "xdraw=1" is ignored, the rest of the line still reads */
}

/* ------------------------------------------------------------------------------------------------- files */

static void test_store_files()
{
    const std::string data = g_tmp + "/data/klondike-solitaire", config = g_tmp + "/config/klondike-solitaire";
    Store store(data, config);
    Stats stats;
    CHECK(store.load_stats(stats) == Store::Load::Missing);
    Game game;
    CHECK(store.load_game(game) == Store::Load::Missing);
    Rules rules;
    CHECK(store.load_rules(rules) == Store::Load::Missing && rules == Rules());

    /* stats: save, load */
    Rules vegas;
    vegas.scoring = Scoring::Vegas;
    stats.record(vegas, rec(true, 208, 120, 99));
    stats.record(Rules(), rec(false, 55, 60, 40));
    CHECK(store.save_stats(stats));
    CHECK(!path_exists(data + "/stats.json.tmp")); /* written through a temp file, renamed */
    Stats loaded;
    CHECK(store.load_stats(loaded) == Store::Load::Ok && loaded == stats);

    /* a cut stats file: Bad, kept as .bad, the app starts with empty statistics */
    std::string text;
    CHECK(read_file(data + "/stats.json", text));
    CHECK(write_file_atomic(data + "/stats.json", text.substr(0, text.size() - 9)));
    CHECK(store.load_stats(loaded) == Store::Load::Bad && loaded.all().empty());
    CHECK(path_exists(data + "/stats.json.bad") && !path_exists(data + "/stats.json"));
    CHECK(!store.last_error().empty());
    CHECK(store.load_stats(loaded) == Store::Load::Missing);
    CHECK(write_file_atomic(data + "/stats.json", std::string("\0\x01\x02 binary", 10)));
    CHECK(store.load_stats(loaded) == Store::Load::Bad);

    /* the game in progress */
    Rules r3;
    r3.draw = 3;
    const Game g = played_game(4242, r3, 70, 11);
    CHECK(store.save_game(g));
    Game resumed;
    CHECK(store.load_game(resumed) == Store::Load::Ok && same_game(g, resumed));
    CHECK(read_file(data + "/game.json", text));
    CHECK(write_file_atomic(data + "/game.json", text.substr(0, text.size() / 2)));
    Game none;
    CHECK(store.load_game(none) == Store::Load::Bad && path_exists(data + "/game.json.bad"));
    CHECK(store.load_game(none) == Store::Load::Missing);
    CHECK(store.save_game(g) && store.clear_game() && store.load_game(none) == Store::Load::Missing);
    CHECK(store.clear_game()); /* nothing to remove is fine */

    /* the rules for the next deal */
    Rules mine;
    mine.draw = 3;
    mine.passes = 3;
    mine.scoring = Scoring::Vegas;
    mine.fill = TableauFill::AnyCard;
    CHECK(store.save_rules(mine));
    CHECK(store.load_rules(rules) == Store::Load::Ok && rules == mine);
    CHECK(write_file_atomic(config + "/settings.json", "{\"version\": 1, \"rules\": 5}"));
    CHECK(store.load_rules(rules) == Store::Load::Bad && rules == Rules());

    /* default folders follow the environment */
    setenv("KLONDIKE_DATA", "/x/data", 1);
    CHECK(Store::default_data_dir() == "/x/data");
    unsetenv("KLONDIKE_DATA");
    setenv("XDG_DATA_HOME", "/x/share", 1);
    setenv("XDG_CONFIG_HOME", "/x/conf", 1);
    CHECK(Store::default_data_dir() == "/x/share/klondike-solitaire");
    CHECK(Store::default_config_dir() == "/x/conf/klondike-solitaire");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/deck", 1);
    CHECK(Store::default_data_dir() == "/home/deck/.local/share/klondike-solitaire");
    CHECK(Store::default_config_dir() == "/home/deck/.config/klondike-solitaire");
}

void test_store_group()
{
    char tmpl[] = "/tmp/klondike-test-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    g_tmp = dir;
    test_json();
    test_rules_line();
    test_stats();
    test_save_resume();
    test_store_files();
    remove_tree(g_tmp);
}
