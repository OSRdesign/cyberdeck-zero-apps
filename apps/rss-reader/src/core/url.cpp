/*
 * SPDX-License-Identifier: MIT
 */
#include "url.hpp"

#include "util.hpp"

#include <cctype>
#include <vector>

namespace rssreader {

namespace {

struct Parts {
    std::string scheme, auth, path, query;
    bool has_query = false;
    bool ok = false;
};

Parts parse(const std::string &url)
{
    Parts p;
    std::size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return p;
    for (std::size_t i = 0; i < sep; ++i) {
        char c = url[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.')) return p;
    }
    p.scheme = lower(url.substr(0, sep));
    std::size_t a = sep + 3;
    std::size_t e = url.find_first_of("/?#", a);
    if (e == std::string::npos) e = url.size();
    p.auth = url.substr(a, e - a);
    std::string rest = url.substr(e);
    std::size_t hash = rest.find('#');
    if (hash != std::string::npos) rest.erase(hash);
    std::size_t q = rest.find('?');
    if (q != std::string::npos) {
        p.has_query = true;
        p.query = rest.substr(q + 1);
        rest.erase(q);
    }
    p.path = rest;
    p.ok = !p.auth.empty();
    return p;
}

std::string remove_dots(const std::string &path)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    bool trailing = false;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string seg = path.substr(i, j - i);
        trailing = false;
        if (seg == "..") {
            if (!out.empty()) out.pop_back();
            trailing = true;
        } else if (seg == ".") {
            trailing = true;
        } else if (!seg.empty()) {
            out.push_back(seg);
        } else if (j == path.size()) {
            trailing = true;
        }
        i = j + 1;
    }
    std::string r = "/";
    for (std::size_t k = 0; k < out.size(); ++k) {
        r += out[k];
        if (k + 1 < out.size()) r += '/';
    }
    if (trailing && !out.empty()) r += '/';
    return r;
}

std::string build(const Parts &p, const std::string &path, bool has_query, const std::string &query)
{
    std::string r = p.scheme + "://" + p.auth + path;
    if (has_query) r += "?" + query;
    return r;
}

bool http_scheme(const std::string &s) { return s == "http" || s == "https"; }

} // namespace

bool is_http_url(const std::string &url)
{
    Parts p = parse(url);
    return p.ok && http_scheme(p.scheme);
}

std::string resolve_url(const std::string &base, const std::string &ref_in)
{
    std::string ref;
    for (char c : trim(ref_in))
        if (c != '\n' && c != '\r' && c != '\t') ref += c;
    if (ref.empty()) return "";
    Parts b = parse(base);
    Parts r = parse(ref);
    if (r.ok) {
        if (!http_scheme(r.scheme)) return "";
        return build(r, r.path.empty() ? "/" : remove_dots(r.path), r.has_query, r.query);
    }
    /* "scheme:" without // (mailto:, javascript:, data:, tel:) */
    std::size_t colon = ref.find(':');
    if (colon != std::string::npos && colon > 0) {
        bool scheme_like = std::isalpha(static_cast<unsigned char>(ref[0])) != 0;
        for (std::size_t i = 0; i < colon && scheme_like; ++i)
            if (!(std::isalnum(static_cast<unsigned char>(ref[i])) || ref[i] == '+' || ref[i] == '-' || ref[i] == '.')) scheme_like = false;
        std::size_t slash = ref.find('/');
        if (scheme_like && (slash == std::string::npos || colon < slash)) return "";
    }
    if (!b.ok || !http_scheme(b.scheme)) return "";
    if (starts_with(ref, "//")) return resolve_url(base, b.scheme + ":" + ref);
    std::size_t hash = ref.find('#');
    if (hash == 0) return build(b, b.path.empty() ? "/" : b.path, b.has_query, b.query);
    if (hash != std::string::npos) ref.erase(hash);
    if (ref.empty()) return build(b, b.path.empty() ? "/" : b.path, b.has_query, b.query);
    std::string path = ref, query;
    bool hq = false;
    std::size_t q = ref.find('?');
    if (q != std::string::npos) {
        hq = true;
        query = ref.substr(q + 1);
        path = ref.substr(0, q);
    }
    if (path.empty()) return build(b, b.path.empty() ? "/" : b.path, hq, query);
    if (path[0] == '/') return build(b, remove_dots(path), hq, query);
    std::string dir = b.path;
    std::size_t ls = dir.rfind('/');
    dir = (ls == std::string::npos) ? "/" : dir.substr(0, ls + 1);
    return build(b, remove_dots(dir + path), hq, query);
}

std::string url_origin(const std::string &url)
{
    Parts p = parse(url);
    if (!p.ok || !http_scheme(p.scheme)) return "";
    return p.scheme + "://" + p.auth;
}

std::string url_host(const std::string &url)
{
    Parts p = parse(url);
    if (!p.ok) return "";
    std::string h = p.auth;
    std::size_t at = h.rfind('@');
    if (at != std::string::npos) h.erase(0, at + 1);
    if (!h.empty() && h[0] == '[') {
        std::size_t e = h.find(']');
        return lower(h.substr(0, e == std::string::npos ? h.size() : e + 1));
    }
    std::size_t c = h.find(':');
    if (c != std::string::npos) h.erase(c);
    return lower(h);
}

std::string url_path(const std::string &url)
{
    Parts p = parse(url);
    if (!p.ok) return "";
    return p.path.empty() ? "/" : p.path;
}

std::string normalize_user_url(const std::string &input)
{
    std::string s = trim(input);
    for (char c : s)
        if (static_cast<unsigned char>(c) <= 0x20) return "";
    if (starts_with(lower(s), "feed://")) s = "https://" + s.substr(7);
    else if (starts_with(lower(s), "feed:http")) s = s.substr(5);
    if (s.find("://") == std::string::npos) {
        if (s.find('.') == std::string::npos && s.find("localhost") == std::string::npos) return "";
        s = "https://" + s;
    }
    if (!is_http_url(s)) return "";
    return s;
}

} // namespace rssreader
