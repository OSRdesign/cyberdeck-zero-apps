/*
 * SPDX-License-Identifier: MIT
 */
#include "text_codec.hpp"

#include <cstring>

namespace mdnotes {

const char *const kTabGlyph = "\xE2\x87\xA5";

namespace {

constexpr std::size_t kGlyphLen = 3;

std::string replace_all(const std::string &s, const std::string &from, const std::string &to)
{
    if (from.empty() || s.find(from) == std::string::npos) return s;
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    std::size_t i = 0;
    for (;;) {
        const std::size_t p = s.find(from, i);
        if (p == std::string::npos) break;
        out.append(s, i, p - i);
        out += to;
        i = p + from.size();
    }
    out.append(s, i, std::string::npos);
    return out;
}

} // namespace

Decoded decode_text(const std::string &raw)
{
    Decoded d;
    std::size_t start = 0;
    if (raw.size() >= 3 && std::memcmp(raw.data(), "\xEF\xBB\xBF", 3) == 0) {
        d.fmt.bom = true;
        start = 3;
    }
    std::size_t crlf = 0, lf = 0;
    for (std::size_t i = start; i < raw.size(); ++i) {
        if (raw[i] != '\n') continue;
        if (i > start && raw[i - 1] == '\r') ++crlf;
        else ++lf;
    }
    if (crlf && lf) d.fmt.eol = Eol::Mixed;
    else if (crlf) d.fmt.eol = Eol::Crlf;
    else d.fmt.eol = Eol::Lf;
    d.fmt.write_crlf = crlf > lf;
    if (crlf == 0) {
        d.text.assign(raw, start, std::string::npos);
    } else {
        d.text.reserve(raw.size() - start);
        for (std::size_t i = start; i < raw.size(); ++i) {
            if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;   /* the CR of a CRLF pair */
            d.text += raw[i];
        }
    }
    d.fmt.map_tabs = d.text.find(kTabGlyph) == std::string::npos;
    return d;
}

std::string encode_text(const std::string &body, const TextFormat &fmt)
{
    std::string out;
    out.reserve(body.size() + (fmt.bom ? 3 : 0) + (fmt.write_crlf ? body.size() / 16 : 0));
    if (fmt.bom) out += "\xEF\xBB\xBF";
    if (!fmt.write_crlf) {
        out += body;
        return out;
    }
    for (char c : body) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

std::string to_edit(const std::string &body, const TextFormat &fmt)
{
    if (!fmt.map_tabs) return body;
    return replace_all(body, "\t", kTabGlyph);
}

std::string from_edit(const std::string &edit, const TextFormat &fmt)
{
    if (!fmt.map_tabs) return edit;
    return replace_all(edit, kTabGlyph, "\t");
}

std::size_t body_to_edit_offset(const std::string &body, std::size_t offset, const TextFormat &fmt)
{
    if (!fmt.map_tabs) return offset;
    std::size_t tabs = 0;
    const std::size_t end = offset < body.size() ? offset : body.size();
    for (std::size_t i = 0; i < end; ++i)
        if (body[i] == '\t') ++tabs;
    return offset + tabs * (kGlyphLen - 1);
}

} // namespace mdnotes
