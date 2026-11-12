/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl and the launcher (see launcher_link.hpp).
 */
#include "launcher_link.hpp"

#include "../input/media_keys.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vinyl {

bool LauncherStatus::operator==(const LauncherStatus &o) const {
    return have_status == o.have_status && have_board == o.have_board && volume == o.volume && mute == o.mute &&
           battery == o.battery && backlight == o.backlight && sound == o.sound && requests == o.requests &&
           facts == o.facts;
}

namespace {
int int_of(const StateText &st, const char *key, int dflt) {
    auto it = st.kv.find(key);
    if (it == st.kv.end() || it->second.empty()) return dflt;
    char *end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    return end && *end == 0 ? static_cast<int>(v) : dflt;
}
std::string str_of(const StateText &st, const char *key) {
    auto it = st.kv.find(key);
    return it == st.kv.end() ? std::string() : it->second;
}
}  // namespace

void parse_launcher_status(const std::string &status_text, bool status_trusted, const std::string &board_text,
                           bool board_trusted, LauncherStatus &out) {
    out = LauncherStatus();
    if (status_trusted) {
        const StateText st = parse_state_text(status_text);
        if (st.ok) {
            out.have_status = true;
            out.volume = int_of(st, "volume", -1);
            out.mute = int_of(st, "mute", 0) == 1;
            out.facts.have_status = true;
            apply_status_text(st, out.facts);
        }
    }
    if (board_trusted) {
        const StateText bt = parse_state_text(board_text);
        if (bt.ok) {
            out.have_board = true;
            out.battery = int_of(bt, "battery", -1);
            out.backlight = str_of(bt, "backlight");
            out.sound = int_of(bt, "sound", -1);
            out.requests = str_of(bt, "requests");
            out.facts.have_board = true;
            apply_board_text(bt, out.facts);
        }
    }
}

LauncherStatusReader::LauncherStatusReader(std::unique_ptr<FactsIo> io, std::string status_path, std::string board_path)
    : io_(io ? std::move(io) : make_posix_facts_io()), status_path_(std::move(status_path)), board_path_(std::move(board_path)) {
    const char *sd = std::getenv("APPLAUNCH_STATE_DIR");
    const char *rd = std::getenv("XDG_RUNTIME_DIR");
    if (status_path_.empty()) status_path_ = launcher_state_path("status.state", sd ? sd : "", rd ? rd : "");
    if (board_path_.empty()) board_path_ = launcher_state_path("board.state", sd ? sd : "", rd ? rd : "");
}

bool LauncherStatusReader::poll(uint64_t now_ms) {
    if (looked_ && now_ms - last_ < 1000) return false;
    looked_ = true;
    last_ = now_ms;
    bool changed_files = false;
    auto look = [&](const std::string &path, std::string &stamp, std::string &text) {
        std::string s;
        if (!io_->stat(path, s)) {
            if (!text.empty() || !stamp.empty()) changed_files = true;
            text.clear();
            stamp.clear();
            return;
        }
        if (s == stamp) return;
        stamp = s;
        std::string t;
        if (io_->read(path, t, 64 * 1024)) text = t;
        changed_files = true;
    };
    look(status_path_, status_stamp_, status_text_);
    look(board_path_, board_stamp_, board_text_);
    /* the pid check runs every time: a launcher that died leaves files nobody should believe */
    auto trusted = [&](const std::string &text) {
        if (text.empty()) return false;
        const StateText st = parse_state_text(text);
        return st.ok && st.pid > 0 && io_->pid_alive(st.pid);
    };
    LauncherStatus next;
    parse_launcher_status(status_text_, trusted(status_text_), board_text_, trusted(board_text_), next);
    (void)changed_files;
    if (next == st_) return false;
    st_ = next;
    return true;
}

std::string launcher_socket_path() {
    const char *rd = std::getenv("XDG_RUNTIME_DIR");
    if (!rd || !*rd) return "";
    return std::string(rd) + "/applaunch/launcher.sock";
}

int launcher_call(const std::string &socket_path, const std::string &request, std::string *reply, int timeout_ms) {
    if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, socket_path.c_str(), socket_path.size() + 1);
    int r = ::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a);
    if (r < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    const uint64_t deadline = mono_ms_now() + static_cast<uint64_t>(timeout_ms);
    auto wait = [&](short ev) {
        const uint64_t now = mono_ms_now();
        if (now >= deadline) return false;
        pollfd p{fd, ev, 0};
        return ::poll(&p, 1, static_cast<int>(deadline - now)) == 1 && (p.revents & ev);
    };
    if (r < 0) {
        if (!wait(POLLOUT)) {
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof err;
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }
    const std::string line = request + "\n";
    size_t off = 0;
    while (off < line.size()) {
        const ssize_t k = ::send(fd, line.data() + off, line.size() - off, MSG_NOSIGNAL);
        if (k > 0) {
            off += static_cast<size_t>(k);
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EINTR) && wait(POLLOUT)) continue;
        ::close(fd);
        return -1;
    }
    std::string got;
    char buf[256];
    while (got.find('\n') == std::string::npos && got.size() < 255) {
        if (!wait(POLLIN)) break;
        const ssize_t k = ::recv(fd, buf, sizeof buf, 0);
        if (k <= 0) break;
        got.append(buf, static_cast<size_t>(k));
    }
    ::close(fd);
    const size_t nl = got.find('\n');
    if (nl != std::string::npos) got.resize(nl);
    if (reply) *reply = got;
    if (got.compare(0, 2, "ok") == 0) return 0;
    if (got.compare(0, 3, "err") == 0) return 1;
    return -1;
}

LauncherRequests::LauncherRequests(std::string socket_path) : path_(std::move(socket_path)) {
    th_ = std::thread([this] { run(); });
}

LauncherRequests::~LauncherRequests() {
    {
        std::lock_guard<std::mutex> g(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void LauncherRequests::send(const std::string &request) {
    {
        std::lock_guard<std::mutex> g(mu_);
        const std::string verb = request.substr(0, request.find(' '));
        for (auto it = q_.begin(); it != q_.end();) {
            if (it->substr(0, it->find(' ')) == verb) it = q_.erase(it);
            else ++it;
        }
        q_.push_back(request);
    }
    cv_.notify_all();
}

void LauncherRequests::run() {
    for (;;) {
        std::string req;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return quit_ || !q_.empty(); });
            if (quit_) return;
            req = q_.front();
            q_.pop_front();
        }
        const uint64_t now = mono_ms_now();
        if (now < dead_until_) continue; /* no service a moment ago: do not retry per event */
        std::string reply;
        const int r = launcher_call(path_, req, &reply, 200);
        if (r < 0) dead_until_ = now + 5000;
        else sent_.fetch_add(1);
    }
}

}  // namespace vinyl
