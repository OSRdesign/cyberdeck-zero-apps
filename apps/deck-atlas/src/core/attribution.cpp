/*
 * SPDX-License-Identifier: MIT
 *
 * Deck Atlas: the map credit (see attribution.hpp).
 */
#include "attribution.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace atlas {

namespace {

void put_utf8(std::string &out, unsigned cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void put_cp(std::string &out, unsigned cp)
{
    if (cp == 0xA9) out += "(C)";
    else put_utf8(out, cp);
}

} // namespace

std::string attribution_plain(const std::string &raw)
{
    static const struct {
        const char *name;
        unsigned cp;
    } ents[] = {{"copy", 0xA9}, {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '}};
    std::string out;
    bool space = false;
    for (const char *p = raw.c_str(); *p;) {
        if (*p == '<') {
            const char *e = std::strchr(p, '>');
            if (!e) break;
            p = e + 1;
            space = true;
            continue;
        }
        if (*p == '&') {
            const char *e = std::strchr(p, ';');
            if (e && e - p <= 8) {
                const std::size_t len = static_cast<std::size_t>(e - p - 1);
                unsigned cp = 0;
                if (p[1] == '#') {
                    const bool hex = p[2] == 'x' || p[2] == 'X';
                    cp = static_cast<unsigned>(std::strtoul(p + (hex ? 3 : 2), nullptr, hex ? 16 : 10));
                } else {
                    for (const auto &en : ents)
                        if (std::strlen(en.name) == len && std::strncmp(p + 1, en.name, len) == 0) cp = en.cp;
                }
                if (cp && cp < 0x10000) {
                    p = e + 1;
                    if (cp == ' ') {
                        space = true;
                        continue;
                    }
                    if (space && !out.empty()) out += ' ';
                    space = false;
                    put_cp(out, cp);
                    continue;
                }
            }
        }
        const unsigned char c = static_cast<unsigned char>(*p++);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            continue;
        }
        if (c < 0x20) continue;
        if (space && !out.empty()) out += ' ';
        space = false;
        if (c == 0xC2 && static_cast<unsigned char>(*p) == 0xA9) { /* the sign typed as UTF-8 */
            ++p;
            out += "(C)";
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string attribution_abbrev(const std::string &text)
{
    std::string out = text;
    static const char *const kLong = "OpenStreetMap";
    std::size_t pos = 0;
    while ((pos = out.find(kLong, pos)) != std::string::npos) {
        out.replace(pos, std::strlen(kLong), "OSM");
        pos += 3;
    }
    return out;
}

std::vector<std::string> wrap_lines(const std::string &text, int max_w, int max_lines, const MeasureFn &measure)
{
    std::vector<std::string> lines;
    if (text.empty() || max_lines < 1) return lines;
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : text) {
            if (c == ' ') {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) words.push_back(cur);
    }
    /* the longest start of s that fits with "..." (UTF-8 aware) */
    auto fit_dots = [&](const std::string &s) {
        if (measure(s) <= max_w) return s;
        std::size_t n = s.size();
        while (n > 0) {
            do --n;
            while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80);
            std::string t = s.substr(0, n);
            while (!t.empty() && t.back() == ' ') t.pop_back();
            t += "...";
            if (measure(t) <= max_w) return t;
        }
        return std::string("...");
    };
    std::string line;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string cand = line.empty() ? words[i] : line + " " + words[i];
        if (measure(cand) <= max_w) {
            line = cand;
            continue;
        }
        if (line.empty()) { /* one word wider than the line: it stays alone and is cut below */
            line = words[i];
            continue;
        }
        if (static_cast<int>(lines.size()) + 1 >= max_lines) { /* this is the last line: all that is left, cut */
            std::string rest = line;
            for (std::size_t j = i; j < words.size(); ++j) rest += " " + words[j];
            lines.push_back(fit_dots(rest));
            return lines;
        }
        lines.push_back(fit_dots(line));
        line = words[i];
    }
    if (!line.empty()) lines.push_back(fit_dots(line));
    return lines;
}

MapMetrics map_metrics(const AttrInput &in)
{
    MapMetrics m;
    const int shorter = std::min(in.w, in.h);
    int target = in.target > 0 ? in.target : 44;
    if (target > shorter / 3) target = shorter / 3;
    if (target < 8) target = 8;
    int plate = target * 3 / 4;
    if (plate > shorter * 15 / 100) plate = shorter * 15 / 100;
    if (plate < 8) plate = 8;
    if (plate > target) plate = target;
    m.target = target;
    m.plate = plate;
    m.text_h = in.text_h > 0 ? in.text_h : 12;
    int margin = in.space > 0 ? in.space : 8;
    if (margin > plate / 2) margin = plate / 2;
    if (margin < 2) margin = 2;
    m.margin = margin;
    m.pad = std::max(margin / 2, 2);
    m.row_h = m.text_h + 2 * m.pad;
    int sb_max = in.w / 4;
    if (sb_max > 200) sb_max = 200;
    if (sb_max < 48) sb_max = 48;
    if (sb_max > in.w - 2 * margin - 2 * m.pad) sb_max = in.w - 2 * margin - 2 * m.pad;
    if (sb_max < 8) sb_max = 8;
    const int bar_h = std::max(m.text_h / 4, 3);
    m.sb_w = sb_max + 2 * m.pad;
    m.sb_h = m.pad + m.text_h + m.pad / 2 + bar_h + m.pad;
    m.sb_y = in.h - margin - m.sb_h;
    return m;
}

AttrPlan plan_attribution(const AttrInput &in, const std::string &text, const MeasureFn &measure)
{
    AttrPlan plan;
    if (text.empty() || in.w <= 0 || in.h <= 0) return plan;
    const int w = in.w, h = in.h;
    const MapMetrics mm = map_metrics(in);
    const int margin = mm.margin, pad = mm.pad, plate = mm.plate, text_h = mm.text_h, sb_w = mm.sb_w, sb_y = mm.sb_y;

    const int full = measure(text) + 2 * pad;
    const int beside = w - margin - (margin + sb_w) - margin;
    if (full <= beside) { /* the engine shows it whole beside the scale bar */
        plan.row_top = h - margin - mm.row_h;
        return plan;
    }
    const int right = w - margin - plate - margin; /* the button column starts here */
    int aw = std::min(full, right - margin);
    if (aw < 2 * pad + 8) aw = 2 * pad + 8;
    if (full <= aw) return plan;     /* the engine shows it whole, a row higher */

    /* the engine would cut it: our plate over its box (as wide as that box), growing upward from the box's bottom edge,
     * which is just above the scale bar */
    plan.pad = pad;
    plan.lines = wrap_lines(attribution_abbrev(text), aw - 2 * pad, kMaxLines, measure);
    if (plan.lines.empty()) return plan;
    plan.w = aw;
    plan.h = static_cast<int>(plan.lines.size()) * text_h + 2 * pad;
    plan.x = right - aw;
    plan.y = (sb_y - pad) - plan.h;
    plan.cover = true;
    return plan;
}

} // namespace atlas
