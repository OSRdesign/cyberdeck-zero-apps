/*
 * SPDX-License-Identifier: MIT
 */
#include "html_text.hpp"

#include "url.hpp"
#include "util.hpp"

#include <cctype>
#include <cstdlib>
#include <set>

namespace rssreader {

constexpr std::size_t kMaxHtmlBytes = 1024 * 1024;
constexpr std::size_t kMaxBlocks = 4000;

std::string HtmlToken::attr(const std::string &n) const
{
    for (const auto &a : attrs)
        if (a.first == n) return a.second;
    return "";
}

bool HtmlToken::has_attr(const std::string &n) const
{
    for (const auto &a : attrs)
        if (a.first == n) return true;
    return false;
}

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool tag_start(const std::string &s, std::size_t i)
{
    if (i + 1 >= s.size()) return false;
    char c = s[i + 1];
    return std::isalpha(static_cast<unsigned char>(c)) || (c == '/' && i + 2 < s.size() && std::isalpha(static_cast<unsigned char>(s[i + 2])));
}

} // namespace

std::vector<HtmlToken> html_tokenize(const std::string &input)
{
    std::vector<HtmlToken> out;
    const std::string s = input.size() > kMaxHtmlBytes ? input.substr(0, kMaxHtmlBytes) : input;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<' || !tag_start(s, i)) {
            if (s.compare(i, 4, "<!--") == 0) {
                std::size_t e = s.find("-->", i + 4);
                i = (e == std::string::npos) ? s.size() : e + 3;
                continue;
            }
            if (s[i] == '<' && i + 1 < s.size() && (s[i + 1] == '!' || s[i + 1] == '?')) {
                std::size_t e = s.find('>', i);
                i = (e == std::string::npos) ? s.size() : e + 1;
                continue;
            }
            std::size_t e = i + 1;
            while (e < s.size() && !(s[e] == '<' && (tag_start(s, e) || s.compare(e, 2, "<!") == 0 || s.compare(e, 2, "<?") == 0))) ++e;
            HtmlToken t;
            t.kind = HtmlToken::Kind::Text;
            t.text = s.substr(i, e - i);
            out.push_back(std::move(t));
            i = e;
            continue;
        }
        HtmlToken t;
        std::size_t p = i + 1;
        if (s[p] == '/') { t.kind = HtmlToken::Kind::End; ++p; }
        else t.kind = HtmlToken::Kind::Start;
        std::size_t ns = p;
        while (p < s.size() && !is_ws(s[p]) && s[p] != '/' && s[p] != '>') ++p;
        t.name = lower(s.substr(ns, p - ns));
        bool closed = false;
        while (p < s.size()) {
            while (p < s.size() && is_ws(s[p])) ++p;
            if (p >= s.size()) break;
            if (s[p] == '>') { ++p; closed = true; break; }
            if (s[p] == '/') {
                ++p;
                if (p < s.size() && s[p] == '>') { t.self_closing = true; ++p; closed = true; break; }
                continue;
            }
            std::size_t as = p;
            while (p < s.size() && !is_ws(s[p]) && s[p] != '=' && s[p] != '>' && s[p] != '/') ++p;
            if (p == as) { ++p; continue; }
            std::string an = lower(s.substr(as, p - as)), av;
            while (p < s.size() && is_ws(s[p])) ++p;
            if (p < s.size() && s[p] == '=') {
                ++p;
                while (p < s.size() && is_ws(s[p])) ++p;
                if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                    char q = s[p++];
                    std::size_t e = s.find(q, p);
                    if (e == std::string::npos) e = s.size();
                    av = s.substr(p, e - p);
                    p = (e < s.size()) ? e + 1 : e;
                } else {
                    std::size_t vs = p;
                    while (p < s.size() && !is_ws(s[p]) && s[p] != '>') ++p;
                    av = s.substr(vs, p - vs);
                }
            }
            t.attrs.emplace_back(std::move(an), decode_entities(av));
        }
        i = p;
        (void)closed;
        bool raw_text = t.kind == HtmlToken::Kind::Start && (t.name == "script" || t.name == "style");
        out.push_back(t);
        if (raw_text && !t.self_closing) {
            /* skip to the matching end tag */
            std::string low = lower(s.substr(i, std::string::npos));
            std::size_t e = low.find("</" + t.name);
            if (e == std::string::npos) { i = s.size(); }
            else {
                i += e;
            }
        }
    }
    return out;
}

namespace {

bool dropped_tag(const std::string &n)
{
    static const std::set<std::string> d = {"script", "style", "iframe", "object", "embed", "noscript", "svg", "video", "audio",
                                            "head", "template", "canvas", "select", "button", "textarea", "map", "applet", "title"};
    return d.count(n) != 0;
}

bool block_tag(const std::string &n)
{
    static const std::set<std::string> b = {"p", "div", "section", "article", "header", "footer", "main", "aside", "nav", "figure",
                                            "figcaption", "ul", "ol", "li", "dl", "dt", "dd", "table", "thead", "tbody", "tfoot",
                                            "tr", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre", "hr", "address",
                                            "details", "summary", "fieldset", "form", "center", "body", "html", "caption"};
    return b.count(n) != 0;
}

bool tiny_image(const HtmlToken &t)
{
    for (const char *k : {"width", "height"}) {
        std::string v = trim(t.attr(k));
        if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) {
            int n = std::atoi(v.c_str());
            if (n >= 0 && n <= 2 && v.find('%') == std::string::npos) return true;
        }
    }
    return false;
}

/* The URL of an <img>: src, else data-src / data-lazy-src / data-original, else the first srcset candidate. */
std::string img_source(const HtmlToken &t, const std::string &base)
{
    std::vector<std::string> cands;
    cands.push_back(t.attr("src"));
    for (const char *k : {"data-src", "data-lazy-src", "data-original", "data-lazy"}) cands.push_back(t.attr(k));
    std::string ss = t.attr("srcset");
    if (ss.empty()) ss = t.attr("data-srcset");
    if (!ss.empty()) {
        std::string first = trim(ss.substr(0, ss.find(',')));
        std::size_t sp = first.find_first_of(" \t\r\n");
        cands.push_back(sp == std::string::npos ? first : first.substr(0, sp));
    }
    for (const std::string &c : cands) {
        std::string u = trim(c);
        if (u.empty() || starts_with(lower(u), "data:")) continue;
        std::string r = base.empty() ? (is_http_url(u) ? u : std::string()) : resolve_url(base, u);
        if (!r.empty()) return r;
    }
    return "";
}

bool tracking_url(const std::string &u)
{
    std::string l = lower(u);
    return l.find("feedburner.com/~") != std::string::npos || l.find("doubleclick.net") != std::string::npos ||
           l.find("/pixel.gif") != std::string::npos || l.find("/pixel.png") != std::string::npos;
}

struct Builder {
    std::vector<Block> blocks;
    std::string cur;                 /* inline text being collected, whitespace not yet normalised */
    bool cur_pre = false;
    int heading = 0;
    int quote = 0;
    bool in_pre = false;
    struct ListCtx { bool ordered; int count; };
    std::vector<ListCtx> lists;
    int li_open = 0;
    bool li_first = true;            /* the next flush inside the open <li> carries the marker */
    int td_in_row = 0;
    bool in_row = false;
    std::string base;

    void add(Block b)
    {
        if (blocks.size() < kMaxBlocks) blocks.push_back(std::move(b));
    }

    void flush()
    {
        std::string text;
        if (in_pre) {
            text = cur;
            /* trim blank lines at the ends */
            std::size_t a = 0;
            while (a < text.size() && (text[a] == '\n' || text[a] == '\r')) ++a;
            std::size_t e = text.size();
            while (e > a && (text[e - 1] == '\n' || text[e - 1] == '\r' || text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
            text = text.substr(a, e - a);
            std::string clean;
            for (char c : text)
                if (c != '\r') clean += c;
            text = clean;
        } else {
            /* whitespace normalisation, keeping the '\n' that <br> put there (as \x01) */
            const std::string &t = cur;
            std::string out;
            std::string seg;
            for (std::size_t k = 0; k <= t.size(); ++k) {
                if (k == t.size() || t[k] == '\x01') {
                    std::string c2 = collapse_ws(seg);
                    out += c2;
                    if (k < t.size()) out += '\n';
                    seg.clear();
                } else {
                    seg += t[k];
                }
            }
            /* a line break next to nothing is dropped */
            std::string fin;
            for (char c : out) {
                if (c == '\n' && (fin.empty() || fin.back() == '\n')) continue;
                fin += c;
            }
            while (!fin.empty() && fin.back() == '\n') fin.pop_back();
            text = fin;
            for (std::size_t k = 0; k + 1 < text.size();) {
                if (text[k] == '\n' && text[k + 1] == ' ') text.erase(k + 1, 1);
                else if (text[k] == ' ' && text[k + 1] == '\n') text.erase(k, 1);
                else ++k;
            }
        }
        cur.clear();
        if (text.empty()) return;
        Block b;
        b.text = text;
        if (in_pre) {
            b.type = BlockType::Preformatted;
        } else if (heading > 0) {
            b.type = BlockType::Heading;
            b.level = heading;
        } else if (li_open > 0 && !lists.empty()) {
            b.type = BlockType::ListItem;
            b.level = static_cast<int>(lists.size());
            b.ordered = lists.back().ordered;
            if (li_first) b.number = b.ordered ? lists.back().count : 1;
            else b.number = 0;
            li_first = false;
        } else if (quote > 0) {
            b.type = BlockType::Quote;
            b.level = quote;
        } else {
            b.type = BlockType::Paragraph;
        }
        add(std::move(b));
    }

    void text(const std::string &raw)
    {
        std::string d = decode_entities(raw);
        if (in_pre) {
            cur += d;
            return;
        }
        for (char c : d) cur += (c == '\n' || c == '\r') ? ' ' : c;
    }
};

} // namespace

std::vector<Block> html_to_blocks(const std::string &html, const std::string &base_url)
{
    std::vector<HtmlToken> toks = html_tokenize(html);
    Builder b;
    b.base = base_url;
    std::string drop;          /* name of the dropped element being skipped */
    int drop_depth = 0;
    for (const HtmlToken &t : toks) {
        if (b.blocks.size() >= kMaxBlocks) break;
        if (!drop.empty()) {
            if (t.name == drop) {
                if (t.kind == HtmlToken::Kind::Start && !t.self_closing) ++drop_depth;
                else if (t.kind == HtmlToken::Kind::End && --drop_depth <= 0) drop.clear();
            }
            continue;
        }
        if (t.kind == HtmlToken::Kind::Text) {
            b.text(t.text);
            continue;
        }
        const std::string &n = t.name;
        if (t.kind == HtmlToken::Kind::Start) {
            if (dropped_tag(n)) {
                if (!t.self_closing) {
                    b.flush();
                    drop = n;
                    drop_depth = 1;
                }
                continue;
            }
            if (n == "br") { b.cur += b.in_pre ? "\n" : "\x01"; continue; }
            if (n == "img") {
                std::string u = img_source(t, base_url);
                if (u.empty() || tiny_image(t) || tracking_url(u)) continue;
                b.flush();
                Block im;
                im.type = BlockType::Image;
                im.url = u;
                im.alt = collapse_ws(t.attr("alt"));
                b.add(std::move(im));
                continue;
            }
            if (n == "hr") { b.flush(); Block r; r.type = BlockType::Rule; b.add(std::move(r)); continue; }
            if (!block_tag(n)) continue;       /* inline: a, b, i, span, code ... contribute their text only */
            b.flush();
            if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') b.heading = n[1] - '0';
            else if (n == "ul" || n == "ol") { b.lists.push_back({n == "ol", 0}); }
            else if (n == "li") {
                if (b.lists.empty()) b.lists.push_back({false, 0});     /* a stray <li> */
                b.lists.back().count++;
                b.li_open++;
                b.li_first = true;
            } else if (n == "blockquote") ++b.quote;
            else if (n == "pre") b.in_pre = true;
            else if (n == "tr") { b.in_row = true; b.td_in_row = 0; }
        } else {
            if (n == "td" || n == "th") {
                if (b.in_row) { b.cur += " | "; ++b.td_in_row; }
                continue;
            }
            if (!block_tag(n)) continue;
            b.flush();
            if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') b.heading = 0;
            else if (n == "ul" || n == "ol") { if (!b.lists.empty()) b.lists.pop_back(); if (b.lists.empty()) b.li_open = 0; }
            else if (n == "li") { if (b.li_open > 0) --b.li_open; }
            else if (n == "blockquote") { if (b.quote > 0) --b.quote; }
            else if (n == "pre") b.in_pre = false;
            else if (n == "tr") {
                b.in_row = false;
            }
        }
    }
    b.flush();
    /* table cells: " | " separators came with leading / trailing bars; tidy them */
    for (Block &bl : b.blocks) {
        if (bl.type == BlockType::Image || bl.type == BlockType::Rule) continue;
        if (bl.text.find(" | ") == std::string::npos && bl.text.find('|') == std::string::npos) continue;
        std::string t = bl.text;
        while (starts_with(t, "| ")) t.erase(0, 2);
        while (starts_with(t, "|")) t.erase(0, 1);
        while (ends_with(t, " |")) t.erase(t.size() - 2);
        bl.text = trim(t);
    }
    std::vector<Block> clean;
    for (Block &bl : b.blocks)
        if (bl.type == BlockType::Image || bl.type == BlockType::Rule || !bl.text.empty()) clean.push_back(std::move(bl));
    return clean;
}

std::string blocks_to_string(const std::vector<Block> &blocks)
{
    std::string out;
    for (const Block &b : blocks) {
        switch (b.type) {
        case BlockType::Heading: out += std::string(static_cast<std::size_t>(b.level > 0 ? b.level : 1), '#') + " " + b.text; break;
        case BlockType::Paragraph: out += b.text; break;
        case BlockType::ListItem: {
            out += std::string(static_cast<std::size_t>(b.level > 1 ? (b.level - 1) * 2 : 0), ' ');
            if (b.number == 0) out += "  ";
            else if (b.ordered) out += std::to_string(b.number) + ". ";
            else out += "- ";
            out += b.text;
            break;
        }
        case BlockType::Quote: out += std::string(static_cast<std::size_t>(b.level > 0 ? b.level : 1), '>') + " " + b.text; break;
        case BlockType::Preformatted: out += b.text; break;
        case BlockType::Image: out += "[image" + (b.alt.empty() ? std::string() : ": " + b.alt) + "]"; break;
        case BlockType::Rule: out += "---"; break;
        }
        out += '\n';
    }
    return out;
}

std::string html_to_text(const std::string &html) { return blocks_to_string(html_to_blocks(html)); }

std::string html_to_inline(const std::string &html)
{
    std::string out;
    std::string drop;
    int depth = 0;
    for (const HtmlToken &t : html_tokenize(html)) {
        if (!drop.empty()) {
            if (t.name == drop) {
                if (t.kind == HtmlToken::Kind::Start && !t.self_closing) ++depth;
                else if (t.kind == HtmlToken::Kind::End && --depth <= 0) drop.clear();
            }
            continue;
        }
        if (t.kind == HtmlToken::Kind::Text) out += decode_entities(t.text);
        else if (t.kind == HtmlToken::Kind::Start && dropped_tag(t.name) && !t.self_closing) { drop = t.name; depth = 1; }
        else if (block_tag(t.name) || t.name == "br") out += ' ';
    }
    return collapse_ws(out);
}

std::vector<std::string> html_image_urls(const std::string &html, const std::string &base_url)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const HtmlToken &t : html_tokenize(html)) {
        if (t.kind != HtmlToken::Kind::Start || t.name != "img") continue;
        if (tiny_image(t)) continue;
        std::string u = img_source(t, base_url);
        if (u.empty() || tracking_url(u) || !seen.insert(u).second) continue;
        out.push_back(u);
    }
    return out;
}

} // namespace rssreader
