/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: a small tolerant XML parser (no libxml2 / expat) that builds a light DOM.
 *
 * What real feeds need, and nothing more:
 *   - entities (the five XML ones, numeric ones, and the HTML names real feeds use without declaring them), CDATA,
 *     comments, processing instructions, a DOCTYPE (skipped; its entity declarations are NOT expanded, so there is no
 *     entity bomb);
 *   - namespaces by prefix: the prefix in use is replaced by the canonical one for the well-known URIs, so `dc:creator`,
 *     `content:encoded`, `media:content`, `atom:link`, `itunes:image` and `rdf:RDF` are found whatever prefix the feed
 *     declared. Elements of the default namespace (Atom, RSS 1.0) get the bare local name. An undeclared prefix is kept as
 *     written (broken feeds use `dc:` without declaring it);
 *   - the encoding: a UTF-8 BOM is skipped; UTF-8 / US-ASCII are used as they are (invalid bytes become U+FFFD);
 *     ISO-8859-1 (read as windows-1252, as browsers do), windows-1252 and ISO-8859-15 are transcoded to UTF-8; anything else (UTF-16, Shift_JIS ...) is refused
 *     with BadEncoding;
 *   - limits: documents over 5 MB, more than 400 000 nodes or nesting deeper than 64 levels are refused;
 *   - recovery: mismatched or missing end tags, unquoted attributes, a truncated document and junk after the root are
 *     tolerated (XmlDoc::recovered says so); text outside the root is ignored.
 * It never throws and never reads outside the input.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace rssreader {

struct XmlAttr {
    std::string name;    /* as written (xml:lang, href, type ...) */
    std::string value;   /* entities decoded */
};

struct XmlNode {
    bool is_text = false;                 /* a text node: `text` is set, the rest is empty */
    std::string name;                     /* canonical qualified name ("item", "dc:creator", "atom:link") */
    std::string local;                    /* the part after the colon */
    std::string text;
    std::vector<XmlAttr> attrs;
    std::vector<std::unique_ptr<XmlNode>> kids;   /* elements and text nodes, in document order */
    XmlNode *parent = nullptr;

    /* First child element with this name (case-insensitive), or nullptr. */
    const XmlNode *child(const std::string &name) const;
    std::vector<const XmlNode *> children(const std::string &name) const;
    /* The attribute value (names compared case-insensitively), or "". */
    std::string attr(const std::string &name) const;
    bool has_attr(const std::string &name) const;
    /* All the text inside, concatenated (CDATA included), not trimmed. */
    std::string text_content() const;
    /* Trimmed text_content of the first child element `name`, or "". */
    std::string child_text(const std::string &name) const;
};

enum class XmlStatus {
    Ok,
    Empty,         /* no element found (not XML at all) */
    TooBig,
    TooDeep,
    BadEncoding,
};

struct XmlDoc {
    std::unique_ptr<XmlNode> root;
    bool recovered = false;               /* something was malformed and repaired */
};

constexpr std::size_t kMaxXmlBytes = 5u * 1024 * 1024;
constexpr int kMaxXmlDepth = 64;
constexpr std::size_t kMaxXmlNodes = 400000;

XmlStatus xml_parse(const std::string &input, XmlDoc &out);

/* The children of `node` written back as markup (for Atom type="xhtml" content). */
std::string xml_inner(const XmlNode &node);

} // namespace rssreader
