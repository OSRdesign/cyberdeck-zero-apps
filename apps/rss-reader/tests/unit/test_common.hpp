// Shared pieces of the RSS Reader host tests: the CHECK macro, fixtures, temp folders and a fake HTTP client.
#pragma once

#include "fetch.hpp"
#include "fsutil.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "../data"
#endif

extern int g_fail, g_checks;

#define CHECK(...)                                                                       \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(__VA_ARGS__)) {                                                           \
            ++g_fail;                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);                \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                  \
        ++g_checks;                                                                                       \
        auto va_ = (a);                                                                                   \
        auto vb_ = (b);                                                                                   \
        if (!(va_ == vb_)) {                                                                              \
            ++g_fail;                                                                                     \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);                           \
            std::printf("     got: [%s]\n", rssreader_test::show(va_).c_str());                          \
            std::printf("    want: [%s]\n", rssreader_test::show(vb_).c_str());                          \
        }                                                                                                 \
    } while (0)

namespace rssreader_test {

inline std::string show(const std::string &s) { return s; }
inline std::string show(const char *s) { return s; }
inline std::string show(long long v) { return std::to_string(v); }
inline std::string show(unsigned long long v) { return std::to_string(v); }
inline std::string show(long v) { return std::to_string(v); }
inline std::string show(unsigned long v) { return std::to_string(v); }
inline std::string show(int v) { return std::to_string(v); }
inline std::string show(unsigned v) { return std::to_string(v); }
inline std::string show(double v) { return std::to_string(v); }
inline std::string show(bool v) { return v ? "true" : "false"; }

inline std::string fixture(const std::string &name)
{
    std::string s;
    if (!rssreader::read_file(std::string(TEST_DATA_DIR) + "/" + name, s)) {
        std::printf("FAIL cannot read fixture %s\n", name.c_str());
        ++g_fail;
    }
    return s;
}

/* A fresh empty temp folder, removed by the destructor. */
struct TempDir {
    std::string path;
    TempDir()
    {
        const char *t = std::getenv("TMPDIR");
        std::string tmpl = std::string(t && *t ? t : "/tmp") + "/rssreader-test-XXXXXX";
        std::vector<char> b(tmpl.begin(), tmpl.end());
        b.push_back('\0');
        if (::mkdtemp(b.data())) path = b.data();
    }
    ~TempDir() { if (!path.empty()) rssreader::remove_tree(path); }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

/* Answers by URL, in the order the answers were queued (the last one repeats); records every request. */
class FakeHttp : public rssreader::HttpClient {
public:
    std::map<std::string, std::deque<rssreader::HttpResponse>> answers;
    std::vector<rssreader::HttpRequest> requests;

    void add(const std::string &url, rssreader::HttpResponse r)
    {
        if (r.final_url.empty()) r.final_url = url;
        answers[url].clear();                       // the latest answer wins: it repeats until replaced
        answers[url].push_back(std::move(r));
    }
    void add_ok(const std::string &url, const std::string &body, const std::string &ct = "application/xml", const std::string &etag = "")
    {
        rssreader::HttpResponse r;
        r.status = 200;
        r.body = body;
        r.content_type = ct;
        r.etag = etag;
        add(url, std::move(r));
    }
    void add_error(const std::string &url, rssreader::FetchError e, int status = 0, const std::string &detail = "")
    {
        rssreader::HttpResponse r;
        r.error = e;
        r.status = status;
        r.error_detail = detail;
        add(url, std::move(r));
    }
    rssreader::HttpResponse get(const rssreader::HttpRequest &req) override
    {
        requests.push_back(req);
        auto it = answers.find(req.url);
        if (it == answers.end() || it->second.empty()) {
            rssreader::HttpResponse r;
            r.error = rssreader::FetchError::Http;
            r.status = 404;
            r.final_url = req.url;
            return r;
        }
        rssreader::HttpResponse r = it->second.front();
        if (it->second.size() > 1) it->second.pop_front();
        return r;
    }
    int count(const std::string &url) const
    {
        int n = 0;
        for (const auto &r : requests) n += r.url == url;
        return n;
    }
};

} // namespace rssreader_test
