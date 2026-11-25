/*
 * SPDX-License-Identifier: MIT
 *
 * The connection to the bridge (see link.hpp).
 */
#include "link.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ret {

namespace {

double mono_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr std::size_t kMaxLine = 1 << 20; /* the bridge sends at most 64 KiB per line; more is a broken stream */
constexpr std::size_t kMaxEvents = 256;

} // namespace

bool is_transmit_enabling(const std::string &verb, const json::Value &params)
{
    if (verb == "announce" || verb == "lxmf.send" || verb == "messages.send" || verb == "messages.retry") return true;
    if (verb == "iface.outgoing") return params["outgoing"].as_bool(false);
    return false;
}

bool parse_reply(const std::string &line, std::int64_t id, Response &out)
{
    json::Value v;
    if (!json::parse(line, v) || !v.is_object()) return false;
    if (v.has("event") && !v.has("id")) return false;
    if (!v["id"].is_number() || v["id"].as_int(-1) != id) return false;
    Response r;
    r.ok = v["ok"].as_bool(false);
    if (r.ok) {
        r.result = v["result"];
    } else {
        r.error = v["error"];
        r.code = r.error["code"].as_string("internal");
        r.message = r.error["message"].as_string();
        r.field = r.error["field"].as_string();
    }
    out = std::move(r);
    return true;
}

Link::Link(std::string sock_path) : path_(std::move(sock_path))
{
    if (::pipe(wake_) == 0) {
        for (int i = 0; i < 2; ++i) {
            ::fcntl(wake_[i], F_SETFL, ::fcntl(wake_[i], F_GETFL) | O_NONBLOCK);
            ::fcntl(wake_[i], F_SETFD, FD_CLOEXEC);
        }
    }
    thread_ = std::thread([this] { run(); });
}

Link::~Link()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        queue_.clear();
    }
    if (wake_[1] >= 0) {
        const char c = 1;
        if (::write(wake_[1], &c, 1) < 0) { /* the pipe is full: the thread wakes anyway */ }
    }
    if (thread_.joinable()) thread_.join();
    close_socket();
    for (int i = 0; i < 2; ++i)
        if (wake_[i] >= 0) ::close(wake_[i]);
}

void Link::request(const std::string &verb, json::Value params, double timeout_s, Done done, bool user_action)
{
    ++pending_;
    if (is_transmit_enabling(verb, params) && !user_action) {
        ++blocked_;
        Response r;
        r.ok = false;
        r.transport = true;
        r.code = "blocked";
        r.message = "refused by the app: this needs an explicit user action";
        std::lock_guard<std::mutex> lock(mu_);
        done_.emplace_back(std::move(done), std::move(r));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) {
            --pending_;
            return;
        }
        Item item;
        item.verb = verb;
        item.params = std::move(params);
        item.timeout = timeout_s < 0.5 ? 0.5 : (timeout_s > 120 ? 120 : timeout_s);
        item.done = std::move(done);
        queue_.push_back(std::move(item));
    }
    const char c = 1;
    if (::write(wake_[1], &c, 1) < 0) { /* full: already awake */ }
}

void Link::poll()
{
    std::vector<std::pair<Done, Response>> ready;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ready.swap(done_);
    }
    for (auto &d : ready) {
        if (d.first) d.first(d.second);
        --pending_;
    }
}

std::vector<json::Value> Link::take_events()
{
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<json::Value> out;
    out.swap(events_);
    return out;
}

void Link::finish(Done done, Response r)
{
    std::lock_guard<std::mutex> lock(mu_);
    done_.emplace_back(std::move(done), std::move(r));
}

bool Link::open_socket(std::string &why)
{
    if (path_.empty()) {
        why = "no run folder (XDG_RUNTIME_DIR is not set)";
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof addr.sun_path) {
        why = "the socket path is too long";
        return false;
    }
    std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        why = std::strerror(errno);
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        why = std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_ = fd;
    buf_.clear();
    connected_ = true;
    return true;
}

void Link::close_socket()
{
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    buf_.clear();
    connected_ = false;
}

bool Link::send_line(const std::string &line, double budget_s)
{
    const double deadline = mono_s() + budget_s;
    std::size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n = ::send(fd_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const double left = deadline - mono_s();
            if (left <= 0) return false;
            pollfd p{fd_, POLLOUT, 0};
            ::poll(&p, 1, static_cast<int>(left * 1000) + 1);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void Link::handle_line(const std::string &line)
{
    json::Value v;
    if (!json::parse(line, v) || !v.is_object()) return;
    if (v.has("event") && !v.has("id")) {
        std::lock_guard<std::mutex> lock(mu_);
        if (events_.size() < kMaxEvents) events_.push_back(std::move(v));
    }
}

/* Reads what the socket holds; complete lines that are events are stored, the others returned to the caller through
 * buf_ (left in place). */
void Link::take_lines(bool *got_eof)
{
    char tmp[4096];
    for (;;) {
        const ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
        if (n > 0) {
            buf_.append(tmp, static_cast<std::size_t>(n));
            if (buf_.size() > kMaxLine) {
                *got_eof = true;
                return;
            }
            continue;
        }
        if (n == 0) {
            *got_eof = true;
            return;
        }
        if (errno == EINTR) continue;
        return; /* EAGAIN: nothing more now */
    }
}

bool Link::read_reply(std::int64_t id, double budget_s, Response &out)
{
    const double deadline = mono_s() + budget_s;
    eof_ = false;
    for (;;) {
        std::size_t nl;
        while ((nl = buf_.find('\n')) != std::string::npos) {
            const std::string line = buf_.substr(0, nl);
            buf_.erase(0, nl + 1);
            if (parse_reply(line, id, out)) return true;
            handle_line(line);
        }
        const double left = deadline - mono_s();
        if (left <= 0) return false;
        pollfd p{fd_, POLLIN, 0};
        const int rc = ::poll(&p, 1, static_cast<int>(left * 1000) + 1);
        if (rc < 0 && errno != EINTR) return false;
        if (rc > 0) {
            bool eof = false;
            take_lines(&eof);
            if (eof) {
                eof_ = true;
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return false;
        }
    }
}

void Link::run()
{
    for (;;) {
        Item item;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop_front();
                have = true;
            }
        }
        if (!have) {
            /* idle: wait for a request, and keep reading the events the bridge sends */
            pollfd fds[2];
            int n = 0;
            fds[n++] = {wake_[0], POLLIN, 0};
            if (fd_ >= 0) fds[n++] = {fd_, POLLIN, 0};
            const int rc = ::poll(fds, static_cast<nfds_t>(n), 1000);
            if (rc < 0 && errno != EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (rc > 0 && (fds[0].revents & POLLIN)) {
                char tmp[64];
                while (::read(wake_[0], tmp, sizeof tmp) > 0) {
                }
            }
            if (rc > 0 && fd_ >= 0 && n > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                bool eof = false;
                take_lines(&eof);
                std::size_t nl;
                while ((nl = buf_.find('\n')) != std::string::npos) {
                    const std::string line = buf_.substr(0, nl);
                    buf_.erase(0, nl + 1);
                    handle_line(line);
                }
                if (eof) close_socket();
            }
            continue;
        }

        Response r;
        r.transport = false;
        if (fd_ < 0) {
            std::string why;
            if (!open_socket(why)) {
                r.code = "no_service";
                r.message = why;
                finish(std::move(item.done), std::move(r));
                continue;
            }
            if (subscribe_.load()) {
                /* read-only: the heard announces, as events from now on and the last ones at once */
                json::Value req = json::Value::object();
                const std::int64_t sid = ++next_id_;
                req.set("id", json::Value::number(static_cast<double>(sid)));
                req.set("verb", json::Value::string("announces.subscribe"));
                req.set("params", json::Value::object());
                req.set("timeout", json::Value::number(5));
                Response sr;
                if (send_line(json::dump(req) + "\n", 5) && read_reply(sid, 8, sr) && sr.ok) {
                    const json::Value &recent = sr.result["recent"];
                    std::lock_guard<std::mutex> lock(mu_);
                    for (const json::Value &e : recent.items()) {
                        json::Value ev = json::Value::object();
                        ev.set("event", json::Value::string("announce"));
                        ev.set("data", e);
                        if (events_.size() < kMaxEvents) events_.push_back(std::move(ev));
                    }
                }
                /* P2: the message stream (read-only too): new messages, delivery states, contact changes. A bridge
                 * without it answers unknown_verb and the app polls instead. */
                if (fd_ >= 0) {
                    json::Value mreq = json::Value::object();
                    const std::int64_t mid = ++next_id_;
                    mreq.set("id", json::Value::number(static_cast<double>(mid)));
                    mreq.set("verb", json::Value::string("messages.subscribe"));
                    mreq.set("params", json::Value::object());
                    mreq.set("timeout", json::Value::number(5));
                    Response mr;
                    if (send_line(json::dump(mreq) + "\n", 5)) read_reply(mid, 8, mr);
                    resubscribed_.store(true); /* the app refreshes its lists: events may have been missed */
                }
            }
        }
        if (fd_ < 0) { /* the subscribe step found the stream closed */
            r.code = "lost";
            r.message = "the service closed the connection";
            finish(std::move(item.done), std::move(r));
            continue;
        }
        json::Value req = json::Value::object();
        const std::int64_t id = ++next_id_;
        req.set("id", json::Value::number(static_cast<double>(id)));
        req.set("verb", json::Value::string(item.verb));
        req.set("params", item.params.is_object() ? item.params : json::Value::object());
        req.set("timeout", json::Value::number(item.timeout));
        if (!send_line(json::dump(req) + "\n", 5)) {
            close_socket();
            r.code = "lost";
            r.message = "the service closed the connection";
            finish(std::move(item.done), std::move(r));
            continue;
        }
        Response reply;
        if (read_reply(id, item.timeout + 3, reply)) {
            finish(std::move(item.done), std::move(reply));
            continue;
        }
        const bool stopping = [&] {
            std::lock_guard<std::mutex> lock(mu_);
            return stop_;
        }();
        const bool timed_out = !eof_ && !stopping;
        close_socket(); /* a late answer would be matched to nothing; start clean next time */
        r.code = timed_out ? "no_answer" : "lost";
        r.message = timed_out ? "the service did not answer in time" : "the service closed the connection";
        finish(std::move(item.done), std::move(r));
    }
}

} // namespace ret
