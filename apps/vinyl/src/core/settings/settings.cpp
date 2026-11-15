/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl settings (see settings.hpp).
 */
#include "settings.hpp"

#include "../util/atomic_file.hpp"

#include <algorithm>
#include <cmath>
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

const char *volume_steps_name(VolumeSteps v) {
    switch (v) {
    case VolumeSteps::Db1: return "1";
    case VolumeSteps::Db05: return "0.5";
    case VolumeSteps::Db01: return "0.1";
    default: return "normal";
    }
}

const char *volume_steps_word(VolumeSteps v) {
    switch (v) {
    case VolumeSteps::Db1: return "1 dB";
    case VolumeSteps::Db05: return "0.5 dB";
    case VolumeSteps::Db01: return "0.1 dB";
    default: return "Normal";
    }
}

VolumeSteps volume_steps_from(const std::string &s, VolumeSteps fallback) {
    for (VolumeSteps v : {VolumeSteps::Normal, VolumeSteps::Db1, VolumeSteps::Db05, VolumeSteps::Db01})
        if (s == volume_steps_name(v)) return v;
    return fallback;
}

double volume_steps_db(VolumeSteps v) {
    switch (v) {
    case VolumeSteps::Db1: return 1.0;
    case VolumeSteps::Db05: return 0.5;
    case VolumeSteps::Db01: return 0.1;
    default: return 0.0;
    }
}

std::string boost_word(int db) {
    if (db <= 0) return "Off";
    return "+" + std::to_string(db > kMaxBoostDb ? kMaxBoostDb : db) + " dB";
}

std::string settings_to_json(const Settings &s, const json::Value *keep) {
    json::Value root = keep && keep->is_obj() ? *keep : json::Value::object();
    root.set("v", json::Value::number(1));
    root.set("gapless", json::Value::boolean(s.gapless));
    root.set("replaygain", json::Value::string(replaygain_mode_name(s.replaygain)));
    root.set("remember_long", json::Value::boolean(s.remember_long));
    root.set("volume_steps", json::Value::string(volume_steps_name(s.volume_steps)));
    if (!s.boost_db.empty() || root.get("boost_db")) {
        json::Value b = json::Value::object();
        for (const auto &kv : s.boost_db) b.set(kv.first, json::Value::number(kv.second));
        root.set("boost_db", b);
    }
    const json::Value *old = root.get("library");
    if (!s.library_folders.empty() || (old && old->is_obj())) {
        json::Value lib = old && old->is_obj() ? *old : json::Value::object(); /* a newer version's keys kept */
        json::Value folders;
        folders.type = json::Value::Arr;
        for (const std::string &f : s.library_folders) folders.arr.push_back(json::Value::string(f));
        lib.set("folders", folders);
        root.set("library", lib);
    }
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
    if (const json::Value *v = root.get("volume_steps"); v && v->type == json::Value::Str)
        out.volume_steps = volume_steps_from(v->s, out.volume_steps);
    if (const json::Value *b = root.get("boost_db"); b && b->is_obj())
        for (const auto &kv : b->obj)
            if (kv.second.type == json::Value::Num && !kv.first.empty() && kv.second.n == kv.second.n)
                out.set_boost(kv.first, static_cast<int>(std::lround(std::max(-100.0, std::min(100.0, kv.second.n)))));
    if (const json::Value *lib = root.get("library"); lib && lib->is_obj())
        if (const json::Value *f = lib->get("folders"); f && f->type == json::Value::Arr)
            for (const json::Value &v : f->arr)
                if (v.type == json::Value::Str && !v.s.empty() && v.s[0] == '/') out.library_folders.push_back(v.s);
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
