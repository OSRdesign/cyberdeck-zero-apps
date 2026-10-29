/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the solver and the winnable deals (see solver.hpp).
 */
#include "solver.hpp"

#include <algorithm>

namespace klondike {

namespace {

/* A solver move: `ops` stock operations (draw, or recycle when the stock is empty), then from -> to (count cards). */
struct SMove {
    std::uint8_t ops;
    std::uint8_t from;
    std::uint8_t to;
    std::uint8_t count;
};

struct Scored {
    int score;
    SMove m;
};

inline std::uint64_t mix64(std::uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

constexpr std::uint64_t kFnvPrime = 0x100000001B3ull;

} // namespace

const char *verdict_name(Verdict v)
{
    switch (v) {
    case Verdict::Winnable: return "winnable";
    case Verdict::NotWinnable: return "not winnable";
    case Verdict::Unknown: break;
    }
    return "unknown";
}

struct Solver::Impl {
    SolverLimits lim;
    Rules rules;
    int max_rec = -1;
    bool draw1 = true;
    bool draw1_unlimited = true;
    bool any_fill = false;

    std::vector<std::uint64_t> tt;     /* position keys (without the recycles), 0 = free slot */
    std::vector<std::uint8_t> tt_rec;  /* the fewest recycles that position was seen with */
    std::uint64_t mask = 0;
    std::uint32_t used = 0;
    std::vector<SMove> path;
    std::vector<Scored> buf;
    std::uint32_t nodes = 0;
    bool aborted = false;
    bool incomplete = false;
    bool prune = true;   /* first pass: skip run moves that free nothing usable now */
    bool pruned = false; /* the first pass skipped a move: its exhaustion proves nothing */

    /* Nothing could ever be placed on c any more (see solver.hpp): rank 1 or 2, or both suits of the other colour
     * hold rank - 1; with foundation_back also the other suit of its colour holds rank - 2 (else a chain of
     * foundation cards brought back could still need c as its base). */
    bool safe_rank(const Board &b, Card c) const
    {
        const int r = rank_of(c);
        if (r <= 2) return true;
        const bool red = is_red(c);
        for (int s = 0; s < kSuits; ++s) {
            if (is_red_suit(s) != red) {
                if (b.suit_count(s) < r - 1) return false;
            } else if (s != suit_of(c) && rules.foundation_back) {
                if (b.suit_count(s) < r - 2) return false;
            }
        }
        return true;
    }

    void apply(Board &b, const SMove &m) const
    {
        for (int k = 0; k < m.ops; ++k) do_move(b, rules, stock_move(b, rules), nullptr);
        Move t;
        t.from = m.from;
        t.to = m.to;
        t.count = m.count;
        do_move(b, rules, t, nullptr);
    }

    void autoplay(Board &b)
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int c = 0; c < kColumns; ++c) {
                if (b.ntab[c] == 0) continue;
                const Card card = b.tab[c][b.ntab[c] - 1];
                const int f = foundation_for(b, card);
                if (f < 0 || !safe_rank(b, card)) continue;
                const SMove m{0, static_cast<std::uint8_t>(tableau_pile(c)), static_cast<std::uint8_t>(f), 1};
                apply(b, m);
                path.push_back(m);
                changed = true;
            }
            if (!draw1 || b.ntalon == 0) continue;
            /* draw 1: the waste top; with unlimited passes any stock card (drawing to it costs nothing) */
            const int lo = draw1_unlimited ? 0 : (b.tp > 0 ? b.tp - 1 : b.ntalon);
            const int hi = draw1_unlimited ? b.ntalon : b.tp;
            for (int i = lo; i < hi; ++i) {
                const Card card = b.talon[i];
                const int f = foundation_for(b, card);
                if (f < 0 || !safe_rank(b, card)) continue;
                int ops;
                if (i == b.tp - 1) ops = 0;
                else if (i >= b.tp) ops = i - b.tp + 1;
                else ops = (b.ntalon - b.tp) + 1 + (i + 1); /* to the end, recycle, then to it */
                const SMove m{static_cast<std::uint8_t>(ops), kWaste, static_cast<std::uint8_t>(f), 1};
                apply(b, m);
                path.push_back(m);
                changed = true;
                break; /* the talon changed: look again */
            }
        }
    }

    std::uint64_t key(const Board &b) const
    {
        std::uint64_t cols = 0;
        for (int c = 0; c < kColumns; ++c) {
            if (b.ntab[c] == 0) continue;
            std::uint64_t h = 0xCBF29CE484222325ull ^ (static_cast<std::uint64_t>(b.ndown[c]) << 40);
            for (int i = 0; i < b.ntab[c]; ++i) h = (h ^ b.tab[c][i]) * kFnvPrime;
            h = (h ^ (0x100u + b.ntab[c])) * kFnvPrime;
            cols += mix64(h); /* a sum: the order of the columns does not matter */
        }
        std::uint64_t tal = 0x84222325CBF29CE4ull;
        for (int i = 0; i < b.ntalon; ++i) tal = (tal ^ b.talon[i]) * kFnvPrime;
        tal = (tal ^ (0x200u + b.ntalon)) * kFnvPrime;
        if (!draw1_unlimited) tal = (tal ^ (0x400u + b.tp)) * kFnvPrime;
        /* the recycles are not in the key: tt_insert() keeps the fewest seen (fewer is never worse) */
        std::uint64_t fnd = 0;
        for (int s = 0; s < kSuits; ++s) fnd |= static_cast<std::uint64_t>(b.suit_count(s)) << (s * 4);
        std::uint64_t k = mix64(cols + 0x632BE59BD9B4E019ull * mix64(tal) + mix64(fnd + 0x9E3779B97F4A7C15ull));
        return k ? k : 1;
    }

    /* false when the position was seen already with as few recycles used (with unlimited passes they stay 0) */
    bool tt_insert(std::uint64_t k, int recycles)
    {
        const std::uint8_t rec = static_cast<std::uint8_t>(max_rec >= 0 ? recycles : 0);
        std::uint64_t i = k & mask;
        while (tt[i]) {
            if (tt[i] == k) {
                if (tt_rec[i] <= rec) return false;
                tt_rec[i] = rec; /* seen again with fewer recycles: worth searching again */
                return true;
            }
            i = (i + 1) & mask;
        }
        if (static_cast<std::uint64_t>(used) * 4 >= (mask + 1) * 3) {
            incomplete = true; /* full (cannot happen with 2 x max_nodes slots): search on without remembering */
            return true;
        }
        tt[i] = k;
        tt_rec[i] = rec;
        ++used;
        return true;
    }

    void add(int score, int ops, int from, int to, int count)
    {
        buf.push_back(Scored{score, SMove{static_cast<std::uint8_t>(ops), static_cast<std::uint8_t>(from),
                                          static_cast<std::uint8_t>(to), static_cast<std::uint8_t>(count)}});
    }

    bool fits_empty(Card c) const { return rank_of(c) == kRanks || any_fill; }

    /* A card could go to an empty column now: a King (any card with fill = AnyCard) in the stock or the waste, face
     * up on another card in a column, or the top of a foundation (with foundation_back). */
    bool filler_waiting(const Board &b) const
    {
        for (int i = 0; i < b.ntalon; ++i)
            if (fits_empty(b.talon[i])) return true;
        for (int c = 0; c < kColumns; ++c)
            for (int i = b.ndown[c] > 0 ? b.ndown[c] : 1; i < b.ntab[c]; ++i)
                if (fits_empty(b.tab[c][i])) return true;
        if (rules.foundation_back)
            for (int f = 0; f < kFoundations; ++f)
                if (b.fcount[f] && fits_empty(make_card(b.fsuit[f], b.fcount[f]))) return true;
        return false;
    }

    void gen(const Board &b)
    {
        const std::size_t start = buf.size();
        int first_empty = -1;
        for (int c = 0; c < kColumns; ++c)
            if (b.ntab[c] == 0) {
                first_empty = c;
                break;
            }
        /* tableau */
        for (int s = 0; s < kColumns; ++s) {
            const int n = b.ntab[s];
            if (n == 0) continue;
            const int down = b.ndown[s];
            const int f = foundation_for(b, b.tab[s][n - 1]);
            if (f >= 0) add(900 + ((n - 1 == down && down > 0) ? 60 + down : 0), 0, tableau_pile(s), f, 1);
            for (int i = down; i < n; ++i) {
                const Card base = b.tab[s][i];
                const bool exposes = i == down && down > 0;
                const bool whole = i == 0;
                int score;
                bool skip = false; /* the first pass leaves it out (see solver.hpp) */
                if (exposes) {
                    score = 800 + down;
                } else if (whole) {
                    score = 400;
                    skip = prune && !filler_waiting(b); /* an empty column nobody could use now */
                } else if (foundation_for(b, b.tab[s][i - 1]) >= 0) {
                    score = 600;
                } else {
                    score = 200;
                    skip = prune; /* a run moved between two equal cards: it frees nothing now */
                }
                for (int t = 0; t < kColumns; ++t) {
                    if (t == s) continue;
                    if (b.ntab[t] == 0) {
                        if (whole || t != first_empty || !fits_empty(base)) continue;
                    } else if (!builds_on(base, b.tab[t][b.ntab[t] - 1])) {
                        continue;
                    }
                    if (skip) {
                        pruned = true;
                        break;
                    }
                    add(score, 0, tableau_pile(s), tableau_pile(t), n - i);
                }
            }
        }
        /* the stock: every card drawing can bring to the waste top */
        for_each_reachable_talon_card(b, rules, [&](Card c, int ops, int) {
            const int f = foundation_for(b, c);
            if (f >= 0) add(750 - ops, ops, kWaste, f, 1);
            for (int t = 0; t < kColumns; ++t) {
                if (b.ntab[t] == 0) {
                    if (t != first_empty || !fits_empty(c)) continue;
                } else if (!builds_on(c, b.tab[t][b.ntab[t] - 1])) {
                    continue;
                }
                add(700 - ops, ops, kWaste, tableau_pile(t), 1);
            }
        });
        /* foundation -> tableau */
        if (rules.foundation_back) {
            for (int f = 0; f < kFoundations; ++f) {
                if (b.fcount[f] == 0) continue;
                const Card c = make_card(b.fsuit[f], b.fcount[f]);
                if (safe_rank(b, c)) continue; /* would go straight back up: nothing could use it */
                for (int t = 0; t < kColumns; ++t) {
                    if (b.ntab[t] == 0) {
                        if (t != first_empty || !fits_empty(c)) continue;
                    } else if (!builds_on(c, b.tab[t][b.ntab[t] - 1])) {
                        continue;
                    }
                    add(100, 0, foundation_pile(f), tableau_pile(t), 1);
                }
            }
        }
        /* best first; stable (insertion sort, no allocation), so equal scores keep the order above */
        for (std::size_t i = start + 1; i < buf.size(); ++i) {
            const Scored x = buf[i];
            std::size_t j = i;
            while (j > start && buf[j - 1].score < x.score) {
                buf[j] = buf[j - 1];
                --j;
            }
            buf[j] = x;
        }
    }

    bool dfs(const Board &in, int depth)
    {
        if (++nodes > lim.max_nodes) {
            aborted = true;
            return false;
        }
        Board b = in;
        const std::size_t mark = path.size();
        autoplay(b);
        if (b.won()) return true;
        if (depth >= lim.max_depth) {
            incomplete = true;
            path.resize(mark);
            return false;
        }
        if (!tt_insert(key(b), b.recycles)) {
            path.resize(mark);
            return false;
        }
        const std::size_t start = buf.size();
        gen(b);
        const std::size_t end = buf.size();
        for (std::size_t i = start; i < end; ++i) {
            const SMove m = buf[i].m;
            Board child = b;
            apply(child, m);
            path.push_back(m);
            if (dfs(child, depth + 1)) {
                buf.resize(start);
                return true;
            }
            path.pop_back();
            if (aborted) break;
        }
        buf.resize(start);
        path.resize(mark);
        return false;
    }
};

Solver::Solver(const SolverLimits &limits) : impl_(new Impl)
{
    impl_->lim = limits;
}

Solver::~Solver()
{
    delete impl_;
}

SolveResult Solver::solve(const Board &start, const Rules &rules)
{
    Impl &s = *impl_;
    s.rules = rules;
    s.max_rec = rules.max_recycles();
    s.draw1 = rules.draw == 1;
    s.draw1_unlimited = rules.draw == 1 && s.max_rec < 0;
    s.any_fill = rules.fill == TableauFill::AnyCard;
    std::size_t cap = 4096;
    while (cap < 2 * static_cast<std::size_t>(s.lim.max_nodes) + 2) cap <<= 1;
    s.mask = cap - 1;
    s.nodes = 0;
    s.aborted = false;
    s.incomplete = false;
    s.pruned = false;
    auto pass = [&s, &start, cap](bool prune) {
        if (s.tt.size() != cap) {
            s.tt.assign(cap, 0);
            s.tt_rec.assign(cap, 0);
        } else {
            std::fill(s.tt.begin(), s.tt.end(), 0);
        }
        s.used = 0;
        s.path.clear();
        s.buf.clear();
        s.prune = prune;
        return s.dfs(start, 0);
    };

    /* first the pruned search (finds most wins sooner); when it ran out of moves only because it skipped some, the
     * complete search goes on with what is left of the budget, so that a NotWinnable is always a proof */
    bool win = pass(true);
    if (!win && !s.aborted && s.pruned) {
        s.incomplete = false;
        win = pass(false);
    }
    SolveResult res;
    res.nodes = s.nodes > s.lim.max_nodes ? s.lim.max_nodes : s.nodes;
    if (!win) {
        res.verdict = (s.aborted || s.incomplete) ? Verdict::Unknown : Verdict::NotWinnable;
        return res;
    }
    /* the solution as plain moves, checked against the rules */
    Board b = start;
    for (const SMove &m : s.path) {
        for (int k = 0; k < m.ops; ++k) {
            const Move sm = stock_move(b, rules);
            if (!sm.count || !can_move(b, rules, sm)) return res; /* Unknown: cannot happen */
            do_move(b, rules, sm, nullptr);
            res.solution.push_back(sm);
        }
        Move t;
        t.from = m.from;
        t.to = m.to;
        t.count = m.count;
        if (!can_move(b, rules, t)) {
            res.solution.clear();
            return res;
        }
        do_move(b, rules, t, nullptr);
        res.solution.push_back(t);
    }
    if (!b.won()) {
        res.solution.clear();
        return res;
    }
    res.verdict = Verdict::Winnable;
    return res;
}

std::uint32_t winnable_candidate(std::uint32_t user, std::uint32_t k)
{
    if (k == 0) return user;
    const std::uint64_t state = (static_cast<std::uint64_t>(user) << 32) | k;
    return static_cast<std::uint32_t>(mix64(state + 0x9E3779B97F4A7C15ull));
}

WinnableDeal find_winnable_deal(std::uint32_t user, const Rules &rules, const WinnableLimits &limits)
{
    WinnableDeal out;
    SolverLimits sl;
    sl.max_nodes = limits.nodes_per_try;
    Solver solver(sl);
    for (std::uint32_t k = 0; k < limits.max_tries; ++k) {
        const std::uint32_t deal = winnable_candidate(user, k);
        SolveResult r = solver.solve(deal_board(deal), rules);
        ++out.tries;
        out.nodes += r.nodes;
        if (r.verdict == Verdict::Winnable) {
            out.found = true;
            out.deal = deal;
            out.solution = std::move(r.solution);
            return out;
        }
    }
    return out;
}

} // namespace klondike
