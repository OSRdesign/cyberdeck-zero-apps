/*
 * SPDX-License-Identifier: MIT
 *
 * Markdown Notes core: the preview's model of a note, built on md4c v0.6.0 (third_party/md4c, MIT; report 037 section
 * 2.3). No LVGL: part 2 draws one lv_spangroup per Block, one span per Run.
 *
 * A flat list of leaf blocks (headings, paragraphs, code blocks, rules) that carry their context: block-quote depth,
 * list depth, the list item's marker on the item's first block, its task box with the box's source offset. Inline
 * content is a list of runs (bold, italic, strike-through, code; a link index). Links: [[wikilinks]] ([[a]], [[a|b]],
 * [[a#h]]), Markdown links and images (an image or an ![[embed]] is a link in N1), autolinks (URLs, www., e-mails).
 *
 * Parser flags (parser_flags()): CommonMark plus strike-through, task lists, wikilinks, permissive autolinks, no raw
 * HTML (it shows as text). Tables are N2: without the flag a table reads as a paragraph, one line per row.
 * Line breaks: a single newline inside a paragraph is kept as a line break (Obsidian's default, "strict line breaks"
 * off), so Run texts can hold '\n'.
 *
 * Frontmatter: a "---" first line up to the next "---" or "..." line is not Markdown; it is cut off before md4c sees the
 * text (it would read as a rule or a setext heading), kept verbatim (Document::frontmatter) and shown as the first
 * block, kind Properties, muted; every offset still counts from the start of the text given to parse_markdown().
 * At most max_blocks blocks (the frontmatter counts): the rest is dropped and Document::truncated set.
 *
 * md4c details: it reads "![[x]]" as an image opener, so the embed arrives as plain text; the model turns each complete
 * one (outside code) into a Wiki link with image set, showing its alias or target. In code blocks md4c expands the
 * tabs of the indentation to spaces (the file keeps its tabs: the model is display only).
 */
#pragma once

#include "links.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdnotes {

constexpr std::size_t kPreviewMaxBlocks = 400;
constexpr std::size_t kNoOffset = static_cast<std::size_t>(-1);

enum class BlockKind { Properties, Heading, Paragraph, Code, Rule };

enum : std::uint8_t { kStyleBold = 1, kStyleItalic = 2, kStyleStrike = 4, kStyleCode = 8 };

struct Run {
    std::string text;
    std::uint8_t style = 0;   /* kStyle* bits */
    int link = -1;            /* index into Document::links, -1: none */
};

struct DocLink {
    LinkKind kind = LinkKind::File;
    std::string target;              /* Wiki: md4c's target ("Note#Heading", no alias); else the destination */
    std::size_t offset = kNoOffset;  /* the target's byte offset in the text, when md4c points into it */
    bool image = false;              /* ![alt](src) or ![[embed]] */
};

struct Property {
    std::string key;     /* "" for a line that is not "key: value" */
    std::string value;   /* a YAML list ("- a" lines, or [a, b]) joined with ", " */
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int level = 0;                 /* Heading: 1 to 6 */
    int quote = 0;                 /* block-quote depth */
    int list = 0;                  /* list depth, 0 outside lists */
    bool item = false;             /* the first block of a list item: it carries the marker */
    bool ordered = false;          /* item: numbered */
    unsigned number = 0;           /* item, ordered */
    char bullet = 0;               /* item, unordered: '-', '*' or '+' as written */
    bool task = false;             /* item: a task box */
    bool checked = false;
    std::size_t task_offset = kNoOffset;   /* the mark between '[' and ']': tasks.hpp toggle_task_at() */
    int task_line = -1;                    /* its 0-based line: the same in the body and in the edit text */
    std::vector<Run> runs;         /* Heading, Paragraph */
    std::string code;              /* Code: the text, without the final newline (real tabs) */
    std::string lang;              /* Code: the first word of the info string ("" for an indented block) */
    bool fenced = false;           /* Code */
    std::vector<Property> props;   /* Properties */
    std::size_t src_offset = kNoOffset;   /* the byte where its first text starts, when known */
};

struct Document {
    std::vector<Block> blocks;
    std::vector<DocLink> links;
    bool truncated = false;          /* more than max_blocks: the rest is not in blocks */
    std::size_t frontmatter_len = 0; /* bytes of the frontmatter (0: none) */
    std::string frontmatter;         /* verbatim, its --- lines included */
    int status = 0;                  /* md_parse(): 0, or -1 (out of memory) */
};

unsigned parser_flags();
/* text: the note's body (text_codec.hpp: LF, no BOM, real tabs). */
Document parse_markdown(const std::string &text, std::size_t max_blocks = kPreviewMaxBlocks);

/* Bytes of the frontmatter at the start of text (its closing line and that line's newline included), 0 when none. */
std::size_t frontmatter_length(const std::string &text);
/* The lines between the --- delimiters as properties. */
std::vector<Property> parse_properties(const std::string &inner);

/* "&amp;" -> "&", "&#233;" / "&#xE9;" -> "é", about thirty common names; an unknown name stays as written. */
std::string decode_entity(const std::string &entity);

/* The block's text: its runs joined (Code: the code; Properties: "key: value" lines). Tests, outlines, search. */
std::string block_text(const Block &b);

} // namespace mdnotes
