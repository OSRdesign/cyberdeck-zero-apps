/*
 * SPDX-License-Identifier: MIT
 */
#include "tasks.hpp"

#include "text_codec.hpp"

#include <cstring>

namespace mdnotes {

namespace {

/* Skips spaces, tabs and the tab glyph from i, up to end. */
std::size_t skip_blank(const std::string &t, std::size_t i, std::size_t end)
{
    const std::size_t g = std::strlen(kTabGlyph);
    while (i < end) {
        if (t[i] == ' ' || t[i] == '\t') {
            ++i;
        } else if (i + g <= end && t.compare(i, g, kTabGlyph) == 0) {
            i += g;
        } else {
            break;
        }
    }
    return i;
}

bool blank_at(const std::string &t, std::size_t i, std::size_t end)
{
    if (i >= end) return true;
    return t[i] == ' ' || t[i] == '\t' || t.compare(i, std::strlen(kTabGlyph), kTabGlyph) == 0;
}

} // namespace

bool toggle_task_at(std::string &text, std::size_t offset)
{
    if (offset == 0 || offset + 1 >= text.size()) return false;
    if (text[offset - 1] != '[' || text[offset + 1] != ']') return false;
    char &c = text[offset];
    if (c == ' ') c = 'x';
    else if (c == 'x' || c == 'X') c = ' ';
    else return false;
    return true;
}

std::size_t task_mark_on_line(const std::string &t, std::size_t pos)
{
    if (pos > t.size()) return std::string::npos;
    std::size_t ls = 0;
    if (pos > 0) {
        const std::size_t nl = t.rfind('\n', pos - 1);
        ls = nl == std::string::npos ? 0 : nl + 1;
    }
    std::size_t le = t.find('\n', ls);
    if (le == std::string::npos) le = t.size();
    std::size_t i = skip_blank(t, ls, le);
    while (i < le && t[i] == '>') i = skip_blank(t, i + 1, le);   /* block quotes */
    if (i >= le) return std::string::npos;
    if (t[i] == '-' || t[i] == '*' || t[i] == '+') {
        ++i;
    } else if (t[i] >= '0' && t[i] <= '9') {
        std::size_t d = 0;
        while (i < le && t[i] >= '0' && t[i] <= '9' && d < 9) {
            ++i;
            ++d;
        }
        if (i >= le || (t[i] != '.' && t[i] != ')')) return std::string::npos;
        ++i;
    } else {
        return std::string::npos;
    }
    if (i >= le || !blank_at(t, i, le)) return std::string::npos;
    i = skip_blank(t, i, le);
    if (i + 3 > le) return std::string::npos;
    if (t[i] != '[' || t[i + 2] != ']') return std::string::npos;
    const char c = t[i + 1];
    if (c != ' ' && c != 'x' && c != 'X') return std::string::npos;
    if (!blank_at(t, i + 3, le)) return std::string::npos;
    return i + 1;
}

bool toggle_task_on_line(std::string &text, std::size_t pos, std::size_t *mark_offset)
{
    const std::size_t m = task_mark_on_line(text, pos);
    if (m == std::string::npos) return false;
    if (mark_offset) *mark_offset = m;
    return toggle_task_at(text, m);
}

} // namespace mdnotes
