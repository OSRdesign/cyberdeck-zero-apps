/*
 * SPDX-License-Identifier: MIT
 */
#include "fetch.hpp"

#include "url.hpp"
#include "util.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace rssreader {

const char *fetch_error_text(FetchError e)
{
    switch (e) {
    case FetchError::None: return "ok";
    case FetchError::NotModified: return "not modified";
    case FetchError::Network: return "network error";
    case FetchError::Timeout: return "timed out";
    case FetchError::Tls: return "secure connection failed";
    case FetchError::Http: return "server error";
    case FetchError::TooLarge: return "too large";
    case FetchError::TooManyRedirects: return "too many redirects";
    case FetchError::BadUrl: return "bad address";
    case FetchError::NotImage: return "not an image";
    case FetchError::Cancelled: return "cancelled";
    case FetchError::Internal: return "internal error";
    }
    return "error";
}

namespace {

constexpr const char *kMarker = "@@CDK@@";
constexpr const char *kDefaultAccept = "application/rss+xml, application/atom+xml, application/xml;q=0.9, text/xml;q=0.8, text/html;q=0.6, */*;q=0.5";

bool header_value_ok(const std::string &v)
{
    for (char c : v)
        if (c == '\r' || c == '\n' || c == '\0') return false;
    return true;
}

struct SpawnResult {
    bool started = false;
    int exit_code = -1;
    int term_signal = 0;
    bool timed_out = false;
    bool cancelled = false;
    bool truncated = false;
    std::string out, err;
};

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void kill_group(pid_t pid, int sig) { ::kill(-pid, sig); ::kill(pid, sig); }

SpawnResult run_process(const std::vector<std::string> &args, std::size_t max_out, int timeout_ms, const std::atomic<bool> *cancel)
{
    SpawnResult r;
    int po[2], pe[2];
    if (::pipe2(po, O_CLOEXEC) != 0) return r;
    if (::pipe2(pe, O_CLOEXEC) != 0) { ::close(po[0]); ::close(po[1]); return r; }

    std::vector<char *> argv;
    for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> envs;
    for (char **e = environ; e && *e; ++e)
        if (std::strncmp(*e, "LC_ALL=", 7) != 0) envs.push_back(*e);
    envs.push_back("LC_ALL=C");
    std::vector<char *> envp;
    for (std::string &e : envs) envp.push_back(&e[0]);
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    posix_spawnattr_setpgroup(&at, 0);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    sigset_t all;
    sigfillset(&all);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = 0;
    int rc = ::posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(po[1]);
    ::close(pe[1]);
    if (rc != 0) {
        ::close(po[0]);
        ::close(pe[0]);
        r.err = std::strerror(rc);
        return r;
    }
    r.started = true;

    const long long deadline = now_ms() + timeout_ms + 1500;      /* curl's own --max-time fires first */
    int fo = po[0], fe = pe[0];
    bool killed = false;
    char buf[16384];
    while (fo >= 0 || fe >= 0) {
        if (cancel && cancel->load()) { r.cancelled = true; kill_group(pid, SIGTERM); killed = true; break; }
        if (now_ms() > deadline) { r.timed_out = true; kill_group(pid, SIGTERM); killed = true; break; }
        struct pollfd p[2];
        int n = 0, idx_o = -1, idx_e = -1;
        if (fo >= 0) { p[n] = {fo, POLLIN, 0}; idx_o = n++; }
        if (fe >= 0) { p[n] = {fe, POLLIN, 0}; idx_e = n++; }
        int pr = ::poll(p, static_cast<nfds_t>(n), 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (idx_o >= 0 && (p[idx_o].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = ::read(fo, buf, sizeof buf);
            if (k > 0) {
                if (r.out.size() + static_cast<std::size_t>(k) > max_out) {
                    r.truncated = true;
                    kill_group(pid, SIGTERM);
                    killed = true;
                    break;
                }
                r.out.append(buf, static_cast<std::size_t>(k));
            } else if (k == 0 || errno != EINTR) {
                ::close(fo);
                fo = -1;
            }
        }
        if (idx_e >= 0 && (p[idx_e].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = ::read(fe, buf, sizeof buf);
            if (k > 0) {
                if (r.err.size() < 16384) r.err.append(buf, static_cast<std::size_t>(k));
            } else if (k == 0 || errno != EINTR) {
                ::close(fe);
                fe = -1;
            }
        }
    }
    if (fo >= 0) ::close(fo);
    if (fe >= 0) ::close(fe);

    int status = 0;
    long long wait_until = now_ms() + (killed ? 700 : 3000);
    bool reaped = false;
    for (;;) {
        pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) { reaped = true; break; }
        if (w < 0 && errno != EINTR) { reaped = true; status = 0; break; }
        if (now_ms() > wait_until) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!reaped) {
        kill_group(pid, SIGKILL);
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.term_signal = WTERMSIG(status);
    return r;
}

std::string temp_dir()
{
    const char *t = std::getenv("TMPDIR");
    return (t && *t) ? t : "/tmp";
}

std::string first_line(const std::string &s)
{
    std::size_t e = s.find_first_of("\r\n");
    return trim(s.substr(0, e));
}

} // namespace

std::vector<std::string> build_curl_argv(const HttpRequest &r, const std::string &header_file, const std::string &ua, const std::string &curl_path)
{
    if (!is_http_url(r.url)) return {};
    for (char c : r.url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return {};
    std::vector<std::string> a = {curl_path, "-q", "--silent", "--show-error", "--globoff", "--location", "--max-redirs", "5",
                                  "--proto", "=http,https", "--proto-redir", "=http,https", "--compressed",
                                  "--connect-timeout", std::to_string((r.connect_timeout_ms + 999) / 1000),
                                  "--max-time", std::to_string((r.timeout_ms + 999) / 1000),
                                  "--max-filesize", std::to_string(r.max_bytes),
                                  "--user-agent", ua.empty() ? std::string("CyberdeckZero-RSS/") + kVersion : ua,
                                  "--header", "Accept: " + (r.accept.empty() ? std::string(kDefaultAccept) : r.accept),
                                  "--dump-header", header_file,
                                  "--output", "-",
                                  "--write-out", std::string("%{stderr}") + kMarker + "%{http_code}\t%{content_type}\t%{num_redirects}\t%{url_effective}\n"};
    if (!r.etag.empty() && header_value_ok(r.etag)) { a.push_back("--header"); a.push_back("If-None-Match: " + r.etag); }
    if (!r.last_modified.empty() && header_value_ok(r.last_modified)) { a.push_back("--header"); a.push_back("If-Modified-Since: " + r.last_modified); }
    a.push_back("--url");
    a.push_back(r.url);
    return a;
}

void parse_response_headers(const std::string &dump, HttpResponse &out)
{
    /* the last block that starts with HTTP/ */
    std::size_t start = 0, pos = 0;
    while ((pos = dump.find("HTTP/", pos)) != std::string::npos) {
        if (pos == 0 || dump[pos - 1] == '\n') start = pos;
        pos += 5;
    }
    std::size_t i = start;
    out.etag.clear();
    out.last_modified.clear();
    while (i < dump.size()) {
        std::size_t e = dump.find('\n', i);
        if (e == std::string::npos) e = dump.size();
        std::string line = trim(dump.substr(i, e - i));
        i = e + 1;
        std::size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = lower(line.substr(0, c)), v = trim(line.substr(c + 1));
        if (k == "etag") out.etag = v.substr(0, 256);
        else if (k == "last-modified") out.last_modified = v.substr(0, 128);
        else if (k == "content-type" && out.content_type.empty()) {
            std::string lv = lower(v);
            std::size_t semi = lv.find(';');
            out.content_type = trim(lv.substr(0, semi));
            std::size_t cs = lv.find("charset=");
            if (cs != std::string::npos) {
                std::string ch = lv.substr(cs + 8);
                if (!ch.empty() && (ch[0] == '"' || ch[0] == '\'')) ch.erase(0, 1);
                std::size_t end = ch.find_first_of("\"'; ");
                out.charset = ch.substr(0, end);
            }
        }
    }
}

CurlClient::CurlClient(std::string user_agent, std::string curl_path)
    : ua_(user_agent.empty() ? std::string("CyberdeckZero-RSS/") + kVersion : std::move(user_agent)), curl_(std::move(curl_path))
{
}

bool CurlClient::available() const
{
    SpawnResult r = run_process({curl_, "--version"}, 16384, 5000, nullptr);
    return r.started && r.exit_code == 0;
}

HttpResponse CurlClient::get(const HttpRequest &req)
{
    HttpResponse res;
    res.final_url = req.url;
    std::string hdr_path = temp_dir() + "/cdk-rss-XXXXXX";
    std::vector<char> tmpl(hdr_path.begin(), hdr_path.end());
    tmpl.push_back('\0');
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        res.error = FetchError::Internal;
        res.error_detail = "cannot create a temporary file";
        return res;
    }
    ::close(fd);
    hdr_path = tmpl.data();

    std::vector<std::string> argv = build_curl_argv(req, hdr_path, ua_, curl_);
    if (argv.empty()) {
        ::unlink(hdr_path.c_str());
        res.error = FetchError::BadUrl;
        return res;
    }
    SpawnResult sp = run_process(argv, req.max_bytes + 4096, req.timeout_ms, req.cancel);

    std::string dump;
    if (FILE *f = std::fopen(hdr_path.c_str(), "rb")) {
        char b[4096];
        std::size_t n;
        while (dump.size() < 65536 && (n = std::fread(b, 1, sizeof b, f)) > 0) dump.append(b, n);
        std::fclose(f);
    }
    ::unlink(hdr_path.c_str());

    if (!sp.started) {
        res.error = FetchError::Internal;
        res.error_detail = "cannot start curl: " + sp.err;
        return res;
    }
    if (sp.cancelled) { res.error = FetchError::Cancelled; return res; }
    if (sp.timed_out) { res.error = FetchError::Timeout; return res; }

    /* the -w line on stderr: "@@CDK@@<code>\t<ctype>\t<redirects>\t<url>\n"; curl's own error message is the rest */
    std::string msg = sp.err;
    std::size_t m = sp.err.rfind(kMarker);
    if (m != std::string::npos) {
        std::size_t eol = sp.err.find('\n', m);
        msg = sp.err.substr(0, m) + (eol == std::string::npos ? std::string() : sp.err.substr(eol + 1));
        std::string line = sp.err.substr(m + std::strlen(kMarker), eol == std::string::npos ? std::string::npos : eol - m - std::strlen(kMarker));
        std::size_t t1 = line.find('\t');
        if (t1 != std::string::npos) {
            res.status = std::atoi(line.substr(0, t1).c_str());
            std::size_t t2 = line.find('\t', t1 + 1);
            std::size_t t3 = t2 == std::string::npos ? t2 : line.find('\t', t2 + 1);
            if (t3 != std::string::npos) {
                std::string u = trim(line.substr(t3 + 1));
                if (!u.empty()) res.final_url = u;
            }
        }
    }
    parse_response_headers(dump, res);
    res.error_detail = first_line(trim(msg));

    if (sp.truncated) { res.error = FetchError::TooLarge; return res; }
    if (sp.term_signal != 0) { res.error = FetchError::Internal; res.error_detail = "curl was killed"; return res; }
    if (sp.exit_code != 0) {
        switch (sp.exit_code) {
        case 1: case 3: res.error = FetchError::BadUrl; break;
        case 28: res.error = FetchError::Timeout; break;
        case 47: res.error = FetchError::TooManyRedirects; break;
        case 63: res.error = FetchError::TooLarge; break;
        case 35: case 51: case 53: case 54: case 58: case 59: case 60: case 64: case 66: case 77: case 80: case 82: case 83: case 90: case 91:
            res.error = FetchError::Tls; break;
        case 127: res.error = FetchError::Internal; res.error_detail = "curl is not installed"; break;
        default: res.error = FetchError::Network; break;
        }
        if (res.error_detail.empty()) res.error_detail = "curl exit " + std::to_string(sp.exit_code);
        return res;
    }
    if (res.status == 304) { res.error = FetchError::NotModified; return res; }
    if (res.status < 200 || res.status >= 300) {
        res.error = FetchError::Http;
        return res;
    }
    std::string body = std::move(sp.out);     /* stdout is the body only: -w writes to stderr */
    if (body.size() > req.max_bytes) { res.error = FetchError::TooLarge; return res; }
    res.body = std::move(body);
    return res;
}

std::string sniff_image_type(const std::string &b)
{
    auto at = [&](std::size_t i, const char *s) { return b.compare(i, std::strlen(s), s) == 0; };
    if (b.size() >= 8 && at(0, "\x89PNG")) return "image/png";
    if (b.size() >= 3 && at(0, "\xFF\xD8\xFF")) return "image/jpeg";
    if (b.size() >= 6 && (at(0, "GIF87a") || at(0, "GIF89a"))) return "image/gif";
    if (b.size() >= 12 && at(0, "RIFF") && at(8, "WEBP")) return "image/webp";
    if (b.size() >= 26 && at(0, "BM")) return "image/bmp";
    return "";
}

std::string image_extension(const std::string &ct)
{
    std::string t = lower(ct);
    if (t == "image/png") return ".png";
    if (t == "image/jpeg" || t == "image/jpg" || t == "image/pjpeg") return ".jpg";
    if (t == "image/gif") return ".gif";
    if (t == "image/webp") return ".webp";
    if (t == "image/bmp") return ".bmp";
    return ".img";
}

HttpResponse fetch_image(HttpClient &client, const std::string &url, const std::atomic<bool> *cancel)
{
    HttpRequest req;
    req.url = url;
    req.max_bytes = kMaxImageBytes;
    // what the app decodes (docs/IMAGES.md): no image/webp or image/avif, so a server that picks the format by this header
    // (image CDNs do) sends a JPEG or a PNG
    req.accept = "image/jpeg,image/png,image/gif;q=0.9,image/*;q=0.5";
    req.cancel = cancel;
    HttpResponse res = client.get(req);
    if (!res.ok()) {
        res.body.clear();
        return res;
    }
    if (res.body.size() > kMaxImageBytes) {
        res.error = FetchError::TooLarge;
        res.body.clear();
        return res;
    }
    std::string ct = lower(res.content_type);
    std::string sniffed = sniff_image_type(res.body);
    bool declared_image = starts_with(ct, "image/") && ct != "image/svg+xml";
    if (declared_image) {
        if (!sniffed.empty()) res.content_type = sniffed;     /* trust the bytes over a wrong label */
        return res;
    }
    if ((ct.empty() || ct == "application/octet-stream" || ct == "binary/octet-stream") && !sniffed.empty()) {
        res.content_type = sniffed;
        return res;
    }
    res.error = FetchError::NotImage;
    res.body.clear();
    return res;
}

} // namespace rssreader
