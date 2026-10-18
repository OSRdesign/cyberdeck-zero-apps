// HTML to the plain-text document model.
#include "test_common.hpp"

#include "html_text.hpp"

using namespace rssreader;

static std::string T(const std::string &html) { return html_to_text(html); }

void test_html_text()
{
    CHECK_EQ(T("<h1>Title</h1><p>Hello <b>bold</b> <a href='x'>link</a> world</p>"), "# Title\nHello bold link world\n");
    CHECK_EQ(T("<h2>Two</h2><h3>Three</h3><h6>Six</h6>"), "## Two\n### Three\n###### Six\n");
    CHECK_EQ(T(""), "");
    CHECK_EQ(T("   \n\t "), "");
    CHECK_EQ(T("<div><span></span></div>"), "");
    CHECK_EQ(T("Just text\nwith newline"), "Just text with newline\n");

    // links show their text only, no URL
    CHECK_EQ(T("<p>See <a href=\"https://example.com/very/long/url?x=1\">the report</a>.</p>"), "See the report.\n");

    // lists
    CHECK_EQ(T("<ul><li>a<ul><li>b</li><li>c</li></ul></li><li>d</li></ul>"), "- a\n  - b\n  - c\n- d\n");
    CHECK_EQ(T("<ol><li>one</li><li>two</li><li>three</li></ol>"), "1. one\n2. two\n3. three\n");
    CHECK_EQ(T("<ol><li>x<ol><li>y</li></ol></li><li>z</li></ol>"), "1. x\n  1. y\n2. z\n");
    CHECK_EQ(T("<ul><li><p>First</p><p>second para</p></li><li>next</li></ul>"), "- First\n  second para\n- next\n");
    CHECK_EQ(T("<ul><li>x</li></ul>tail"), "- x\ntail\n");
    CHECK_EQ(T("<li>stray</li><li>items</li>"), "- stray\n- items\n");
    {
        std::vector<Block> b = html_to_blocks("<ul><li>a<ul><li>b</li></ul></li></ul>");
        CHECK_EQ(b.size(), std::size_t(2));
        if (b.size() == 2) {
            CHECK(b[0].type == BlockType::ListItem && b[0].level == 1 && b[0].number == 1 && !b[0].ordered);
            CHECK(b[1].type == BlockType::ListItem && b[1].level == 2 && b[1].text == "b");
        }
    }

    // entities
    CHECK_EQ(T("<p>Fish &amp; Chips &copy; 2026 &#8212; caf&eacute; &nbsp;x &lt;tag&gt; &bogus; AT&T</p>"),
             "Fish & Chips \xC2\xA9 2026 \xE2\x80\x94 caf\xC3\xA9 x <tag> &bogus; AT&T\n");
    CHECK_EQ(T("<p>&#8220;q&#8221; &#x2019;</p>"), "\xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x99\n");

    // dropped content
    CHECK_EQ(T("<p>a</p><script>var x='<p>evil</p>';</script><style>p{color:red}</style><iframe src=x>frame text</iframe>"
               "<noscript><img src=/n.png></noscript><object>obj</object><svg><text>svg</text></svg><p>b</p>"),
             "a\nb\n");
    CHECK_EQ(T("<p>a</p><script>unterminated script <p>x</p>"), "a\n");
    CHECK_EQ(T("<iframe><iframe>nested</iframe>still dropped</iframe><p>ok</p>"), "ok\n");
    CHECK_EQ(T("<p>x</p><!-- <p>comment</p> --><p>y</p><!DOCTYPE html><?php echo 1 ?>"), "x\ny\n");
    CHECK_EQ(T("<head><title>T</title></head><body>B</body>"), "B\n");

    // preformatted
    {
        std::vector<Block> b = html_to_blocks("<pre>line1\n  line2\n\nline4</pre><p>after</p>");
        CHECK_EQ(b.size(), std::size_t(2));
        if (b.size() == 2) {
            CHECK(b[0].type == BlockType::Preformatted);
            CHECK_EQ(b[0].text, "line1\n  line2\n\nline4");
            CHECK(b[1].type == BlockType::Paragraph);
        }
        CHECK_EQ(html_to_blocks("<pre><code>a &lt; b\n\tc</code></pre>")[0].text, "a < b\n\tc");
        CHECK_EQ(html_to_blocks("<pre>\n\nx\n\n</pre>")[0].text, "x");
    }

    // line breaks, whitespace
    CHECK_EQ(T("<p>a<br>b<br/><br>c</p>"), "a\nb\nc\n");
    CHECK_EQ(T("<p>a<br>  b</p>"), "a\nb\n");
    CHECK_EQ(T("<br><br><p>x</p>"), "x\n");
    CHECK_EQ(T("<p>   many \n\t spaces   here </p>\n\n  <div>  next  </div>"), "many spaces here\nnext\n");
    CHECK_EQ(T("<p>a\xC2\xA0\xC2\xA0 b\xE2\x80\x8B" "c</p>"), "a bc\n");

    // quotes, rules
    CHECK_EQ(T("<blockquote><p>Quote one</p><blockquote>nested</blockquote></blockquote><p>out</p>"), "> Quote one\n>> nested\nout\n");
    CHECK_EQ(T("<p>a</p><hr><p>b</p>"), "a\n---\nb\n");

    // tables
    CHECK_EQ(T("<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>"), "A | B\n1 | 2\n");

    // misnested / unclosed
    CHECK_EQ(T("<p>one<p>two<b>bold<i>both</b>italic</i></p>"), "one\ntwoboldbothitalic\n");
    CHECK_EQ(T("<div><p>unclosed <b>bold"), "unclosed bold\n");
    CHECK_EQ(T("</p></div></b>text</li></ul>"), "text\n");
    T("<<<>>><p<p>>x");                                                // must not crash; the shape is unimportant
    CHECK_EQ(T("1 < 2 and 3 > 2"), "1 < 2 and 3 > 2\n");
    CHECK_EQ(T("<p attr='unterminated>text"), "");                    // the tag swallows the rest, like a browser's bogus tag

    // images
    {
        std::vector<Block> b = html_to_blocks("<p>t</p><img src=\"../img/x.png\" alt=\"An  X\"><img src=\"https://cdn.org/y.jpg\">"
                                              "<img data-src=\"/lazy.jpg\"><img src=\"data:image/gif;base64,AAA\"><img src=\"/p.gif\" width=\"1\" height=\"1\">"
                                              "<img srcset=\"/s1.png 1x, /s2.png 2x\"><img alt=\"no src\"><img src=\"javascript:x\">",
                                              "https://ex.com/a/b.html");
        std::vector<std::string> urls;
        for (const Block &x : b)
            if (x.type == BlockType::Image) urls.push_back(x.url);
        CHECK_EQ(urls.size(), std::size_t(4));
        if (urls.size() == 4) {
            CHECK_EQ(urls[0], "https://ex.com/img/x.png");
            CHECK_EQ(urls[1], "https://cdn.org/y.jpg");
            CHECK_EQ(urls[2], "https://ex.com/lazy.jpg");
            CHECK_EQ(urls[3], "https://ex.com/s1.png");
        }
        CHECK(b.size() >= 2 && b[1].alt == "An X");
        CHECK_EQ(blocks_to_string(html_to_blocks("<img src='https://a.b/c.png' alt='Cat &amp; dog'><img src='https://a.b/d.png'>")),
                 "[image: Cat & dog]\n[image]\n");
        // without a base only absolute URLs are usable
        std::vector<Block> nb = html_to_blocks("<img src='/rel.png'><img src='https://a.b/c.png'>");
        CHECK_EQ(nb.size(), std::size_t(1));
        // an image inside a link inside a paragraph splits the text around it
        std::vector<Block> mixed = html_to_blocks("<p>before <a href='/x'><img src='https://a.b/i.png' alt='pic'></a> after</p>");
        CHECK_EQ(mixed.size(), std::size_t(3));
        if (mixed.size() == 3) CHECK(mixed[0].type == BlockType::Paragraph && mixed[1].type == BlockType::Image && mixed[2].text == "after");
    }

    // bounded
    {
        std::string many;
        for (int i = 0; i < 10000; ++i) many += "<p>p</p>";
        CHECK(html_to_blocks(many).size() <= 4000);
        std::string deep;
        for (int i = 0; i < 20000; ++i) deep += "<div>";
        deep += "text";
        CHECK_EQ(T(deep), "text\n");
        std::string nested_lists;
        for (int i = 0; i < 300; ++i) nested_lists += "<ul><li>x";
        CHECK(!T(nested_lists).empty());
        std::string big(3 * 1024 * 1024, 'a');
        CHECK(html_to_text("<p>" + big + "</p>").size() <= 1024 * 1024 + 8);
        CHECK(html_to_blocks(std::string("<p>\0x\0</p>", 10)).size() <= 1);
        std::string bin;
        for (int i = 0; i < 4096; ++i) bin += static_cast<char>(i * 7);
        html_to_blocks(bin);        // must not crash
    }
}

void test_html_helpers()
{
    CHECK_EQ(html_to_inline("<b>Hello</b> &amp; <i>bye</i><script>x</script>"), "Hello & bye");
    CHECK_EQ(html_to_inline("a<br>b"), "a b");
    CHECK_EQ(html_to_inline("<p>x</p><p>y</p>"), "x y");
    CHECK_EQ(html_to_inline("plain &#8217; text"), "plain \xE2\x80\x99 text");
    CHECK_EQ(html_to_inline(""), "");

    std::vector<std::string> u = html_image_urls("<img src='/a.png'><img src='/a.png'><img src=\"b.jpg\" width=2><img data-lazy-src='https://x.org/c.jpg' src='data:image/gif;base64,R0lG'>"
                                                  "<img src='http://feeds.feedburner.com/~r/x/1.gif'><img src='/tiny.gif' height='1'>",
                                                  "https://ex.com/dir/page");
    CHECK_EQ(u.size(), std::size_t(2));
    if (u.size() == 2) {
        CHECK_EQ(u[0], "https://ex.com/a.png");
        CHECK_EQ(u[1], "https://x.org/c.jpg");
    }
    CHECK(html_image_urls("", "https://a.b").empty());

    // tokenizer
    std::vector<HtmlToken> t = html_tokenize("<A HREF='x&amp;y' Data=1 flag>t</A><br/><!-- c --><script>if (a<b) {}</script>after");
    CHECK(t.size() >= 6);
    if (t.size() >= 6) {
        CHECK(t[0].kind == HtmlToken::Kind::Start && t[0].name == "a" && t[0].attr("href") == "x&y" && t[0].attr("data") == "1" && t[0].has_attr("flag"));
        CHECK(t[1].kind == HtmlToken::Kind::Text && t[1].text == "t");
        CHECK(t[2].kind == HtmlToken::Kind::End && t[2].name == "a");
        CHECK(t[3].name == "br" && t[3].self_closing);
    }
    bool script_text = false;
    for (const HtmlToken &k : t)
        if (k.kind == HtmlToken::Kind::Text && k.text.find("if (a<b)") != std::string::npos) script_text = true;
    CHECK(!script_text);
}
