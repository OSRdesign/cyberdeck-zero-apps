// fetch: curl argv, header parsing, image fetching through a fake client, and the real CurlClient against a local server.
#include "test_common.hpp"

#include "fetch.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

using namespace rssreader;
using namespace rssreader_test;

static bool has(const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); }
static std::size_t index_of(const std::vector<std::string> &v, const std::string &s)
{
    return static_cast<std::size_t>(std::find(v.begin(), v.end(), s) - v.begin());
}

static const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(40, '\0');

void test_curl_argv()
{
    HttpRequest r;
    r.url = "https://example.com/feed?a=1&b=$(rm -rf);`id`";   // contains a space: refused
    CHECK(build_curl_argv(r, "/tmp/h", "UA").empty());
    r.url = "https://example.com/feed?a=1&b=$(id)`id`;|&>";
    std::vector<std::string> a = build_curl_argv(r, "/tmp/h", "");
    CHECK(!a.empty());
    CHECK_EQ(a.empty() ? std::string() : a.back(), r.url);                  // one argument, untouched, no shell
    CHECK_EQ(a[0], "curl");
    CHECK_EQ(a[1], "-q");                                                   // ignore ~/.curlrc
    CHECK(has(a, "--url") && a[a.size() - 2] == "--url");
    CHECK(has(a, "--compressed") && has(a, "--location"));
    CHECK_EQ(a[index_of(a, "--max-redirs") + 1], "5");
    CHECK_EQ(a[index_of(a, "--proto") + 1], "=http,https");
    CHECK_EQ(a[index_of(a, "--proto-redir") + 1], "=http,https");
    CHECK_EQ(a[index_of(a, "--connect-timeout") + 1], "8");
    CHECK_EQ(a[index_of(a, "--max-time") + 1], "15");
    CHECK_EQ(a[index_of(a, "--max-filesize") + 1], "5242880");
    CHECK_EQ(a[index_of(a, "--user-agent") + 1], std::string("CyberdeckZero-RSS/") + kVersion);
    CHECK_EQ(a[index_of(a, "--dump-header") + 1], "/tmp/h");
    CHECK_EQ(a[index_of(a, "--output") + 1], "-");
    CHECK(has(a, "--globoff"));
    CHECK(!has(a, "If-None-Match: x"));

    r.etag = "\"abc\"";
    r.last_modified = "Sun, 18 Oct 2026 15:00:00 GMT";
    r.max_bytes = kMaxImageBytes;
    r.timeout_ms = 3500;
    a = build_curl_argv(r, "/tmp/h", "My UA/1.0", "/usr/bin/curl");
    CHECK_EQ(a[0], "/usr/bin/curl");
    CHECK(has(a, "If-None-Match: \"abc\"") && has(a, "If-Modified-Since: Sun, 18 Oct 2026 15:00:00 GMT"));
    CHECK_EQ(a[index_of(a, "--max-time") + 1], "4");                        // rounded up to whole seconds
    CHECK_EQ(a[index_of(a, "--max-filesize") + 1], "2097152");
    CHECK_EQ(a[index_of(a, "--user-agent") + 1], "My UA/1.0");

    // header injection through the validators is refused (the header is left out)
    r.etag = "x\r\nX-Evil: 1";
    r.last_modified = "y\nz";
    a = build_curl_argv(r, "/tmp/h", "");
    bool evil = false;
    for (const std::string &s : a) evil = evil || s.find("X-Evil") != std::string::npos || s.find("If-None-Match") == 0 || s.find("If-Modified-Since") == 0;
    CHECK(!evil);

    for (const char *bad : {"", "file:///etc/passwd", "ftp://x.org/f", "gopher://x", "https://", "x.org", "-o /etc/passwd", "https://a.b/\nx", "https://a.b/\x7f"}) {
        HttpRequest q;
        q.url = bad;
        CHECK(build_curl_argv(q, "/tmp/h", "").empty());
    }
}

void test_response_headers()
{
    HttpResponse r;
    parse_response_headers("HTTP/1.1 301 Moved\r\nLocation: /x\r\nETag: \"old\"\r\n\r\n"
                           "HTTP/2 200\r\ncontent-type: Text/XML; charset=\"UTF-8\"\r\nETag: W/\"new\"\r\nLast-Modified: Sun, 18 Oct 2026 15:00:00 GMT\r\n\r\n", r);
    CHECK_EQ(r.etag, "W/\"new\"");                                         // the last block wins
    CHECK_EQ(r.last_modified, "Sun, 18 Oct 2026 15:00:00 GMT");
    CHECK_EQ(r.content_type, "text/xml");
    CHECK_EQ(r.charset, "utf-8");

    HttpResponse e;
    parse_response_headers("", e);
    CHECK(e.etag.empty() && e.content_type.empty());
    parse_response_headers("garbage without colon\nanother", e);
    CHECK(e.etag.empty());
    HttpResponse c;
    parse_response_headers("HTTP/1.1 200 OK\nContent-Type: application/atom+xml;charset=iso-8859-1;x=y\n", c);
    CHECK_EQ(c.content_type, "application/atom+xml");
    CHECK_EQ(c.charset, "iso-8859-1");
    HttpResponse l;
    parse_response_headers("HTTP/1.1 200 OK\r\nETag: " + std::string(1000, 'x') + "\r\n\r\n", l);
    CHECK_EQ(l.etag.size(), std::size_t(256));                              // bounded
}

void test_image_fetch()
{
    CHECK_EQ(sniff_image_type(kPng), "image/png");
    CHECK_EQ(sniff_image_type("\xFF\xD8\xFF\xE0xxxx"), "image/jpeg");
    CHECK_EQ(sniff_image_type("GIF89a......"), "image/gif");
    CHECK_EQ(sniff_image_type(std::string("RIFF\x10\0\0\0WEBPVP8 ", 16)), "image/webp");
    CHECK_EQ(sniff_image_type(std::string("BM") + std::string(40, '\0')), "image/bmp");
    CHECK_EQ(sniff_image_type("<html>"), "");
    CHECK_EQ(sniff_image_type(""), "");
    CHECK_EQ(sniff_image_type("\x89PN"), "");
    CHECK_EQ(image_extension("image/png"), ".png");
    CHECK_EQ(image_extension("IMAGE/JPEG"), ".jpg");
    CHECK_EQ(image_extension("image/webp"), ".webp");
    CHECK_EQ(image_extension("image/tiff"), ".img");

    FakeHttp http;
    http.add_ok("https://i.org/a.png", kPng, "image/png");
    http.add_ok("https://i.org/lie.jpg", kPng, "image/jpeg");                        // the bytes win over the label
    http.add_ok("https://i.org/octet", kPng, "application/octet-stream");
    http.add_ok("https://i.org/nolabel", kPng, "");
    http.add_ok("https://i.org/page", "<html>sorry</html>", "text/html");
    http.add_ok("https://i.org/svg", "<svg/>", "image/svg+xml");
    http.add_ok("https://i.org/liar", "<html>not an image</html>", "image/png");     // labelled image, but the bytes are not: kept (the decoder decides)
    http.add_ok("https://i.org/octet-text", "plain text", "application/octet-stream");
    http.add_ok("https://i.org/huge", std::string(kMaxImageBytes + 1, 'x'), "image/png");
    http.add_error("https://i.org/gone", FetchError::Http, 404);
    http.add_error("https://i.org/slow", FetchError::Timeout);

    HttpResponse r = fetch_image(http, "https://i.org/a.png");
    CHECK(r.ok() && r.content_type == "image/png" && r.body == kPng);
    CHECK_EQ(http.requests.back().max_bytes, kMaxImageBytes);
    CHECK(http.requests.back().accept.find("image/") == 0);
    r = fetch_image(http, "https://i.org/lie.jpg");
    CHECK(r.ok() && r.content_type == "image/png");
    r = fetch_image(http, "https://i.org/octet");
    CHECK(r.ok() && r.content_type == "image/png");
    r = fetch_image(http, "https://i.org/nolabel");
    CHECK(r.ok() && r.content_type == "image/png");
    r = fetch_image(http, "https://i.org/page");
    CHECK(r.error == FetchError::NotImage && r.body.empty());
    r = fetch_image(http, "https://i.org/svg");
    CHECK(r.error == FetchError::NotImage && r.body.empty());
    r = fetch_image(http, "https://i.org/liar");
    CHECK(r.ok());
    r = fetch_image(http, "https://i.org/octet-text");
    CHECK(r.error == FetchError::NotImage);
    r = fetch_image(http, "https://i.org/huge");
    CHECK(r.error == FetchError::TooLarge && r.body.empty());
    r = fetch_image(http, "https://i.org/gone");
    CHECK(r.error == FetchError::Http && r.status == 404);
    r = fetch_image(http, "https://i.org/slow");
    CHECK(r.error == FetchError::Timeout);
    std::atomic<bool> cancel{false};
    fetch_image(http, "https://i.org/a.png", &cancel);
    CHECK(http.requests.back().cancel == &cancel);

    for (FetchError e : {FetchError::None, FetchError::NotModified, FetchError::Network, FetchError::Timeout, FetchError::Tls, FetchError::Http,
                         FetchError::TooLarge, FetchError::TooManyRedirects, FetchError::BadUrl, FetchError::NotImage, FetchError::Cancelled, FetchError::Internal})
        CHECK(std::string(fetch_error_text(e)).size() > 1);
}

static HttpResponse get(CurlClient &c, const std::string &url, std::size_t max = kMaxFeedBytes, int timeout_ms = 15000, const std::string &etag = "",
                        const std::string &lm = "", const std::atomic<bool> *cancel = nullptr)
{
    HttpRequest r;
    r.url = url;
    r.max_bytes = max;
    r.timeout_ms = timeout_ms;
    r.etag = etag;
    r.last_modified = lm;
    r.cancel = cancel;
    return c.get(r);
}

void test_curl_client_offline()
{
    CurlClient missing("", "/nonexistent/curl-binary");
    CHECK(!missing.available());
    HttpResponse r = get(missing, "https://example.com/");
    CHECK(r.error == FetchError::Internal);
    CHECK(!r.error_detail.empty());
    CurlClient c;
    CHECK_EQ(c.user_agent(), std::string("CyberdeckZero-RSS/") + kVersion);
    CHECK(get(c, "file:///etc/passwd").error == FetchError::BadUrl);
    CHECK(get(c, "not a url").error == FetchError::BadUrl);
    CHECK(get(c, "").error == FetchError::BadUrl);
    // no stray temp files from the failed calls
    const char *t = std::getenv("TMPDIR");
    int leftovers = 0;
    for (const std::string &n : list_dir(t && *t ? t : "/tmp"))
        if (starts_with(n, "cdk-rss-")) ++leftovers;
    CHECK_EQ(leftovers, 0);
}

void test_curl_client_local(const std::string &base)
{
    CurlClient c;
    if (!c.available()) {
        std::printf("note: curl is not installed, local-server tests skipped\n");
        return;
    }
    HttpResponse r = get(c, base + "/feed");
    CHECK(r.ok() && r.status == 200);
    CHECK(r.body.find("<title>Local</title>") != std::string::npos);
    CHECK_EQ(r.etag, "\"v1\"");
    CHECK_EQ(r.last_modified, "Sun, 18 Oct 2026 15:00:00 GMT");
    CHECK_EQ(r.content_type, "application/rss+xml");
    CHECK_EQ(r.charset, "utf-8");
    CHECK_EQ(r.final_url, base + "/feed");

    r = get(c, base + "/feed", kMaxFeedBytes, 15000, "\"v1\"");
    CHECK(r.not_modified() && r.status == 304 && r.body.empty());
    r = get(c, base + "/feed", kMaxFeedBytes, 15000, "", "Sun, 18 Oct 2026 15:00:00 GMT");
    CHECK(r.not_modified());
    r = get(c, base + "/feed", kMaxFeedBytes, 15000, "\"other\"");
    CHECK(r.ok());

    r = get(c, base + "/gzip");
    CHECK(r.ok() && r.body.find("<title>Local</title>") != std::string::npos);        // --compressed

    r = get(c, base + "/redirect");
    CHECK(r.ok() && r.final_url == base + "/feed" && r.etag == "\"v1\"");
    r = get(c, base + "/redirect3");
    CHECK(r.ok() && r.final_url == base + "/feed");
    r = get(c, base + "/loop");
    CHECK(r.error == FetchError::TooManyRedirects);
    r = get(c, base + "/to-ftp");
    CHECK(!r.ok() && r.body.empty());                                                  // redirect to a non-http scheme is refused

    r = get(c, base + "/404");
    CHECK(r.error == FetchError::Http && r.status == 404 && r.body.empty());
    r = get(c, base + "/500");
    CHECK(r.error == FetchError::Http && r.status == 500);

    r = get(c, base + "/big");
    CHECK(r.error == FetchError::TooLarge && r.body.empty());
    r = get(c, base + "/bigchunk");                                                     // no Content-Length: stopped by the byte cap
    CHECK(r.error == FetchError::TooLarge);
    r = get(c, base + "/big", 8 * 1024 * 1024);
    CHECK(r.ok() && r.body.size() == 6u * 1024 * 1024);

    auto t0 = std::chrono::steady_clock::now();
    r = get(c, base + "/slow", kMaxFeedBytes, 1000);
    long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    CHECK(r.error == FetchError::Timeout);
    CHECK(ms < 4500);

    std::atomic<bool> cancel{false};
    std::thread th([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        cancel = true;
    });
    t0 = std::chrono::steady_clock::now();
    r = get(c, base + "/slow", kMaxFeedBytes, 15000, "", "", &cancel);
    ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    th.join();
    CHECK(r.error == FetchError::Cancelled);
    CHECK(ms < 3000);

    r = get(c, "http://127.0.0.1:1/");                                                  // connection refused
    CHECK(r.error == FetchError::Network);
    CHECK(!r.error_detail.empty());
    r = get(c, "http://no-such-host.invalid/");
    CHECK(r.error == FetchError::Network);

    r = get(c, base + "/ua");
    CHECK(r.ok());
    CHECK(r.body.find(std::string("UA=CyberdeckZero-RSS/") + kVersion) != std::string::npos);
    CHECK(r.body.find("Accept=application/rss+xml") != std::string::npos);

    HttpResponse img = fetch_image(c, base + "/png");
    CHECK(img.ok() && img.content_type == "image/png" && !img.body.empty());
    img = fetch_image(c, base + "/fakeimg");
    CHECK(img.error == FetchError::NotImage);
    img = fetch_image(c, base + "/bigimg");
    CHECK(img.error == FetchError::TooLarge);
    img = fetch_image(c, base + "/404");
    CHECK(img.error == FetchError::Http);
}
