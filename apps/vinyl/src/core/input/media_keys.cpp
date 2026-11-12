/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl input: the media-key reader (see media_keys.hpp).
 */
#include "media_keys.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <poll.h>
#include <sstream>
#include <unistd.h>

namespace vinyl {

uint64_t mono_ms_now() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const char *media_action_name(MediaAction a) {
    switch (a) {
    case MediaAction::Toggle: return "play/pause";
    case MediaAction::Next: return "next";
    case MediaAction::Prev: return "previous";
    case MediaAction::Stop: return "stop";
    default: return "none";
    }
}

MediaAction media_action_for(unsigned code) {
    switch (code) {
    case 164: /* KEY_PLAYPAUSE */
    case 200: /* KEY_PLAYCD */
    case 201: /* KEY_PAUSECD */
        return MediaAction::Toggle;
    case 163: return MediaAction::Next;  /* KEY_NEXTSONG */
    case 165: return MediaAction::Prev;  /* KEY_PREVIOUSSONG */
    case 166: return MediaAction::Stop;  /* KEY_STOPCD */
    default: return MediaAction::None;
    }
}

bool InputDevice::has_key(unsigned code) const {
    const size_t w = code / 64;
    return w < keys.size() && ((keys[w] >> (code % 64)) & 1u);
}

bool InputDevice::media() const {
    for (unsigned c : {164u, 200u, 201u, 163u, 165u, 166u})
        if (has_key(c)) return true;
    return false;
}

std::vector<InputDevice> parse_input_devices(const std::string &text) {
    std::vector<InputDevice> out;
    std::istringstream in(text);
    std::string line;
    InputDevice cur;
    bool any = false;
    auto flush = [&] {
        if (any && !cur.event.empty()) out.push_back(cur);
        cur = InputDevice();
        any = false;
    };
    while (std::getline(in, line)) {
        if (line.empty()) {
            flush();
            continue;
        }
        any = true;
        if (line.compare(0, 9, "N: Name=\"") == 0) {
            cur.name = line.substr(9);
            if (!cur.name.empty() && cur.name.back() == '"') cur.name.pop_back();
        } else if (line.compare(0, 12, "H: Handlers=") == 0) {
            std::istringstream hs(line.substr(12));
            std::string h;
            while (hs >> h)
                if (h.compare(0, 5, "event") == 0) cur.event = h;
        } else if (line.compare(0, 7, "B: KEY=") == 0) {
            std::istringstream ks(line.substr(7));
            std::vector<uint64_t> words;
            std::string w;
            while (ks >> w) words.push_back(std::strtoull(w.c_str(), nullptr, 16));
            cur.keys.assign(words.rbegin(), words.rend()); /* the kernel prints the highest word first */
        }
    }
    flush();
    return out;
}

bool Dedupe::accept(MediaAction a, uint64_t now_ms) {
    if (a == MediaAction::None) return false;
    const int k = static_cast<int>(a);
    auto it = last_.find(k);
    if (it != last_.end() && now_ms - it->second < window_) return false;
    last_[k] = now_ms;
    return true;
}

bool MediaPresses::feed(const std::string &source, MediaAction a, int value, uint64_t now_ms, MediaEvent &ev) {
    if (a == MediaAction::None || value == 2) return false;
    const bool held_kind = a == MediaAction::Next || a == MediaAction::Prev;
    const int k = static_cast<int>(a);
    if (value == 1) {
        if (!dedupe_.accept(a, now_ms)) return false;
        if (held_kind) held_[k] = source;
        ev = MediaEvent{a, true};
        return true;
    }
    if (value != 0 || !held_kind) return false;
    auto it = held_.find(k);
    if (it == held_.end() || it->second != source) return false; /* the release of a press that did not act */
    held_.erase(it);
    ev = MediaEvent{a, false};
    return true;
}

std::vector<MediaEvent> MediaPresses::drop_source(const std::string &source) {
    std::vector<MediaEvent> out;
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->second == source) {
            out.push_back(MediaEvent{static_cast<MediaAction>(it->first), false});
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

MediaKeyReader::~MediaKeyReader() { stop(); }

void MediaKeyReader::start(const std::string &devices_path, const std::string &dev_dir) {
    if (th_.joinable()) return;
    devices_path_ = devices_path;
    dev_dir_ = dev_dir;
    quit_.store(false);
    th_ = std::thread([this] { run(); });
}

void MediaKeyReader::stop() {
    quit_.store(true);
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> g(mu_);
    for (auto &kv : fds_) ::close(kv.second);
    fds_.clear();
    names_.clear();
}

std::vector<MediaEvent> MediaKeyReader::take() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<MediaEvent> v(pending_.begin(), pending_.end());
    pending_.clear();
    return v;
}

std::vector<std::string> MediaKeyReader::open_nodes() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<std::string> v;
    for (const auto &kv : names_) v.push_back(kv.second + " " + kv.first);
    return v;
}

bool MediaKeyReader::accept_other(MediaAction a, int value, MediaEvent &ev) {
    std::lock_guard<std::mutex> g(mu_);
    return presses_.feed("lvgl", a, value, mono_ms_now(), ev);
}

void MediaKeyReader::close_node(const std::string &event) {
    auto it = fds_.find(event);
    if (it != fds_.end()) {
        ::close(it->second);
        fds_.erase(it);
    }
    names_.erase(event);
    for (const MediaEvent &e : presses_.drop_source(event)) pending_.push_back(e); /* a held button whose node went away */
}

void MediaKeyReader::rescan() {
    std::ifstream f(devices_path_);
    std::ostringstream ss;
    if (f) ss << f.rdbuf();
    const std::vector<InputDevice> devs = parse_input_devices(ss.str());
    std::lock_guard<std::mutex> g(mu_);
    std::map<std::string, bool> want;
    for (const InputDevice &d : devs) {
        if (!d.media()) continue;
        want[d.event] = true;
        if (fds_.count(d.event)) continue;
        const std::string path = dev_dir_ + "/" + d.event;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue; /* not readable (permissions): skipped silently, tried again at the next scan */
        fds_[d.event] = fd;
        names_[d.event] = d.name;
        std::fprintf(stderr, "[vinyl] media keys: reading \"%s\" (%s)\n", d.name.c_str(), d.event.c_str());
    }
    std::vector<std::string> gone;
    for (const auto &kv : fds_)
        if (!want.count(kv.first)) gone.push_back(kv.first);
    for (const std::string &e : gone) close_node(e);
}

void MediaKeyReader::run() {
    uint64_t last_scan = 0;
    while (!quit_.load()) {
        const uint64_t now = mono_ms_now();
        if (last_scan == 0 || now - last_scan >= 2000) {
            rescan();
            last_scan = now;
        }
        std::vector<pollfd> pfds;
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (const auto &kv : fds_) {
                pfds.push_back(pollfd{kv.second, POLLIN, 0});
                names.push_back(kv.first);
            }
        }
        if (pfds.empty()) {
            for (int i = 0; i < 10 && !quit_.load(); ++i) ::usleep(50 * 1000);
            continue;
        }
        const int r = ::poll(pfds.data(), pfds.size(), 300);
        if (r <= 0) continue;
        for (size_t i = 0; i < pfds.size(); ++i) {
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                std::lock_guard<std::mutex> g(mu_);
                close_node(names[i]);
                continue;
            }
            if (!(pfds[i].revents & POLLIN)) continue;
            input_event ev[16];
            for (;;) {
                const ssize_t n = ::read(pfds[i].fd, ev, sizeof ev);
                if (n <= 0) break;
                const size_t count = static_cast<size_t>(n) / sizeof(input_event);
                for (size_t k = 0; k < count; ++k) {
                    if (ev[k].type != EV_KEY) continue;
                    const MediaAction a = media_action_for(ev[k].code);
                    if (a == MediaAction::None) continue;
                    std::lock_guard<std::mutex> g(mu_);
                    MediaEvent me;
                    if (presses_.feed(names[i], a, ev[k].value, mono_ms_now(), me)) pending_.push_back(me);
                }
                if (static_cast<size_t>(n) < sizeof ev) break;
            }
        }
    }
}

}  // namespace vinyl
