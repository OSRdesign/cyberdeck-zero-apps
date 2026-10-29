/*
 * SPDX-License-Identifier: MIT
 *
 * Mine Field core: the settings (see settings.hpp).
 */
#include "settings.hpp"

#include "json.hpp"

namespace minefield {

constexpr int kSettingsVersion = 2;   /* 1 had a custom board: its members are read and dropped, preset "custom" becomes beginner */

const char *animations_name(Animations a)
{
    switch (a) {
    case Animations::Reduced: return "reduced";
    case Animations::Off: return "off";
    default: return "on";
    }
}

bool parse_animations(const std::string &name, Animations &out)
{
    for (Animations a : {Animations::On, Animations::Reduced, Animations::Off})
        if (name == animations_name(a)) {
            out = a;
            return true;
        }
    return false;
}

bool Settings::operator==(const Settings &o) const
{
    return preset == o.preset && fair == o.fair && animations == o.animations && flag_mode == o.flag_mode && show_timer == o.show_timer;
}

std::string Settings::to_json() const
{
    const auto b = [](bool v) { return v ? "true" : "false"; };
    return "{\n  \"version\": " + std::to_string(kSettingsVersion) + ",\n  \"preset\": " + json::quote(preset_name(preset)) +
           ",\n  \"fair\": " + b(fair) +
           ",\n  \"animations\": " + json::quote(animations_name(animations)) + ",\n  \"flag_mode\": " + b(flag_mode) +
           ",\n  \"show_timer\": " + b(show_timer) + "\n}\n";
}

bool Settings::from_json(const std::string &text, Settings &out, std::string *why)
{
    const auto fail = [&](const char *m) {
        if (why) *why = m;
        return false;
    };
    json::Value root;
    std::string jwhy;
    if (!json::parse(text, root, &jwhy)) return fail("not JSON");
    if (root.kind != json::Value::Kind::Object) return fail("not an object");
    const json::Value *ver = root.get("version");
    if (!ver || !ver->is_int() || ver->as_int() < 1 || ver->as_int() > kSettingsVersion) return fail("bad version");
    Settings s;
    if (const json::Value *v = root.get("preset")) {
        if (v->kind != json::Value::Kind::String) return fail("bad preset");
        if (v->text == "custom") s.preset = Preset::Beginner;   /* version 1's custom board is gone */
        else if (!parse_preset(v->text, s.preset)) return fail("bad preset");
    }
    const auto flag = [&](const char *key, bool &dst) {
        const json::Value *v = root.get(key);
        if (!v) return true;
        if (v->kind != json::Value::Kind::Bool) return false;
        dst = v->boolean;
        return true;
    };
    if (!flag("fair", s.fair) || !flag("flag_mode", s.flag_mode) || !flag("show_timer", s.show_timer)) return fail("bad flag");
    if (const json::Value *v = root.get("animations")) {
        if (v->kind != json::Value::Kind::String || !parse_animations(v->text, s.animations)) return fail("bad animations");
    }
    out = s;
    return true;
}

} // namespace minefield
