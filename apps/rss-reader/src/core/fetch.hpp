/*
 * SPDX-License-Identifier: MIT
 *
 * RSS Reader core: HTTP through the `curl` command (no library dependency; the package depends on curl and
 * ca-certificates).
 *
 *   HttpClient   the interface everything else uses (refresh, discovery, images); tests inject a fake.
 *   CurlClient   the real one: an argv (never a shell) for curl, run with posix_spawn in its own process group, stdin
 *                /dev/null, LC_ALL=C. The body is read from curl's stdout and capped at max_bytes (the process is ended
 *                when it goes over), the headers go to a private temp file (-D), the status / final URL / content type
 *                come back on stderr through -w (after a marker), so a body can never be mistaken for them.
 *                GET only; follows at most 5 redirects, http / https only; 15 s total, 8 s to connect; gzip.
 *
 * The API is synchronous: call it from a worker thread, never from the UI thread. A `cancel` flag (checked every
 * 100 ms) ends the transfer early.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace rssreader {

enum class FetchError {
    None,
    NotModified,        /* 304 for a conditional request (status 304, empty body) */
    Network,            /* DNS, connection refused / reset, empty reply ... (detail in error_detail) */
    Timeout,
    Tls,                /* certificate or handshake problem */
    Http,               /* the server answered 4xx / 5xx (status is set) */
    TooLarge,
    TooManyRedirects,
    BadUrl,             /* not an http(s) URL, or one that cannot be passed safely */
    NotImage,           /* image fetch: the answer is not an image */
    Cancelled,
    Internal,           /* curl missing or could not be started */
};

const char *fetch_error_text(FetchError e);

constexpr std::size_t kMaxFeedBytes = 5u * 1024 * 1024;
constexpr std::size_t kMaxImageBytes = 2u * 1024 * 1024;

struct HttpRequest {
    std::string url;
    std::string etag;            /* If-None-Match, when not empty */
    std::string last_modified;   /* If-Modified-Since, when not empty */
    std::string accept;          /* Accept header; "" = the feed default */
    std::size_t max_bytes = kMaxFeedBytes;
    int timeout_ms = 15000;      /* whole transfer */
    int connect_timeout_ms = 8000;
    const std::atomic<bool> *cancel = nullptr;
};

struct HttpResponse {
    FetchError error = FetchError::None;
    int status = 0;              /* HTTP status of the last response, 0 when none */
    std::string body;            /* empty unless error == None */
    std::string etag;
    std::string last_modified;
    std::string final_url;       /* after redirects */
    std::string content_type;    /* lower case, without parameters */
    std::string charset;         /* from Content-Type, lower case, or "" */
    std::string error_detail;    /* curl's message for Network / Tls / Internal */
    bool ok() const { return error == FetchError::None; }
    bool not_modified() const { return error == FetchError::NotModified; }
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    /* Blocking; never throws. */
    virtual HttpResponse get(const HttpRequest &request) = 0;
};

class CurlClient : public HttpClient {
public:
    /* user_agent "" = "CyberdeckZero-RSS/<kVersion>" */
    explicit CurlClient(std::string user_agent = "", std::string curl_path = "curl");
    HttpResponse get(const HttpRequest &request) override;
    /* Does `curl --version` run? */
    bool available() const;
    const std::string &user_agent() const { return ua_; }

private:
    std::string ua_, curl_;
};

/* The argv for one request (public for the tests): no shell involved, every value is one argument. Empty when the URL
 * is not an http(s) URL or contains control characters / spaces. */
std::vector<std::string> build_curl_argv(const HttpRequest &request, const std::string &header_file, const std::string &user_agent,
                                         const std::string &curl_path = "curl");

/* Parses a curl -D dump (all the header blocks of a redirect chain): the last block's etag / last-modified / content type. */
void parse_response_headers(const std::string &dump, HttpResponse &out);

/* "image/png" | "image/jpeg" | "image/gif" | "image/webp" | "image/bmp" from the first bytes, or "". */
std::string sniff_image_type(const std::string &body);

/* An image: max 2 MB, content type "image/..." (a missing or octet-stream type is accepted when the bytes are an image).
 * HttpResponse::content_type is the detected image type. error NotImage otherwise. */
HttpResponse fetch_image(HttpClient &client, const std::string &url, const std::atomic<bool> *cancel = nullptr);

/* File extension for an image content type: ".png", ".jpg", ".gif", ".webp", ".bmp", else ".img". */
std::string image_extension(const std::string &content_type);

} // namespace rssreader
