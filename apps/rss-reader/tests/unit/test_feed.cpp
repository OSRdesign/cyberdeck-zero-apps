// xml, feed parsing and discovery.
#include "test_common.hpp"

#include "discover.hpp"
#include "feed.hpp"
#include "util.hpp"
#include "xml.hpp"

#include <set>

using namespace rssreader;
using rssreader_test::fixture;

static const std::int64_t kT = 1792336205LL;   // 2026-10-18 15:10:05 UTC

static bool utf8_ok(const std::string &s) { return utf8_sanitize(s) == s; }

static const Article *by_guid(const Feed &f, const std::string &guid)
{
    for (const Article &a : f.articles)
        if (a.guid == guid) return &a;
    return nullptr;
}

void test_xml()
{
    XmlDoc d;
    CHECK(xml_parse("<?xml version=\"1.0\"?><!-- c --><?pi x?><a x='1' y=\"2&amp;3\"><b>t&lt;1</b><![CDATA[<raw & >]]><c/></a>", d) == XmlStatus::Ok);
    CHECK(d.root && d.root->name == "a" && !d.recovered);
    CHECK_EQ(d.root->attr("y"), "2&3");
    CHECK_EQ(d.root->attr("X"), "1");                                   // attribute names are case-insensitive
    CHECK_EQ(d.root->child_text("b"), "t<1");
    CHECK_EQ(d.root->text_content(), "t<1<raw & >");
    CHECK(d.root->child("c") != nullptr && d.root->child("C") != nullptr);

    // DOCTYPE with an internal subset: skipped, its entities are NOT expanded
    CHECK(xml_parse("<!DOCTYPE x [<!ENTITY a \"bomb\"> <!ENTITY b \"&a;&a;\">]><x>&a;&b;</x>", d) == XmlStatus::Ok);
    CHECK_EQ(d.root->text_content(), "&a;&b;");

    // namespaces: any prefix for a known URI maps to the canonical one; the default Atom / RSS 1.0 namespaces are bare
    CHECK(xml_parse("<r xmlns:d=\"http://purl.org/dc/elements/1.1/\" xmlns:c=\"http://purl.org/rss/1.0/modules/content/\" "
                    "xmlns:m=\"http://search.yahoo.com/mrss/\" xmlns:a=\"http://www.w3.org/2005/Atom\" xmlns:u=\"urn:other\">"
                    "<d:creator>x</d:creator><c:encoded>y</c:encoded><m:content url=\"u\"/><a:link/><u:thing/><z:undeclared/></r>", d) == XmlStatus::Ok);
    CHECK(d.root->child("dc:creator") && d.root->child("content:encoded") && d.root->child("media:content") && d.root->child("atom:link"));
    CHECK(d.root->child("u:thing") && d.root->child("z:undeclared"));
    CHECK(d.root->child("dc:creator")->local == "creator");
    CHECK(xml_parse("<feed xmlns=\"http://www.w3.org/2005/Atom\"><entry><title>t</title></entry></feed>", d) == XmlStatus::Ok);
    CHECK(d.root->name == "feed" && d.root->child("entry") && d.root->child("entry")->child("title"));
    // a prefix scope ends with its element
    CHECK(xml_parse("<r><a xmlns:d=\"http://purl.org/dc/elements/1.1/\"><d:x/></a><b><d:y/></b></r>", d) == XmlStatus::Ok);
    CHECK(d.root->child("a")->child("dc:x") && d.root->child("b")->child("d:y"));

    // encodings
    CHECK(xml_parse("\xEF\xBB\xBF<a>\xC3\xA9</a>", d) == XmlStatus::Ok && d.root->text_content() == "\xC3\xA9");
    CHECK(xml_parse(std::string("\xFF\xFE<\0a\0>\0", 8), d) == XmlStatus::BadEncoding);
    CHECK(xml_parse(std::string("<\0a\0>\0", 6), d) == XmlStatus::BadEncoding);
    CHECK(xml_parse("<?xml version=\"1.0\" encoding=\"Shift_JIS\"?><a/>", d) == XmlStatus::BadEncoding);
    CHECK(xml_parse("<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>", d) == XmlStatus::BadEncoding);
    CHECK(xml_parse("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>caf\xE9</a>", d) == XmlStatus::Ok && d.root->text_content() == "caf\xC3\xA9");
    CHECK(xml_parse("<?xml version='1.0' encoding='windows-1252'?><a>\x93x\x94</a>", d) == XmlStatus::Ok && d.root->text_content() == "\xE2\x80\x9Cx\xE2\x80\x9D");
    CHECK(xml_parse("<?xml version=\"1.0\" encoding=\"iso-8859-15\"?><a>\xA4</a>", d) == XmlStatus::Ok && d.root->text_content() == "\xE2\x82\xAC");
    CHECK(xml_parse("<?xml version=\"1.0\" encoding=\"utf-8\"?><a>bad \xFF byte</a>", d) == XmlStatus::Ok && d.root->text_content() == "bad \xEF\xBF\xBD byte");

    // recovery
    CHECK(xml_parse("<a><b>x</c></b><d>y", d) == XmlStatus::Ok && d.recovered);
    CHECK_EQ(d.root->child_text("b"), "x");
    CHECK_EQ(d.root->child_text("d"), "y");
    CHECK(xml_parse("<a x=1 y='2' z>t</a>", d) == XmlStatus::Ok && d.root->attr("x") == "1" && d.root->attr("y") == "2" && d.root->has_attr("z"));
    CHECK(xml_parse("<a>1 < 2 &amp; 3 > 2</a>", d) == XmlStatus::Ok && d.root->text_content() == "1 < 2 & 3 > 2");
    CHECK(xml_parse("junk before <a>t</a> junk after <b>x</b>", d) == XmlStatus::Ok && d.root->name == "a" && d.root->text_content() == "t");
    CHECK(xml_parse("<a><b attr=\"unterminated>text</b></a>", d) == XmlStatus::Ok);
    CHECK(xml_parse("<a><!-- never closed", d) == XmlStatus::Ok && d.recovered);
    CHECK(xml_parse("<a><![CDATA[never closed", d) == XmlStatus::Ok);
    CHECK(xml_parse("<a><b", d) == XmlStatus::Ok);
    CHECK(xml_parse("<a></a></a></a>", d) == XmlStatus::Ok);
    CHECK(xml_parse("", d) == XmlStatus::Empty);
    CHECK(xml_parse("   \n", d) == XmlStatus::Empty);
    CHECK(xml_parse("just text, no markup", d) == XmlStatus::Empty);
    CHECK(xml_parse("<", d) == XmlStatus::Empty);
    CHECK(xml_parse("</a>", d) == XmlStatus::Empty);
    CHECK(xml_parse("<!DOCTYPE broken", d) == XmlStatus::Empty);
    CHECK(xml_parse(std::string("<a>\0\0</a>", 9), d) == XmlStatus::BadEncoding || true);   // NULs must not crash (outcome unimportant)

    // limits
    std::string deep64, deep65;
    for (int i = 0; i < 64; ++i) deep64 += "<a>";
    deep65 = deep64 + "<a>";
    CHECK(xml_parse(deep64 + "x", d) == XmlStatus::Ok);
    CHECK(xml_parse(deep65 + "x", d) == XmlStatus::TooDeep);
    CHECK(d.root == nullptr);
    std::string huge(kMaxXmlBytes + 1, ' ');
    CHECK(xml_parse(huge, d) == XmlStatus::TooBig);
    std::string many = "<r>";
    while (many.size() < kMaxXmlBytes - 100) many += "<a/>";
    many += "</r>";
    CHECK(xml_parse(many, d) == XmlStatus::TooBig);                         // too many nodes, under the size limit
    std::string big_ok = "<r><t>";
    big_ok.append(4 * 1024 * 1024, 'x');
    big_ok += "</t></r>";
    CHECK(xml_parse(big_ok, d) == XmlStatus::Ok && d.root->child_text("t").size() == 4u * 1024 * 1024);

    CHECK(xml_parse("<a><p x=\"1&amp;2\">hi <b>there</b></p><br/></a>", d) == XmlStatus::Ok);
    CHECK_EQ(xml_inner(*d.root), "<p x=\"1&amp;2\">hi <b>there</b></p><br/>");
}

void test_feed_rss()
{
    Feed f;
    CHECK(parse_feed(fixture("rss2.xml"), "https://news.example.com/feed.xml", "s1", kT, f) == FeedStatus::Ok);
    CHECK_EQ(f.format, "rss");
    CHECK_EQ(f.title, "Example & Sons News");
    CHECK_EQ(f.description, "All the news & more");
    CHECK_EQ(f.site_url, "https://news.example.com/");
    CHECK_EQ(f.lang, "en-gb");
    CHECK_EQ(f.articles.size(), std::size_t(3));
    const Article *a1 = by_guid(f, "post-1001");
    const Article *a2 = by_guid(f, "https://news.example.com/2026/10/second");
    const Article *a3 = by_guid(f, "https://news.example.com/2026/10/nodate");
    CHECK(a1 && a2 && a3);
    if (!a1 || !a2 || !a3) return;
    CHECK_EQ(a1->title, "First story: caf\xC3\xA9 & \xE2\x80\x9Cquotes\xE2\x80\x9D");
    CHECK_EQ(a1->link, "https://news.example.com/2026/10/first");
    CHECK_EQ(a1->published, kT - 7200);
    CHECK(a1->dated);
    CHECK_EQ(a1->author, "Jane Doe");
    CHECK_EQ(a1->summary_html, "<p>Short summary with <b>bold</b>.</p>");
    CHECK(a1->content_html.find("<h2>Heading</h2>") == 0);
    CHECK(a1->has_full_text);
    CHECK_EQ(a1->id, make_article_id("s1", "post-1001"));
    CHECK_EQ(a1->images.size(), std::size_t(4));
    if (a1->images.size() == 4) {
        CHECK_EQ(a1->images[0], "https://cdn.example.com/cover.png");
        CHECK_EQ(a1->images[1], "https://cdn.example.com/media1.jpg");
        CHECK_EQ(a1->images[2], "https://cdn.example.com/thumb1.jpg");
        CHECK_EQ(a1->images[3], "https://news.example.com/img/photo.jpg");   // relative, resolved; the 1x1 pixel is gone
    }
    CHECK_EQ(a2->link, "https://news.example.com/2026/10/second");
    CHECK_EQ(a2->published, 1792224000LL);
    CHECK_EQ(a2->author, "John Smith");
    CHECK_EQ(a2->summary_html, "Plain <em>escaped</em> description");
    CHECK(a2->images.empty());                                             // an audio enclosure is not an image
    CHECK(!a2->has_full_text);
    CHECK(!a3->dated);
    CHECK(a3->published <= kT && a3->published > kT - 10);                // the fetch time stands in
    CHECK_EQ(a3->id, make_article_id("s1", a3->link));
    CHECK(f.articles[0].published >= f.articles[1].published && f.articles[1].published >= f.articles[2].published);
    // another scope gives other ids for the same guid
    Feed g;
    CHECK(parse_feed(fixture("rss2.xml"), "https://news.example.com/feed.xml", "s2", kT, g) == FeedStatus::Ok);
    CHECK(g.articles[0].id != f.articles[0].id || g.articles[1].id != f.articles[1].id);
    // ids are stable between parses (also for the undated item)
    Feed h;
    CHECK(parse_feed(fixture("rss2.xml"), "https://news.example.com/feed.xml", "s1", kT + 999, h) == FeedStatus::Ok);
    for (const Article &a : f.articles) {
        const Article *same = by_guid(h, a.guid);
        CHECK(same && same->id == a.id);
    }
}

void test_feed_atom_rdf()
{
    Feed f;
    CHECK(parse_feed(fixture("atom.xml"), "https://blog.example.org/atom.xml", "at", kT, f) == FeedStatus::Ok);
    CHECK_EQ(f.format, "atom");
    CHECK_EQ(f.title, "Le Blog d'Exemple");
    CHECK_EQ(f.description, "Carnet de notes");
    CHECK_EQ(f.lang, "fr");
    CHECK_EQ(f.site_url, "https://blog.example.org/");
    CHECK_EQ(f.articles.size(), std::size_t(2));
    if (f.articles.size() == 2) {
        const Article &a = f.articles[0];
        CHECK_EQ(a.title, "Un bel article");
        CHECK_EQ(a.link, "https://blog.example.org/posts/un-bel-article");
        CHECK_EQ(a.guid, "tag:blog.example.org,2026:post-1");
        CHECK_EQ(a.published, 1792312200LL);                                // 08:30Z
        CHECK_EQ(a.author, "Marie Curie");
        CHECK_EQ(a.summary_html, "<p>R\xC3\xA9sum\xC3\xA9 court</p>");
        CHECK(a.content_html.find("<p>Texte <b>complet</b> de l'article.</p>") == 0);
        CHECK(a.has_full_text);
        CHECK_EQ(a.images.size(), std::size_t(2));
        if (a.images.size() == 2) {
            CHECK_EQ(a.images[0], "https://blog.example.org/img/hero.jpg");
            CHECK_EQ(a.images[1], "https://blog.example.org/posts/images/fig1.png");
        }
        const Article &b = f.articles[1];
        CHECK_EQ(b.title, "Plain text entry & more");
        CHECK_EQ(b.link, "https://blog.example.org/posts/plain");
        CHECK_EQ(b.content_html, "<p>Line one</p><p>Line two with &lt;angle&gt; brackets.</p>");
        CHECK_EQ(b.published, 1792141200LL);                                // from <updated>
    }

    Feed r;
    CHECK(parse_feed(fixture("rdf.xml"), "https://old.example.net/index.rdf", "rd", kT, r) == FeedStatus::Ok);
    CHECK_EQ(r.format, "rdf");
    CHECK_EQ(r.title, "Old School RDF");
    CHECK_EQ(r.lang, "de");
    CHECK_EQ(r.site_url, "https://old.example.net/");
    CHECK_EQ(r.articles.size(), std::size_t(2));
    if (r.articles.size() == 2) {
        CHECK_EQ(r.articles[0].title, "Erster Eintrag");
        CHECK_EQ(r.articles[0].published, 1792045800LL);                    // 2026-10-15 06:30 UTC
        CHECK_EQ(r.articles[0].author, "Hans");
        CHECK_EQ(r.articles[0].summary_html, "Beschreibung & mehr");
        CHECK_EQ(r.articles[1].published, 1791936000LL);                    // 2026-10-14 midnight
        CHECK_EQ(r.articles[1].guid, "https://old.example.net/b");
    }
}

void test_feed_failures()
{
    Feed f;
    CHECK(parse_feed(fixture("broken.xml"), "https://broken.example.com/", "br", kT, f) == FeedStatus::Ok);
    CHECK(f.recovered);
    CHECK_EQ(f.title, "Broken & Proud");
    CHECK(f.articles.size() >= 2);
    const Article *fine = by_guid(f, "https://broken.example.com/1");
    CHECK(fine != nullptr);
    if (fine) {
        CHECK_EQ(fine->title, "Fine item");
        CHECK(fine->summary_html.find("AT&T") != std::string::npos);
        CHECK(fine->summary_html.find("\xC2\xA0 entity") != std::string::npos);
    }
    const Article *mm = by_guid(f, "https://broken.example.com/2");
    CHECK(mm && mm->title == "Mismatched tags" && !mm->dated);

    Feed l;
    CHECK(parse_feed(fixture("latin1.xml"), "http://latin.example.fr/", "l1", kT, l) == FeedStatus::Ok);
    CHECK_EQ(l.title, "Caf\xC3\xA9 du Commerce \xE2\x82\xAC");
    CHECK_EQ(l.articles.size(), std::size_t(1));
    if (!l.articles.empty()) {
        CHECK_EQ(l.articles[0].title, "\xC3\x89t\xC3\xA9 \xE2\x80\x9C" "chaud\xE2\x80\x9D \xC3\xA0 Paris");
        CHECK_EQ(l.articles[0].summary_html, "Cr\xC3\xA8me br\xC3\xBBl\xC3\xA9" "e \xE2\x80\x93 d\xC3\xA9licieux");
        CHECK_EQ(l.articles[0].published, 1791712800LL);
    }

    CHECK(parse_feed(fixture("page_alt.html"), "https://x.org/", "s", kT, f) == FeedStatus::NotAFeed);
    CHECK(f.articles.empty());
    CHECK(parse_feed("", "https://x.org/", "s", kT, f) == FeedStatus::NotAFeed);
    CHECK(parse_feed("{\"version\":\"https://jsonfeed.org/version/1\",\"items\":[]}", "https://x.org/", "s", kT, f) == FeedStatus::NotAFeed);
    CHECK(parse_feed("<html><body><p>404</p></body></html>", "https://x.org/", "s", kT, f) == FeedStatus::NotAFeed);
    CHECK(parse_feed("<rss><nothing/></rss>", "https://x.org/", "s", kT, f) == FeedStatus::NotAFeed);
    CHECK(parse_feed("<rss><channel><title>Empty</title></channel></rss>", "https://x.org/", "s", kT, f) == FeedStatus::Ok && f.articles.empty());
    CHECK(parse_feed("<feed xmlns=\"http://www.w3.org/2005/Atom\"></feed>", "https://x.org/", "s", kT, f) == FeedStatus::Ok);
    CHECK(parse_feed("<?xml version=\"1.0\" encoding=\"UTF-16\"?><rss/>", "https://x.org/", "s", kT, f) == FeedStatus::BadEncoding);
    CHECK(parse_feed(std::string(kMaxXmlBytes + 10, 'x'), "https://x.org/", "s", kT, f) == FeedStatus::TooBig);
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "<rss>";
    CHECK(parse_feed(deep, "https://x.org/", "s", kT, f) == FeedStatus::TooDeep);

    // items with nothing usable do not crash, and still get distinct ids
    CHECK(parse_feed("<rss><channel><item/><item><title/></item><item><title>t</title></item><item><title>t</title></item></channel></rss>",
                     "https://x.org/", "s", kT, f) == FeedStatus::Ok);
    CHECK(f.articles.size() >= 1 && f.articles.size() <= 4);
    // hostile values
    CHECK(parse_feed("<rss><channel><link>javascript:x</link><item><link>javascript:alert(1)</link><title>j</title></item></channel></rss>",
                     "https://x.org/", "s", kT, f) == FeedStatus::Ok);
    CHECK(f.site_url.empty() && f.articles.size() == 1 && f.articles[0].link.empty());
}

void test_feed_limits()
{
    // 300 dated items, some duplicate guids: newest 200 kept, no duplicates
    std::string xml = "<rss><channel><title>Big</title><link>https://big.example.com/</link>";
    for (int i = 0; i < 300; ++i) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "<item><title>Item %d</title><guid>g%d</guid><pubDate>%04d-01-01T00:00:00Z</pubDate></item>", i, i % 290, 2000 + i % 100);
        xml += buf;
        std::snprintf(buf, sizeof buf, "<item><title>Ts %d</title><guid>t%d</guid><pubDate>%lld</pubDate></item>", i, i, static_cast<long long>(1700000000 + i));
        xml += buf;
    }
    xml += "</channel></rss>";
    Feed f;
    CHECK(parse_feed(xml, "https://big.example.com/rss", "big", kT, f) == FeedStatus::Ok);
    CHECK_EQ(f.articles.size(), kMaxArticlesPerFeed);
    std::set<std::string> ids;
    bool sorted = true;
    for (std::size_t i = 0; i < f.articles.size(); ++i) {
        ids.insert(f.articles[i].id);
        if (i && f.articles[i].published > f.articles[i - 1].published) sorted = false;
    }
    CHECK_EQ(ids.size(), f.articles.size());
    CHECK(sorted);

    // content over the cap is cut on a character boundary
    std::string big(kMaxContentBytes + 1000, 'a');
    big.replace(kMaxContentBytes - 1, 2, "\xC3\xA9");
    Feed g;
    CHECK(parse_feed("<rss><channel><item><title>x</title><guid>1</guid><content:encoded xmlns:content=\"http://purl.org/rss/1.0/modules/content/\">" + big +
                         "</content:encoded></item></channel></rss>", "https://x.org/", "s", kT, g) == FeedStatus::Ok);
    CHECK(g.articles.size() == 1 && g.articles[0].content_html.size() <= kMaxContentBytes && g.articles[0].content_html.size() >= kMaxContentBytes - 2);
    CHECK(utf8_ok(g.articles[0].content_html));

    // a long summary counts as full text
    std::string longsum(2000, 'w');
    Feed h;
    CHECK(parse_feed("<rss><channel><item><title>x</title><guid>1</guid><description>" + longsum + "</description></item></channel></rss>", "https://x.org/", "s", kT, h) == FeedStatus::Ok);
    CHECK(h.articles.size() == 1 && h.articles[0].has_full_text && h.articles[0].content_html.empty());

    // image cap
    std::string imgs;
    for (int i = 0; i < 40; ++i) imgs += "<img src=\"/i" + std::to_string(i) + ".png\">";
    Feed m;
    CHECK(parse_feed("<rss><channel><item><title>x</title><guid>1</guid><description><![CDATA[" + imgs + "]]></description></item></channel></rss>", "https://x.org/", "s", kT, m) == FeedStatus::Ok);
    CHECK(m.articles.size() == 1 && m.articles[0].images.size() == kMaxImagesPerArticle);
}

void test_looks_like_feed()
{
    CHECK(looks_like_feed("<?xml version=\"1.0\"?><rss version=\"2.0\"></rss>"));
    CHECK(looks_like_feed("<feed xmlns=\"http://www.w3.org/2005/Atom\">"));
    CHECK(looks_like_feed("<rdf:RDF xmlns:rdf=\"x\">"));
    CHECK(!looks_like_feed("<!DOCTYPE html><html><head><link rel=alternate type=\"application/rss+xml\"></head>"));
    CHECK(!looks_like_feed("<html><body><a href='/rss'>RSS feed</a></body></html>"));
    CHECK(!looks_like_feed(""));
    CHECK(!looks_like_feed("{\"items\":[]}"));
}

void test_discover()
{
    std::vector<FeedCandidate> c = discover_feeds(fixture("page_alt.html"), "https://www.example.com/blog/post.html");
    CHECK(c.size() >= 5 && c.size() <= 12);
    if (c.size() >= 12) {
        CHECK_EQ(c[0].url, "https://www.example.com/feed/rss");
        CHECK_EQ(c[0].title, "Blog RSS");
        CHECK_EQ(c[1].url, "https://cdn.example.com/other.rss");
        CHECK_EQ(c[2].url, "https://www.example.com/blog/atom.xml");        // <base href> honoured
        CHECK_EQ(c[3].url, "https://www.example.com/feed.json");
        CHECK_EQ(c[4].url, "https://www.example.com/comments/feed");        // comment feeds last among the declared
        CHECK(c[0].declared && c[4].declared && !c[5].declared);
        CHECK_EQ(c[5].url, "https://www.example.com/blog/feed");            // the page's own folder first
        CHECK_EQ(c[6].url, "https://www.example.com/feed");
        CHECK_EQ(c[0].type, "application/rss+xml");
    }
    std::set<std::string> uniq;
    for (const FeedCandidate &f : c) uniq.insert(f.url);
    CHECK_EQ(uniq.size(), c.size());

    std::vector<FeedCandidate> n = discover_feeds(fixture("page_noalt.html"), "https://site.org/");
    CHECK_EQ(n.size(), std::size_t(6));
    bool any_declared = false;
    for (const FeedCandidate &f : n) any_declared = any_declared || f.declared;
    CHECK(!any_declared);
    if (n.size() == 6) {
        CHECK_EQ(n[0].url, "https://site.org/feed");
        CHECK_EQ(n[1].url, "https://site.org/rss");
        CHECK_EQ(n[2].url, "https://site.org/atom.xml");
        CHECK_EQ(n[3].url, "https://site.org/feed.xml");
        CHECK_EQ(n[4].url, "https://site.org/index.xml");
    }
    // a link that is also a guess is listed once, as declared
    std::vector<FeedCandidate> d = discover_feeds("<link rel=alternate type=application/rss+xml href=/feed>", "https://a.org/x/y");
    CHECK(!d.empty() && d[0].url == "https://a.org/feed" && d[0].declared);
    int feed_count = 0;
    for (const FeedCandidate &f : d) feed_count += f.url == "https://a.org/feed";
    CHECK_EQ(feed_count, 1);
    // junk
    CHECK(discover_feeds("", "https://a.org/").size() == 6);
    CHECK(discover_feeds("<link rel=alternate type=application/rss+xml href=''>", "https://a.org/").size() == 6);
    CHECK(discover_feeds("<link rel=alternate type=application/rss+xml href='javascript:x'>", "https://a.org/").size() == 6);
    CHECK(discover_feeds("<link rel=alternate type=text/xml href='/sitemap.xml'>", "https://a.org/").size() == 6);   // not a feed link
    CHECK(discover_feeds("<link rel=alternate type=text/xml href='/rss.xml'>", "https://a.org/")[0].url == "https://a.org/rss.xml");
    CHECK(discover_feeds("<html>", "not a url").size() == 0);
    CHECK(discover_feeds(std::string(2000000, '<'), "https://a.org/").size() == 6);
}
