/*
 * SPDX-License-Identifier: MIT
 */
#include "names.hpp"

#include "fsutil.hpp"
#include "utf8.hpp"

#include <cstring>
#include <vector>

namespace mdnotes {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool ends_with_md(const std::string &s)
{
    return s.size() >= 3 && s[s.size() - 3] == '.' && (s[s.size() - 2] == 'm' || s[s.size() - 2] == 'M') &&
           (s[s.size() - 1] == 'd' || s[s.size() - 1] == 'D');
}

bool reserved_device(const std::string &name)
{
    /* the part before the first dot, without trailing spaces, against the Windows device names */
    std::string base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    const std::string b = fold_case(base);
    if (b == "con" || b == "prn" || b == "aux" || b == "nul") return true;
    if (b.size() == 4 && (b.compare(0, 3, "com") == 0 || b.compare(0, 3, "lpt") == 0) && b[3] >= '1' && b[3] <= '9')
        return true;
    return false;
}

} // namespace

std::string clean_typed_name(const std::string &typed)
{
    std::string s = trim(typed);
    if (ends_with_md(s) && s.size() > 3) s = trim(s.substr(0, s.size() - 3));
    return s;
}

NameCheck check_name(const std::string &name)
{
    NameCheck c;
    if (name.empty()) {
        c.error = NameError::Empty;
        return c;
    }
    if (!utf8_valid(name)) {
        c.error = NameError::NotUtf8;
        return c;
    }
    if (name.size() > kNameMaxBytes) {
        c.error = NameError::TooLong;
        return c;
    }
    static const char kForbidden[] = "/\\:*?\"<>|#^[]";
    for (char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u == 0x7F) {
            c.error = NameError::ControlChar;
            return c;
        }
        if (std::strchr(kForbidden, ch)) {
            c.error = NameError::Forbidden;
            c.bad = std::string(1, ch);
            return c;
        }
    }
    if (name[0] == '.') {
        c.error = NameError::LeadingDot;
        return c;
    }
    if (name.back() == ' ' || name.back() == '.') {
        c.error = NameError::TrailingSpaceOrDot;
        return c;
    }
    if (reserved_device(name)) {
        c.error = NameError::Reserved;
        return c;
    }
    return c;
}

NameCheck check_note_path(const std::string &rel, int max_depth)
{
    NameCheck c;
    if (rel.empty()) {
        c.error = NameError::Empty;
        return c;
    }
    std::vector<std::string> parts;
    std::size_t i = 0;
    for (;;) {
        const std::size_t j = rel.find('/', i);
        parts.push_back(rel.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (static_cast<int>(parts.size()) - 1 > max_depth) {
        c.error = NameError::TooDeep;
        return c;
    }
    for (const std::string &p : parts) {
        if (p.empty() || p == "." || p == "..") {
            c.error = NameError::EmptyFolder;
            c.bad = p;
            return c;
        }
        NameCheck one = check_name(p);
        if (!one.ok()) {
            if (one.bad.empty()) one.bad = p;
            return one;
        }
    }
    return c;
}

std::string note_file_name(const std::string &name) { return name + ".md"; }

bool is_note_file(const std::string &file_name) { return file_name.size() > 3 && ends_with_md(file_name); }

std::string strip_md(const std::string &file_name)
{
    return is_note_file(file_name) ? file_name.substr(0, file_name.size() - 3) : file_name;
}

std::string find_existing_ci(const std::string &dir, const std::string &file_name)
{
    std::vector<DirEntry> entries;
    if (!list_dir(dir.empty() ? "." : dir, entries)) return std::string();
    const std::string want = fold_case(file_name);
    for (const DirEntry &e : entries)
        if (e.name == file_name || fold_case(e.name) == want) return e.name;
    return std::string();
}

} // namespace mdnotes
