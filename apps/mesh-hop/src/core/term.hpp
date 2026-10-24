/*
 * SPDX-License-Identifier: MIT
 *
 * Mesh Hop's CLI page (report 033, P5): the output of meshcore-cli as lines. A copy of Serial Console's
 * apps/serial-console/src/core/term.hpp (same code, namespace meshzero::term instead of sc): a copy, not a lift to
 * apps/_common, because every file of _common is compiled into every app and Serial Console stays untouched.
 * Tested by tests/unit/test_cli.inc (and Serial Console's tests/test_term.cpp on the original).
 *
 * TermBuffer turns the bytes of the port into lines for a plain text view. It is not a terminal emulator: no screen,
 * no colours, no cursor addressing. It keeps what a line-oriented console needs:
 *   LF (and VT, FF)  a new line          CR   back to the start of the line (the next text overwrites it)
 *   BS               one column back      TAB  spaces to the next multiple of 8
 *   BEL              counted (bells())    NUL, SO, SI, DEL  ignored
 *   ESC sequences    CSI (ESC [ ... final byte): K (erase in line), C / D (cursor right / left), G (column), P (delete
 *                    characters) and @ (insert blanks) are applied, every other one (colours "m", modes, cursor
 *                    moves) is dropped; OSC (ESC ] ... BEL or ESC \), DCS / SOS / PM / APC strings (up to ESC \),
 *                    charset choices (ESC ( B) and two-byte ESC sequences are dropped. An unfinished sequence longer
 *                    than a sane maximum is abandoned, so garbage never hides the text that follows for long.
 *   other C0 bytes and bytes that are not UTF-8   shown as "." (a wrong speed gives a line of dots, never garbage
 *                    glyphs)
 * A line longer than max_cols is cut there into a new line. The newest max_lines finished lines are kept (a ring);
 * lines have absolute numbers (first() .. end()), so a view that is scrolled back keeps its place while lines arrive
 * and old ones go.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace meshzero {
namespace term {

class TermBuffer {
public:
    explicit TermBuffer(std::size_t max_lines = 2000, std::size_t max_cols = 4096);

    void feed(const char *data, std::size_t size);
    void feed(const std::string &bytes) { feed(bytes.data(), bytes.size()); }
    void clear();

    /* Absolute numbers of the lines kept: first() .. end() - 1; the last one is the line being received (it may be
     * empty: the cursor is on a new line). */
    std::uint64_t first() const { return first_; }
    std::uint64_t end() const { return first_ + done_.size() + 1; }
    /* The line's text, UTF-8 ("" outside first() .. end() - 1). */
    std::string line(std::uint64_t index) const;

    std::uint64_t version() const { return version_; } /* changes whenever the text changes */
    std::uint64_t bells() const { return bells_; }
    std::size_t max_lines() const { return max_lines_; }

private:
    enum class State { Text, Esc, Csi, Osc, OscEsc, Str, StrEsc, Charset };

    void put(char32_t cp);
    void newline();
    void control(unsigned char c);
    void csi_final(unsigned char final);
    void bad_byte() { put(U'.'); }
    void text_byte(unsigned char c);

    std::size_t max_lines_, max_cols_;
    std::deque<std::string> done_;
    std::u32string cur_;
    std::size_t col_ = 0;
    std::uint64_t first_ = 0;
    std::uint64_t version_ = 0;
    std::uint64_t bells_ = 0;

    State state_ = State::Text;
    std::string seq_;           /* the CSI parameters collected so far */
    std::size_t seq_len_ = 0;   /* bytes of the current sequence (the abandon limit) */
    char32_t utf8_cp_ = 0;      /* a UTF-8 sequence being decoded */
    int utf8_need_ = 0, utf8_have_ = 0;
    unsigned char utf8_bytes_[4] = {};
};

/* ---------------------------------------------------------------- the view: lines cut into rows of `cols` */

/* A line cut into display rows of at most cols code points (an empty line is one empty row). */
std::vector<std::string> wrap_line(const std::string &utf8, int cols);
/* Code points of a UTF-8 string (a malformed byte counts as one). */
std::size_t utf8_count(const std::string &utf8);

/* The top of a view: a line and a display row of it. */
struct ViewPos {
    std::uint64_t line = 0;
    int row = 0;
    bool operator==(const ViewPos &o) const { return line == o.line && row == o.row; }
};

/* The last `rows` display rows (the view that follows new text); *top gets the position of its first row. */
std::vector<std::string> view_bottom(const TermBuffer &buf, int cols, int rows, ViewPos *top);
/* `rows` display rows from *top; *top is first clamped into the text (not before the oldest line, not past the
 * bottom view). Returns fewer rows only when the whole text is shorter. */
std::vector<std::string> view_at(const TermBuffer &buf, int cols, int rows, ViewPos *top);
/* *top moved by delta display rows (> 0 towards the newest text), clamped like view_at. Returns true when the result
 * is the bottom view (the caller follows the text again). */
bool scroll_view(const TermBuffer &buf, int cols, int rows, ViewPos *top, int delta);
bool is_bottom(const TermBuffer &buf, int cols, int rows, const ViewPos &top);

/* "512 B", "12.3 kB", "4.56 MB" */
std::string format_bytes(std::uint64_t bytes);

} // namespace term
} // namespace meshzero
