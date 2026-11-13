/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl settings (see settings.hpp).
 */
#include "settings.hpp"

#include "../util/atomic_file.hpp"

#include <cstdlib>

namespace vinyl {

std::string settings_path() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) base = xdg;
    else {
        const char *home = std::getenv("HOME");
        base = std::string(home ? home : ".") + "/.config";
    }
    return base + "/vinyl/settings.json";
}

std::string settings_to_json(const Settings &s, const json::Value *keep) {
    json::Value root = keep && keep->is_obj() ? *keep : json::Value::object();
    root.set("v", json::Value::number(1));
    root.set("gapless", json::Value::boolean(s.gapless));
    root.set("replaygain", json::Value::string(replaygain_mode_name(s.replaygain)));
    root.set("remember_long", json::Value::boolean(s.remember_long));
    return json::write(root);
}

bool settings_from_json(const std::string &text, Settings &out, json::Value *raw) {
    out = Settings();
    json::Value root;
    if (!json::parse(text, root) || !root.is_obj()) return false;
    if (const json::Value *g = root.get("gapless"); g && g->type == json::Value::Bool) out.gapless = g->b;
    if (const json::Value *r = root.get("replaygain"); r && r->type == json::Value::Str)
        out.replaygain = replaygain_mode_from(r->s, out.replaygain);
    if (const json::Value *l = root.get("remember_long"); l && l->type == json::Value::Bool) out.remember_long = l->b;
    if (raw) *raw = root;
    return true;
}

SettingsStore::SettingsStore(FileOps &ops) : ops_(ops) {}

void SettingsStore::load(const std::string &path) {
    path_ = path;
    s_ = Settings();
    raw_ = json::Value::object();
    std::string text;
    if (read_small_file(ops_, path, text, 256u << 10) != 0) return;
    json::Value raw;
    if (settings_from_json(text, s_, &raw)) raw_ = raw;
    else s_ = Settings();
}

int SettingsStore::set(const Settings &s) {
    if (s == s_) return 1;
    s_ = s;
    if (path_.empty()) return 0;
    int r = make_dirs(ops_, dir_name(path_));
    if (r >= 0) r = atomic_write_file(ops_, path_, settings_to_json(s_, &raw_));
    ++writes_;
    return r < 0 ? r : 0;
}

}  // namespace vinyl
