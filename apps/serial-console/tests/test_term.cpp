/*
 * SPDX-License-Identifier: MIT
 *
 * Tests of the byte filter, the ring of lines and the text view (core/term).
 */
#include "check.hpp"

#include "term.hpp"

using namespace sc;

namespace {

/* The finished lines of a feed, joined by '|' (the line being received is left out). */
std::string lines_of(const std::string &bytes, std::size_t max_lines = 2000, std::size_t max_cols = 4096)
{
    TermBuffer b(max_lines, max_cols);
    b.feed(bytes);
    std::string out;
    for (std::uint64_t i = b.first(); i + 1 < b.end(); ++i) out += (out.empty() ? "" : "|") + b.line(i);
    return out;
}

bool valid_utf8_no_controls(const std::string &s)
{
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) return false;
        std::size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

} // namespace

void test_term()
{
    // line ends and the simple controls
    CHECK_EQ(lines_of("hello\r\nworld\r\n"), std::string("hello|world"));
    CHECK_EQ(lines_of("one\ntwo\n"), std::string("one|two"));
    CHECK_EQ(lines_of("a\x0b" "b\x0c" "c\n"), std::string("a|b|c"));
    CHECK_EQ(lines_of("Loading 10%\rLoading 55%\rLoading 100%\r\n"), std::string("Loading 100%"));
    CHECK_EQ(lines_of("abcdef\r12\n"), std::string("12cdef"));
    CHECK_EQ(lines_of("abc\bX\n"), std::string("abX"));
    CHECK_EQ(lines_of("\b\bab\n"), std::string("ab"));
    CHECK_EQ(lines_of("a\tb\n"), std::string("a       b"));
    CHECK_EQ(lines_of("12345678\tx\n"), std::string("12345678        x"));
    {
        TermBuffer b;
        b.feed("x\ay\a\n");
        CHECK_EQ(b.line(b.first()), std::string("xy"));
        CHECK_EQ(b.bells(), static_cast<std::uint64_t>(2));
    }
    CHECK_EQ(lines_of(std::string("a\x00" "b\x7f" "c\x0e\x0f\n", 8)), std::string("abc"));
    CHECK_EQ(lines_of("\x01\x02" "a\x1c\n"), std::string("..a."));

    // escape sequences: colours, titles, modes, charsets are dropped; the line editing ones applied
    CHECK_EQ(lines_of("\x1b[0;32mI (29) boot\x1b[0m\n"), std::string("I (29) boot"));
    CHECK_EQ(lines_of("\x1b[1;31;40mred\x1b[m\n"), std::string("red"));
    CHECK_EQ(lines_of("\x1b]0;title\x07" "after\n"), std::string("after"));
    CHECK_EQ(lines_of("\x1b]2;t\x1b\\x\n"), std::string("x"));
    CHECK_EQ(lines_of("\x1bP1$r0m\x1b\\y\n"), std::string("y"));
    CHECK_EQ(lines_of("\x1b[?25lok\x1b[?25h\n"), std::string("ok"));
    CHECK_EQ(lines_of("\x1b(Bok\x1b)0\n"), std::string("ok"));
    CHECK_EQ(lines_of("\x1b" "7ok\x1b" "8\x1b=\x1bM\n"), std::string("ok"));
    CHECK_EQ(lines_of("\x1b[2J\x1b[H\x1b[10;20Hhi\n"), std::string("hi"));
    CHECK_EQ(lines_of("hello world\r\x1b[Khi\n"), std::string("hi"));
    CHECK_EQ(lines_of("abcdef\x1b[3D\x1b[K\n"), std::string("abc"));
    CHECK_EQ(lines_of("abcdef\r\x1b[2C\x1b[2P\n"), std::string("abef"));
    CHECK_EQ(lines_of("abcdef\r\x1b[2C\x1b[2@\n"), std::string("ab  cdef"));
    CHECK_EQ(lines_of("abcdef\x1b[1K\n"), std::string("      "));
    CHECK_EQ(lines_of("abc\x1b[2Kx\n"), std::string("   x"));
    CHECK_EQ(lines_of("abc\x1b[1Gz\n"), std::string("zbc"));
    CHECK_EQ(lines_of("ab\x1b[5Cc\n"), std::string("ab     c"));
    CHECK_EQ(lines_of("\x1b[0;3"), std::string(""));
    {
        TermBuffer b; /* a sequence cut between two reads */
        b.feed("\x1b[0;3");
        b.feed("2mhi\n");
        b.feed("\xc3");
        b.feed("\xa9\n");
        CHECK_EQ(b.line(b.first()), std::string("hi"));
        CHECK_EQ(b.line(b.first() + 1), std::string("\xc3\xa9"));
    }
    // an endless sequence is abandoned: the text after it shows
    CHECK(lines_of("\x1b[" + std::string(100, '1') + "\nvisible\n").find("visible") != std::string::npos);
    CHECK(lines_of("\x1b]" + std::string(600, 'a') + "\nvisible\n").find("visible") != std::string::npos);
    CHECK(lines_of("\x1bP" + std::string(600, 'a') + "\nvisible\n").find("visible") != std::string::npos);

    // UTF-8 kept, everything else a dot
    CHECK_EQ(lines_of("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\n"), std::string("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"));
    CHECK_EQ(lines_of("caf\xe9!\n"), std::string("caf.!"));
    CHECK_EQ(lines_of("\xe2\x82X\n"), std::string("..X"));
    CHECK_EQ(lines_of("\xc0\xaf\n"), std::string(".."));
    CHECK_EQ(lines_of("\xed\xa0\x80\n"), std::string("..."));
    CHECK_EQ(lines_of("\xc2\x85\n"), std::string(".."));
    CHECK_EQ(lines_of("\xff\xfe\x80\n"), std::string("..."));

    // noise never leaves a control byte or broken UTF-8 in the text
    {
        TermBuffer b(100, 80);
        std::uint32_t seed = 7;
        std::string noise;
        for (int i = 0; i < 20000; ++i) {
            seed = seed * 1103515245u + 12345u;
            noise += static_cast<char>((seed >> 16) & 0xFF);
        }
        b.feed(noise);
        bool clean = true;
        for (std::uint64_t i = b.first(); i < b.end(); ++i) clean = clean && valid_utf8_no_controls(b.line(i));
        CHECK(clean);
    }

    // the ring keeps the newest lines, with absolute numbers
    {
        TermBuffer b(5, 4096);
        for (int i = 0; i < 10; ++i) b.feed("line " + std::to_string(i) + "\n");
        CHECK_EQ(b.first(), static_cast<std::uint64_t>(5));
        CHECK_EQ(b.end() - b.first(), static_cast<std::uint64_t>(6)); /* 5 finished + the current one */
        CHECK_EQ(b.line(5), std::string("line 5"));
        CHECK_EQ(b.line(9), std::string("line 9"));
        CHECK_EQ(b.line(10), std::string(""));
        CHECK_EQ(b.line(2), std::string("")); /* gone */
        const std::uint64_t v = b.version();
        b.feed("x");
        CHECK(b.version() != v);
        CHECK_EQ(b.line(10), std::string("x"));
        b.clear();
        CHECK_EQ(b.end() - b.first(), static_cast<std::uint64_t>(1));
        CHECK(b.first() > 10);
        CHECK_EQ(b.line(b.first()), std::string(""));
    }
    // a line longer than max_cols goes on in a new line
    CHECK_EQ(lines_of(std::string(25, 'x') + "\n", 100, 10), std::string("xxxxxxxxxx|xxxxxxxxxx|xxxxx"));

    // wrapping
    CHECK(wrap_line("abcdefgh", 3) == (std::vector<std::string>{"abc", "def", "gh"}));
    CHECK(wrap_line("", 5) == (std::vector<std::string>{""}));
    CHECK(wrap_line("abc", 3) == (std::vector<std::string>{"abc"}));
    CHECK(wrap_line("\xc3\xa9\xc3\xa9\xc3\xa9", 2) == (std::vector<std::string>{"\xc3\xa9\xc3\xa9", "\xc3\xa9"}));
    CHECK_EQ(utf8_count("a\xc3\xa9\xe2\x82\xac"), static_cast<std::size_t>(3));

    // the view: the bottom rows, scrolling back, an anchored view while text arrives
    {
        TermBuffer b;
        for (int i = 0; i < 10; ++i) b.feed("L" + std::to_string(i) + "\n");
        ViewPos top;
        CHECK(view_bottom(b, 80, 3, &top) == (std::vector<std::string>{"L8", "L9", ""}));
        CHECK(top == (ViewPos{8, 0}));
        CHECK(is_bottom(b, 80, 3, top));
        CHECK(!scroll_view(b, 80, 3, &top, -2));
        CHECK(top == (ViewPos{6, 0}));
        CHECK(view_at(b, 80, 3, &top) == (std::vector<std::string>{"L6", "L7", "L8"}));
        b.feed("L10\nL11\n"); /* new text does not move a view that is scrolled back */
        CHECK(view_at(b, 80, 3, &top) == (std::vector<std::string>{"L6", "L7", "L8"}));
        CHECK(!is_bottom(b, 80, 3, top));
        CHECK(scroll_view(b, 80, 3, &top, 100)); /* back at the bottom: follow again */
        CHECK(view_at(b, 80, 3, &top) == (std::vector<std::string>{"L10", "L11", ""}));
        CHECK(!scroll_view(b, 80, 3, &top, -100));
        CHECK(top == (ViewPos{0, 0}));
        ViewPos past{500, 0};
        CHECK(view_at(b, 80, 3, &past) == (std::vector<std::string>{"L10", "L11", ""}));
    }
    {
        TermBuffer b;
        b.feed("abcdefghij\nk");
        ViewPos top;
        CHECK(view_bottom(b, 4, 3, &top) == (std::vector<std::string>{"efgh", "ij", "k"}));
        CHECK(top == (ViewPos{0, 1}));
        CHECK(!scroll_view(b, 4, 3, &top, -1));
        CHECK(view_at(b, 4, 3, &top) == (std::vector<std::string>{"abcd", "efgh", "ij"}));
        CHECK(scroll_view(b, 4, 3, &top, 1));
        ViewPos big{0, 99};
        CHECK(view_at(b, 4, 3, &big) == (std::vector<std::string>{"efgh", "ij", "k"}));
        CHECK(view_bottom(b, 4, 10, &top).size() == 4); /* shorter than the view */
    }
    {
        TermBuffer b(3, 4096); /* a view scrolled back to lines that left the ring goes to the oldest kept */
        for (int i = 0; i < 3; ++i) b.feed("L" + std::to_string(i) + "\n");
        ViewPos top{0, 0};
        for (int i = 3; i < 8; ++i) b.feed("L" + std::to_string(i) + "\n");
        CHECK(view_at(b, 80, 2, &top) == (std::vector<std::string>{"L5", "L6"}));
    }

    CHECK_EQ(format_bytes(512), std::string("512 B"));
    CHECK_EQ(format_bytes(1234), std::string("1.2 kB"));
    CHECK_EQ(format_bytes(4560000), std::string("4.56 MB"));
}
