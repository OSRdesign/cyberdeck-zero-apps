/*
 * SPDX-License-Identifier: MIT
 *
 * Serial Console: the received text (see term.hpp).
 */
#include "term.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace sc {
namespace {

constexpr std::size_t kCsiMax = 32;   /* bytes of a CSI sequence before it is abandoned */
constexpr std::size_t kStringMax = 512; /* OSC / DCS strings */
constexpr unsigned char ESC = 0x1B, CAN = 0x18, SUB = 0x1A, BEL = 0x07;

void append_utf8(std::string &out, char32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string to_utf8(const std::u32string &text)
{
    std::string out;
    out.reserve(text.size());
    for (const char32_t cp : text) append_utf8(out, cp);
    return out;
}

/* Bytes of the UTF-8 sequence that starts with c (1 for anything that is not a valid lead byte). */
std::size_t utf8_seq_len(unsigned char c)
{
    if (c >= 0xF0 && c <= 0xF4) return 4;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    return 1;
}

/* The display rows of a line of n code points. */
int rows_for(std::size_t n, int cols)
{
    if (cols <= 0 || n == 0) return 1;
    return static_cast<int>((n + static_cast<std::size_t>(cols) - 1) / static_cast<std::size_t>(cols));
}

int line_rows(const TermBuffer &buf, std::uint64_t index, int cols)
{
    return rows_for(utf8_count(buf.line(index)), cols);
}

bool after(const ViewPos &a, const ViewPos &b)
{
    return a.line > b.line || (a.line == b.line && a.row > b.row);
}

/* The top of the bottom view. */
ViewPos bottom_top(const TermBuffer &buf, int cols, int rows)
{
    ViewPos top{buf.end() - 1, 0};
    int need = std::max(1, rows);
    for (std::uint64_t i = buf.end(); i-- > buf.first();) {
        const int n = line_rows(buf, i, cols);
        if (n >= need) {
            top = {i, n - need};
            return top;
        }
        need -= n;
        top = {i, 0};
    }
    return top;
}

void clamp_top(const TermBuffer &buf, int cols, int rows, ViewPos *top)
{
    if (top->line < buf.first()) *top = {buf.first(), 0};
    const ViewPos bottom = bottom_top(buf, cols, rows);
    if (top->line >= buf.end() || after(*top, bottom)) {
        *top = bottom;
        return;
    }
    const int n = line_rows(buf, top->line, cols);
    if (top->row >= n) top->row = n - 1;
    if (top->row < 0) top->row = 0;
}

} // namespace

TermBuffer::TermBuffer(std::size_t max_lines, std::size_t max_cols)
    : max_lines_(std::max<std::size_t>(1, max_lines)), max_cols_(std::max<std::size_t>(8, max_cols))
{
}

void TermBuffer::clear()
{
    first_ += done_.size() + 1;
    done_.clear();
    cur_.clear();
    col_ = 0;
    state_ = State::Text;
    utf8_need_ = utf8_have_ = 0;
    ++version_;
}

std::string TermBuffer::line(std::uint64_t index) const
{
    if (index < first_ || index >= end()) return std::string();
    if (index == end() - 1) return to_utf8(cur_);
    return done_[static_cast<std::size_t>(index - first_)];
}

void TermBuffer::put(char32_t cp)
{
    if (col_ >= max_cols_) newline();
    if (col_ > cur_.size()) cur_.resize(col_, U' ');
    if (col_ == cur_.size()) cur_.push_back(cp);
    else cur_[col_] = cp;
    ++col_;
    ++version_;
}

void TermBuffer::newline()
{
    done_.push_back(to_utf8(cur_));
    cur_.clear();
    col_ = 0;
    while (done_.size() > max_lines_) {
        done_.pop_front();
        ++first_;
    }
    ++version_;
}

void TermBuffer::control(unsigned char c)
{
    switch (c) {
    case '\n':
    case 0x0B:
    case 0x0C: newline(); break;
    case '\r': col_ = 0; break;
    case '\b':
        if (col_ > 0) --col_;
        break;
    case '\t': col_ = std::min(col_ + (8 - col_ % 8), max_cols_ - 1); break;
    case BEL: ++bells_; break;
    case ESC:
        state_ = State::Esc;
        seq_len_ = 0;
        break;
    case 0x00:
    case 0x0E:
    case 0x0F:
    case 0x7F: break; /* NUL, shift out / in, DEL: nothing to show */
    default: bad_byte(); break;
    }
}

void TermBuffer::csi_final(unsigned char final)
{
    if (!seq_.empty() && (seq_[0] < '0' || seq_[0] > ';')) return; /* a private sequence (ESC [ ? 25 l ...) */
    std::vector<int> params;
    std::string part;
    for (std::size_t i = 0; i <= seq_.size(); ++i) {
        if (i == seq_.size() || seq_[i] == ';') {
            params.push_back(part.empty() ? -1 : std::atoi(part.c_str()));
            part.clear();
        } else if (seq_[i] >= '0' && seq_[i] <= '9') {
            part += seq_[i];
        }
    }
    const int p0 = params.empty() ? -1 : params[0];
    const std::size_t n = static_cast<std::size_t>(p0 > 0 ? p0 : 1);
    switch (final) {
    case 'K':
        if (p0 <= 0) {
            if (col_ < cur_.size()) cur_.resize(col_);
        } else if (p0 == 1) {
            for (std::size_t i = 0; i < cur_.size() && i <= col_; ++i) cur_[i] = U' ';
        } else if (p0 == 2) {
            cur_.clear();
        }
        ++version_;
        break;
    case 'C': col_ = std::min(col_ + n, max_cols_ - 1); break;
    case 'D': col_ -= std::min(n, col_); break;
    case 'G': col_ = std::min(n - 1, max_cols_ - 1); break;
    case 'P':
        if (col_ < cur_.size()) cur_.erase(col_, std::min(n, cur_.size() - col_));
        ++version_;
        break;
    case '@':
        if (col_ < cur_.size()) {
            cur_.insert(col_, std::min(n, max_cols_), U' ');
            if (cur_.size() > max_cols_) cur_.resize(max_cols_);
        }
        ++version_;
        break;
    default: break; /* colours, modes, cursor addressing: dropped */
    }
}

void TermBuffer::text_byte(unsigned char c)
{
    if (utf8_need_ > 0) {
        if ((c & 0xC0) == 0x80) {
            utf8_bytes_[utf8_have_++] = c;
            utf8_cp_ = (utf8_cp_ << 6) | (c & 0x3F);
            if (utf8_have_ < utf8_need_) return;
            const int len = utf8_need_;
            utf8_need_ = 0;
            const char32_t cp = utf8_cp_;
            const bool valid = (len == 2 && cp >= 0x80) || (len == 3 && cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) ||
                               (len == 4 && cp >= 0x10000 && cp <= 0x10FFFF);
            if (!valid || (cp >= 0x80 && cp < 0xA0)) { /* overlong, surrogate, out of range, C1 control */
                for (int i = 0; i < len; ++i) bad_byte();
                return;
            }
            put(cp);
            return;
        }
        for (int i = 0; i < utf8_have_; ++i) bad_byte(); /* a cut sequence: its bytes are dots */
        utf8_need_ = 0;
    }
    if (c < 0x20 || c == 0x7F) {
        control(c);
        return;
    }
    if (c < 0x80) {
        put(c);
        return;
    }
    const std::size_t len = utf8_seq_len(c);
    if (len == 1) {
        bad_byte();
        return;
    }
    utf8_need_ = static_cast<int>(len);
    utf8_have_ = 1;
    utf8_bytes_[0] = c;
    utf8_cp_ = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
}

void TermBuffer::feed(const char *data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        ++seq_len_;
        switch (state_) {
        case State::Text: text_byte(c); break;
        case State::Esc:
            if (c == '[') {
                state_ = State::Csi;
                seq_.clear();
            } else if (c == ']') {
                state_ = State::Osc;
            } else if (c == 'P' || c == 'X' || c == '^' || c == '_') {
                state_ = State::Str;
            } else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '-' || c == '.' || c == '/' || c == '#' ||
                       c == '%') {
                state_ = State::Charset;
            } else if (c == ESC) {
                seq_len_ = 0;
            } else {
                state_ = State::Text; /* a two-byte sequence (ESC 7, ESC M, ESC =, CAN, SUB...): dropped */
            }
            break;
        case State::Csi:
            if (c >= 0x40 && c <= 0x7E) {
                state_ = State::Text;
                csi_final(c);
            } else if (c >= 0x30 && c <= 0x3F) {
                seq_ += static_cast<char>(c);
            } else if (c >= 0x20 && c <= 0x2F) {
                /* intermediate bytes: nothing applied uses them */
            } else if (c == ESC) {
                state_ = State::Esc;
                seq_len_ = 0;
            } else if (c == CAN || c == SUB) {
                state_ = State::Text;
            } else if (c < 0x20) {
                control(c); /* a control inside a sequence acts at once (VT100) */
            } else {
                state_ = State::Text; /* not a sequence after all (noise): the byte is text */
                text_byte(c);
            }
            if (state_ == State::Csi && seq_len_ > kCsiMax) state_ = State::Text;
            break;
        case State::Osc:
            if (c == BEL || c == CAN || c == SUB) state_ = State::Text;
            else if (c == ESC) state_ = State::OscEsc;
            else if (seq_len_ > kStringMax) state_ = State::Text;
            break;
        case State::Str:
            if (c == CAN || c == SUB) state_ = State::Text;
            else if (c == ESC) state_ = State::StrEsc;
            else if (seq_len_ > kStringMax) state_ = State::Text;
            break;
        case State::OscEsc:
        case State::StrEsc:
            if (c == '\\') {
                state_ = State::Text;
            } else { /* not a string terminator: a new escape sequence starts */
                state_ = State::Esc;
                seq_len_ = 0;
                --i;
            }
            break;
        case State::Charset: state_ = State::Text; break;
        }
    }
}

std::size_t utf8_count(const std::string &utf8)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        std::size_t n = utf8_seq_len(static_cast<unsigned char>(utf8[i]));
        if (i + n > utf8.size()) n = 1;
        i += n;
        ++count;
    }
    return count;
}

std::vector<std::string> wrap_line(const std::string &utf8, int cols)
{
    std::vector<std::string> rows;
    if (cols <= 0) cols = 1;
    std::string row;
    int in_row = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        std::size_t n = utf8_seq_len(static_cast<unsigned char>(utf8[i]));
        if (i + n > utf8.size()) n = 1;
        if (in_row == cols) {
            rows.push_back(row);
            row.clear();
            in_row = 0;
        }
        row.append(utf8, i, n);
        ++in_row;
        i += n;
    }
    rows.push_back(row);
    return rows;
}

std::vector<std::string> view_bottom(const TermBuffer &buf, int cols, int rows, ViewPos *top)
{
    std::vector<std::string> out;
    ViewPos first{buf.end() - 1, 0};
    for (std::uint64_t i = buf.end(); i-- > buf.first() && static_cast<int>(out.size()) < rows;) {
        const std::vector<std::string> w = wrap_line(buf.line(i), cols);
        for (int k = static_cast<int>(w.size()) - 1; k >= 0 && static_cast<int>(out.size()) < rows; --k) {
            out.push_back(w[static_cast<std::size_t>(k)]);
            first = {i, k};
        }
    }
    std::reverse(out.begin(), out.end());
    if (top) *top = first;
    return out;
}

std::vector<std::string> view_at(const TermBuffer &buf, int cols, int rows, ViewPos *top)
{
    clamp_top(buf, cols, rows, top);
    std::vector<std::string> out;
    for (std::uint64_t i = top->line; i < buf.end() && static_cast<int>(out.size()) < rows; ++i) {
        const std::vector<std::string> w = wrap_line(buf.line(i), cols);
        for (std::size_t k = i == top->line ? static_cast<std::size_t>(top->row) : 0;
             k < w.size() && static_cast<int>(out.size()) < rows; ++k)
            out.push_back(w[k]);
    }
    return out;
}

bool is_bottom(const TermBuffer &buf, int cols, int rows, const ViewPos &top)
{
    ViewPos t = top;
    clamp_top(buf, cols, rows, &t);
    return t == bottom_top(buf, cols, rows);
}

bool scroll_view(const TermBuffer &buf, int cols, int rows, ViewPos *top, int delta)
{
    clamp_top(buf, cols, rows, top);
    const ViewPos bottom = bottom_top(buf, cols, rows);
    while (delta > 0 && after(bottom, *top)) {
        if (top->row + 1 < line_rows(buf, top->line, cols)) ++top->row;
        else *top = {top->line + 1, 0};
        --delta;
    }
    while (delta < 0 && (top->line > buf.first() || top->row > 0)) {
        if (top->row > 0) --top->row;
        else *top = {top->line - 1, line_rows(buf, top->line - 1, cols) - 1};
        ++delta;
    }
    return *top == bottom;
}

std::string format_bytes(std::uint64_t bytes)
{
    char buf[32];
    if (bytes < 1000) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1000000) std::snprintf(buf, sizeof(buf), "%.1f kB", static_cast<double>(bytes) / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / 1000000.0);
    return buf;
}

} // namespace sc
