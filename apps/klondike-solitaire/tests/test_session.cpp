/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests of the Session (core/session.hpp): the rules about when a game counts, autosave and resume, the clock,
 * the win, the look preferences, and the solver helper's replies. Files go to a fresh folder under /tmp.
 */
#include "test_util.hpp"

#include "fsutil.hpp"
#include "json.hpp"
#include "session.hpp"
#include "solver.hpp"

#include <cstdlib>
#include <dirent.h>
#include <unistd.h>

using namespace klondike;

namespace {

void rm_tree(const std::string &dir)
{
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string p = dir + "/" + n;
            if (e->d_type == DT_DIR) rm_tree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

Store make(const std::string &root)
{
    return Store(root + "/data", root + "/config");
}

/* a legal move that is not a draw: tableau or waste move, if any */
bool some_play(const Game &g, Move &out)
{
    for (const Move &m : g.legal_moves())
        if (kind_of(m) != MoveKind::Draw && kind_of(m) != MoveKind::Recycle) {
            out = m;
            return true;
        }
    return false;
}

/* a deal that has a non-draw move at once */
std::uint32_t deal_with_play(const Rules &r)
{
    for (std::uint32_t n = 1; n < 200; ++n) {
        Game g;
        g.new_game(n, r);
        Move m;
        if (some_play(g, m)) return n;
    }
    return 1;
}

void test_prefs()
{
    UiPrefs p;
    p.left_handed = true;
    p.four_colour = true;
    p.felt = 2;
    p.back = 1;
    p.anim = 2;
    p.winnable = true;
    UiPrefs q;
    CHECK(prefs_from_json(prefs_to_json(p), q));
    CHECK(q == p);
    CHECK(prefs_from_json("{\"felt\": 9, \"anim\": -4, \"back\": 77, \"unknown\": 1}", q));
    CHECK(q.felt == 2 && q.anim == 0 && q.back == 1); /* out of range values are clamped, other keys ignored */
    CHECK(!prefs_from_json("[1,2", q));
    CHECK(!prefs_from_json("3", q));
}

void test_abandon_and_stats(const std::string &root)
{
    Session s(make(root));
    CHECK(!s.load()); /* first run: a fresh random deal */
    Rules r;
    r.draw = 1;
    s.set_rules(r);
    const std::uint32_t deal = deal_with_play(r);
    s.begin_deal(deal, deal, false);
    CHECK(s.game().deal_number() == deal && !s.in_progress());
    /* an untouched deal costs nothing */
    s.begin_deal(deal + 1, deal + 1, false);
    CHECK(s.stats().get(r.variation_key()).played == 0);
    s.begin_deal(deal, deal, false);
    Move m;
    CHECK(some_play(s.game(), m));
    CHECK(s.play(m));
    CHECK(s.in_progress());
    /* taken back: not in progress any more */
    CHECK(s.undo());
    CHECK(!s.in_progress());
    s.begin_deal(deal + 2, deal + 2, false);
    CHECK(s.stats().get(r.variation_key()).played == 0);
    /* a real game left after a move counts as a loss for ITS variation */
    s.begin_deal(deal, deal, false);
    CHECK(some_play(s.game(), m) && s.play(m));
    Rules next = r;
    next.draw = 3;
    s.set_rules(next); /* the NEXT game's rules only */
    CHECK(s.game().rules().draw == 1 && s.rules_pending());
    s.begin_deal(deal + 3, deal + 3, false);
    VariationStats v = s.stats().get(r.variation_key());
    CHECK(v.played == 1 && v.won == 0 && v.streak == 0);
    CHECK(s.stats().get(next.variation_key()).played == 0);
    CHECK(s.game().rules().draw == 3 && !s.rules_pending());
    /* restart counts the same, keeps the rules and the deal */
    {
        Move m3;
        CHECK(some_play(s.game(), m3) ? s.play(m3) : s.play(s.game().stock_move()));
        const std::uint32_t d = s.game().deal_number();
        s.set_rules(r);
        s.restart();
        CHECK(s.game().deal_number() == d && s.game().rules().draw == 3 && !s.in_progress());
        CHECK(s.stats().get(next.variation_key()).played == 1);
    }
    /* statistics reach the disk: another session sees them */
    Session s2(make(root));
    s2.load();
    CHECK(s2.stats().get(r.variation_key()).played == 1);
    CHECK(s2.stats().get(next.variation_key()).played == 1);
    s2.reset_stats(r.variation_key());
    CHECK(s2.stats().get(r.variation_key()).played == 0 && s2.stats().get(next.variation_key()).played == 1);
    s2.reset_all_stats();
    CHECK(s2.stats().all().empty());
    Session s3(make(root));
    s3.load();
    CHECK(s3.stats().all().empty());
}

void test_resume_and_clock(const std::string &root)
{
    Rules r;
    r.timed = true;
    std::uint64_t ms = 0;
    std::string board;
    std::uint32_t moves = 0;
    {
        Session s(make(root));
        s.load();
        s.set_rules(r);
        const std::uint32_t deal = deal_with_play(r);
        s.begin_deal(deal, 777, true);
        s.tick(5000);
        CHECK(s.game().elapsed_ms() == 0); /* the clock starts with the first move */
        Move m;
        CHECK(some_play(s.game(), m) && s.play(m));
        s.tick(7300);
        s.tick(200);
        ms = s.game().elapsed_ms();
        CHECK(ms == 7500);
        board = s.game().board().to_text();
        moves = s.game().moves_made();
        s.prefs().felt = 1;
        s.save_prefs();
        s.save_game();
    }
    Session t(make(root));
    CHECK(t.load()); /* resumed */
    CHECK(t.game().board().to_text() == board);
    CHECK(t.game().elapsed_ms() == ms && t.game().moves_made() == moves);
    CHECK(t.game().user_number() == 777 && t.game().winnable());
    CHECK(t.in_progress() && t.game().can_undo());
    CHECK(t.prefs().felt == 1);
    CHECK(t.rules().timed);
    /* a damaged saved game is set aside and a new one is dealt */
    {
        write_file_atomic(root + "/data/game.json", "{\"broken\":");
        Session u(make(root));
        CHECK(!u.load());
        CHECK(path_exists(root + "/data/game.json.bad"));
        CHECK(!u.in_progress());
    }
}

void test_win(const std::string &root)
{
    Rules r;
    r.scoring = Scoring::Vegas;
    r.timed = false;
    Session s(make(root));
    s.load();
    s.set_rules(r);
    s.begin_deal(5, 5, false);
    const Board near_win = position("C13 D13 H13 S12", {"KS", "", "", "", "", "", ""}, "", "", false);
    CHECK(near_win.valid());
    CHECK(s.game().new_game(near_win, r, 5));
    s.save_game();
    CHECK(path_exists(root + "/data/game.json"));
    const Move up = mv(T(1), F(4), 1);
    CHECK(s.game().can_apply(up));
    CHECK(s.play(up));
    CHECK(s.game().won() && s.finished() && !s.in_progress());
    VariationStats v = s.stats().get(r.variation_key());
    CHECK(v.played == 1 && v.won == 1 && v.streak == 1);
    CHECK(v.vegas_bank == 208 && s.vegas_bank_now() == 208); /* -52 + 5 * 52 */
    CHECK(!path_exists(root + "/data/game.json")); /* the saved game goes at the win */
    CHECK(!s.undo() && !s.redo());                 /* the result is in the statistics: no taking it back */
    s.tick(1000);
    CHECK(s.game().elapsed_ms() == 0 || s.game().won());
    Session again(make(root));
    CHECK(!again.load()); /* nothing to resume: a new deal */
    CHECK(again.stats().get(r.variation_key()).won == 1);
}

void test_auto_moves(const std::string &root)
{
    Rules r;
    r.timed = false;
    r.auto_foundation = true;
    Session s(make(root));
    s.load();
    s.set_rules(r);
    /* an Ace on top of a column with nothing to wait for: safe moves play themselves, chained to the move before */
    const Board b = position("C1 D1 H1 S1", {"2C", "2D", "2H", "2S", "", "", ""}, "", "", true);
    CHECK(b.valid());
    s.game().new_game(b, r, 9);
    const int n = s.auto_moves();
    CHECK(n == 4);
    CHECK(s.game().board().foundation_cards() == 8);
    CHECK(s.game().can_undo());
    Rules off = r;
    off.auto_foundation = false;
    Session q(make(root + "/q"));
    q.load();
    q.game().new_game(b, off, 9);
    CHECK(q.auto_moves() == 0);
}

void test_worker()
{
    Rules r;
    bool found = false, dead = false;
    std::uint32_t deal = 0;
    CHECK(parse_worker_winnable("winnable 1 4242 3", found, deal) && found && deal == 4242);
    CHECK(parse_worker_winnable("winnable 0", found, deal) && !found);
    CHECK(!parse_worker_winnable("garbage", found, deal));
    CHECK(parse_worker_deadend("deadend 1", dead) && dead);
    CHECK(parse_worker_deadend("deadend 0", dead) && !dead);
    CHECK(!parse_worker_deadend("deadend", dead));
    const std::string reply = worker_winnable_reply(1, r);
    CHECK(parse_worker_winnable(reply, found, deal));
    if (found) {
        /* the deal the helper names is really winnable */
        Solver solver;
        const SolveResult res = solver.solve(deal_board(deal), r);
        CHECK(res.verdict == Verdict::Winnable);
    }
    std::string text = deal_board(1).to_text();
    for (char &c : text)
        if (c == '\n') c = ';';
    CHECK(worker_deadend_reply(text, r) == "deadend 0"); /* a fresh deal is never a dead end */
    CHECK(worker_deadend_reply("nonsense", r) == "deadend 0");
}

} // namespace

void test_session_group()
{
    char tmpl[] = "/tmp/klondike-session-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    const std::string root = dir;
    test_prefs();
    test_abandon_and_stats(root + "/a");
    test_resume_and_clock(root + "/b");
    test_win(root + "/c");
    test_auto_moves(root + "/d");
    test_worker();
    rm_tree(root);
}
