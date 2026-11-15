/*
 * SPDX-License-Identifier: MIT
 *
 * VHS settings (see settings.hpp).
 */
#include "settings.hpp"

#include "scenes/atomic_file.hpp"
#include "scenes/fileops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace vhs {

bool Settings::operator==(const Settings &o) const
{
    return folder == o.folder && prefix == o.prefix && have_photo_mode == o.have_photo_mode &&
           (!have_photo_mode || photo_mode == o.photo_mode) && photo_size == o.photo_size && video_size == o.video_size &&
           self_timer_s == o.self_timer_s && split == o.split && output == o.output && split_mib == o.split_mib &&
           min_free_mb == o.min_free_mb && camera_id == o.camera_id;
}

bool valid_prefix(const std::string &p)
{
    if (p.empty() || p.size() > 24 || p[0] == '.' || p.front() == ' ' || p.back() == ' ') return false;
    for (char c : p) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

int clamp_self_timer(int s)
{
    static const int kSteps[] = {0, 3, 5, 10};
    int best = 0;
    for (int v : kSteps)
        if (std::abs(v - s) < std::abs(best - s)) best = v;
    return best;
}

std::string settings_path()
{
    if (const char *p = std::getenv("VHS_SETTINGS"))
        if (p[0]) return p;
    if (const char *x = std::getenv("XDG_CONFIG_HOME"))
        if (x[0]) return std::string(x) + "/vhs/settings.json";
    const char *home = std::getenv("HOME");
    return std::string(home && home[0] ? home : ".") + "/.config/vhs/settings.json";
}

std::string settings_to_json(const Settings &s, const json::Value *keep)
{
    json::Value v = (keep && keep->is_obj()) ? *keep : json::Value::object();
    v.set("folder", json::Value::string(s.folder));
    v.set("prefix", json::Value::string(s.prefix));
    if (s.have_photo_mode) v.set("photo_mode", json::Value::string(photo_mode_name(s.photo_mode)));
    else v.erase("photo_mode");
    if (s.photo_size.valid()) v.set("photo_size", json::Value::string(size_text(s.photo_size)));
    else v.erase("photo_size");
    if (s.video_size.valid()) v.set("video_size", json::Value::string(size_text(s.video_size)));
    else v.erase("video_size");
    v.set("self_timer_s", json::Value::number(s.self_timer_s));
    v.set("split", json::Value::boolean(s.split));
    v.set("output", json::Value::string(s.output));
    v.set("split_mib", json::Value::number(s.split_mib));
    v.set("min_free_mb", json::Value::number(s.min_free_mb));
    if (!s.camera_id.empty()) v.set("camera_id", json::Value::string(s.camera_id));
    else v.erase("camera_id");
    return json::write(v);
}

bool settings_from_json(const std::string &text, Settings &out, json::Value *raw)
{
    out = Settings();
    json::Value v;
    if (!json::parse(text, v) || !v.is_obj()) {
        if (raw) *raw = json::Value::object();
        return false;
    }
    auto str = [&v](const char *k, std::string &to) {
        const json::Value *x = v.get(k);
        if (x && x->type == json::Value::Str) {
            to = x->s;
            return true;
        }
        return false;
    };
    auto num = [&v](const char *k, int lo, int hi, int &to) {
        const json::Value *x = v.get(k);
        if (x && x->type == json::Value::Num && std::isfinite(x->n))
            to = static_cast<int>(std::lround(std::min<double>(hi, std::max<double>(lo, x->n))));
    };
    std::string s;
    if (str("folder", s) && !s.empty()) out.folder = s;
    if (str("prefix", s) && valid_prefix(s)) out.prefix = s;
    if (str("photo_mode", s)) out.have_photo_mode = parse_photo_mode(s, out.photo_mode);
    if (str("photo_size", s)) parse_size(s, out.photo_size);
    if (str("video_size", s)) parse_size(s, out.video_size);
    int t = 0;
    const json::Value *st = v.get("self_timer_s");
    if (st && st->type == json::Value::Num && std::isfinite(st->n)) {
        t = static_cast<int>(std::lround(std::min(100.0, std::max(0.0, st->n))));
        out.self_timer_s = clamp_self_timer(t);
    }
    const json::Value *sp = v.get("split");
    if (sp && sp->type == json::Value::Bool) out.split = sp->b;
    str("output", out.output);
    num("split_mib", 64, 4096, out.split_mib);
    num("min_free_mb", 100, 100000, out.min_free_mb);
    str("camera_id", out.camera_id);
    if (raw) *raw = v;
    return true;
}

SettingsStore::SettingsStore(FileOps &ops) : ops_(ops) {}

void SettingsStore::load(const std::string &path)
{
    path_ = path;
    std::string text;
    if (read_small_file(ops_, path, text, 1u << 16) != 0 || !settings_from_json(text, s_, &raw_)) {
        s_ = Settings();
        raw_ = json::Value::object();
    }
}

int SettingsStore::set(const Settings &s)
{
    if (s == s_) return 1;
    s_ = s;
    if (read_only_ || path_.empty()) return 0;
    int r = make_dirs(ops_, dir_name(path_));
    if (r == 0) r = atomic_write_file(ops_, path_, settings_to_json(s_, &raw_));
    if (r == 0) {
        ++writes_;
        settings_from_json(settings_to_json(s_, &raw_), s_, &raw_);
    }
    return r;
}

} // namespace vhs
