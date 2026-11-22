/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares, the index and the watch (see share_watch.hpp).
 */
#include "share_watch.hpp"

#include "../library/net_share.hpp"
#include "../library/roots.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace vinyl {
namespace shares {

namespace {

uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool under(const std::string &path, const std::string &dir) {
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

std::string base_name(const std::string &p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

/* ------------------------------------------------------------------------------------------------ ShareIndex */

void ShareIndex::update(const std::string &mounts_text, const std::string &mount_root, const std::vector<ShareEntry> &listed) {
    mount_root_ = mount_root;
    while (mount_root_.size() > 1 && mount_root_.back() == '/') mount_root_.pop_back();
    listed_ = listed;
    net_mounts_.clear();
    for (const MountEntry &m : parse_mounts(mounts_text))
        if (is_share_fs(m.type) && m.dir != "/" && std::find(net_mounts_.begin(), net_mounts_.end(), m.dir) == net_mounts_.end())
            net_mounts_.push_back(m.dir);
    std::sort(net_mounts_.begin(), net_mounts_.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
}

std::string ShareIndex::root_of(const std::string &path) const {
    if (path.empty()) return "";
    if (under(path, mount_root_)) { /* <mount root>/<name>/...: the helper's share (mounted or not) */
        const size_t s = path.find('/', mount_root_.size() + 1);
        return s == std::string::npos ? path : path.substr(0, s);
    }
    for (const std::string &d : net_mounts_)
        if (path == d || under(path, d)) return d;
    return "";
}

std::string ShareIndex::name_of(const std::string &root) const {
    if (const ShareEntry *e = entry_of(root)) return e->name;
    return base_name(root);
}

const ShareEntry *ShareIndex::entry_of(const std::string &root) const {
    for (const ShareEntry &e : listed_)
        if (mount_root_ + "/" + e.name == root) return &e;
    return nullptr;
}

std::vector<std::string> ShareIndex::roots() const {
    std::vector<std::string> out;
    for (const ShareEntry &e : listed_) out.push_back(mount_root_ + "/" + e.name);
    for (const std::string &d : net_mounts_) {
        const std::string r = root_of(d);
        if (!r.empty() && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    }
    return out;
}

Glyph glyph_for(const ShareIndex &idx, ShareState st, const std::string &path) {
    if (idx.root_of(path).empty()) return Glyph::None;
    return st == ShareState::Down ? Glyph::ShareDown : Glyph::Share;
}

/* ------------------------------------------------------------------------------------------------ ShareWatch */

ShareWatch::ShareWatch(Config c) : cfg_(std::move(c)) {
    if (!cfg_.probe) cfg_.probe = [](const std::string &root) { return probe_share_root(root); };
    th_ = std::thread([this] { loop(); });
}

ShareWatch::~ShareWatch() { stop(); }

void ShareWatch::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return;
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void ShareWatch::set_roots(const std::vector<std::string> &roots) {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, Slot> next;
    for (const std::string &r : roots) {
        if (r.empty()) continue;
        auto it = slots_.find(r);
        next[r] = it != slots_.end() ? it->second : Slot{};
    }
    slots_.swap(next);
    cv_.notify_all();
}

ShareState ShareWatch::state(const std::string &root) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(root);
    return it == slots_.end() ? ShareState::Unknown : it->second.st;
}

void ShareWatch::poke(const std::string &root) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(root);
    if (it == slots_.end()) slots_[root] = Slot{};
    else it->second.due = 0;
    cv_.notify_all();
}

void ShareWatch::set_state(const std::string &root, ShareState st) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot &s = slots_[root];
    if (s.st != st) ++gen_;
    s.st = st;
    s.due = UINT64_MAX; /* not asked again */
}

uint64_t ShareWatch::gen() const {
    std::lock_guard<std::mutex> lk(mu_);
    return gen_;
}

void ShareWatch::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!quit_) {
        const uint64_t now = steady_ms();
        std::string due;
        for (const auto &kv : slots_)
            if (kv.second.due <= now) {
                due = kv.first;
                break;
            }
        if (due.empty()) {
            cv_.wait_for(lk, std::chrono::milliseconds(cfg_.tick_ms));
            continue;
        }
        slots_[due].due = now + cfg_.down_every_ms; /* not picked again while it is asked */
        lk.unlock();
        const bool ok = cfg_.probe(due); /* bounded: never longer than the runner's timeout */
        lk.lock();
        auto it = slots_.find(due);
        if (it == slots_.end()) continue; /* forgotten meanwhile */
        const ShareState st = ok ? ShareState::Ok : ShareState::Down;
        if (it->second.st != st) {
            ++gen_;
            std::fprintf(stderr, "[vinyl] shares: %s %s\n", due.c_str(), ok ? "answers" : "does not answer");
        }
        it->second.st = st;
        if (it->second.due != 0) it->second.due = steady_ms() + (ok ? cfg_.ok_every_ms : cfg_.down_every_ms); /* a poke meanwhile keeps its 0 */
    }
}

}  // namespace shares
}  // namespace vinyl
