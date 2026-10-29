/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the deductive solver (see solver.hpp).
 */
#include "solver.hpp"

#include <algorithm>
#include <iterator>

namespace minefield {

namespace {

enum Known : std::uint8_t { kUnknown = 0, kMine = 1, kSafe = 2 };

/* up to eight cell indices, no allocation (a number has at most eight neighbours) */
struct Cells {
    int a[8];
    int n = 0;
    void push_back(int v) { a[n++] = v; }
    bool empty() const { return n == 0; }
    std::size_t size() const { return static_cast<std::size_t>(n); }
    const int *begin() const { return a; }
    const int *end() const { return a + n; }
    int operator[](std::size_t i) const { return a[i]; }
};

struct Constraint {
    Cells cells;               /* unknown cells around the number, ascending */
    int rem = 0;               /* mines still hidden among them */
    int num = 0;               /* the number's cell */
};

/* Backtracking over one group of frontier cells. */
struct Enumerator {
    int n = 0;
    std::vector<std::vector<int>> cell_cons;   /* constraints touching each cell */
    std::vector<int> rem, mines, unassigned;
    std::vector<std::int8_t> val;
    std::vector<long> mine_count;
    long total = 0, nodes = 0, cap = 0;
    Budget *budget = nullptr;
    bool aborted = false;

    void dfs(int k)
    {
        if (aborted) return;
        if (++nodes > cap || (budget && !budget->spend(1))) {
            aborted = true;
            return;
        }
        if (k == n) {
            ++total;
            for (int i = 0; i < n; ++i)
                if (val[static_cast<std::size_t>(i)]) ++mine_count[static_cast<std::size_t>(i)];
            return;
        }
        for (int v = 0; v <= 1; ++v) {
            bool ok = true;
            for (int c : cell_cons[static_cast<std::size_t>(k)]) {
                --unassigned[static_cast<std::size_t>(c)];
                if (v) ++mines[static_cast<std::size_t>(c)];
                if (mines[static_cast<std::size_t>(c)] > rem[static_cast<std::size_t>(c)] ||
                    mines[static_cast<std::size_t>(c)] + unassigned[static_cast<std::size_t>(c)] < rem[static_cast<std::size_t>(c)])
                    ok = false;
            }
            if (ok) {
                val[static_cast<std::size_t>(k)] = static_cast<std::int8_t>(v);
                dfs(k + 1);
            }
            for (int c : cell_cons[static_cast<std::size_t>(k)]) {
                ++unassigned[static_cast<std::size_t>(c)];
                if (v) --mines[static_cast<std::size_t>(c)];
            }
            if (aborted) return;
        }
    }
};

class Solver {
public:
    Solver(const View &v, Budget *b, bool stop_first)
        : v_(v), budget_(b), enum_limit_(b ? b->enum_limit : kEnumLimit), stop_first_(stop_first), known_(static_cast<std::size_t>(v.size()), kUnknown),
          cidx_(static_cast<std::size_t>(v.size()), -1)
    {
    }

    void run(Deduction &out)
    {
        bool progress = true;
        while (progress && !done()) {
            progress = rules();
            if (done()) break;
            if (!progress) progress = enumerate();
        }
        for (int i = 0; i < v_.size(); ++i) {
            if (v_.cell[static_cast<std::size_t>(i)] >= 0) continue;
            if (known_[static_cast<std::size_t>(i)] == kSafe) out.safe.push_back(i);
            else if (known_[static_cast<std::size_t>(i)] == kMine) out.mines.push_back(i);
        }
        out.exhausted = budget_ && budget_->exhausted();
    }

private:
    bool done() const { return found_free_safe_ && stop_first_ ? true : (budget_ && budget_->exhausted()); }
    bool spend(long n = 1) { return !budget_ || budget_->spend(n); }
    bool hidden(int i) const { return v_.cell[static_cast<std::size_t>(i)] < 0; }

    int neighbours(int i, int *out) const
    {
        const int x = i % v_.w, y = i / v_.w;
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < v_.w && ny < v_.h) out[n++] = ny * v_.w + nx;
            }
        return n;
    }

    bool mark_safe(int i)
    {
        if (known_[static_cast<std::size_t>(i)] != kUnknown) return false;
        known_[static_cast<std::size_t>(i)] = kSafe;
        ++safe_known_;
        if (v_.cell[static_cast<std::size_t>(i)] == kViewHidden) found_free_safe_ = true;
        return true;
    }
    bool mark_mine(int i)
    {
        if (known_[static_cast<std::size_t>(i)] != kUnknown) return false;
        known_[static_cast<std::size_t>(i)] = kMine;
        ++mines_known_;
        return true;
    }

    /* The numbers that still have unknown cells around them, with those cells. */
    void build(std::vector<Constraint> &cs)
    {
        cs.clear();
        cs.reserve(64);
        for (int i = 0; i < v_.size(); ++i) {
            const int val = v_.cell[static_cast<std::size_t>(i)];
            if (val < 0) continue;   /* a 0 counts too: a zero whose neighbours are not open (a wrong flag stopped the flood) */
            if (!spend(1)) return;
            int nb[8];
            const int n = neighbours(i, nb);
            Constraint c;
            c.num = i;
            int known_mines = 0;
            for (int k = 0; k < n; ++k) {
                const int j = nb[k];
                if (!hidden(j)) continue;
                const Known kn = static_cast<Known>(known_[static_cast<std::size_t>(j)]);
                if (kn == kMine) ++known_mines;
                else if (kn == kUnknown) c.cells.push_back(j);
            }
            if (c.cells.empty()) continue;
            c.rem = val - known_mines;
            cs.push_back(std::move(c));
        }
    }

    bool rules()
    {
        std::vector<Constraint> cs;
        for (;;) {
            build(cs);
            if (done()) return changed_any_;
            bool changed = false;
            for (const Constraint &c : cs) {
                if (c.rem == 0) {
                    for (int j : c.cells) changed |= mark_safe(j);
                } else if (c.rem == static_cast<int>(c.cells.size())) {
                    for (int j : c.cells) changed |= mark_mine(j);
                }
            }
            if (changed) {
                changed_any_ = true;
                if (stop_first_ && found_free_safe_) return true;
                continue;
            }
            if (subset_rule(cs)) {
                changed_any_ = true;
                if (stop_first_ && found_free_safe_) return true;
                continue;
            }
            if (count_rule()) {
                changed_any_ = true;
                if (stop_first_ && found_free_safe_) return true;
                continue;
            }
            break;
        }
        const bool r = changed_any_;
        changed_any_ = false;
        return r;
    }

    bool subset_rule(const std::vector<Constraint> &cs)
    {
        std::fill(cidx_.begin(), cidx_.end(), -1);
        for (std::size_t k = 0; k < cs.size(); ++k) cidx_[static_cast<std::size_t>(cs[k].num)] = static_cast<int>(k);
        bool changed = false;
        std::vector<int> diff;
        for (std::size_t a = 0; a < cs.size(); ++a) {
            const Constraint &A = cs[a];
            const int ax = A.num % v_.w, ay = A.num / v_.w;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int nx = ax + dx, ny = ay + dy;
                    if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= v_.w || ny >= v_.h) continue;
                    const int bi = cidx_[static_cast<std::size_t>(ny * v_.w + nx)];
                    if (bi < 0) continue;
                    const Constraint &B = cs[static_cast<std::size_t>(bi)];
                    if (!spend(1)) return changed;
                    if (A.cells.size() >= B.cells.size()) continue;   /* a strict subset only */
                    if (!std::includes(B.cells.begin(), B.cells.end(), A.cells.begin(), A.cells.end())) continue;
                    diff.clear();
                    std::set_difference(B.cells.begin(), B.cells.end(), A.cells.begin(), A.cells.end(), std::back_inserter(diff));
                    const int dr = B.rem - A.rem;
                    if (dr == 0) {
                        for (int j : diff) changed |= mark_safe(j);
                    } else if (dr == static_cast<int>(diff.size())) {
                        for (int j : diff) changed |= mark_mine(j);
                    }
                }
        }
        return changed;
    }

    bool count_rule()
    {
        int unknown = 0;
        for (int i = 0; i < v_.size(); ++i)
            if (hidden(i) && known_[static_cast<std::size_t>(i)] == kUnknown) ++unknown;
        const int left = v_.mines - mines_known_;
        if (unknown == 0 || left < 0 || left > unknown) return false;
        bool changed = false;
        if (left == 0) {
            for (int i = 0; i < v_.size(); ++i)
                if (hidden(i) && known_[static_cast<std::size_t>(i)] == kUnknown) changed |= mark_safe(i);
        } else if (left == unknown) {
            for (int i = 0; i < v_.size(); ++i)
                if (hidden(i) && known_[static_cast<std::size_t>(i)] == kUnknown) changed |= mark_mine(i);
        }
        return changed;
    }

    int find_root(std::vector<int> &p, int a)
    {
        while (p[static_cast<std::size_t>(a)] != a) {
            p[static_cast<std::size_t>(a)] = p[static_cast<std::size_t>(p[static_cast<std::size_t>(a)])];
            a = p[static_cast<std::size_t>(a)];
        }
        return a;
    }

    bool enumerate()
    {
        std::vector<Constraint> cs;
        build(cs);
        if (cs.empty() || done()) return false;
        std::vector<int> parent(static_cast<std::size_t>(v_.size()));
        for (int i = 0; i < v_.size(); ++i) parent[static_cast<std::size_t>(i)] = i;
        for (const Constraint &c : cs)
            for (std::size_t k = 1; k < c.cells.size(); ++k) {
                const int a = find_root(parent, c.cells[0]), b = find_root(parent, c.cells[k]);
                if (a != b) parent[static_cast<std::size_t>(a)] = b;
            }
        /* group the cells (ascending) and the constraints by root */
        std::vector<std::vector<int>> group_cells(static_cast<std::size_t>(v_.size()));
        std::vector<std::vector<int>> group_cons(static_cast<std::size_t>(v_.size()));
        std::vector<char> in_front(static_cast<std::size_t>(v_.size()), 0);
        for (const Constraint &c : cs)
            for (int j : c.cells) in_front[static_cast<std::size_t>(j)] = 1;
        for (int i = 0; i < v_.size(); ++i)
            if (in_front[static_cast<std::size_t>(i)]) group_cells[static_cast<std::size_t>(find_root(parent, i))].push_back(i);
        for (std::size_t k = 0; k < cs.size(); ++k)
            group_cons[static_cast<std::size_t>(find_root(parent, cs[k].cells[0]))].push_back(static_cast<int>(k));

        bool changed = false;
        for (int r = 0; r < v_.size(); ++r) {
            const std::vector<int> &cells = group_cells[static_cast<std::size_t>(r)];
            if (cells.empty() || static_cast<int>(cells.size()) > enum_limit_) continue;
            if (done()) break;
            Enumerator e;
            e.n = static_cast<int>(cells.size());
            e.cap = kEnumNodeCap;
            e.budget = budget_;
            e.cell_cons.assign(static_cast<std::size_t>(e.n), {});
            e.val.assign(static_cast<std::size_t>(e.n), 0);
            e.mine_count.assign(static_cast<std::size_t>(e.n), 0);
            const std::vector<int> &gc = group_cons[static_cast<std::size_t>(r)];
            for (std::size_t q = 0; q < gc.size(); ++q) {
                const Constraint &c = cs[static_cast<std::size_t>(gc[q])];
                e.rem.push_back(c.rem);
                e.mines.push_back(0);
                e.unassigned.push_back(static_cast<int>(c.cells.size()));
                for (int j : c.cells) {
                    const int local = static_cast<int>(std::lower_bound(cells.begin(), cells.end(), j) - cells.begin());
                    e.cell_cons[static_cast<std::size_t>(local)].push_back(static_cast<int>(q));
                }
            }
            e.dfs(0);
            if (e.aborted || e.total == 0) continue;
            for (int i = 0; i < e.n; ++i) {
                if (e.mine_count[static_cast<std::size_t>(i)] == 0) changed |= mark_safe(cells[static_cast<std::size_t>(i)]);
                else if (e.mine_count[static_cast<std::size_t>(i)] == e.total) changed |= mark_mine(cells[static_cast<std::size_t>(i)]);
            }
            if (stop_first_ && found_free_safe_) return true;
        }
        return changed;
    }

    const View &v_;
    Budget *budget_;
    int enum_limit_;
    bool stop_first_;
    std::vector<std::uint8_t> known_;
    std::vector<int> cidx_;
    int mines_known_ = 0;
    int safe_known_ = 0;
    bool found_free_safe_ = false;
    bool changed_any_ = false;
};

} // namespace

void deduce(const View &view, Deduction &out, Budget *budget, bool stop_at_first_safe)
{
    out = Deduction();
    if (view.w <= 0 || view.h <= 0 || static_cast<int>(view.cell.size()) != view.size()) return;
    Solver s(view, budget, stop_at_first_safe);
    s.run(out);
}

int find_safe_cell(const View &view, Budget *budget)
{
    Deduction d;
    deduce(view, d, budget, true);
    for (int i : d.safe)
        if (view.cell[static_cast<std::size_t>(i)] == kViewHidden) return i;
    return d.safe.empty() ? -1 : d.safe.front();
}

int find_mine(const View &view, Budget *budget)
{
    Deduction d;
    deduce(view, d, budget, false);
    for (int i : d.mines)
        if (view.cell[static_cast<std::size_t>(i)] == kViewHidden) return i;
    return -1;
}

bool is_solvable_without_guessing(const Board &board, int first_click, Budget *budget, int *rounds)
{
    if (rounds) *rounds = 0;
    if (!board.placed() || first_click < 0 || first_click >= board.size() || board.is_mine(first_click)) return false;
    Board b = board;
    b.reset_states();
    if (b.reveal(first_click) != RevealResult::Opened) return false;
    /* One cell at a time, each the one find_safe_cell() names: exactly what a player who follows the hints does. (Taking all
     * the proven cells of a round at once is not the same: rule 4 only enumerates groups of up to kEnumLimit cells, and a
     * newly opened number can join two small groups into a big one, so what is provable is not monotone in the order.) */
    while (!b.all_safe_revealed()) {
        const int c = find_safe_cell(b.view(), budget);
        if (rounds) ++*rounds;
        if (c < 0 || (budget && budget->exhausted())) return false;
        if (b.reveal(c) == RevealResult::Mine) return false;   /* cannot happen: a proven cell is safe */
    }
    return true;
}

} // namespace minefield
