// util, url, json and date parsing.
#include "test_common.hpp"

#include "datetime.hpp"
#include "json.hpp"
#include "url.hpp"
#include "util.hpp"

using namespace rssreader;
using rssreader_test::show;

void test_util()
{
    CHECK_EQ(trim("  a b \n"), "a b");
    CHECK_EQ(trim(""), "");
    CHECK_EQ(lower("AbC-É"), "abc-É");
    CHECK(iequals("RSS", "rss"));
    CHECK(!iequals("rss", "rs"));
    CHECK(starts_with("hello", "he") && !starts_with("he", "hello") && ends_with("hello", "llo"));

    CHECK_EQ(decode_entities("a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;"), "a & b <c> \"d\" 'e'");
    CHECK_EQ(decode_entities("&#233;&#xE9;&#X00e9;"), "\xC3\xA9\xC3\xA9\xC3\xA9");
    CHECK_EQ(decode_entities("&eacute;&nbsp;&hellip;&mdash;"), "\xC3\xA9\xC2\xA0\xE2\x80\xA6\xE2\x80\x94");
    CHECK_EQ(decode_entities("&#150;"), "\xE2\x80\x93");                      // cp1252 en dash
    CHECK_EQ(decode_entities("AT&T &unknown; & &#; &#xZZ; &amp"), "AT&T &unknown; & &#; &#xZZ; &amp");
    CHECK_EQ(decode_entities("&#0;&#1114112;&#xD800;"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK_EQ(decode_entities("&#99999999999999999999;"), "&#99999999999999999999;");   // too long to be a reference
    CHECK_EQ(decode_entities("&amp;amp;"), "&amp;");                          // one level only
    CHECK_EQ(html_escape("<a href=\"x\">&</a>"), "&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;");

    CHECK_EQ(utf8_sanitize("ok \xC3\xA9"), "ok \xC3\xA9");
    CHECK_EQ(utf8_sanitize("a\xFF" "b"), "a\xEF\xBF\xBD" "b");
    CHECK_EQ(utf8_sanitize("\xC0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD");           // overlong NUL
    CHECK_EQ(utf8_sanitize("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");   // surrogate
    CHECK_EQ(utf8_sanitize("trunc\xE2\x82"), "trunc\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK_EQ(truncate_utf8("ab\xC3\xA9" "cd", 3), "ab");                       // does not cut the e-acute in two
    CHECK_EQ(truncate_utf8("abc", 10), "abc");

    CHECK_EQ(latin1_to_utf8("caf\xE9", false), "caf\xC3\xA9");
    CHECK_EQ(latin1_to_utf8("\x80", true), "\xE2\x82\xAC");
    CHECK_EQ(latin1_to_utf8("\xA4", false, false), "\xC2\xA4");
    CHECK_EQ(latin1_to_utf8("\xA4", false, true), "\xE2\x82\xAC");          // ISO-8859-15 euro
    CHECK_EQ(latin1_to_utf8("\x93", false, false), "\xC2\x93");

    CHECK_EQ(collapse_ws("  a \t\n b\xC2\xA0 c  "), "a b c");
    CHECK_EQ(collapse_ws("a\xE2\x80\x8B" "b"), "ab");                          // zero-width space dropped
    CHECK_EQ(collapse_ws("\xEF\xBB\xBF" "x"), "x");
    CHECK_EQ(collapse_ws(""), "");

    CHECK_EQ(fold_ascii("\xC3\x89ly" "s\xC3\xA9" "e"), "elysee");
    CHECK_EQ(fold_ascii("Stra\xC3\x9F" "e"), "strase");
    CHECK_EQ(fold_ascii("\xE6\x97\xA5" "A"), "\xE6\x97\xA5" "a");

    CHECK_EQ(hash16("a").size(), std::size_t(16));
    CHECK(hash16("a") != hash16("b"));
    CHECK_EQ(hash16("a"), hash16("a"));
    CHECK_EQ(safe_name("a/b c"), "a_b_c");
    CHECK_EQ(safe_name(""), "_");
    CHECK(safe_name("../x").find('/') == std::string::npos && safe_name("../x")[0] != '.');
}

void test_url()
{
    const std::string b = "https://www.example.com/a/b/page.html?x=1#top";
    CHECK_EQ(resolve_url(b, "other.html"), "https://www.example.com/a/b/other.html");
    CHECK_EQ(resolve_url(b, "../c.png"), "https://www.example.com/a/c.png");
    CHECK_EQ(resolve_url(b, "../../../../c.png"), "https://www.example.com/c.png");
    CHECK_EQ(resolve_url(b, "/root.png"), "https://www.example.com/root.png");
    CHECK_EQ(resolve_url(b, "//cdn.example.org/x.js"), "https://cdn.example.org/x.js");
    CHECK_EQ(resolve_url(b, "http://other.org/p?q=1#f"), "http://other.org/p?q=1");
    CHECK_EQ(resolve_url(b, "?y=2"), "https://www.example.com/a/b/page.html?y=2");
    CHECK_EQ(resolve_url(b, "#frag"), "https://www.example.com/a/b/page.html?x=1");
    CHECK_EQ(resolve_url(b, "./x/./y"), "https://www.example.com/a/b/x/y");
    CHECK_EQ(resolve_url(b, "  img/\na.png "), "https://www.example.com/a/b/img/a.png");
    CHECK_EQ(resolve_url(b, "javascript:alert(1)"), "");
    CHECK_EQ(resolve_url(b, "data:image/png;base64,AAAA"), "");
    CHECK_EQ(resolve_url(b, "mailto:a@b.c"), "");
    CHECK_EQ(resolve_url(b, "ftp://x.org/f"), "");
    CHECK_EQ(resolve_url(b, ""), "");
    CHECK_EQ(resolve_url("", "rel.png"), "");
    CHECK_EQ(resolve_url("not a url", "rel.png"), "");
    CHECK_EQ(resolve_url("https://host.org", "x"), "https://host.org/x");
    CHECK_EQ(resolve_url("https://host.org", "http://a.b"), "http://a.b/");
    CHECK_EQ(resolve_url(b, "a:b/c.png"), "");                                // looks like a scheme
    CHECK_EQ(resolve_url(b, "dir/a:b.png"), "https://www.example.com/a/b/dir/a:b.png");

    CHECK(is_http_url("http://a.b") && is_http_url("https://a.b/x") && !is_http_url("ftp://a.b") && !is_http_url("a.b") && !is_http_url("https://"));
    CHECK_EQ(url_origin("https://Example.com:8080/x?y"), "https://Example.com:8080");
    CHECK_EQ(url_host("https://User@Example.COM:8080/x"), "example.com");
    CHECK_EQ(url_host("https://[::1]:80/x"), "[::1]");
    CHECK_EQ(url_path("https://a.b"), "/");
    CHECK_EQ(url_path("https://a.b/x/y?q"), "/x/y");

    CHECK_EQ(normalize_user_url("example.com"), "https://example.com");
    CHECK_EQ(normalize_user_url("  example.com/blog "), "https://example.com/blog");
    CHECK_EQ(normalize_user_url("feed://example.com/rss"), "https://example.com/rss");
    CHECK_EQ(normalize_user_url("feed:https://example.com/rss"), "https://example.com/rss");
    CHECK_EQ(normalize_user_url("http://example.com/rss"), "http://example.com/rss");
    CHECK_EQ(normalize_user_url("ftp://example.com/rss"), "");
    CHECK_EQ(normalize_user_url("nodots"), "");
    CHECK_EQ(normalize_user_url("a b.com"), "");
    CHECK_EQ(normalize_user_url(""), "");
    CHECK_EQ(normalize_user_url("https://a.com/x\ny"), "");
}

void test_json()
{
    Json j;
    CHECK(json_parse("{\"a\":1,\"b\":[true,false,null,\"x\"],\"c\":{\"d\":-2.5e1}}", j));
    CHECK_EQ(j.get_int("a"), 1);
    CHECK(j.get("b") && j.get("b")->arr.size() == 4 && j.get("b")->arr[0].b);
    CHECK_EQ(j.get("c")->get_double("d"), -25.0);
    CHECK_EQ(j.get_int("missing", 7), 7);
    CHECK_EQ(j.get_string("a", "dflt"), "dflt");                              // wrong type -> default

    CHECK(json_parse("\xEF\xBB\xBF{\"a\":1,}", j));                           // BOM + trailing comma
    CHECK(json_parse("[1,2,]", j) && j.arr.size() == 2);
    CHECK(json_parse("// c\n{/* c */\"a\":1}", j));
    CHECK(json_parse("\"\\u00e9\\ud83d\\ude00\\n\\/\"", j) && j.str == "\xC3\xA9\xF0\x9F\x98\x80\n/");
    CHECK(json_parse("\"\\ud800x\"", j));                                    // lone surrogate: replaced, not fatal
    CHECK(json_parse("  42  ", j) && j.num == 42);

    CHECK(!json_parse("", j));
    CHECK(!json_parse("{", j));
    CHECK(!json_parse("{\"a\":}", j));
    CHECK(!json_parse("{\"a\":1} x", j));                                     // trailing garbage
    CHECK(!json_parse("{'a':1}", j));
    CHECK(!json_parse("[1 2]", j));
    CHECK(!json_parse("\"unterminated", j));
    CHECK(!json_parse("nul", j));
    CHECK(!json_parse("-", j));
    CHECK(!json_parse("1e999", j));
    std::string deep(200, '[');
    CHECK(!json_parse(deep, j));
    std::string ok_deep = std::string(60, '[') + std::string(60, ']');
    CHECK(json_parse(ok_deep, j));

    Json o = Json::object();
    o.set("s", Json::string("q\"\\\n\t\x01"));
    o.set("n", Json::number(123456789012.0));
    o.set("f", Json::number(0.5));
    Json a = Json::array();
    a.push(Json::boolean(true));
    a.push(Json::null());
    o.set("a", a);
    o.set("s", Json::string("replaced"));
    std::string text = json_dump(o);
    CHECK_EQ(text, "{\"s\":\"replaced\",\"n\":123456789012,\"f\":0.5,\"a\":[true,null]}");
    Json back;
    CHECK(json_parse(json_dump(o, true), back) && back.get_string("s") == "replaced" && back.get_int("n") == 123456789012LL);
    CHECK_EQ(json_quote("a\x01"), "\"a\\u0001\"");
    Json ctl;
    CHECK(json_parse(json_quote(std::string("x\x01\x1f y")), ctl) && ctl.str == std::string("x\x01\x1f y"));
}

struct DateCase {
    const char *text;
    long long epoch;      // -1 = must fail
};

void test_dates()
{
    // 2026-10-18 15:10:05 UTC = 1792336205
    const long long t = 1792336205LL;
    CHECK_EQ(days_from_civil(1970, 1, 1), 0);
    CHECK_EQ(days_from_civil(2000, 3, 1), 11017);
    CHECK_EQ(format_utc(t), "2026-10-18 15:10");
    CHECK_EQ(format_utc(0), "1970-01-01 00:00");

    const DateCase cases[] = {
        {"Sun, 18 Oct 2026 15:10:05 GMT", t},
        {"Sun, 18 Oct 2026 15:10:05 +0000", t},
        {"Sun, 18 Oct 2026 17:10:05 +0200", t},
        {"Sun, 18 Oct 2026 10:10:05 -0500", t},
        {"Sun, 18 Oct 2026 10:10:05 EST", t},
        {"Sun, 18 Oct 2026 11:10:05 EDT", t},
        {"Sun, 18 Oct 2026 17:10:05 CEST", t},
        {"Sun, 18 Oct 2026 16:10:05 CET", t},
        {"Sun, 18 Oct 2026 17:10:05 +02:00", t},
        {"Wrong, 18 Oct 2026 15:10:05 GMT", t},              // wrong weekday is ignored
        {"18 Oct 2026 15:10:05 GMT", t},                     // no weekday
        {"18 Oct 2026 15:10:05", t},                         // no zone: UTC
        {"Sun, 18 Oct 2026 15:10 GMT", t - 5},               // no seconds
        {"Sun, 18 Oct 26 15:10:05 GMT", t},                  // two-digit year
        {"Sun, 18 October 2026 15:10:05 GMT", t},
        {"Sun, 18 Oct 2026 15:10:05 UT", t},
        {"Sunday, 18-Oct-2026 15:10:05 GMT", t},             // RFC 850-ish
        {"Oct 18, 2026 3:10:05 PM", t},
        {"Oct 18, 2026 12:00:00 AM", 1792281600LL},
        {"2026-10-18T15:10:05Z", t},
        {"2026-10-18T15:10:05z", t},
        {"2026-10-18T15:10:05+00:00", t},
        {"2026-10-18T17:10:05+02:00", t},
        {"2026-10-18T17:10:05+0200", t},
        {"2026-10-18T17:10:05+02", t},
        {"2026-10-18T10:10:05-05:00", t},
        {"2026-10-18T15:10:05.123Z", t},
        {"2026-10-18T15:10:05,5Z", t},
        {"2026-10-18 15:10:05", t},
        {"2026-10-18 15:10:05 UTC", t},
        {"2026-10-18T15:10Z", t - 5},
        {"2026-10-18", 1792281600LL},
        {"20261018", 1792281600LL},
        {"20261018T151005Z", t},
        {"1792336205", t},
        {"1792336205000", t},
        {"  2026-10-18T15:10:05Z  ", t},
        {"Thu, 01 Jan 1970 00:00:00 GMT", 0},
        {"Fri, 29 Feb 2028 12:00:00 GMT", 1835438400LL},
        {"", -1},
        {"garbage", -1},
        {"yesterday", -1},
        {"2026-13-01T00:00:00Z", -1},
        {"2026-02-30", -1},
        {"2027-02-29", -1},
        {"2026-10-18T25:00:00Z", -1},
        {"2026-10-18T10:61:00Z", -1},
        {"32 Oct 2026 10:00:00 GMT", -1},
        {"Sun, 18 Foo 2026 15:10:05 GMT", -1},
        {"Sun, 18 Oct 1969 15:10:05 GMT", -1},
        {"Sun, 18 Oct 2200 15:10:05 GMT", -1},
        {"0000-00-00 00:00:00", -1},
        {"12345", -1},
        {"Oct 2026", -1},
    };
    for (const DateCase &c : cases) {
        std::int64_t e = -1;
        bool ok = parse_date(c.text, e);
        if (c.epoch < 0) {
            CHECK(!ok);
            if (ok) std::printf("   unexpected success for [%s] -> %lld\n", c.text, static_cast<long long>(e));
        } else {
            CHECK(ok && e == c.epoch);
            if (!ok || e != c.epoch) std::printf("   date [%s] -> %s %lld, want %lld\n", c.text, ok ? "ok" : "FAIL", static_cast<long long>(e), c.epoch);
        }
    }
    std::string longs(500, '1');
    std::int64_t e;
    CHECK(!parse_date(longs, e));
}
