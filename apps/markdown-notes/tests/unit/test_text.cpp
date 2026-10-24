// Host tests: UTF-8 helpers, the text codec round trips, task toggles, dates.
#include "test_common.hpp"

#include "dates.hpp"
#include "tasks.hpp"
#include "text_codec.hpp"
#include "utf8.hpp"

using namespace mdnotes;

namespace {
constexpr std::time_t kNow = 1792339200;   // 2026-10-18 16:00 UTC (the tests run with TZ=UTC)
const std::string kGlyph = "\xE2\x87\xA5";
} // namespace

void test_utf8()
{
    CHECK(utf8_valid("h\xC3\xA9llo \xF0\x9F\x98\x80"));
    CHECK(utf8_valid(""));
    CHECK(!utf8_valid("\xC3"));                       // truncated
    CHECK(!utf8_valid("\xC0\x80"));                   // overlong NUL
    CHECK(!utf8_valid("\xE0\x80\xAF"));               // overlong
    CHECK(!utf8_valid("\xED\xA0\x80"));               // surrogate
    CHECK(!utf8_valid("\xF4\x90\x80\x80"));           // above U+10FFFF
    CHECK(!utf8_valid("a\xFF" "b"));
    CHECK_EQ(utf8_length("\xC3\xA9\xF0\x9F\x98\x80" "a"), std::size_t(3));
    std::string s;
    utf8_append(s, 0x20AC);
    CHECK_EQ(s, std::string("\xE2\x82\xAC"));
    std::size_t i = 0;
    CHECK_EQ(utf8_next(std::string("\xFF" "a"), i), 0xFFFDu);
    CHECK_EQ(i, std::size_t(1));
    // folding: ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic; others unchanged
    CHECK_EQ(fold_case("ABC \xC3\x80\xC3\x89\xC3\x8E \xC5\x92\xC5\xB8 \xCE\xA9\xCE\x9C \xD0\x94\xD0\x96 \xC3\x97 \xC3\x9F"),
             std::string("abc \xC3\xA0\xC3\xA9\xC3\xAE \xC5\x93\xC3\xBF \xCF\x89\xCE\xBC \xD0\xB4\xD0\xB6 \xC3\x97 \xC3\x9F"));
    CHECK(equal_ci("Caf\xC3\xA9", "CAF\xC3\x89"));
    CHECK(!equal_ci("Cafe", "Caf\xC3\xA9"));
    CHECK_EQ(fold_case("\xD0\x81"), std::string("\xD1\x91"));   // Ё -> ё
    CHECK_EQ(fold_case("a\xFF" "B"), std::string("a\xFF" "b"));   // a malformed byte stays
}

void test_codec_roundtrip()
{
    const std::vector<std::string> files = {
        "",
        "# Title\n\nSome text.\n",
        "no final newline",
        "a\r\nb\r\n",                                             // CRLF
        "\xEF\xBB\xBF" "a\r\nb",                                  // BOM + CRLF, no final newline
        "\xEF\xBB\xBF" "# BOM and LF\n",
        "\xEF\xBB\xBF",                                            // a BOM alone
        "- a\n\t- b\n\t\t- c\n",                                  // tabs
        "\xC3\x9C" "n\xC3\xAF" "c\xC3\xB8" "d\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80\n",   // non-ASCII
        "tab\tand glyph " + kGlyph + " both\n",                   // the glyph already there: tabs not mapped
        "lone\rcr\n",                                             // a lone CR stays
        "a\r\r\nb\r\n",                                           // a CR before a CRLF
        "\xEF\xBB\xBF" "\t\xC3\xA9\r\n- [ ] x\r\n",              // everything at once
    };
    for (const std::string &raw : files) {
        const Decoded d = decode_text(raw);
        CHECK(encode_text(d.text, d.fmt) == raw);
        const std::string edit = to_edit(d.text, d.fmt);
        CHECK(from_edit(edit, d.fmt) == d.text);
        CHECK(encode_text(from_edit(edit, d.fmt), d.fmt) == raw);
        CHECK(d.text.find("\r\n") == std::string::npos || raw.find("\r\r\n") != std::string::npos);
        if (d.fmt.map_tabs) CHECK(edit.find('\t') == std::string::npos);
    }
    Decoded d = decode_text("a\r\nb\r\n");
    CHECK(d.fmt.eol == Eol::Crlf && d.fmt.write_crlf && !d.fmt.bom);
    CHECK_EQ(d.text, std::string("a\nb\n"));
    d = decode_text("\xEF\xBB\xBF" "x\n");
    CHECK(d.fmt.bom && d.fmt.eol == Eol::Lf && d.text == "x\n");
    d = decode_text("- a\n\t- b\n");
    CHECK(d.fmt.map_tabs);
    CHECK_EQ(to_edit(d.text, d.fmt), std::string("- a\n") + kGlyph + "- b\n");
    d = decode_text("tab\tand glyph " + kGlyph + "\n");
    CHECK(!d.fmt.map_tabs);
    CHECK(to_edit(d.text, d.fmt) == d.text);
    // mixed line ends: LF body, written back with the majority (only after an edit: the app never rewrites untouched)
    d = decode_text("a\r\nb\nc\r\n");
    CHECK(d.fmt.eol == Eol::Mixed && d.fmt.write_crlf);
    CHECK_EQ(d.text, std::string("a\nb\nc\n"));
    CHECK_EQ(encode_text(d.text, d.fmt), std::string("a\r\nb\r\nc\r\n"));
    d = decode_text("a\nb\nc\r\n");
    CHECK(d.fmt.eol == Eol::Mixed && !d.fmt.write_crlf);
    CHECK_EQ(encode_text(d.text, d.fmt), std::string("a\nb\nc\n"));
    // an edit keeps the format: a new line typed in a CRLF file gets CRLF
    d = decode_text("\xEF\xBB\xBF" "one\r\ntwo\r\n");
    std::string edit = to_edit(d.text, d.fmt);
    edit += "three\n";
    CHECK_EQ(encode_text(from_edit(edit, d.fmt), d.fmt), std::string("\xEF\xBB\xBF" "one\r\ntwo\r\nthree\r\n"));
    // offsets: body -> edit text
    d = decode_text("\ta\tb");
    CHECK_EQ(body_to_edit_offset(d.text, 3, d.fmt), std::size_t(7));
    CHECK_EQ(to_edit(d.text, d.fmt).substr(7, 1), std::string("b"));
    CHECK_EQ(body_to_edit_offset(d.text, 0, d.fmt), std::size_t(0));
}

void test_tasks()
{
    std::string t = "- [ ] buy milk\n- [x] done\n* [X] caps\n";
    const std::string orig = t;
    CHECK(toggle_task_at(t, 3));
    CHECK_EQ(t, std::string("- [x] buy milk\n- [x] done\n* [X] caps\n"));
    CHECK(toggle_task_at(t, 3));
    CHECK(t == orig);
    CHECK(!toggle_task_at(t, 0));
    CHECK(!toggle_task_at(t, 6));            // 'b' is not a mark
    CHECK(!toggle_task_at(t, t.size() - 1));
    CHECK(t == orig);
    std::size_t mark = 0;
    CHECK(toggle_task_on_line(t, 20, &mark));   // the cursor somewhere in line 2
    CHECK_EQ(mark, std::size_t(18));
    CHECK_EQ(t, std::string("- [ ] buy milk\n- [ ] done\n* [X] caps\n"));
    CHECK(toggle_task_on_line(t, t.size() - 1));   // on the last line's newline: still line 3
    CHECK_EQ(t.substr(t.size() - 11), std::string("* [ ] caps\n"));
    // other shapes of a task item
    const struct {
        std::string line;
        bool task;
        std::string after;
    } cases[] = {
        {"\t- [ ] tab indent", true, "\t- [x] tab indent"},
        {kGlyph + "- [ ] glyph indent", true, kGlyph + "- [x] glyph indent"},
        {"    + [ ] plus", true, "    + [x] plus"},
        {"> - [ ] quoted", true, "> - [x] quoted"},
        {"> > 1. [x] nested quote", true, "> > 1. [ ] nested quote"},
        {"12) [ ] paren", true, "12) [x] paren"},
        {"- [ ]", true, "- [x]"},
        {"- [ ]\ttab after", true, "- [x]\ttab after"},
        {"-[ ] no space", false, ""},
        {"- [ ]x glued", false, ""},
        {"- [-] other mark", false, ""},
        {"text [ ] not an item", false, ""},
        {"1.[ ] no space", false, ""},
        {"- plain item", false, ""},
        {"", false, ""},
    };
    for (const auto &c : cases) {
        std::string s = "first\n" + c.line + "\nlast";
        const bool r = toggle_task_on_line(s, 6);
        CHECK(r == c.task);
        if (c.task) CHECK(s == "first\n" + c.after + "\nlast");
        else CHECK(s == "first\n" + c.line + "\nlast");
    }
    CHECK_EQ(task_mark_on_line("- [ ] a", 0), std::size_t(3));
    CHECK(task_mark_on_line("abc", 1) == std::string::npos);
    CHECK(task_mark_on_line("abc", 9) == std::string::npos);
}

void test_dates()
{
    CHECK_EQ(date_text(kNow), std::string("2026-10-18"));
    CHECK_EQ(stamp_text(kNow + 3 * 60), std::string("2026-10-18 16:03"));
    CHECK_EQ(conflict_stamp(kNow - 30 * 60), std::string("2026-10-18 1530"));
    CHECK_EQ(daily_note_file(kNow), std::string("2026-10-18.md"));
    CHECK_EQ(build_year("2026-10-24"), 2026);
    CHECK_EQ(build_year("26-10-24"), 0);
    CHECK_EQ(build_year(nullptr), 0);
    CHECK(clock_looks_unsynced(0, 2026));            // 1970: never synced
    CHECK(!clock_looks_unsynced(kNow, 2026));
    CHECK(clock_looks_unsynced(kNow, 2027));
    CHECK(!clock_looks_unsynced(0, 0));              // no build year: no warning
    CHECK_EQ(age_text(kNow, kNow), std::string("just now"));
    CHECK_EQ(age_text(kNow - 59, kNow), std::string("just now"));
    CHECK_EQ(age_text(kNow - 300, kNow), std::string("5 min ago"));
    CHECK_EQ(age_text(kNow - 2 * 3600 - 10, kNow), std::string("2 h ago"));
    CHECK_EQ(age_text(kNow - 13 * 3600, kNow), std::string("13 h ago"));      // 03:00 the same day
    CHECK_EQ(age_text(kNow - 20 * 3600, kNow), std::string("yesterday"));     // 20:00 the day before
    CHECK_EQ(age_text(kNow - 3 * 86400, kNow), std::string("3 days ago"));
    CHECK_EQ(age_text(kNow - 10 * 86400, kNow), std::string("2026-10-08"));
    CHECK_EQ(age_text(kNow + 3600, kNow), std::string("2026-10-18"));         // the future: the date
    CHECK_EQ(age_text(kNow + 30, kNow), std::string("just now"));
}
