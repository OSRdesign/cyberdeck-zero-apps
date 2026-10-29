/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the board, the moves and their rules, the deal (see board.hpp).
 */
#include "board.hpp"

#include <cstring>
#include <sstream>

namespace klondike {

/* ------------------------------------------------------------------------------------------------- moves */

MoveKind kind_of(const Move &m)
{
    if (m.from == kStock && m.to == kWaste) return MoveKind::Draw;
    if (m.from == kWaste && m.to == kStock) return MoveKind::Recycle;
    if (m.from == kWaste && is_foundation(m.to)) return MoveKind::WasteToFoundation;
    if (m.from == kWaste && is_tableau(m.to)) return MoveKind::WasteToTableau;
    if (is_tableau(m.from) && is_foundation(m.to)) return MoveKind::TableauToFoundation;
    if (is_tableau(m.from) && is_tableau(m.to)) return MoveKind::TableauToTableau;
    if (is_foundation(m.from) && is_tableau(m.to)) return MoveKind::FoundationToTableau;
    return MoveKind::Invalid;
}

static std::string pile_name(int p)
{
    if (p == kStock) return "S";
    if (p == kWaste) return "W";
    if (is_foundation(p)) return "F" + std::to_string(p - kFoundation0 + 1);
    if (is_tableau(p)) return "T" + std::to_string(p - kTableau0 + 1);
    return "?";
}

std::string move_name(const Move &m)
{
    switch (kind_of(m)) {
    case MoveKind::Draw: return "draw";
    case MoveKind::Recycle: return "recycle";
    case MoveKind::Invalid: return "invalid";
    default: break;
    }
    std::string s = pile_name(m.from) + ">" + pile_name(m.to);
    if (m.count != 1) s += "x" + std::to_string(m.count);
    return s;
}

std::string encode_move(const Move &m)
{
    return std::to_string(m.from) + "-" + std::to_string(m.to) + "-" + std::to_string(m.count);
}

bool parse_move(const std::string &text, Move &out)
{
    int v[3] = {0, 0, 0};
    int idx = 0;
    bool digit = false;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') {
            v[idx] = v[idx] * 10 + (ch - '0');
            if (v[idx] > 255) return false;
            digit = true;
        } else if (ch == '-' && digit && idx < 2) {
            ++idx;
            digit = false;
        } else {
            return false;
        }
    }
    if (idx != 2 || !digit) return false;
    out.from = static_cast<std::uint8_t>(v[0]);
    out.to = static_cast<std::uint8_t>(v[1]);
    out.count = static_cast<std::uint8_t>(v[2]);
    return kind_of(out) != MoveKind::Invalid;
}

/* ------------------------------------------------------------------------------------------------- board */

Board::Board()
{
    std::memset(talon, kNoCard, sizeof talon);
    std::memset(tab, kNoCard, sizeof tab);
    std::memset(ntab, 0, sizeof ntab);
    std::memset(ndown, 0, sizeof ndown);
    for (int f = 0; f < kFoundations; ++f) {
        fsuit[f] = kNoSuit;
        fcount[f] = 0;
    }
}

int Board::size(int pile) const
{
    if (pile == kStock) return ntalon - tp;
    if (pile == kWaste) return tp;
    if (is_foundation(pile)) return fcount[pile - kFoundation0];
    if (is_tableau(pile)) return ntab[pile - kTableau0];
    return 0;
}

Card Board::card(int pile, int i) const
{
    if (i < 0 || i >= size(pile)) return kNoCard;
    if (pile == kStock) return talon[ntalon - 1 - i];
    if (pile == kWaste) return talon[i];
    if (is_foundation(pile)) return make_card(fsuit[pile - kFoundation0], i + 1);
    return tab[pile - kTableau0][i];
}

bool Board::face_up(int pile, int i) const
{
    if (i < 0 || i >= size(pile)) return false;
    if (pile == kStock) return false;
    if (is_tableau(pile)) return i >= ndown[pile - kTableau0];
    return true;
}

Card Board::top(int pile) const
{
    const int n = size(pile);
    return n ? card(pile, n - 1) : kNoCard;
}

int Board::suit_count(int suit) const
{
    for (int f = 0; f < kFoundations; ++f)
        if (fsuit[f] == suit) return fcount[f];
    return 0;
}

int Board::foundation_cards() const
{
    return fcount[0] + fcount[1] + fcount[2] + fcount[3];
}

bool Board::valid(std::string *why) const
{
    auto fail = [why](const std::string &w) {
        if (why) *why = w;
        return false;
    };
    int seen[kCards] = {};
    if (ntalon > kCards || tp > ntalon) return fail("bad stock pointer");
    for (int i = 0; i < ntalon; ++i) {
        if (talon[i] >= kCards) return fail("bad card in the stock or waste");
        ++seen[talon[i]];
    }
    bool suit_used[kSuits] = {};
    for (int f = 0; f < kFoundations; ++f) {
        if (fcount[f] > kRanks) return fail("foundation over 13 cards");
        if (fcount[f] == 0) {
            if (fsuit[f] != kNoSuit) return fail("empty foundation with a suit");
            continue;
        }
        if (fsuit[f] >= kSuits) return fail("foundation without a suit");
        if (suit_used[fsuit[f]]) return fail("two foundations of one suit");
        suit_used[fsuit[f]] = true;
        for (int r = 1; r <= fcount[f]; ++r) ++seen[make_card(fsuit[f], r)];
    }
    for (int c = 0; c < kColumns; ++c) {
        if (ntab[c] > kMaxColumn) return fail("column too long");
        if (ndown[c] > ntab[c] || (ntab[c] > 0 && ndown[c] == ntab[c]))
            return fail("column " + std::to_string(c + 1) + " has no face-up top");
        for (int i = 0; i < ntab[c]; ++i) {
            if (tab[c][i] >= kCards) return fail("bad card in a column");
            ++seen[tab[c][i]];
            if (i > ndown[c] && !builds_on(tab[c][i], tab[c][i - 1]))
                return fail("column " + std::to_string(c + 1) + " has a face-up card out of sequence");
        }
    }
    for (int i = 0; i < kCards; ++i)
        if (seen[i] != 1) return fail(card_name(static_cast<Card>(i)) + (seen[i] ? " twice" : " missing"));
    return true;
}

static std::uint64_t mix64(std::uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint64_t Board::hash() const
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    auto add = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    add(ntalon);
    add(tp);
    add(recycles);
    for (int i = 0; i < ntalon; ++i) add(talon[i]);
    for (int f = 0; f < kFoundations; ++f) add((static_cast<std::uint64_t>(fsuit[f]) << 8) | fcount[f]);
    for (int c = 0; c < kColumns; ++c) {
        add(0x1000 + ntab[c] * 32 + ndown[c]);
        for (int i = 0; i < ntab[c]; ++i) add(tab[c][i]);
    }
    return mix64(h);
}

bool Board::operator==(const Board &o) const
{
    if (ntalon != o.ntalon || tp != o.tp || recycles != o.recycles) return false;
    if (std::memcmp(talon, o.talon, ntalon) != 0) return false;
    for (int f = 0; f < kFoundations; ++f)
        if (fcount[f] != o.fcount[f] || (fcount[f] && fsuit[f] != o.fsuit[f])) return false;
    for (int c = 0; c < kColumns; ++c) {
        if (ntab[c] != o.ntab[c] || ndown[c] != o.ndown[c]) return false;
        if (std::memcmp(tab[c], o.tab[c], ntab[c]) != 0) return false;
    }
    return true;
}

std::string Board::to_text() const
{
    static const char kSuitChars[] = "CDHS";
    std::string s = "f";
    for (int f = 0; f < kFoundations; ++f) {
        if (fcount[f] == 0) s += " -";
        else s += std::string(" ") + kSuitChars[fsuit[f]] + std::to_string(fcount[f]);
    }
    for (int c = 0; c < kColumns; ++c) {
        s += "\nt";
        for (int i = 0; i < ntab[c]; ++i) {
            std::string n = card_name(tab[c][i]);
            if (i < ndown[c])
                for (char &ch : n) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
            s += " " + n;
        }
    }
    s += "\nw";
    for (int i = 0; i < tp; ++i) s += " " + card_name(talon[i]);
    s += "\ns";
    for (int i = ntalon - 1; i >= tp; --i) s += " " + card_name(talon[i]);
    s += "\nr " + std::to_string(recycles);
    return s;
}

bool Board::from_text(const std::string &text, Board &out, std::string *why)
{
    auto fail = [why](const std::string &w) {
        if (why) *why = w;
        return false;
    };
    Board b;
    int columns = 0;
    bool have_f = false, have_w = false, have_s = false;
    std::vector<Card> waste, stock;
    std::string norm = text;
    for (char &ch : norm)
        if (ch == ';') ch = '\n';
    std::istringstream lines(norm);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string tag;
        if (!(words >> tag)) continue;
        std::vector<std::string> items;
        for (std::string w; words >> w;) items.push_back(w);
        if (tag == "f") {
            if (have_f || items.size() != kFoundations) return fail("'f' needs the four foundations");
            have_f = true;
            for (int f = 0; f < kFoundations; ++f) {
                const std::string &it = items[f];
                if (it == "-") continue;
                static const std::string kSuitChars = "CDHS";
                const std::size_t suit = it.empty() ? std::string::npos : kSuitChars.find(it[0]);
                if (suit == std::string::npos) return fail("bad foundation '" + it + "'");
                int n = 0;
                for (std::size_t i = 1; i < it.size(); ++i) {
                    if (it[i] < '0' || it[i] > '9') return fail("bad foundation '" + it + "'");
                    n = n * 10 + (it[i] - '0');
                    if (n > kRanks) return fail("bad foundation '" + it + "'");
                }
                if (n == 0) return fail("bad foundation '" + it + "'");
                b.fsuit[f] = static_cast<std::uint8_t>(suit);
                b.fcount[f] = static_cast<std::uint8_t>(n);
            }
        } else if (tag == "t") {
            if (columns >= kColumns) return fail("more than 7 columns");
            if (items.size() > static_cast<std::size_t>(kMaxColumn)) return fail("column too long");
            bool up_seen = false;
            for (const std::string &it : items) {
                Card c;
                bool down = false;
                if (!parse_card(it, c, &down)) return fail("bad card '" + it + "'");
                if (down && up_seen) return fail("face-down card on a face-up one");
                if (down) ++b.ndown[columns];
                else up_seen = true;
                b.tab[columns][b.ntab[columns]++] = c;
            }
            ++columns;
        } else if (tag == "w" || tag == "s") {
            std::vector<Card> &dst = tag == "w" ? waste : stock;
            if ((tag == "w" ? have_w : have_s)) return fail("'" + tag + "' twice");
            (tag == "w" ? have_w : have_s) = true;
            for (const std::string &it : items) {
                Card c;
                bool down = false;
                if (!parse_card(it, c, &down) || down) return fail("bad card '" + it + "'");
                dst.push_back(c);
            }
        } else if (tag == "r") {
            if (items.size() != 1) return fail("'r' needs one number");
            const std::string &it = items[0];
            if (it.empty() || it.size() > 2 || it.find_first_not_of("0123456789") != std::string::npos)
                return fail("bad recycles");
            b.recycles = static_cast<std::uint8_t>(std::stoi(it));
        } else {
            return fail("unknown line '" + tag + "'");
        }
    }
    if (!have_f) return fail("no foundations line");
    if (columns != kColumns) return fail("7 columns needed");
    if (waste.size() + stock.size() > static_cast<std::size_t>(kCards)) return fail("too many cards in the talon");
    for (Card c : waste) b.talon[b.ntalon++] = c;
    for (auto it = stock.rbegin(); it != stock.rend(); ++it) b.talon[b.ntalon++] = *it;
    b.tp = static_cast<std::uint8_t>(waste.size());
    std::string w;
    if (!b.valid(&w)) return fail(w);
    out = b;
    return true;
}

/* ------------------------------------------------------------------------------------------------- deal */

namespace {

class SplitMix64 {
public:
    explicit SplitMix64(std::uint32_t number) : state_(number * 0x9E3779B97F4A7C15ull ^ 0x4B4C4F4E44494B45ull) {}
    std::uint64_t next()
    {
        state_ += 0x9E3779B97F4A7C15ull;
        return mix64(state_);
    }
    /* uniform in [0, n), n >= 1, without modulo bias */
    std::uint32_t below(std::uint32_t n)
    {
        const std::uint64_t limit = ~std::uint64_t(0) - (~std::uint64_t(0) % n + 1) % n;
        /* limit = 2^64 - (2^64 mod n) - 1: values above it would favour the low remainders */
        std::uint64_t v;
        do v = next();
        while (v > limit);
        return static_cast<std::uint32_t>(v % n);
    }

private:
    std::uint64_t state_;
};

} // namespace

Board deal_board(std::uint32_t number)
{
    Card deck[kCards];
    for (int i = 0; i < kCards; ++i) deck[i] = static_cast<Card>(i);
    SplitMix64 rng(number);
    for (int i = kCards - 1; i >= 1; --i) {
        const std::uint32_t j = rng.below(static_cast<std::uint32_t>(i + 1));
        const Card t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
    Board b;
    int k = 0;
    for (int row = 0; row < kColumns; ++row)
        for (int c = row; c < kColumns; ++c) b.tab[c][b.ntab[c]++] = deck[k++];
    for (int c = 0; c < kColumns; ++c) b.ndown[c] = static_cast<std::uint8_t>(c);
    /* the rest is the stock, deck[28] drawn first: talon[] is in draw order */
    for (; k < kCards; ++k) b.talon[b.ntalon++] = deck[k];
    b.tp = 0;
    return b;
}

std::uint32_t layout_checksum(const Board &b)
{
    std::uint32_t h = 2166136261u;
    auto add = [&h](std::uint8_t v) { h = (h ^ v) * 16777619u; };
    for (int c = 0; c < kColumns; ++c)
        for (int i = 0; i < b.ntab[c]; ++i) add(b.tab[c][i]);
    for (int i = b.tp; i < b.ntalon; ++i) add(b.talon[i]);
    return h;
}

/* ------------------------------------------------------------------------------------------------- rules */

int foundation_for(const Board &b, Card c)
{
    if (c >= kCards) return -1;
    const int suit = suit_of(c), rank = rank_of(c);
    if (rank == 1) {
        for (int f = 0; f < kFoundations; ++f)
            if (b.fcount[f] == 0) return foundation_pile(f);
        return -1;
    }
    for (int f = 0; f < kFoundations; ++f)
        if (b.fsuit[f] == suit) return b.fcount[f] == rank - 1 ? foundation_pile(f) : -1;
    return -1;
}

bool safe_to_foundation(const Board &b, Card c)
{
    if (foundation_for(b, c) < 0) return false;
    const int rank = rank_of(c);
    if (rank <= 2) return true;
    const bool red = is_red(c);
    for (int s = 0; s < kSuits; ++s)
        if (is_red_suit(s) != red && b.suit_count(s) < rank - 1) return false;
    return true;
}

/* foundation f takes card c: an Ace on any empty foundation, else the next card of its suit */
static bool foundation_accepts(const Board &b, int f, Card c)
{
    if (b.fcount[f] == 0) return rank_of(c) == 1;
    return b.fsuit[f] == suit_of(c) && b.fcount[f] == rank_of(c) - 1;
}

bool fits_column(const Board &b, const Rules &r, Card c, int col)
{
    if (b.ntab[col] == 0) return rank_of(c) == kRanks || r.fill == TableauFill::AnyCard;
    return builds_on(c, b.tab[col][b.ntab[col] - 1]);
}

bool can_move(const Board &b, const Rules &r, const Move &m)
{
    switch (kind_of(m)) {
    case MoveKind::Draw: {
        const int stock = b.stock_size();
        const int want = stock < r.draw ? stock : r.draw;
        return stock > 0 && m.count == want;
    }
    case MoveKind::Recycle: {
        const int max_rec = r.max_recycles();
        return b.stock_size() == 0 && b.tp > 0 && m.count == b.tp && (max_rec < 0 || b.recycles < max_rec);
    }
    case MoveKind::WasteToFoundation:
        return m.count == 1 && b.tp > 0 && foundation_accepts(b, m.to - kFoundation0, b.talon[b.tp - 1]);
    case MoveKind::WasteToTableau:
        return m.count == 1 && b.tp > 0 && fits_column(b, r, b.talon[b.tp - 1], m.to - kTableau0);
    case MoveKind::TableauToFoundation: {
        const int c = m.from - kTableau0;
        return m.count == 1 && b.ntab[c] > 0 && foundation_accepts(b, m.to - kFoundation0, b.tab[c][b.ntab[c] - 1]);
    }
    case MoveKind::TableauToTableau: {
        const int s = m.from - kTableau0, t = m.to - kTableau0;
        if (s == t || m.count < 1) return false;
        const int up = b.ntab[s] - b.ndown[s];
        if (m.count > up) return false;
        const int base = b.ntab[s] - m.count;
        for (int i = base + 1; i < b.ntab[s]; ++i)
            if (!builds_on(b.tab[s][i], b.tab[s][i - 1])) return false;
        if (b.ntab[t] + m.count > kMaxColumn) return false;
        return fits_column(b, r, b.tab[s][base], t);
    }
    case MoveKind::FoundationToTableau: {
        const int f = m.from - kFoundation0;
        if (!r.foundation_back || m.count != 1 || b.fcount[f] == 0) return false;
        return fits_column(b, r, make_card(b.fsuit[f], b.fcount[f]), m.to - kTableau0);
    }
    case MoveKind::Invalid: break;
    }
    return false;
}

static void put_on_foundation(Board &b, int f, Card c)
{
    if (b.fcount[f] == 0) b.fsuit[f] = static_cast<std::uint8_t>(suit_of(c));
    ++b.fcount[f];
}

static Card take_from_foundation(Board &b, int f)
{
    const Card c = make_card(b.fsuit[f], b.fcount[f]);
    if (--b.fcount[f] == 0) b.fsuit[f] = Board::kNoSuit;
    return c;
}

static Card take_waste_top(Board &b)
{
    const int pos = b.tp - 1;
    const Card c = b.talon[pos];
    std::memmove(b.talon + pos, b.talon + pos + 1, b.ntalon - pos - 1);
    --b.ntalon;
    --b.tp;
    b.talon[b.ntalon] = kNoCard;
    return c;
}

static void put_waste_top(Board &b, Card c)
{
    const int pos = b.tp;
    std::memmove(b.talon + pos + 1, b.talon + pos, b.ntalon - pos);
    b.talon[pos] = c;
    ++b.ntalon;
    ++b.tp;
}

static bool flip_if_needed(Board &b, int col)
{
    if (b.ntab[col] > 0 && b.ndown[col] == b.ntab[col]) {
        --b.ndown[col];
        return true;
    }
    return false;
}

void do_move(Board &b, const Rules &r, const Move &m, bool *flipped)
{
    (void)r;
    bool flip = false;
    switch (kind_of(m)) {
    case MoveKind::Draw: b.tp = static_cast<std::uint8_t>(b.tp + m.count); break;
    case MoveKind::Recycle:
        b.tp = 0;
        ++b.recycles;
        break;
    case MoveKind::WasteToFoundation: put_on_foundation(b, m.to - kFoundation0, take_waste_top(b)); break;
    case MoveKind::WasteToTableau: {
        const int t = m.to - kTableau0;
        b.tab[t][b.ntab[t]++] = take_waste_top(b);
        break;
    }
    case MoveKind::TableauToFoundation: {
        const int s = m.from - kTableau0;
        const Card c = b.tab[s][--b.ntab[s]];
        b.tab[s][b.ntab[s]] = kNoCard;
        put_on_foundation(b, m.to - kFoundation0, c);
        flip = flip_if_needed(b, s);
        break;
    }
    case MoveKind::TableauToTableau: {
        const int s = m.from - kTableau0, t = m.to - kTableau0;
        const int base = b.ntab[s] - m.count;
        std::memcpy(&b.tab[t][b.ntab[t]], &b.tab[s][base], m.count);
        b.ntab[t] = static_cast<std::uint8_t>(b.ntab[t] + m.count);
        std::memset(&b.tab[s][base], kNoCard, m.count);
        b.ntab[s] = static_cast<std::uint8_t>(base);
        flip = flip_if_needed(b, s);
        break;
    }
    case MoveKind::FoundationToTableau: {
        const int t = m.to - kTableau0;
        b.tab[t][b.ntab[t]++] = take_from_foundation(b, m.from - kFoundation0);
        break;
    }
    case MoveKind::Invalid: break;
    }
    if (flipped) *flipped = flip;
}

void undo_move(Board &b, const Move &m, bool flipped)
{
    if (flipped && is_tableau(m.from)) ++b.ndown[m.from - kTableau0];
    switch (kind_of(m)) {
    case MoveKind::Draw: b.tp = static_cast<std::uint8_t>(b.tp - m.count); break;
    case MoveKind::Recycle:
        b.tp = static_cast<std::uint8_t>(m.count);
        --b.recycles;
        break;
    case MoveKind::WasteToFoundation: put_waste_top(b, take_from_foundation(b, m.to - kFoundation0)); break;
    case MoveKind::WasteToTableau: {
        const int t = m.to - kTableau0;
        const Card c = b.tab[t][--b.ntab[t]];
        b.tab[t][b.ntab[t]] = kNoCard;
        put_waste_top(b, c);
        break;
    }
    case MoveKind::TableauToFoundation: {
        const int s = m.from - kTableau0;
        b.tab[s][b.ntab[s]++] = take_from_foundation(b, m.to - kFoundation0);
        break;
    }
    case MoveKind::TableauToTableau: {
        const int s = m.from - kTableau0, t = m.to - kTableau0;
        const int base = b.ntab[t] - m.count;
        std::memcpy(&b.tab[s][b.ntab[s]], &b.tab[t][base], m.count);
        b.ntab[s] = static_cast<std::uint8_t>(b.ntab[s] + m.count);
        std::memset(&b.tab[t][base], kNoCard, m.count);
        b.ntab[t] = static_cast<std::uint8_t>(base);
        break;
    }
    case MoveKind::FoundationToTableau: {
        const int t = m.to - kTableau0;
        const Card c = b.tab[t][--b.ntab[t]];
        b.tab[t][b.ntab[t]] = kNoCard;
        put_on_foundation(b, m.from - kFoundation0, c);
        break;
    }
    case MoveKind::Invalid: break;
    }
}

Move stock_move(const Board &b, const Rules &r)
{
    Move m;
    if (b.stock_size() > 0) {
        m.from = kStock;
        m.to = kWaste;
        m.count = static_cast<std::uint8_t>(b.stock_size() < r.draw ? b.stock_size() : r.draw);
        return m;
    }
    const int max_rec = r.max_recycles();
    if (b.tp > 0 && (max_rec < 0 || b.recycles < max_rec)) {
        m.from = kWaste;
        m.to = kStock;
        m.count = b.tp;
    }
    return m;
}

void legal_moves(const Board &b, const Rules &r, std::vector<Move> &out)
{
    out.clear();
    const Move sm = stock_move(b, r);
    if (sm.count) out.push_back(sm);
    auto try_add = [&](int from, int to, int count) {
        Move m;
        m.from = static_cast<std::uint8_t>(from);
        m.to = static_cast<std::uint8_t>(to);
        m.count = static_cast<std::uint8_t>(count);
        if (can_move(b, r, m)) out.push_back(m);
    };
    if (b.tp > 0) {
        for (int f = 0; f < kFoundations; ++f) try_add(kWaste, foundation_pile(f), 1);
        for (int t = 0; t < kColumns; ++t) try_add(kWaste, tableau_pile(t), 1);
    }
    for (int s = 0; s < kColumns; ++s) {
        if (b.ntab[s] == 0) continue;
        for (int f = 0; f < kFoundations; ++f) try_add(tableau_pile(s), foundation_pile(f), 1);
        const int up = b.ntab[s] - b.ndown[s];
        for (int k = 1; k <= up; ++k)
            for (int t = 0; t < kColumns; ++t)
                if (t != s) try_add(tableau_pile(s), tableau_pile(t), k);
    }
    if (r.foundation_back)
        for (int f = 0; f < kFoundations; ++f)
            if (b.fcount[f])
                for (int t = 0; t < kColumns; ++t) try_add(foundation_pile(f), tableau_pile(t), 1);
}

Move safe_auto_move(const Board &b)
{
    Move m;
    for (int s = 0; s < kColumns; ++s) {
        if (b.ntab[s] == 0) continue;
        const Card c = b.tab[s][b.ntab[s] - 1];
        if (safe_to_foundation(b, c)) {
            m.from = static_cast<std::uint8_t>(tableau_pile(s));
            m.to = static_cast<std::uint8_t>(foundation_for(b, c));
            m.count = 1;
            return m;
        }
    }
    if (b.tp > 0 && safe_to_foundation(b, b.talon[b.tp - 1])) {
        m.from = kWaste;
        m.to = static_cast<std::uint8_t>(foundation_for(b, b.talon[b.tp - 1]));
        m.count = 1;
    }
    return m;
}

} // namespace klondike
