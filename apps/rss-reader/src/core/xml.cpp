/*
 * SPDX-License-Identifier: MIT
 */
#include "xml.hpp"

#include "util.hpp"

#include <cctype>
#include <algorithm>
#include <utility>

namespace rssreader {

const XmlNode *XmlNode::child(const std::string &n) const
{
    for (const auto &k : kids)
        if (!k->is_text && iequals(k->name, n)) return k.get();
    return nullptr;
}

std::vector<const XmlNode *> XmlNode::children(const std::string &n) const
{
    std::vector<const XmlNode *> v;
    for (const auto &k : kids)
        if (!k->is_text && iequals(k->name, n)) v.push_back(k.get());
    return v;
}

std::string XmlNode::attr(const std::string &n) const
{
    for (const auto &a : attrs)
        if (iequals(a.name, n)) return a.value;
    return "";
}

bool XmlNode::has_attr(const std::string &n) const
{
    for (const auto &a : attrs)
        if (iequals(a.name, n)) return true;
    return false;
}

std::string XmlNode::text_content() const
{
    if (is_text) return text;
    std::string out;
    /* iterative: depth is bounded by the parser, but stay cheap */
    std::vector<const XmlNode *> stack{this};
    std::vector<std::size_t> idx{0};
    while (!stack.empty()) {
        const XmlNode *n = stack.back();
        std::size_t &i = idx.back();
        if (i >= n->kids.size()) {
            stack.pop_back();
            idx.pop_back();
            continue;
        }
        const XmlNode *k = n->kids[i++].get();
        if (k->is_text) {
            out += k->text;
        } else {
            stack.push_back(k);
            idx.push_back(0);
        }
    }
    return out;
}

std::string XmlNode::child_text(const std::string &n) const
{
    const XmlNode *c = child(n);
    return c ? trim(c->text_content()) : "";
}

namespace {

/* canonical prefix for a namespace URI: "" = none known, "-" = the default of RSS 1.0 (bare names) */
const char *canon_prefix(const std::string &uri_in)
{
    std::string uri = lower(uri_in);
    while (!uri.empty() && uri.back() == '/') uri.pop_back();
    if (uri == "http://purl.org/dc/elements/1.1" || uri == "http://purl.org/dc/terms") return "dc";
    if (uri == "http://purl.org/rss/1.0/modules/content") return "content";
    if (uri == "http://search.yahoo.com/mrss" || uri == "http://video.search.yahoo.com/mrss") return "media";
    if (uri == "http://www.w3.org/2005/atom") return "atom";
    if (uri == "http://www.w3.org/1999/02/22-rdf-syntax-ns#" || uri == "http://www.w3.org/1999/02/22-rdf-syntax-ns") return "rdf";
    if (uri == "http://www.itunes.com/dtds/podcast-1.0.dtd") return "itunes";
    if (uri == "http://purl.org/rss/1.0") return "-";
    if (uri == "http://www.w3.org/xml/1998/namespace") return "xml";
    return "";
}

struct Decl {
    std::string prefix, uri;
};

struct Parser {
    std::string s;
    std::size_t i = 0;
    XmlDoc *doc = nullptr;
    std::size_t nodes = 0;
    XmlStatus fail = XmlStatus::Ok;

    struct Open {
        XmlNode *node;
        std::string raw;            /* the name as written, to match end tags */
        std::size_t decls;          /* declarations added by this element */
    };
    std::vector<Open> stack;
    std::vector<Decl> decls;

    static bool name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == ':' || (static_cast<unsigned char>(c) >= 0x80); }
    static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    std::string lookup(const std::string &prefix) const
    {
        for (std::size_t k = decls.size(); k-- > 0;)
            if (decls[k].prefix == prefix) return decls[k].uri;
        return "\x01";   /* unbound */
    }

    std::string canonical(const std::string &raw, bool is_element) const
    {
        std::size_t c = raw.find(':');
        if (c == std::string::npos) return raw;
        std::string prefix = raw.substr(0, c), local = raw.substr(c + 1);
        if (prefix == "xml" || prefix == "xmlns" || !is_element) return raw;
        std::string uri = lookup(prefix);
        if (uri == "\x01") return raw;
        std::string cp = canon_prefix(uri);
        if (cp.empty()) return raw;
        if (cp == "-") return local;
        return cp + ":" + local;
    }

    XmlNode *add_child(XmlNode *parent, bool text)
    {
        if (++nodes > kMaxXmlNodes) { fail = XmlStatus::TooBig; return nullptr; }
        auto n = std::make_unique<XmlNode>();
        n->is_text = text;
        n->parent = parent;
        XmlNode *raw = n.get();
        if (parent) parent->kids.push_back(std::move(n));
        else doc->root = std::move(n);
        return raw;
    }

    void add_text(const std::string &t)
    {
        if (stack.empty() || t.empty()) return;
        XmlNode *parent = stack.back().node;
        if (!parent->kids.empty() && parent->kids.back()->is_text) {
            parent->kids.back()->text += t;
            return;
        }
        XmlNode *n = add_child(parent, true);
        if (n) n->text = t;
    }

    void skip_declaration()      /* "<!DOCTYPE ...>" with a possible [ internal subset ] and quoted strings */
    {
        int bracket = 0;
        char quote = 0;
        while (i < s.size()) {
            char c = s[i++];
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') quote = c;
            else if (c == '[') ++bracket;
            else if (c == ']') { if (bracket > 0) --bracket; }
            else if (c == '>' && bracket == 0) return;
        }
        doc->recovered = true;
    }

    bool parse()
    {
        bool root_done = false;
        while (i < s.size() && !root_done && fail == XmlStatus::Ok) {
            if (s[i] != '<') {
                std::size_t e = s.find('<', i);
                if (e == std::string::npos) e = s.size();
                if (!stack.empty()) add_text(decode_entities(s.substr(i, e - i)));
                i = e;
                continue;
            }
            if (s.compare(i, 4, "<!--") == 0) {
                std::size_t e = s.find("-->", i + 4);
                if (e == std::string::npos) { doc->recovered = true; i = s.size(); }
                else i = e + 3;
            } else if (s.compare(i, 9, "<![CDATA[") == 0) {
                std::size_t e = s.find("]]>", i + 9);
                std::string t = s.substr(i + 9, e == std::string::npos ? std::string::npos : e - i - 9);
                if (e == std::string::npos) { doc->recovered = true; i = s.size(); }
                else i = e + 3;
                add_text(t);
            } else if (s.compare(i, 2, "<?") == 0) {
                std::size_t e = s.find("?>", i + 2);
                if (e == std::string::npos) { doc->recovered = true; i = s.size(); }
                else i = e + 2;
            } else if (s.compare(i, 2, "<!") == 0) {
                i += 2;
                skip_declaration();
            } else if (s.compare(i, 2, "</") == 0) {
                end_tag();
                if (stack.empty() && doc->root) root_done = true;
            } else if (i + 1 < s.size() && name_start(s[i + 1])) {
                start_tag();
                if (stack.empty() && doc->root) root_done = true;
            } else {
                add_text("<");     /* a stray '<' in text */
                ++i;
                doc->recovered = true;
            }
        }
        if (fail != XmlStatus::Ok) return false;
        if (!stack.empty()) doc->recovered = true;   /* truncated: whatever is open stays as parsed */
        return doc->root != nullptr;
    }

    void end_tag()
    {
        std::size_t e = s.find('>', i);
        std::string raw = trim(s.substr(i + 2, (e == std::string::npos ? s.size() : e) - i - 2));
        i = (e == std::string::npos) ? s.size() : e + 1;
        for (std::size_t k = stack.size(); k-- > 0;) {
            if (stack[k].raw == raw) {
                if (k + 1 != stack.size()) doc->recovered = true;
                while (stack.size() > k) pop();
                return;
            }
        }
        doc->recovered = true;        /* an end tag nothing opened: ignored */
    }

    void pop()
    {
        decls.resize(decls.size() - stack.back().decls);
        stack.pop_back();
    }

    void start_tag()
    {
        std::size_t st = i + 1;
        std::size_t p = st;
        while (p < s.size() && !is_ws(s[p]) && s[p] != '/' && s[p] != '>') ++p;
        std::string raw = s.substr(st, p - st);
        std::vector<XmlAttr> attrs;
        bool self = false, closed = false;
        while (p < s.size()) {
            while (p < s.size() && is_ws(s[p])) ++p;
            if (p >= s.size()) break;
            if (s[p] == '>') { ++p; closed = true; break; }
            if (s[p] == '/') {
                ++p;
                if (p < s.size() && s[p] == '>') { ++p; self = true; closed = true; break; }
                continue;
            }
            std::size_t ns = p;
            while (p < s.size() && !is_ws(s[p]) && s[p] != '=' && s[p] != '>' && s[p] != '/') ++p;
            if (p == ns) { ++p; continue; }   /* a byte that cannot start a name */
            XmlAttr a;
            a.name = s.substr(ns, p - ns);
            while (p < s.size() && is_ws(s[p])) ++p;
            if (p < s.size() && s[p] == '=') {
                ++p;
                while (p < s.size() && is_ws(s[p])) ++p;
                if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
                    char q = s[p++];
                    std::size_t e = s.find(q, p);
                    if (e == std::string::npos) {
                        doc->recovered = true;
                        e = s.find('>', p);
                        if (e == std::string::npos) e = s.size();
                        a.value = s.substr(p, e - p);
                        p = e;
                    } else {
                        a.value = s.substr(p, e - p);
                        p = e + 1;
                    }
                } else {
                    std::size_t vs = p;
                    while (p < s.size() && !is_ws(s[p]) && s[p] != '>') ++p;
                    a.value = s.substr(vs, p - vs);
                    if (!a.value.empty() && a.value.back() == '/' && p < s.size() && s[p] == '>') { a.value.pop_back(); self = true; }
                    doc->recovered = true;
                }
                a.value = decode_entities(a.value);
            }
            attrs.push_back(std::move(a));
        }
        i = p;
        if (!closed) { doc->recovered = true; return; }   /* truncated inside the tag: dropped */

        if (static_cast<int>(stack.size()) >= kMaxXmlDepth) { fail = XmlStatus::TooDeep; return; }
        std::size_t added = 0;
        for (const XmlAttr &a : attrs) {
            if (a.name == "xmlns") { decls.push_back({"", a.value}); ++added; }
            else if (starts_with(a.name, "xmlns:")) { decls.push_back({a.name.substr(6), a.value}); ++added; }
        }
        XmlNode *parent = stack.empty() ? nullptr : stack.back().node;
        if (!parent && doc->root) return;   /* a second root: ignored */
        XmlNode *n = add_child(parent, false);
        if (!n) { decls.resize(decls.size() - added); return; }
        n->name = canonical(raw, true);
        std::size_t c = n->name.rfind(':');
        n->local = (c == std::string::npos) ? n->name : n->name.substr(c + 1);
        n->attrs = std::move(attrs);
        if (self) {
            decls.resize(decls.size() - added);
            return;
        }
        stack.push_back({n, raw, added});
    }
};

} // namespace

XmlStatus xml_parse(const std::string &input, XmlDoc &out)
{
    out = XmlDoc();
    if (input.size() > kMaxXmlBytes) return XmlStatus::TooBig;
    std::size_t start = 0;
    if (input.compare(0, 3, "\xEF\xBB\xBF") == 0) start = 3;
    else if (input.compare(0, 2, "\xFE\xFF") == 0 || input.compare(0, 2, "\xFF\xFE") == 0) return XmlStatus::BadEncoding;
    if (input.size() >= start + 4 && (input[start] == 0 || input[start + 1] == 0)) return XmlStatus::BadEncoding;   /* UTF-16 / 32 without BOM */

    Parser p;
    p.doc = &out;
    p.s = input.substr(start);

    /* the encoding declaration of the prolog */
    std::size_t lead = 0;
    while (lead < p.s.size() && Parser::is_ws(p.s[lead])) ++lead;
    int mode = 0;                       /* 0 UTF-8, 1 latin1, 2 cp1252, 3 latin9 */
    if (p.s.compare(lead, 5, "<?xml") == 0) {
        std::size_t e = p.s.find("?>", lead);
        std::string decl = lower(p.s.substr(lead, (e == std::string::npos ? std::min<std::size_t>(p.s.size() - lead, 200) : e - lead)));
        std::size_t k = decl.find("encoding");
        if (k != std::string::npos) {
            k = decl.find_first_of("\"'", k);
            if (k != std::string::npos) {
                std::size_t k2 = decl.find(decl[k], k + 1);
                std::string enc = decl.substr(k + 1, k2 == std::string::npos ? std::string::npos : k2 - k - 1);
                if (enc == "utf-8" || enc == "utf8" || enc == "us-ascii" || enc == "ascii" || enc.empty()) mode = 0;
                else if (enc == "iso-8859-1" || enc == "iso8859-1" || enc == "latin1" || enc == "latin-1" || enc == "l1" || enc == "iso_8859-1") mode = 1;
                else if (enc == "windows-1252" || enc == "cp1252" || enc == "win-1252") mode = 2;
                else if (enc == "iso-8859-15" || enc == "latin9" || enc == "latin-9") mode = 3;
                else return XmlStatus::BadEncoding;
            }
        }
    }
    if (mode == 0) p.s = utf8_sanitize(p.s);
    else p.s = latin1_to_utf8(p.s, true, mode == 3);   /* "ISO-8859-1" is read as windows-1252, as browsers do */

    if (!p.parse()) {
        XmlStatus st = p.fail != XmlStatus::Ok ? p.fail : XmlStatus::Empty;
        out = XmlDoc();
        return st;
    }
    return XmlStatus::Ok;
}

std::string xml_inner(const XmlNode &node)
{
    std::string out;
    for (const auto &k : node.kids) {
        if (k->is_text) {
            out += html_escape(k->text);
            continue;
        }
        out += "<" + k->local;
        for (const XmlAttr &a : k->attrs) out += " " + a.name + "=\"" + html_escape(a.value) + "\"";
        if (k->kids.empty()) {
            out += "/>";
        } else {
            out += ">" + xml_inner(*k) + "</" + k->local + ">";
        }
    }
    return out;
}

} // namespace rssreader
