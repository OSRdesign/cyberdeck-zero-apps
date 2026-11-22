/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl network shares, which tracks live on one and whether it answers (the glyph: a small server, teal when fine, amber and
 * slashed when the share is not answering; 0.8.0, brief D):
 *
 *   ShareIndex  "is this path on a share": under <mount root>/<name> (the helper's shares: /mnt/cyberdeck/<name>), or on a
 *               mount of a network type the system made (0.7.1's rule: cifs, nfs, sshfs, an automount point ...: net_share).
 *               Built from the text of /proc/self/mounts (reading it never touches a share) and the helper's list; pure,
 *               cheap to ask per row.
 *   ShareWatch  a thread of its own that asks each root it is given with the bounded probe of 0.7.1 (net_share
 *               probe_share_root: at most 3 s, then the root's back-off): every 10 s while it answers, every 5 s while it
 *               does not. The UI only reads the states (never a read of a share on the UI thread: a dead share holds a read
 *               20-45 s). gen() changes when a state changes.
 * LVGL-free.
 */
#pragma once

#include "share_helper.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vinyl {
namespace shares {

class ShareIndex {
public:
    /* mounts_text: /proc/self/mounts; mount_root: kMountRoot (the scenes: a folder of their own); listed: the helper's shares */
    void update(const std::string &mounts_text, const std::string &mount_root, const std::vector<ShareEntry> &listed);
    /* the root of the share `path` lives on ("" : not on a share) */
    std::string root_of(const std::string &path) const;
    /* the name shown for a root: the helper's name, else the folder's own name */
    std::string name_of(const std::string &root) const;
    /* the helper's share at that root (null: none, a share the system mounted) */
    const ShareEntry *entry_of(const std::string &root) const;
    /* every root known (the helper's shares, the network mounts) */
    std::vector<std::string> roots() const;
    const std::string &mount_root() const { return mount_root_; }

private:
    std::string mount_root_ = kMountRoot;
    std::vector<std::string> net_mounts_; /* the system's network mounts, longest first */
    std::vector<ShareEntry> listed_;
};

enum class ShareState { Unknown, Ok, Down };
/* the glyph a path gets: none (not on a share), Share (teal: it answers, or not asked yet), ShareDown (amber, slashed) */
enum class Glyph { None, Share, ShareDown };
Glyph glyph_for(const ShareIndex &idx, ShareState st, const std::string &path);

class ShareWatch {
public:
    struct Config {
        uint32_t ok_every_ms = 10000;   /* a root that answers is asked again after this */
        uint32_t down_every_ms = 5000;  /* one that does not, after this */
        uint32_t tick_ms = 250;         /* how often the thread looks for a root that is due */
        std::function<bool(const std::string &root)> probe; /* default: probe_share_root (bounded, 3 s) */
    };
    explicit ShareWatch(Config c);
    ~ShareWatch();
    ShareWatch(const ShareWatch &) = delete;
    ShareWatch &operator=(const ShareWatch &) = delete;

    /* the roots to watch (others are forgotten) */
    void set_roots(const std::vector<std::string> &roots);
    ShareState state(const std::string &root) const;
    /* ask this root at once (a track on it is about to start, the Music folders page opened) */
    void poke(const std::string &root);
    /* the scenes and the tests: a state set from outside */
    void set_state(const std::string &root, ShareState st);
    uint64_t gen() const;
    void stop();

private:
    void loop();
    struct Slot {
        ShareState st = ShareState::Unknown;
        uint64_t due = 0; /* steady ms */
    };
    Config cfg_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Slot> slots_;
    uint64_t gen_ = 1;
    bool quit_ = false;
    std::thread th_;
};

}  // namespace shares
}  // namespace vinyl
