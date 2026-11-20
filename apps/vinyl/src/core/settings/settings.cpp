/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl settings (see settings.hpp).
 */
#include "settings.hpp"

#include "../dsp/crossfade.hpp"
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

const char *transition_name(Transition t) {
    switch (t) {
    case Transition::Normal: return "normal";
    case Transition::Crossfade5: return "crossfade_5";
    case Transition::Crossfade10: return "crossfade_10";
    case Transition::Crossfade20: return "crossfade_20";
    default: return "gapless";
    }
}

const char *transition_word(Transition t) {
    switch (t) {
    case Transition::Normal: return "Normal";
    case Transition::Crossfade5: return "Crossfade 5 sec";
    case Transition::Crossfade10: return "Crossfade 10 sec";
    case Transition::Crossfade20: return "Crossfade 20 sec";
    default: return "Gapless";
    }
}

Transition transition_from(const std::string &s, Transition fallback) {
    for (Transition t : kTransitions)
        if (s == transition_name(t)) return t;
    return fallback;
}

int transition_crossfade_s(Transition t) {
    switch (t) {
    case Transition::Crossfade5: return 5;
    case Transition::Crossfade10: return 10;
    case Transition::Crossfade20: return 20;
    default: return 0;
    }
}

std::string transition_chip_text(Transition t, bool fading, int fade_s) {
    if (fading) {
        const int s = crossfade_valid(fade_s) ? crossfade_valid(fade_s) : transition_crossfade_s(t);
        return s > 0 ? "Crossfade " + crossfade_word(s) : std::string("Crossfade");
    }
    if (t == Transition::Normal) return "";
    const int s = transition_crossfade_s(t);
    return s > 0 ? "Crossfade " + crossfade_word(s) : std::string("Gapless");
}

Transition transition_migrate(bool gapless, int crossfade_s) {
    if (crossfade_s <= 0) return gapless ? Transition::Gapless : Transition::Normal;
    /* the nearest of 5, 10, 20 (the draft offered 2 ... 10: 2-6 s are 5 sec, 8 and 10 s are 10 sec; a tie goes down) */
    if (crossfade_s <= 7) return Transition::Crossfade5;
    if (crossfade_s <= 15) return Transition::Crossfade10;
    return Transition::Crossfade20;
}

std::string settings_to_json(const Settings &s, const json::Value *keep) {
    json::Value root = keep && keep->is_obj() ? *keep : json::Value::object();
    root.set("v", json::Value::number(1));
    root.set("transition", json::Value::string(transition_name(s.transition)));
    root.erase("gapless"); /* the older keys: read once when "transition" is missing, never written again */
    root.erase("crossfade");
    root.set("replaygain", json::Value::string(replaygain_mode_name(s.replaygain)));
    root.set("remember_long", json::Value::boolean(s.remember_long));
    root.set("knock_knock", json::Value::boolean(s.knock));
    root.set("volume_steps", json::Value::string(volume_steps_name(s.volume_steps)));
    if (!s.boost_db.empty() || root.get("boost_db")) {
        json::Value b = json::Value::object();
        for (const auto &kv : s.boost_db) b.set(kv.first, json::Value::number(kv.second));
        root.set("boost_db", b);
    }
    {
        const json::Value *oe = root.get("eq");
        json::Value eq = oe && oe->is_obj() ? *oe : json::Value::object(); /* a newer version's keys kept */
        eq.set("on", json::Value::boolean(s.eq.on));
        eq.set("preset", json::Value::string(eq_preset_name(s.eq.preset)));
        json::Value bands;
        bands.type = json::Value::Arr;
        for (int v : s.eq.custom) bands.arr.push_back(json::Value::number(v));
        eq.set("custom", bands);
        root.set("eq", eq);
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
    {
        /* "transition"; else 0.6.0's "gapless" and the 0.7.0 draft's "crossfade" (seconds) */
        bool gapless = true;
        int fade = 0;
        if (const json::Value *g = root.get("gapless"); g && g->type == json::Value::Bool) gapless = g->b;
        if (const json::Value *x = root.get("crossfade"); x && x->type == json::Value::Num && x->n == x->n)
            fade = static_cast<int>(std::lround(std::max(-100.0, std::min(100.0, x->n))));
        out.transition = transition_migrate(gapless, fade);
        if (const json::Value *t = root.get("transition"); t && t->type == json::Value::Str)
            out.transition = transition_from(t->s, out.transition);
    }
    if (const json::Value *r = root.get("replaygain"); r && r->type == json::Value::Str)
        out.replaygain = replaygain_mode_from(r->s, out.replaygain);
    if (const json::Value *l = root.get("remember_long"); l && l->type == json::Value::Bool) out.remember_long = l->b;
    if (const json::Value *k = root.get("knock_knock"); k && k->type == json::Value::Bool) out.knock = k->b;
    if (const json::Value *v = root.get("volume_steps"); v && v->type == json::Value::Str)
        out.volume_steps = volume_steps_from(v->s, out.volume_steps);
    if (const json::Value *b = root.get("boost_db"); b && b->is_obj())
        for (const auto &kv : b->obj)
            if (kv.second.type == json::Value::Num && !kv.first.empty() && kv.second.n == kv.second.n)
                out.set_boost(kv.first, static_cast<int>(std::lround(std::max(-100.0, std::min(100.0, kv.second.n)))));
    if (const json::Value *eq = root.get("eq"); eq && eq->is_obj()) {
        if (const json::Value *on = eq->get("on"); on && on->type == json::Value::Bool) out.eq.on = on->b;
        if (const json::Value *p = eq->get("preset"); p && p->type == json::Value::Str) out.eq.preset = eq_preset_from(p->s, EqPreset::Flat);
        if (const json::Value *c = eq->get("custom"); c && c->type == json::Value::Arr && c->arr.size() == static_cast<size_t>(kEqBands))
            for (int i = 0; i < kEqBands; ++i) {
                const json::Value &v = c->arr[static_cast<size_t>(i)];
                if (v.type == json::Value::Num && v.n == v.n)
                    out.eq.custom[static_cast<size_t>(i)] = static_cast<int>(std::lround(std::max<double>(-kEqMaxDb, std::min<double>(kEqMaxDb, v.n))));
            }
    }
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
