/*
 * SPDX-License-Identifier: MIT
 */
#include "markdown.hpp"

#include "utf8.hpp"

#include "../../third_party/md4c/md4c.h"

#include <cstring>
#include <string_view>

namespace mdnotes {

namespace {

struct ListCtx {
    bool ordered = false;
    unsigned next = 1;
    char bullet = 0;
};

struct ItemCtx {
    bool task = false;
    bool checked = false;
    std::size_t offset = kNoOffset;
    bool has_block = false;
    bool ordered = false;
    unsigned number = 0;
    char bullet = 0;
};

struct Ctx {
    const char *base = nullptr;   /* the whole text: offsets count from here */
    std::size_t len = 0;
    std::size_t parse_off = 0;    /* md4c sees base + parse_off (after the frontmatter) */
    Document *doc = nullptr;
    std::size_t max_blocks = kPreviewMaxBlocks;
    int quote = 0;
    std::vector<ListCtx> lists;
    std::vector<ItemCtx> items;
    int cur = -1;                 /* the open leaf block (index into doc->blocks) */
    bool implicit = false;        /* cur was opened by the text of a tight list item */
    int bold = 0, italic = 0, strike = 0, code = 0;
    std::vector<int> link_stack;
    std::size_t lc_off = 0;           /* line_of()'s cache: tasks come in source order */
    int lc_line = 0;

    std::size_t offset_of(const char *p) const
    {
        if (!p || p < base || p >= base + len) return kNoOffset;
        return static_cast<std::size_t>(p - base);
    }

    int line_of(std::size_t off)
    {
        if (off < lc_off) {
            lc_off = 0;
            lc_line = 0;
        }
        for (std::size_t i = lc_off; i < off && i < len; ++i) lc_line += base[i] == '\n';
        lc_off = off;
        return lc_line;
    }

    /* "![[embed]]" (an embed is a link in N1): md4c reads "![" as an image opener, so the wikilink is lost and the
     * whole thing arrives as plain text. Each complete one at the end of the block's last plain run becomes a Wiki
     * link (image set) showing its alias or its target; `end` is the source offset just past the text md4c gave last. */
    void split_embeds(Block &b, std::size_t end)
    {
        std::size_t from = 0;
        while (!b.runs.empty()) {
            Run &r = b.runs.back();
            if (r.link >= 0 || (r.style & kStyleCode)) return;
            const std::size_t p = r.text.find("![[", from);
            if (p == std::string::npos) return;
            const std::size_t e = r.text.find("]]", p + 3);
            if (e == std::string::npos) return;
            const std::string inner = r.text.substr(p + 3, e - p - 3);
            if (inner.empty() || inner.find('\n') != std::string::npos || inner.find('[') != std::string::npos) {
                from = p + 1;
                continue;
            }
            const std::uint8_t style = r.style;
            const std::string after = r.text.substr(e + 2);
            r.text.resize(p);
            if (r.text.empty()) b.runs.pop_back();
            DocLink l;
            l.kind = LinkKind::Wiki;
            l.image = true;
            const std::size_t bar = inner.find('|');
            l.target = inner.substr(0, bar);
            if (end != kNoOffset && end <= len) {
                const std::string pattern = "![[" + inner + "]]";
                const std::size_t at = std::string_view(base, end).rfind(pattern);
                if (at != std::string::npos) l.offset = at + 3;
            }
            doc->links.push_back(std::move(l));
            Run er;
            er.text = bar == std::string::npos ? inner : inner.substr(bar + 1);
            er.style = style;
            er.link = static_cast<int>(doc->links.size()) - 1;
            b.runs.push_back(std::move(er));
            if (after.empty()) return;
            Run ar;
            ar.text = after;
            ar.style = style;
            b.runs.push_back(std::move(ar));
            from = 0;
        }
    }

    /* A new leaf block with its context; false when the cap is reached (the parse stops). */
    bool open_leaf(BlockKind kind)
    {
        if (doc->blocks.size() >= max_blocks) {
            doc->truncated = true;
            return false;
        }
        Block b;
        b.kind = kind;
        b.quote = quote;
        b.list = static_cast<int>(lists.size());
        if (!items.empty() && !items.back().has_block) {
            ItemCtx &it = items.back();
            it.has_block = true;
            b.item = true;
            b.ordered = it.ordered;
            b.number = it.number;
            b.bullet = it.bullet;
            b.task = it.task;
            b.checked = it.checked;
            b.task_offset = it.offset;
            if (it.task && it.offset != kNoOffset) b.task_line = line_of(it.offset);
        }
        doc->blocks.push_back(std::move(b));
        cur = static_cast<int>(doc->blocks.size()) - 1;
        implicit = false;
        return true;
    }

    void close_implicit()
    {
        if (implicit) {
            cur = -1;
            implicit = false;
        }
    }

    std::uint8_t style() const
    {
        std::uint8_t s = 0;
        if (bold) s |= kStyleBold;
        if (italic) s |= kStyleItalic;
        if (strike) s |= kStyleStrike;
        if (code) s |= kStyleCode;
        return s;
    }
};

std::string attr_text(const MD_ATTRIBUTE &a)
{
    std::string out;
    if (!a.text || !a.size) return out;
    if (!a.substr_offsets || !a.substr_types) return std::string(a.text, a.size);
    for (int i = 0; a.substr_offsets[i] < a.size; ++i) {
        const MD_OFFSET b = a.substr_offsets[i], e = a.substr_offsets[i + 1];
        const std::string piece(a.text + b, e - b);
        switch (a.substr_types[i]) {
        case MD_TEXT_ENTITY: out += decode_entity(piece); break;
        case MD_TEXT_NULLCHAR: out += "\xEF\xBF\xBD"; break;
        default: out += piece; break;
        }
    }
    return out;
}

void append_run(Block &b, const std::string &text, std::uint8_t style, int link)
{
    if (text.empty()) return;
    if (!b.runs.empty() && b.runs.back().style == style && b.runs.back().link == link) {
        b.runs.back().text += text;
        return;
    }
    Run r;
    r.text = text;
    r.style = style;
    r.link = link;
    b.runs.push_back(std::move(r));
}

int on_enter_block(MD_BLOCKTYPE type, void *detail, void *user)
{
    Ctx &c = *static_cast<Ctx *>(user);
    c.close_implicit();
    switch (type) {
    case MD_BLOCK_DOC: break;
    case MD_BLOCK_QUOTE: ++c.quote; break;
    case MD_BLOCK_UL: {
        ListCtx l;
        l.bullet = static_cast<const MD_BLOCK_UL_DETAIL *>(detail)->mark;
        c.lists.push_back(l);
        break;
    }
    case MD_BLOCK_OL: {
        ListCtx l;
        l.ordered = true;
        l.next = static_cast<const MD_BLOCK_OL_DETAIL *>(detail)->start;
        c.lists.push_back(l);
        break;
    }
    case MD_BLOCK_LI: {
        const auto *d = static_cast<const MD_BLOCK_LI_DETAIL *>(detail);
        ItemCtx it;
        it.task = d->is_task != 0;
        if (it.task) {
            it.checked = d->task_mark == 'x' || d->task_mark == 'X';
            it.offset = c.parse_off + d->task_mark_offset;
        }
        if (!c.lists.empty()) {
            ListCtx &l = c.lists.back();
            it.ordered = l.ordered;
            it.bullet = l.bullet;
            if (l.ordered) it.number = l.next++;
        }
        c.items.push_back(it);
        break;
    }
    case MD_BLOCK_HR:
        if (!c.open_leaf(BlockKind::Rule)) return 1;
        break;
    case MD_BLOCK_H:
        if (!c.open_leaf(BlockKind::Heading)) return 1;
        c.doc->blocks[static_cast<std::size_t>(c.cur)].level = static_cast<int>(static_cast<const MD_BLOCK_H_DETAIL *>(detail)->level);
        break;
    case MD_BLOCK_CODE: {
        if (!c.open_leaf(BlockKind::Code)) return 1;
        const auto *d = static_cast<const MD_BLOCK_CODE_DETAIL *>(detail);
        Block &b = c.doc->blocks[static_cast<std::size_t>(c.cur)];
        b.fenced = d->fence_char != 0;
        b.lang = attr_text(d->lang);
        break;
    }
    case MD_BLOCK_P:
    case MD_BLOCK_HTML:
    default:   /* tables are not enabled; anything else reads as a paragraph */
        if (!c.open_leaf(BlockKind::Paragraph)) return 1;
        break;
    }
    return 0;
}

int on_leave_block(MD_BLOCKTYPE type, void *, void *user)
{
    Ctx &c = *static_cast<Ctx *>(user);
    switch (type) {
    case MD_BLOCK_DOC: break;
    case MD_BLOCK_QUOTE:
        c.close_implicit();
        if (c.quote > 0) --c.quote;
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        c.close_implicit();
        if (!c.lists.empty()) c.lists.pop_back();
        break;
    case MD_BLOCK_LI: {
        c.close_implicit();
        if (!c.items.empty() && !c.items.back().has_block) {   /* "- [ ]" with nothing after it */
            if (!c.open_leaf(BlockKind::Paragraph)) return 1;
            c.cur = -1;
        }
        if (!c.items.empty()) c.items.pop_back();
        break;
    }
    case MD_BLOCK_CODE:
        if (c.cur >= 0) {
            std::string &code = c.doc->blocks[static_cast<std::size_t>(c.cur)].code;
            if (!code.empty() && code.back() == '\n') code.pop_back();
        }
        c.cur = -1;
        break;
    default:
        c.cur = -1;
        c.implicit = false;
        break;
    }
    return 0;
}

int on_enter_span(MD_SPANTYPE type, void *detail, void *user)
{
    Ctx &c = *static_cast<Ctx *>(user);
    switch (type) {
    case MD_SPAN_EM: ++c.italic; break;
    case MD_SPAN_STRONG: ++c.bold; break;
    case MD_SPAN_DEL: ++c.strike; break;
    case MD_SPAN_CODE: ++c.code; break;
    case MD_SPAN_A:
    case MD_SPAN_IMG:
    case MD_SPAN_WIKILINK: {
        DocLink l;
        if (type == MD_SPAN_WIKILINK) {
            const auto *d = static_cast<const MD_SPAN_WIKILINK_DETAIL *>(detail);
            l.kind = LinkKind::Wiki;
            l.target = attr_text(d->target);
            l.offset = c.offset_of(d->target.text);
        } else if (type == MD_SPAN_A) {
            const auto *d = static_cast<const MD_SPAN_A_DETAIL *>(detail);
            l.target = attr_text(d->href);
            l.kind = classify_href(l.target);
            l.offset = c.offset_of(d->href.text);
        } else {
            const auto *d = static_cast<const MD_SPAN_IMG_DETAIL *>(detail);
            l.target = attr_text(d->src);
            l.kind = classify_href(l.target);
            l.offset = c.offset_of(d->src.text);
            l.image = true;
        }
        c.doc->links.push_back(std::move(l));
        c.link_stack.push_back(static_cast<int>(c.doc->links.size()) - 1);
        break;
    }
    default: break;
    }
    return 0;
}

int on_leave_span(MD_SPANTYPE type, void *, void *user)
{
    Ctx &c = *static_cast<Ctx *>(user);
    switch (type) {
    case MD_SPAN_EM: if (c.italic) --c.italic; break;
    case MD_SPAN_STRONG: if (c.bold) --c.bold; break;
    case MD_SPAN_DEL: if (c.strike) --c.strike; break;
    case MD_SPAN_CODE: if (c.code) --c.code; break;
    case MD_SPAN_A:
    case MD_SPAN_IMG:
    case MD_SPAN_WIKILINK:
        if (!c.link_stack.empty()) c.link_stack.pop_back();
        break;
    default: break;
    }
    return 0;
}

int on_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *user)
{
    Ctx &c = *static_cast<Ctx *>(user);
    if (c.cur < 0) {   /* the text of a tight list item (or of an item after its nested list) */
        if (!c.open_leaf(BlockKind::Paragraph)) return 1;
        c.implicit = true;
    }
    Block &b = c.doc->blocks[static_cast<std::size_t>(c.cur)];
    const std::size_t at = c.offset_of(text);
    if (b.src_offset == kNoOffset) b.src_offset = at;
    std::string piece;
    switch (type) {
    case MD_TEXT_NULLCHAR: piece = "\xEF\xBF\xBD"; break;
    case MD_TEXT_BR:
    case MD_TEXT_SOFTBR: piece = "\n"; break;
    case MD_TEXT_ENTITY: piece = decode_entity(std::string(text, size)); break;
    default: piece.assign(text, size); break;
    }
    if (b.kind == BlockKind::Code) {
        b.code += piece;
        return 0;
    }
    const int link = c.link_stack.empty() ? -1 : c.link_stack.back();
    append_run(b, piece, c.style(), link);
    if (link < 0 && type == MD_TEXT_NORMAL && piece.find(']') != std::string::npos)
        c.split_embeds(b, at == kNoOffset ? kNoOffset : at + size);
    return 0;
}

bool line_is(const std::string &t, std::size_t b, std::size_t e, const char *mark)
{
    while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\t')) --e;
    const std::size_t n = std::strlen(mark);
    return e - b == n && t.compare(b, n, mark) == 0;
}

std::string trim(const std::string &s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string unquote(const std::string &v)
{
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
        return v.substr(1, v.size() - 2);
    return v;
}

} // namespace

unsigned parser_flags()
{
    return MD_FLAG_NOHTML | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_WIKILINKS | MD_FLAG_PERMISSIVEAUTOLINKS;
}

namespace {

/* The frontmatter's length, and where its closing line starts (close_begin). 0 when there is none. */
std::size_t scan_frontmatter(const std::string &t, std::size_t *close_begin)
{
    if (t.compare(0, 3, "---") != 0) return 0;
    const std::size_t e = t.find('\n');
    if (e == std::string::npos || !line_is(t, 0, e, "---")) return 0;
    std::size_t b = e + 1;
    while (b < t.size()) {
        std::size_t le = t.find('\n', b);
        const bool last = le == std::string::npos;
        if (last) le = t.size();
        if (line_is(t, b, le, "---") || line_is(t, b, le, "...")) {
            if (close_begin) *close_begin = b;
            return last ? t.size() : le + 1;
        }
        b = le + 1;
    }
    return 0;
}

} // namespace

std::size_t frontmatter_length(const std::string &t) { return scan_frontmatter(t, nullptr); }

std::vector<Property> parse_properties(const std::string &inner)
{
    std::vector<Property> props;
    std::vector<std::string> list;   /* the "- item" lines of the last key */
    const auto flush = [&] {
        if (!list.empty() && !props.empty()) {
            std::string v = props.back().value;
            for (const std::string &x : list) v += (v.empty() ? "" : ", ") + x;
            props.back().value = v;
        }
        list.clear();
    };
    std::size_t i = 0;
    while (i < inner.size()) {
        std::size_t e = inner.find('\n', i);
        if (e == std::string::npos) e = inner.size();
        const std::string raw = inner.substr(i, e - i);
        i = e + 1;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const bool indented = raw[0] == ' ' || raw[0] == '\t';
        if (line[0] == '-' && (line.size() == 1 || line[1] == ' ') && !props.empty()) {
            list.push_back(unquote(trim(line.substr(1))));
            continue;
        }
        if (indented && !props.empty()) {   /* a folded or nested value: joined with a space */
            Property &p = props.back();
            p.value += (p.value.empty() ? "" : " ") + line;
            continue;
        }
        flush();
        Property p;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            p.value = line;
        } else {
            p.key = trim(line.substr(0, colon));
            std::string v = trim(line.substr(colon + 1));
            if (v == "|" || v == ">" || v == "|-" || v == ">-") v.clear();
            if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {   /* [a, "b"] */
                std::string out, item;
                const std::string body = v.substr(1, v.size() - 2);
                std::size_t k = 0;
                while (k <= body.size()) {
                    std::size_t comma = body.find(',', k);
                    if (comma == std::string::npos) comma = body.size();
                    item = unquote(trim(body.substr(k, comma - k)));
                    if (!item.empty()) out += (out.empty() ? "" : ", ") + item;
                    k = comma + 1;
                }
                v = out;
            } else {
                v = unquote(v);
            }
            p.value = v;
        }
        props.push_back(std::move(p));
    }
    flush();
    return props;
}

std::string decode_entity(const std::string &e)
{
    if (e.size() < 3 || e.front() != '&' || e.back() != ';') return e;
    if (e[1] == '#') {
        std::uint32_t cp = 0;
        bool hex = e.size() > 3 && (e[2] == 'x' || e[2] == 'X');
        std::size_t i = hex ? 3 : 2;
        if (i >= e.size() - 1) return e;
        for (; i < e.size() - 1; ++i) {
            const char ch = e[i];
            int d;
            if (ch >= '0' && ch <= '9') d = ch - '0';
            else if (hex && ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
            else if (hex && ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
            else return e;
            cp = cp * (hex ? 16 : 10) + static_cast<std::uint32_t>(d);
            if (cp > 0x10FFFF) cp = 0x110000;
        }
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        std::string out;
        utf8_append(out, cp);
        return out;
    }
    static const struct {
        const char *name;
        std::uint32_t cp;
    } kNames[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
        {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013},
        {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
        {"bull", 0x2022}, {"middot", 0xB7}, {"deg", 0xB0}, {"plusmn", 0xB1}, {"times", 0xD7}, {"divide", 0xF7},
        {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2}, {"sect", 0xA7}, {"para", 0xB6},
        {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194}, {"check", 0x2713},
        {"eacute", 0xE9}, {"egrave", 0xE8}, {"agrave", 0xE0}, {"ccedil", 0xE7}, {"ouml", 0xF6}, {"uuml", 0xFC},
        {"auml", 0xE4}, {"szlig", 0xDF},
    };
    const std::string name = e.substr(1, e.size() - 2);
    for (const auto &n : kNames) {
        if (name == n.name) {
            std::string out;
            utf8_append(out, n.cp);
            return out;
        }
    }
    return e;
}

Document parse_markdown(const std::string &text, std::size_t max_blocks)
{
    Document doc;
    Ctx c;
    std::size_t close_begin = 0;
    const std::size_t fm = scan_frontmatter(text, &close_begin);
    c.base = text.data();
    c.len = text.size();
    c.doc = &doc;
    c.max_blocks = max_blocks;
    if (fm && max_blocks == 0) {
        doc.truncated = true;
        return doc;
    }
    if (fm) {
        doc.frontmatter_len = fm;
        doc.frontmatter = text.substr(0, fm);
        const std::size_t inner = text.find('\n') + 1;   /* after the first "---" line */
        Block b;
        b.kind = BlockKind::Properties;
        b.src_offset = 0;
        b.props = parse_properties(close_begin > inner ? text.substr(inner, close_begin - inner) : std::string());
        doc.blocks.push_back(std::move(b));
    }
    c.parse_off = fm;

    MD_PARSER p;
    std::memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = parser_flags();
    p.enter_block = on_enter_block;
    p.leave_block = on_leave_block;
    p.enter_span = on_enter_span;
    p.leave_span = on_leave_span;
    p.text = on_text;
    const int rc = md_parse(text.data() + fm, static_cast<MD_SIZE>(text.size() - fm), &p, &c);
    doc.status = (rc == 1 && doc.truncated) ? 0 : rc;
    return doc;
}

std::string block_text(const Block &b)
{
    switch (b.kind) {
    case BlockKind::Code: return b.code;
    case BlockKind::Properties: {
        std::string out;
        for (const Property &p : b.props) {
            if (!out.empty()) out += '\n';
            out += p.key.empty() ? p.value : p.key + ": " + p.value;
        }
        return out;
    }
    default: break;
    }
    std::string out;
    for (const Run &r : b.runs) out += r.text;
    return out;
}

} // namespace mdnotes
