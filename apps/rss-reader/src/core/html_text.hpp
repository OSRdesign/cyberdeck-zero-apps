/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: HTML to a plain-text document model, plus the tolerant HTML tokenizer the other modules share
 * (feed.cpp finds <img>, discover.cpp finds <link>).
 *
 * The model is what the reader (phase 3) lays out: a flat list of blocks, each with inline text only. Links are shown
 * as their text (no URL clutter), entities are decoded, script / style / iframe / object / embed / noscript / svg /
 * video / audio / head content is dropped, whitespace is normalised, mis-nested or unclosed tags are tolerated, and the
 * input is bounded (1 MB read, 4000 blocks). Nothing here throws or reads outside its input.
 */
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rssreader {

/* ---- tokenizer ---------------------------------------------------------------------------------------------- */

struct HtmlToken {
    enum class Kind { Text, Start, End };
    Kind kind = Kind::Text;
    std::string name;                                        /* lower-case tag name (Start, End) */
    std::string text;                                        /* raw text, entities NOT decoded (Text) */
    std::vector<std::pair<std::string, std::string>> attrs;  /* lower-case names, entities decoded (Start) */
    bool self_closing = false;
    std::string attr(const std::string &name) const;         /* "" when absent */
    bool has_attr(const std::string &name) const;
};

/* Comments, doctype and processing instructions are skipped; the content of <script> and <style> is skipped. */
std::vector<HtmlToken> html_tokenize(const std::string &html);

/* ---- document model ----------------------------------------------------------------------------------------- */

enum class BlockType { Heading, Paragraph, ListItem, Quote, Preformatted, Image, Rule };

struct Block {
    BlockType type = BlockType::Paragraph;
    int level = 0;          /* Heading: 1..6; ListItem: nesting depth from 1; Quote: nesting depth from 1 */
    bool ordered = false;   /* ListItem: numbered list */
    int number = 0;         /* ListItem: 1-based position in an ordered list; 0 = continuation of the previous item (no marker) */
    std::string text;       /* inline text; '\n' only for <br> and in Preformatted */
    std::string url;        /* Image: absolute URL when a base was given */
    std::string alt;        /* Image */
};

std::vector<Block> html_to_blocks(const std::string &html, const std::string &base_url = "");

/* A readable text rendering of the blocks ("# " headings, "- " / "1. " items, "> " quotes, "[image: alt]", "---"). */
std::string blocks_to_string(const std::vector<Block> &blocks);
/* html_to_blocks + blocks_to_string, for tests and search. */
std::string html_to_text(const std::string &html);
/* One line: tags removed, entities decoded, whitespace collapsed (titles, summaries in lists). */
std::string html_to_inline(const std::string &html);

/* The image URLs of an HTML fragment, in order, absolute against base_url (src, then data-src / srcset when src is
 * missing or a data: URI); tracking pixels (1x1) and non-http(s) sources are skipped; no duplicates. */
std::vector<std::string> html_image_urls(const std::string &html, const std::string &base_url);

} // namespace rssreader
