// Host tests: the preview's block model on md4c (blocks, runs, links, task offsets, frontmatter, the cap).
#include "test_common.hpp"

#include "markdown.hpp"
#include "tasks.hpp"
#include "text_codec.hpp"

using namespace mdnotes;

namespace {

const Run *run_with(const Block &b, const std::string &text)
{
    for (const Run &r : b.runs)
        if (r.text == text) return &r;
    return nullptr;
}

/* The task's box as written, from its offset. */
std::string box_at(const std::string &text, std::size_t off)
{
    if (off == kNoOffset || off == 0 || off + 1 >= text.size()) return "?";
    return text.substr(off - 1, 3);
}

int line_of(const std::string &text, std::size_t off)
{
    int n = 0;
    for (std::size_t i = 0; i < off; ++i) n += text[i] == '\n';
    return n;
}

} // namespace

void test_markdown_blocks()
{
    const std::string text =
        "# Title\n"
        "\n"
        "Para with **bold**, *it*, ~~del~~, `code` and [[Antenna ideas|the mast]] and [web](https://x.y) and https://auto.link.\n"
        "\n"
        "- one\n"
        "- two\n"
        "  - nested\n"
        "\n"
        "3. third\n"
        "4. fourth\n"
        "\n"
        "> quote line\n"
        "> - [ ] quoted task\n"
        "\n"
        "```c\n"
        "int x;\n"
        "\tint y;\n"
        "```\n"
        "\n"
        "    indented code\n"
        "\n"
        "---\n"
        "\n"
        "- [ ] open task\n"
        "- [x] done task\n"
        "- [ ]\n"
        "* [X] star\n";
    const Document d = parse_markdown(text);
    CHECK(d.status == 0 && !d.truncated && d.frontmatter_len == 0);
    CHECK_EQ(d.blocks.size(), std::size_t(16));
    if (d.blocks.size() != 16) return;
    const auto &B = d.blocks;
    CHECK(B[0].kind == BlockKind::Heading && B[0].level == 1 && block_text(B[0]) == "Title");
    CHECK(B[0].src_offset == 2);
    CHECK(B[1].kind == BlockKind::Paragraph && B[1].list == 0 && !B[1].item);
    CHECK_EQ(block_text(B[1]), std::string("Para with bold, it, del, code and the mast and web and https://auto.link."));
    const Run *r = run_with(B[1], "bold");
    CHECK(r && r->style == kStyleBold && r->link < 0);
    r = run_with(B[1], "it");
    CHECK(r && r->style == kStyleItalic);
    r = run_with(B[1], "del");
    CHECK(r && r->style == kStyleStrike);
    r = run_with(B[1], "code");
    CHECK(r && r->style == kStyleCode);
    r = run_with(B[1], "the mast");
    CHECK(r && r->link == 0);
    r = run_with(B[1], "web");
    CHECK(r && r->link == 1);
    r = run_with(B[1], "https://auto.link");
    CHECK(r && r->link == 2);
    CHECK_EQ(B[1].src_offset, text.find("Para"));
    CHECK_EQ(d.links.size(), std::size_t(3));
    if (d.links.size() == 3) {
        CHECK(d.links[0].kind == LinkKind::Wiki && d.links[0].target == "Antenna ideas" && !d.links[0].image);
        CHECK_EQ(d.links[0].offset, text.find("Antenna ideas"));
        CHECK(d.links[1].kind == LinkKind::External && d.links[1].target == "https://x.y");
        CHECK_EQ(d.links[1].offset, text.find("https://x.y"));
        CHECK(d.links[2].kind == LinkKind::External && d.links[2].target == "https://auto.link");
    }
    // lists
    CHECK(B[2].item && !B[2].ordered && B[2].bullet == '-' && B[2].list == 1 && block_text(B[2]) == "one");
    CHECK(B[3].item && block_text(B[3]) == "two" && B[3].list == 1);
    CHECK(B[4].item && block_text(B[4]) == "nested" && B[4].list == 2);
    CHECK(B[5].item && B[5].ordered && B[5].number == 3 && block_text(B[5]) == "third");
    CHECK(B[6].item && B[6].ordered && B[6].number == 4);
    // a quote, and a task inside it
    CHECK(B[7].quote == 1 && B[7].list == 0 && block_text(B[7]) == "quote line");
    CHECK(B[8].quote == 1 && B[8].list == 1 && B[8].item && B[8].task && !B[8].checked && block_text(B[8]) == "quoted task");
    CHECK_EQ(box_at(text, B[8].task_offset), std::string("[ ]"));
    CHECK_EQ(B[8].task_line, line_of(text, text.find("[ ] quoted")));
    // code blocks: fenced (md4c expands an indenting tab to spaces: display only) and indented
    CHECK(B[9].kind == BlockKind::Code && B[9].fenced && B[9].lang == "c");
    CHECK_EQ(B[9].code, std::string("int x;\n    int y;"));
    CHECK(B[10].kind == BlockKind::Code && !B[10].fenced && B[10].lang.empty() && B[10].code == "indented code");
    CHECK(B[11].kind == BlockKind::Rule);
    // tasks: offsets of the boxes in the source
    CHECK(B[12].task && !B[12].checked && block_text(B[12]) == "open task");
    CHECK_EQ(B[12].task_offset, text.find("[ ] open") + 1);
    CHECK(B[13].task && B[13].checked && box_at(text, B[13].task_offset) == "[x]");
    CHECK(B[14].task && !B[14].checked && B[14].item && B[14].runs.empty());   // "- [ ]" with nothing after it
    CHECK_EQ(box_at(text, B[14].task_offset), std::string("[ ]"));
    CHECK(B[15].task && B[15].checked && B[15].bullet == '*' && box_at(text, B[15].task_offset) == "[X]");
    for (const Block &b : B)
        if (b.task) CHECK_EQ(b.task_line, line_of(text, b.task_offset));
    // ticking through the offset, then parsing again
    std::string t2 = text;
    CHECK(toggle_task_at(t2, B[12].task_offset));
    const Document d2 = parse_markdown(t2);
    CHECK(d2.blocks.size() == 16 && d2.blocks[12].checked);
    CHECK(t2.size() == text.size());
}

void test_markdown_inline()
{
    Document d = parse_markdown("line one\nline two  \nline three\\\nfour");
    CHECK(d.blocks.size() == 1 && block_text(d.blocks[0]) == "line one\nline two\nline three\nfour");   // soft and hard breaks
    d = parse_markdown("***both*** and **_mix_**");
    CHECK(d.blocks.size() == 1);
    if (d.blocks.size() == 1) {
        const Run *r = run_with(d.blocks[0], "both");
        CHECK(r && r->style == (kStyleBold | kStyleItalic));
        r = run_with(d.blocks[0], "mix");
        CHECK(r && r->style == (kStyleBold | kStyleItalic));
    }
    d = parse_markdown("a &amp; b &copy; &#233; &#x1F600; &unknown; &#0; &lt;tag&gt;");
    CHECK_EQ(block_text(d.blocks[0]), std::string("a & b \xC2\xA9 \xC3\xA9 \xF0\x9F\x98\x80 &unknown; \xEF\xBF\xBD <tag>"));
    CHECK_EQ(decode_entity("&amp;"), std::string("&"));
    CHECK_EQ(decode_entity("&#X41;"), std::string("A"));
    CHECK_EQ(decode_entity("&#xD800;"), std::string("\xEF\xBF\xBD"));
    CHECK_EQ(decode_entity("&#99999999;"), std::string("\xEF\xBF\xBD"));
    CHECK_EQ(decode_entity("&#;"), std::string("&#;"));
    CHECK_EQ(decode_entity("&nope;"), std::string("&nope;"));
    CHECK_EQ(decode_entity("x"), std::string("x"));
    // raw HTML is text; a table (N2) is a paragraph, one line per row
    d = parse_markdown("<b>x</b> <!-- c -->");
    CHECK(d.blocks.size() == 1 && block_text(d.blocks[0]) == "<b>x</b> <!-- c -->");
    d = parse_markdown("| a | b |\n|---|---|\n| 1 | 2 |\n");
    CHECK(d.blocks.size() == 1 && block_text(d.blocks[0]) == "| a | b |\n|---|---|\n| 1 | 2 |");
    // wikilinks: heading, alias, embeds; images; local note links; www and e-mail autolinks
    const std::string t = "[[Plan#Steps]] [[Plan|alias]] ![[Pic.png]] ![alt](img.png) [o](Other%20note.md) www.ex.com me@ex.com";
    d = parse_markdown(t);
    CHECK_EQ(d.links.size(), std::size_t(7));
    if (d.links.size() == 7) {
        CHECK(d.links[0].kind == LinkKind::Wiki && d.links[0].target == "Plan#Steps" && d.links[0].offset == 2);
        CHECK(d.links[1].kind == LinkKind::Wiki && d.links[1].target == "Plan");
        CHECK(d.links[2].kind == LinkKind::Wiki && d.links[2].image && d.links[2].target == "Pic.png");
        CHECK_EQ(d.links[2].offset, t.find("Pic.png"));
        CHECK(d.links[3].kind == LinkKind::File && d.links[3].image && d.links[3].target == "img.png");
        CHECK(d.links[4].kind == LinkKind::Note && d.links[4].target == "Other%20note.md");
        CHECK(d.links[5].kind == LinkKind::External && d.links[5].target.find("www.ex.com") != std::string::npos);
        CHECK(d.links[6].kind == LinkKind::External);
    }
    CHECK_EQ(block_text(d.blocks[0]), std::string("Plan#Steps alias Pic.png alt o www.ex.com me@ex.com"));   // the '!' of the embed dropped
    const Run *r = run_with(d.blocks[0], "alias");
    CHECK(r && r->link == 1);
    // embeds anywhere: a list item, a paragraph's start, two in a row; never in code; a lone "![[" stays text
    const std::string e = "- ![[a]]\n\n![[b|B]] x ![[c]]![[d]]\n\n`![[code]]` and\n\n    ![[indented]]\n\n![[open\n";
    d = parse_markdown(e);
    CHECK_EQ(d.links.size(), std::size_t(4));
    for (const DocLink &l : d.links) CHECK(l.kind == LinkKind::Wiki && l.image);
    if (d.links.size() == 4) {
        CHECK(d.links[1].target == "b" && d.links[2].target == "c" && d.links[3].target == "d");
        CHECK_EQ(d.links[0].offset, e.find("a]]"));
        CHECK_EQ(d.links[2].offset, e.find("c]]"));
        CHECK_EQ(d.links[3].offset, e.find("d]]"));
    }
    if (d.blocks.size() == 5) {
        CHECK_EQ(block_text(d.blocks[0]), std::string("a"));
        CHECK_EQ(block_text(d.blocks[1]), std::string("B x cd"));
        CHECK_EQ(block_text(d.blocks[2]), std::string("![[code]] and"));
        CHECK(d.blocks[3].kind == BlockKind::Code && d.blocks[3].code == "![[indented]]");
        CHECK_EQ(block_text(d.blocks[4]), std::string("![[open"));
    } else {
        CHECK_EQ(d.blocks.size(), std::size_t(5));
    }
    // NUL bytes, setext and deep headings
    d = parse_markdown(std::string("a\0b", 3));
    CHECK_EQ(block_text(d.blocks[0]), std::string("a\xEF\xBF\xBD" "b"));
    d = parse_markdown("Title\n===\n\n###### six\n#no heading");
    CHECK(d.blocks.size() == 3 && d.blocks[0].level == 1 && d.blocks[1].level == 6 && d.blocks[2].kind == BlockKind::Paragraph);
    // a loose item with two paragraphs; a tab-indented nested list (Obsidian writes tabs)
    d = parse_markdown("- a\n\n  more\n- b\n");
    CHECK(d.blocks.size() == 3);
    if (d.blocks.size() == 3) {
        CHECK(d.blocks[0].item && block_text(d.blocks[0]) == "a");
        CHECK(!d.blocks[1].item && d.blocks[1].list == 1 && block_text(d.blocks[1]) == "more");
        CHECK(d.blocks[2].item && block_text(d.blocks[2]) == "b");
    }
    d = parse_markdown("- a\n\t- [ ] b\n");
    CHECK(d.blocks.size() == 2 && d.blocks[1].list == 2 && d.blocks[1].task);
    CHECK(parse_markdown("").blocks.empty());
}

void test_markdown_frontmatter()
{
    const std::string fm =
        "---\n"
        "title: Deck build log\n"
        "tags:\n"
        "  - cyberdeck\n"
        "  - \"build\"\n"
        "aliases: [one, \"two\"]\n"
        "empty:\n"
        "note: |\n"
        "  folded line\n"
        "just text\n"
        "---\n";
    const std::string text = fm + "# Heading\n- [ ] task after fm\n";
    CHECK_EQ(frontmatter_length(text), fm.size());
    const Document d = parse_markdown(text);
    CHECK(d.frontmatter_len == fm.size() && d.frontmatter == fm);
    CHECK_EQ(d.blocks.size(), std::size_t(3));
    if (d.blocks.size() != 3) return;
    const Block &p = d.blocks[0];
    CHECK(p.kind == BlockKind::Properties && p.src_offset == 0);
    CHECK_EQ(p.props.size(), std::size_t(6));
    if (p.props.size() == 6) {
        CHECK(p.props[0].key == "title" && p.props[0].value == "Deck build log");
        CHECK(p.props[1].key == "tags" && p.props[1].value == "cyberdeck, build");
        CHECK(p.props[2].key == "aliases" && p.props[2].value == "one, two");
        CHECK(p.props[3].key == "empty" && p.props[3].value.empty());
        CHECK(p.props[4].key == "note" && p.props[4].value == "folded line");
        CHECK(p.props[5].key.empty() && p.props[5].value == "just text");
    }
    CHECK_EQ(block_text(p), std::string("title: Deck build log\ntags: cyberdeck, build\naliases: one, two\nempty: \nnote: folded line\njust text"));
    CHECK(d.blocks[1].kind == BlockKind::Heading && d.blocks[1].src_offset == fm.size() + 2);
    // offsets count from the start of the whole text, frontmatter included
    CHECK_EQ(d.blocks[2].task_offset, text.find("[ ] task") + 1);
    CHECK_EQ(d.blocks[2].task_line, 12);
    // the same line in the editor's text (tabs as the glyph): the line numbers agree
    const Decoded dec = decode_text("\t" + text);
    const Document d3 = parse_markdown(dec.text);
    CHECK(d3.frontmatter_len == 0);   // a tab before "---": not frontmatter
    // other shapes
    CHECK_EQ(frontmatter_length("---\n---\nx"), std::size_t(8));
    CHECK_EQ(frontmatter_length("---  \na: 1\n...\nbody"), std::size_t(15));
    CHECK_EQ(frontmatter_length("---\na: 1\n---"), std::size_t(12));          // no final newline
    CHECK_EQ(frontmatter_length("---\ntitle: never closed\n"), std::size_t(0));
    CHECK_EQ(frontmatter_length("----\na\n----\n"), std::size_t(0));
    CHECK_EQ(frontmatter_length("x\n---\na\n---\n"), std::size_t(0));
    const Document open = parse_markdown("---\ntitle: never closed\n");
    CHECK(open.frontmatter_len == 0 && !open.blocks.empty() && open.blocks[0].kind == BlockKind::Rule);
    CHECK(parse_markdown("---\n---\n").blocks.size() == 1);
    CHECK(parse_markdown("---\n---\n").blocks[0].props.empty());
}

void test_markdown_cap_and_lines()
{
    std::string many;
    for (int i = 0; i < 450; ++i) many += "p" + std::to_string(i) + "\n\n";
    Document d = parse_markdown(many);
    CHECK(d.blocks.size() == kPreviewMaxBlocks && d.truncated && d.status == 0);
    CHECK_EQ(block_text(d.blocks.back()), std::string("p399"));
    d = parse_markdown(many, 10);
    CHECK(d.blocks.size() == 10 && d.truncated);
    d = parse_markdown("p1\n\np2\n", 2);
    CHECK(d.blocks.size() == 2 && !d.truncated);
    d = parse_markdown("---\na: 1\n---\nbody\n", 1);
    CHECK(d.blocks.size() == 1 && d.blocks[0].kind == BlockKind::Properties && d.truncated);
    d = parse_markdown("---\na: 1\n---\nbody\n", 0);
    CHECK(d.blocks.empty() && d.truncated);
    // task lines agree between the body and the editor's text, so the editor finds the box of a tapped task
    const std::string body = "- a\n\t- [ ] tab task\n\t\t- [x] deeper\n";
    const Decoded dec = decode_text(body);
    const std::string edit = to_edit(dec.text, dec.fmt);
    d = parse_markdown(dec.text);
    int tasks = 0;
    for (const Block &b : d.blocks) {
        if (!b.task) continue;
        ++tasks;
        std::size_t ls = 0;
        for (int l = 0; l < b.task_line; ++l) ls = edit.find('\n', ls) + 1;
        const std::size_t mark = task_mark_on_line(edit, ls);
        CHECK(mark != std::string::npos);
        CHECK(mark == body_to_edit_offset(dec.text, b.task_offset, dec.fmt));
    }
    CHECK_EQ(tasks, 2);
    CHECK((parser_flags() & 0x2000u) != 0);   // MD_FLAG_WIKILINKS
}
