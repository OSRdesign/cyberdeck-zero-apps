/*
 * SPDX-License-Identifier: MIT
 */
#include "links.hpp"

#include "fsutil.hpp"
#include "names.hpp"
#include "utf8.hpp"

#include <vector>

namespace mdnotes {

namespace {

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

int depth_of(const std::string &rel)
{
    int n = 0;
    for (char c : rel) n += c == '/';
    return n;
}

/* The note of the index whose path without ".md" equals stem (folded), -1 when none. */
int find_by_stem(const VaultIndex &ix, const std::string &stem_folded)
{
    for (std::size_t i = 0; i < ix.notes.size(); ++i)
        if (fold_case(strip_md(ix.notes[i].rel)) == stem_folded) return static_cast<int>(i);
    return -1;
}

/* Among several candidates: the one in `folder`, else the shallowest, else the shortest path, else the first path. */
bool better(const NoteEntry &a, const NoteEntry &b, const std::string &folder)
{
    const bool fa = a.folder == folder, fb = b.folder == folder;
    if (fa != fb) return fa;
    const int da = depth_of(a.rel), db = depth_of(b.rel);
    if (da != db) return da < db;
    if (a.rel.size() != b.rel.size()) return a.rel.size() < b.rel.size();
    return a.rel < b.rel;
}

Resolved missing_at(const std::string &rel_stem, const std::string &heading)
{
    Resolved r;
    r.heading = heading;
    if (!check_note_path(rel_stem).ok()) {
        r.invalid = true;
        return r;
    }
    r.rel = rel_stem + ".md";
    return r;
}

} // namespace

WikiTarget parse_wikilink(const std::string &inner)
{
    WikiTarget t;
    std::string target = inner;
    const std::size_t bar = inner.find('|');
    if (bar != std::string::npos) {
        target = inner.substr(0, bar);
        t.alias = trim(inner.substr(bar + 1));
    }
    const std::size_t hash = target.find('#');
    if (hash != std::string::npos) {
        t.heading = trim(target.substr(hash + 1));
        target = target.substr(0, hash);
    }
    t.note = trim(target);
    return t;
}

LinkKind classify_href(const std::string &href)
{
    const std::string h = trim(href);
    if (h.empty()) return LinkKind::File;
    if (h[0] == '#') return LinkKind::Note;
    /* a scheme: letters, digits, + - . then ':' */
    if ((h[0] >= 'a' && h[0] <= 'z') || (h[0] >= 'A' && h[0] <= 'Z')) {
        std::size_t i = 1;
        while (i < h.size() && ((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9') ||
                                h[i] == '+' || h[i] == '-' || h[i] == '.'))
            ++i;
        if (i < h.size() && h[i] == ':' && i > 1) return LinkKind::External;   /* "C:" style one-letter names are not schemes */
    }
    if (h.size() > 4 && fold_case(h.substr(0, 4)) == "www.") return LinkKind::External;
    std::string path = h.substr(0, h.find_first_of("#?"));
    if (path.find('@') != std::string::npos && path.find('/') == std::string::npos) return LinkKind::External;
    path = url_decode(path);
    if (is_note_file(path) || (!path.empty() && is_note_file(base_name(path)))) return LinkKind::Note;
    return LinkKind::File;
}

std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int a = hex(s[i + 1]), b = hex(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out += static_cast<char>(a * 16 + b);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

bool normalize_rel(const std::string &path, std::string &out)
{
    std::vector<std::string> parts;
    std::size_t i = 0;
    for (;;) {
        const std::size_t j = path.find('/', i);
        const std::string p = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (p == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else if (!p.empty() && p != ".") {
            parts.push_back(p);
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    out.clear();
    for (const std::string &p : parts) {
        if (!out.empty()) out += '/';
        out += p;
    }
    return true;
}

Resolved resolve_wikilink(const VaultIndex &ix, const std::string &from_rel, const std::string &target, NewNoteFolder where)
{
    const WikiTarget t = parse_wikilink(target);
    Resolved r;
    r.heading = t.heading;
    std::string note = t.note;
    if (note.empty()) {
        r.same_note = true;
        r.found = ix.find_note(from_rel) >= 0;
        r.rel = from_rel;
        return r;
    }
    note = strip_md(note);
    const std::string from_folder = dir_name(from_rel);
    const bool relative = note.compare(0, 2, "./") == 0 || note.compare(0, 3, "../") == 0;
    const bool absolute = !note.empty() && note[0] == '/';   /* from the vault root only */
    while (!note.empty() && note[0] == '/') note.erase(0, 1);

    if (absolute || note.find('/') != std::string::npos) {
        std::string vault_path, local_path;
        const bool vault_ok = !relative && normalize_rel(note, vault_path) && !vault_path.empty();
        const bool local_ok = !absolute && normalize_rel(path_join(from_folder, note), local_path) && !local_path.empty();
        if (vault_ok) {
            const int i = find_by_stem(ix, fold_case(vault_path));
            if (i >= 0) {
                r.found = true;
                r.rel = ix.notes[static_cast<std::size_t>(i)].rel;
                return r;
            }
        }
        if (local_ok) {
            const int i = find_by_stem(ix, fold_case(local_path));
            if (i >= 0) {
                r.found = true;
                r.rel = ix.notes[static_cast<std::size_t>(i)].rel;
                return r;
            }
        }
        if (!relative && !absolute && vault_ok) {
            const std::string tail = "/" + fold_case(vault_path);
            int best = -1;
            for (std::size_t i = 0; i < ix.notes.size(); ++i) {
                const std::string stem = fold_case(strip_md(ix.notes[i].rel));
                if (stem.size() > tail.size() && stem.compare(stem.size() - tail.size(), tail.size(), tail) == 0 &&
                    (best < 0 || better(ix.notes[i], ix.notes[static_cast<std::size_t>(best)], from_folder)))
                    best = static_cast<int>(i);
            }
            if (best >= 0) {
                r.found = true;
                r.rel = ix.notes[static_cast<std::size_t>(best)].rel;
                return r;
            }
        }
        if (relative ? local_ok : vault_ok) return missing_at(relative ? local_path : vault_path, t.heading);
        r.invalid = true;
        return r;
    }

    const std::string want = fold_case(note);
    int best = -1;
    for (std::size_t i = 0; i < ix.notes.size(); ++i) {
        if (ix.notes[i].folded != want) continue;
        if (best < 0 || better(ix.notes[i], ix.notes[static_cast<std::size_t>(best)], from_folder)) best = static_cast<int>(i);
    }
    if (best >= 0) {
        r.found = true;
        r.rel = ix.notes[static_cast<std::size_t>(best)].rel;
        return r;
    }
    const std::string where_dir = (where == NewNoteFolder::SameFolder) ? from_folder : std::string();
    return missing_at(path_join(where_dir, note), t.heading);
}

Resolved resolve_note_href(const VaultIndex &ix, const std::string &from_rel, const std::string &href)
{
    Resolved r;
    std::string h = trim(href);
    if (h.size() >= 2 && h.front() == '<' && h.back() == '>') h = h.substr(1, h.size() - 2);
    const std::size_t hash = h.find('#');
    if (hash != std::string::npos) {
        r.heading = url_decode(h.substr(hash + 1));
        h = h.substr(0, hash);
    }
    const std::size_t q = h.find('?');
    if (q != std::string::npos) h = h.substr(0, q);
    std::string path = url_decode(h);
    if (path.empty()) {
        r.same_note = true;
        r.found = ix.find_note(from_rel) >= 0;
        r.rel = from_rel;
        return r;
    }
    std::string norm;
    const bool ok = path[0] == '/' ? normalize_rel(path.substr(1), norm) : normalize_rel(path_join(dir_name(from_rel), path), norm);
    if (!ok || norm.empty() || !is_note_file(base_name(norm))) {
        r.invalid = true;
        return r;
    }
    const std::string want = fold_case(norm);
    for (const NoteEntry &n : ix.notes) {
        if (fold_case(n.rel) == want) {
            r.found = true;
            r.rel = n.rel;
            return r;
        }
    }
    Resolved m = missing_at(strip_md(norm), r.heading);
    if (!m.invalid) m.rel = norm;   /* keep the suffix as written */
    return m;
}

LinkAt link_at(const std::string &text, std::size_t offset)
{
    LinkAt la;
    if (offset > text.size()) return la;
    std::size_t ls = 0;
    if (offset > 0) {
        const std::size_t nl = text.rfind('\n', offset - 1);
        ls = nl == std::string::npos ? 0 : nl + 1;
    }
    std::size_t le = text.find('\n', offset);
    if (le == std::string::npos) le = text.size();
    const std::string line = text.substr(ls, le - ls);
    const std::size_t at = offset - ls;
    /* [[...]] */
    for (std::size_t p = line.find("[["); p != std::string::npos; p = line.find("[[", p + 2)) {
        const std::size_t e = line.find("]]", p + 2);
        if (e == std::string::npos) break;
        if (at >= p && at <= e + 2) {
            la.found = true;
            la.wiki = true;
            la.target = line.substr(p + 2, e - p - 2);
            la.begin = ls + p;
            la.end = ls + e + 2;
            return la;
        }
        p = e;
    }
    /* [text](href) */
    for (std::size_t m = line.find("]("); m != std::string::npos; m = line.find("](", m + 2)) {
        const std::size_t open = line.rfind('[', m);
        const std::size_t close = line.find(')', m + 2);
        if (open == std::string::npos || close == std::string::npos) continue;
        std::size_t b = open;
        if (b > 0 && line[b - 1] == '!') --b;
        if (at >= b && at <= close + 1) {
            std::string href = trim(line.substr(m + 2, close - m - 2));
            const std::size_t sp = href.find(' ');   /* [t](dest "title") */
            if (sp != std::string::npos && href.front() != '<') href = href.substr(0, sp);
            if (href.size() >= 2 && href.front() == '<' && href.back() == '>') href = href.substr(1, href.size() - 2);
            la.found = true;
            la.target = href;
            la.begin = ls + b;
            la.end = ls + close + 1;
            return la;
        }
    }
    return la;
}

} // namespace mdnotes
