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

std::vector<std::string> wrap_words(const std::string &text, int max_w, const MeasureFn &measure)
{
    std::vector<std::string> lines;
    if (text.empty()) return lines;
    if (max_w < 1) max_w = 1;
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
    /* a word wider than a line: split between characters (UTF-8 aware), so nothing is lost */
    std::vector<std::string> pieces;
    for (const std::string &w : words) {
        std::string rest = w;
        while (measure(rest) > max_w && rest.size() > 1) {
            std::size_t n = rest.size();
            do --n;
            while (n > 1 && (static_cast<unsigned char>(rest[n]) & 0xC0) == 0x80);
            while (n > 1 && measure(rest.substr(0, n)) > max_w) {
                do --n;
                while (n > 1 && (static_cast<unsigned char>(rest[n]) & 0xC0) == 0x80);
            }
            pieces.push_back(rest.substr(0, n));
            rest = rest.substr(n);
        }
        pieces.push_back(rest);
    }
    std::string line;
    for (const std::string &w : pieces) {
        const std::string cand = line.empty() ? w : line + " " + w;
        if (line.empty() || measure(cand) <= max_w) {
            line = cand;
        } else {
            lines.push_back(line);
            line = w;
        }
    }
    if (!line.empty()) lines.push_back(line);
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

int engine_reserve(const AttrInput &in, int credit_w)
{
    int r = 0;
    for (int it = 0; it < 8; ++it) {
        AttrInput t = in;
        t.h = in.h + r;
        const MapMetrics mm = map_metrics(t);
        int top = mm.sb_y; /* the topmost row the engine draws, in widget coordinates */
        if (credit_w > 0) {
            const int full = credit_w + 2 * mm.pad;
            const int beside = t.w - mm.margin - (mm.margin + mm.sb_w) - mm.margin;
            if (full <= beside) top = std::min(top, t.h - mm.margin - mm.row_h);
            else top = std::min(top, mm.sb_y - mm.pad - mm.row_h); /* a row higher */
        }
        const int need = in.h - top;
        if (need <= 0) break;
        r += need;
    }
    return r;
}

CreditLayout plan_credit(const CreditInput &in, const std::string &text, const MeasureFn &measure)
{
    CreditLayout L;
    const int text_h = in.text_h > 0 ? in.text_h : 9;
    L.hp = std::max(in.pad / 2, 2);
    L.vp = std::max(in.pad / 4, 1);
    L.row_h = text_h + 2 * L.vp;
    int sb_max = in.w / 4;
    if (sb_max > 200) sb_max = 200;
    if (sb_max < 40) sb_max = 40;
    if (sb_max > in.w / 3) sb_max = std::max(in.w / 3, 8);
    L.sb_bar_max = sb_max;
    L.sb_label_w = measure("5000 km");
    const int sb_w = L.hp + sb_max + L.hp + L.sb_label_w + L.hp;
    L.scale = Rect{0, in.h - L.row_h, sb_w, L.row_h};
    L.text_y = in.h - L.row_h + L.vp;
    if (text.empty() || in.w <= 0 || in.h <= 0) return L;

    const int right = in.right > 0 && in.right < in.w ? in.right : in.w;
    const int avail = std::max(right - sb_w - L.hp, 2 * L.hp + 8); /* the credit plate's widest width: a gap to the scale bar */
    const std::string abbr = attribution_abbrev(text);
    std::vector<std::string> lines;
    if (measure(text) + 2 * L.hp <= avail) {
        lines = {text};
    } else if (abbr != text && measure(abbr) + 2 * L.hp <= avail) {
        lines = {abbr};
        L.abbreviated = true;
    } else {
        std::vector<std::string> full = wrap_words(text, avail - 2 * L.hp, measure);
        if (full.size() <= 2) {
            lines = full;
        } else {
            lines = wrap_words(abbr, avail - 2 * L.hp, measure);
            L.abbreviated = abbr != text;
        }
    }
    int widest = 0;
    for (const std::string &l : lines) widest = std::max(widest, measure(l));
    L.lines = lines;
    L.wrapped = lines.size() > 1;
    const int w = widest + 2 * L.hp, h = static_cast<int>(lines.size()) * text_h + 2 * L.vp;
    L.credit = Rect{right - w, in.h - h, w, h};
    L.text_y = in.h - L.row_h + L.vp;
    return L;
}

Rect scale_plate(const CreditLayout &l, int bar_px, int label_w)
{
    int w = l.hp + std::max(bar_px, 1) + l.hp + label_w + l.hp;
    if (w > l.scale.w) w = l.scale.w;
    return Rect{0, l.scale.y, w, l.scale.h};
}

} // namespace atlas
