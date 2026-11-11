/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette tests: the launcher's state files in memory (FactsIo of launcher_facts.hpp, task T8): text, a stamp that moves at every
 * write, the live pids, counters of the stats and reads. Shared by tests/pulse and tests/app.
 */
#pragma once

#include "launcher_facts.hpp"

#include <map>
#include <string>
#include <vector>

namespace cassette {

/* Files in memory: text, a stamp that moves at every write, the live pids, and how many reads and stats were made. */
struct FakeFiles {
    std::map<std::string, std::string> text;
    std::map<std::string, int> gen;
    std::vector<long> alive;
    int stats = 0, reads = 0;
    void put(const std::string &path, const std::string &t) {
        text[path] = t;
        ++gen[path];
    }
    void remove(const std::string &path) { text.erase(path); }
};

class FakeFactsIo : public FactsIo {
public:
    explicit FakeFactsIo(FakeFiles &f) : f_(f) {}
    bool stat(const std::string &path, std::string &stamp) override {
        ++f_.stats;
        if (!f_.text.count(path)) return false;
        stamp = std::to_string(f_.gen[path]);
        return true;
    }
    bool read(const std::string &path, std::string &text, size_t) override {
        ++f_.reads;
        auto it = f_.text.find(path);
        if (it == f_.text.end()) return false;
        text = it->second;
        return true;
    }
    bool pid_alive(long pid) override {
        for (long p : f_.alive)
            if (p == pid) return true;
        return false;
    }

private:
    FakeFiles &f_;
};

inline std::string fake_status_text(const std::string &bt_audio_dev, long pid = 801) {
    std::string t = "v=1\ngen=12\npid=" + std::to_string(pid) + "\nwifi=1\nbt=connected\nvolume=40\n";
    if (!bt_audio_dev.empty()) t += "bt_audio_dev=" + bt_audio_dev + "\n";
    return t;
}

inline std::string fake_board_text(const std::string &extra, long pid = 801) {
    return "v=1\ngen=2\npid=" + std::to_string(pid) + "\nlauncher=0.10.1\n" + extra;
}

}  // namespace cassette
