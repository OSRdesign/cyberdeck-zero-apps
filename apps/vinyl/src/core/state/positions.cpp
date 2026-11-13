/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl positions in long files (see positions.hpp).
 */
#include "positions.hpp"

#include "../util/atomic_file.hpp"
#include "../util/json.hpp"

#include <algorithm>
#include <cstdlib>

namespace vinyl {

uint64_t Positions::get(const std::string &path) const {
    for (const auto &kv : items_)
        if (kv.first == path) return kv.second;
    return 0;
}

bool Positions::erase(const std::string &path) {
    for (auto it = items_.begin(); it != items_.end(); ++it)
        if (it->first == path) {
            items_.erase(it);
            return true;
        }
    return false;
}

bool Positions::note(const std::string &path, uint64_t pos_ms, uint64_t duration_ms) {
    if (path.empty()) return false;
    if (!is_long_file(duration_ms) || pos_ms < kPositionMinMs || pos_ms + kPositionEndMarginMs >= duration_ms)
        return erase(path);
    const auto it = std::find_if(items_.begin(), items_.end(), [&](const auto &kv) { return kv.first == path; });
    if (it != items_.end()) {
        if (it->second == pos_ms && it + 1 == items_.end()) return false;
        items_.erase(it);
    }
    items_.emplace_back(path, pos_ms);
    while (items_.size() > kMaxPositions) items_.erase(items_.begin());
    return true;
}

std::string Positions::to_json() const {
    json::Value root = json::Value::object();
    root.set("v", json::Value::number(1));
    json::Value arr;
    arr.type = json::Value::Arr;
    for (const auto &kv : items_) {
        json::Value e = json::Value::object();
        e.set("path", json::Value::string(kv.first));
        e.set("ms", json::Value::number(static_cast<double>(kv.second)));
        arr.arr.push_back(e);
    }
    root.set("positions", arr);
    return json::write(root);
}

bool Positions::from_json(const std::string &text) {
    items_.clear();
    json::Value root;
    if (!json::parse(text, root) || !root.is_obj()) return false;
    const json::Value *a = root.get("positions");
    if (!a || a->type != json::Value::Arr) return false;
    for (const json::Value &e : a->arr) {
        const json::Value *p = e.get("path"), *ms = e.get("ms");
        if (!p || p->type != json::Value::Str || p->s.empty() || !ms || ms->type != json::Value::Num || ms->n < 0) continue;
        erase(p->s);
        items_.emplace_back(p->s, static_cast<uint64_t>(ms->n));
    }
    while (items_.size() > kMaxPositions) items_.erase(items_.begin());
    return true;
}

std::string positions_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else {
        const char *home = std::getenv("HOME");
        base = std::string(home ? home : ".") + "/.config";
    }
    return base + "/vinyl/positions.json";
}

int save_positions(FileOps &ops, const std::string &path, const Positions &p) {
    const int r = make_dirs(ops, dir_name(path));
    if (r < 0) return r;
    return atomic_write_file(ops, path, p.to_json());
}

bool load_positions(FileOps &ops, const std::string &path, Positions &p) {
    std::string text;
    if (read_small_file(ops, path, text, 1u << 20) != 0) {
        p = Positions();
        return false;
    }
    return p.from_json(text);
}

}  // namespace vinyl
