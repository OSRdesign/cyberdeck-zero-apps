/*
 * SPDX-License-Identifier: MIT
 */
#include "settings.hpp"

#include "atomic_file.hpp"
#include "naming.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace cassette {

const int kRecordTimerChoices[4] = {0, 3, 10, 20};

namespace {

bool get_bool(const json::Value &o, const char *k, bool &out) {
    const json::Value *v = o.get(k);
    if (!v || v->type != json::Value::Bool) return false;
    out = v->b;
    return true;
}

bool get_num(const json::Value &o, const char *k, double &out) {
    const json::Value *v = o.get(k);
    if (!v || v->type != json::Value::Num || !std::isfinite(v->n)) return false;
    out = v->n;
    return true;
}

bool get_str(const json::Value &o, const char *k, std::string &out) {
    const json::Value *v = o.get(k);
    if (!v || v->type != json::Value::Str) return false;
    out = v->s;
    return true;
}

bool valid_folder(const std::string &f) {
    if (f.empty() || f.size() > 1024) return false;
    for (unsigned char c : f)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

std::string card_descriptor(const std::string &name, const std::string &by_id, const std::string &vid_pid) {
    const bool no_id = vid_pid.empty() || vid_pid == "0000:0000";
    return name + "|" + by_id + "|" + (no_id ? std::string() : vid_pid);
}

std::string default_settings_path(const std::string &home) { return home + "/.config/cassette/settings.json"; }

const char *format_key(RecFormat f) {
    switch (f) {
    case RecFormat::Flac: return "flac";
    case RecFormat::Opus: return "opus";
    default: return "wav";
    }
}

const char *scan_mode_key(ScanMode m) { return m == ScanMode::Jump ? "jump" : "scan"; }

void clamp_settings(Settings &s) {
    if (s.sync_interval_s < kSyncMin) s.sync_interval_s = kSyncMin;
    if (s.sync_interval_s > kSyncMax) s.sync_interval_s = kSyncMax;
    int best = kRecordTimerChoices[0];
    for (int c : kRecordTimerChoices)
        if (std::abs(c - s.record_timer_s) < std::abs(best - s.record_timer_s)) best = c;
    s.record_timer_s = best;
    if (!std::isfinite(s.soft_gain_db)) s.soft_gain_db = 0;
    s.soft_gain_db = clampd(s.soft_gain_db, kSoftGainMin, kSoftGainMax);
    if (!valid_prefix(s.prefix)) s.prefix = kDefaultPrefix;
    if (!valid_folder(s.folder)) s.folder = Settings().folder;
}

int SettingsStore::load() {
    s_ = Settings();
    cards_.clear();
    forgotten_.clear();
    raw_ = json::Value::object();
    std::string body;
    int r = read_small_file(ops_, path_, body, 1u << 20);
    if (r < 0) return r == -ENOENT ? -ENOENT : -EINVAL;
    json::Value root;
    if (!json::parse(body, root) || !root.is_obj()) return -EINVAL;
    raw_ = root;

    std::string str;
    double d = 0;
    if (get_str(root, "format", str)) {
        if (str == "wav") s_.format = RecFormat::Wav;
        else if (str == "flac") s_.format = RecFormat::Flac;
        else if (str == "opus") s_.format = RecFormat::Opus;
    }
    if (get_str(root, "folder", str) && valid_folder(str)) s_.folder = str;
    if (get_str(root, "prefix", str) && valid_prefix(str)) s_.prefix = str;
    if (get_num(root, "sync_interval_s", d)) s_.sync_interval_s = static_cast<int>(std::lround(clampd(d, -1e6, 1e6)));
    get_bool(root, "auto_split", s_.auto_split);
    if (get_num(root, "record_timer_s", d)) s_.record_timer_s = static_cast<int>(std::lround(clampd(d, -1e6, 1e6)));
    if (get_str(root, "rew_fwd", str)) {
        if (str == "scan") s_.rew_fwd = ScanMode::Fast;
        else if (str == "jump") s_.rew_fwd = ScanMode::Jump;
    }
    get_bool(root, "low_cut", s_.low_cut);
    get_bool(root, "limiter", s_.limiter);
    if (get_num(root, "soft_gain_db", d)) s_.soft_gain_db = d;
    if (get_str(root, "card", str) && valid_folder(str)) s_.card = str;
    if (get_str(root, "output_device", str) && valid_folder(str)) s_.output_device = str;
    clamp_settings(s_);

    if (const json::Value *cards = root.get("cards")) {
        if (cards->is_obj()) {
            for (const auto &kv : cards->obj) {
                if (!kv.second.is_obj()) continue;
                CardState c;
                if (get_num(kv.second, "level_db", d)) {
                    c.has_level = true;
                    c.level_db = clampd(d, kCardLevelMin, kCardLevelMax);
                }
                get_bool(kv.second, "auto_gain", c.auto_gain);
                get_bool(kv.second, "mute", c.mute);
                cards_[kv.first] = c;
            }
        }
    }
    return 0;
}

int SettingsStore::save() {
    clamp_settings(s_);
    json::Value root = raw_.is_obj() ? raw_ : json::Value::object();
    root.set("version", json::Value::number(1));
    root.set("format", json::Value::string(format_key(s_.format)));
    root.set("folder", json::Value::string(s_.folder));
    root.set("prefix", json::Value::string(s_.prefix));
    root.set("sync_interval_s", json::Value::number(s_.sync_interval_s));
    root.set("auto_split", json::Value::boolean(s_.auto_split));
    root.set("record_timer_s", json::Value::number(s_.record_timer_s));
    root.set("rew_fwd", json::Value::string(scan_mode_key(s_.rew_fwd)));
    root.set("low_cut", json::Value::boolean(s_.low_cut));
    root.set("limiter", json::Value::boolean(s_.limiter));
    root.set("soft_gain_db", json::Value::number(s_.soft_gain_db));
    root.set("card", json::Value::string(s_.card));
    root.set("output_device", json::Value::string(s_.output_device));

    json::Value cards = json::Value::object();
    if (const json::Value *old = root.get("cards"))
        if (old->is_obj()) cards = *old;
    for (const std::string &f : forgotten_) cards.erase(f);
    for (const auto &kv : cards_) {
        json::Value entry = json::Value::object();
        if (json::Value *old = cards.get(kv.first))
            if (old->is_obj()) entry = *old;
        if (kv.second.has_level) entry.set("level_db", json::Value::number(kv.second.level_db));
        else entry.erase("level_db");
        entry.set("auto_gain", json::Value::boolean(kv.second.auto_gain));
        entry.set("mute", json::Value::boolean(kv.second.mute));
        cards.set(kv.first, entry);
    }
    root.set("cards", cards);

    int r = make_dirs(ops_, dir_name(path_));
    if (r < 0) return r;
    r = atomic_write_file(ops_, path_, json::write(root));
    if (r < 0) return r;
    raw_ = root;
    forgotten_.clear();
    return 0;
}

bool SettingsStore::card_state(const std::string &descriptor, CardState &out) const {
    auto it = cards_.find(descriptor);
    if (it == cards_.end()) return false;
    out = it->second;
    return true;
}

void SettingsStore::set_card_state(const std::string &descriptor, const CardState &st) {
    CardState c = st;
    if (!std::isfinite(c.level_db)) c.has_level = false, c.level_db = 0;
    c.level_db = clampd(c.level_db, kCardLevelMin, kCardLevelMax);
    cards_[descriptor] = c;
    for (auto it = forgotten_.begin(); it != forgotten_.end();)
        it = (*it == descriptor) ? forgotten_.erase(it) : it + 1;
}

bool SettingsStore::forget_card(const std::string &descriptor) {
    if (!cards_.erase(descriptor)) return false;
    forgotten_.push_back(descriptor);
    return true;
}

std::vector<std::string> SettingsStore::cards() const {
    std::vector<std::string> v;
    for (const auto &kv : cards_) v.push_back(kv.first);
    return v;
}

}  // namespace cassette
