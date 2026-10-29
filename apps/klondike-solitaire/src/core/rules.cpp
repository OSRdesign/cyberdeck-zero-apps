/*
 * SPDX-License-Identifier: MIT
 *
 * Klondike core: the rule options and the scoring helpers (see rules.hpp), and the card names (card.hpp).
 */
#include "rules.hpp"
#include "card.hpp"

#include <cctype>
#include <cstdlib>

namespace klondike {

/* ------------------------------------------------------------------------------------------------- cards */

static const char kRankChars[] = "A23456789TJQK";
static const char kSuitChars[] = "CDHS";

std::string card_name(Card c)
{
    if (c >= kCards) return "--";
    return std::string{kRankChars[rank_of(c) - 1], kSuitChars[suit_of(c)]};
}

bool parse_card(const std::string &text, Card &out, bool *face_down)
{
    std::string t = text;
    if (t.size() == 3 && t[0] == '1' && t[1] == '0') t = "T" + t.substr(2);
    if (t.size() != 2) return false;
    const bool lower = std::islower(static_cast<unsigned char>(t[0])) || std::islower(static_cast<unsigned char>(t[1]));
    const bool upper = std::isupper(static_cast<unsigned char>(t[0])) || std::isupper(static_cast<unsigned char>(t[1]));
    if (lower && upper) return false; /* "aS": neither face up nor face down */
    const char rc = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    const char sc = static_cast<char>(std::toupper(static_cast<unsigned char>(t[1])));
    int rank = 0, suit = -1;
    for (int i = 0; i < kRanks; ++i)
        if (kRankChars[i] == rc) rank = i + 1;
    for (int i = 0; i < kSuits; ++i)
        if (kSuitChars[i] == sc) suit = i;
    if (rank == 0 || suit < 0) return false;
    out = make_card(suit, rank);
    if (face_down) *face_down = lower;
    return true;
}

/* ------------------------------------------------------------------------------------------------- rules */

static const char *scoring_name(Scoring s)
{
    switch (s) {
    case Scoring::Vegas: return "vegas";
    case Scoring::None: return "none";
    case Scoring::Standard: break;
    }
    return "standard";
}

std::string Rules::serialize() const
{
    std::string s;
    s += "draw=" + std::to_string(draw);
    s += ",passes=" + std::to_string(passes);
    s += std::string(",scoring=") + scoring_name(scoring);
    s += std::string(",timed=") + (timed ? "1" : "0");
    s += std::string(",auto=") + (auto_foundation ? "1" : "0");
    s += std::string(",fback=") + (foundation_back ? "1" : "0");
    s += std::string(",fill=") + (fill == TableauFill::AnyCard ? "any" : "king");
    return s;
}

static bool parse_flag(const std::string &v, bool &out)
{
    if (v == "1" || v == "true" || v == "on") { out = true; return true; }
    if (v == "0" || v == "false" || v == "off") { out = false; return true; }
    return false;
}

bool Rules::parse(const std::string &text, Rules &out)
{
    out = Rules();
    int understood = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find_first_of(",;\n ", pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq), v = item.substr(eq + 1);
        bool ok = false;
        if (key == "draw") {
            if (v == "1" || v == "3") { out.draw = v[0] - '0'; ok = true; }
        } else if (key == "passes") {
            if (v == "0" || v == "1" || v == "3") { out.passes = v[0] - '0'; ok = true; }
        } else if (key == "scoring") {
            if (v == "standard") { out.scoring = Scoring::Standard; ok = true; }
            else if (v == "vegas") { out.scoring = Scoring::Vegas; ok = true; }
            else if (v == "none") { out.scoring = Scoring::None; ok = true; }
        } else if (key == "timed") {
            ok = parse_flag(v, out.timed);
        } else if (key == "auto") {
            ok = parse_flag(v, out.auto_foundation);
        } else if (key == "fback") {
            ok = parse_flag(v, out.foundation_back);
        } else if (key == "fill") {
            if (v == "king") { out.fill = TableauFill::KingOnly; ok = true; }
            else if (v == "any") { out.fill = TableauFill::AnyCard; ok = true; }
        }
        if (ok) ++understood;
    }
    return understood > 0;
}

std::string Rules::variation_key() const
{
    std::string k = "d" + std::to_string(draw) + "-";
    k += passes == 0 ? std::string("unl") : "p" + std::to_string(passes);
    k += std::string("-") + scoring_name(scoring);
    return k;
}

bool Rules::operator==(const Rules &o) const
{
    return draw == o.draw && passes == o.passes && scoring == o.scoring && timed == o.timed &&
           auto_foundation == o.auto_foundation && foundation_back == o.foundation_back && fill == o.fill;
}

/* ------------------------------------------------------------------------------------------------- scoring */

namespace score {

std::int64_t time_penalty(std::uint32_t seconds)
{
    return -static_cast<std::int64_t>(kTimePenalty) * static_cast<std::int64_t>(seconds / kTimePenaltySeconds);
}

std::int64_t time_bonus(std::uint32_t seconds)
{
    if (seconds <= static_cast<std::uint32_t>(kTimeBonusMinSeconds)) return 0;
    return kTimeBonus / seconds;
}

} // namespace score

} // namespace klondike
