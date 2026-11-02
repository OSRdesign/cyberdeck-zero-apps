/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: entity icons (see icons.hpp).
 */
#include "icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ha {

namespace {

std::string domain_name(const std::string &entity_id)
{
    const std::size_t dot = entity_id.find('.');
    return dot == std::string::npos ? std::string() : entity_id.substr(0, dot);
}

bool number_of(const std::string &s, double &out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool read_rule(const json::Value &v, IconRule &r)
{
    if (!v.is_object()) return false;
    if (const json::Value *d = v.get("default")) r.def = d->as_string();
    if (const json::Value *s = v.get("state"))
        for (const auto &m : s->members)
            if (m.second.is_string() && !m.second.text.empty()) r.state.emplace_back(m.first, m.second.text);
    if (const json::Value *g = v.get("range"))
        for (const auto &m : g->members) {
            double lo = 0;
            if (m.second.is_string() && !m.second.text.empty() && number_of(m.first, lo)) r.range.emplace_back(lo, m.second.text);
        }
    std::sort(r.range.begin(), r.range.end());
    return !r.empty();
}

std::string rule_json(const IconRule &r)
{
    std::string s = "{\"default\":" + json::quote(r.def);
    if (!r.state.empty()) {
        s += ",\"state\":{";
        for (std::size_t i = 0; i < r.state.size(); ++i)
            s += (i ? "," : "") + json::quote(r.state[i].first) + ":" + json::quote(r.state[i].second);
        s += "}";
    }
    if (!r.range.empty()) {
        s += ",\"range\":{";
        for (std::size_t i = 0; i < r.range.size(); ++i)
            s += (i ? "," : "") + json::quote(json::number(r.range[i].first)) + ":" + json::quote(r.range[i].second);
        s += "}";
    }
    return s + "}";
}

void push_unique(std::vector<std::string> &v, const std::string &s)
{
    if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

/* The battery level steps Home Assistant's frontend draws in code: our own reading of the MDI battery set. */
std::string battery_icon(const State &s)
{
    double level = 0;
    if (!number_of(s.state, level)) return std::string();
    if (level >= 95) return "mdi:battery";
    if (level < 5) return "mdi:battery-outline";
    const int step = static_cast<int>(level / 10) * 10;
    return step < 10 ? std::string("mdi:battery-10") : "mdi:battery-" + std::to_string(step);
}

} // namespace

std::string pick_icon(const IconRule &rule, const std::string &state)
{
    for (const auto &kv : rule.state)
        if (kv.first == state) return kv.second;
    double v = 0;
    if (!rule.range.empty() && number_of(state, v)) {
        std::string best;
        for (const auto &kv : rule.range)
            if (kv.first <= v) best = kv.second;
        if (!best.empty()) return best;
    }
    return rule.def;
}

const std::vector<std::string> &icon_domains()
{
    static const std::vector<std::string> d = {"binary_sensor", "climate", "cover", "input_boolean", "light",
                                               "lock",          "scene",   "script", "sensor",       "switch"};
    return d;
}

bool IconRules::set_component(const json::Value &result)
{
    const json::Value *res = result.is_object() ? result.get("resources") : nullptr;
    if (!res || !res->is_object()) return false;
    component_.clear();
    for (const auto &dom : res->members) {
        if (std::find(icon_domains().begin(), icon_domains().end(), dom.first) == icon_domains().end()) continue;
        for (const auto &cls : dom.second.members) {
            IconRule r;
            if (read_rule(cls.second, r)) component_[dom.first + "|" + cls.first] = std::move(r);
        }
    }
    return true;
}

bool IconRules::set_entity(const json::Value &result, const std::set<std::string> &keep)
{
    const json::Value *res = result.is_object() ? result.get("resources") : nullptr;
    if (!res || !res->is_object()) return false;
    entity_.clear();
    for (const auto &integ : res->members)
        for (const auto &dom : integ.second.members)
            for (const auto &key : dom.second.members) {
                const std::string k = integ.first + "|" + dom.first + "|" + key.first;
                if (!keep.count(k)) continue;
                IconRule r;
                if (read_rule(key.second, r)) entity_[k] = std::move(r);
            }
    return true;
}

const IconRule *IconRules::component(const std::string &domain, const std::string &device_class) const
{
    if (!device_class.empty()) {
        auto it = component_.find(domain + "|" + device_class);
        if (it != component_.end()) return &it->second;
    }
    auto it = component_.find(domain + "|_");
    return it == component_.end() ? nullptr : &it->second;
}

const IconRule *IconRules::entity(const std::string &platform, const std::string &domain, const std::string &key) const
{
    if (platform.empty() || key.empty()) return nullptr;
    auto it = entity_.find(platform + "|" + domain + "|" + key);
    return it == entity_.end() ? nullptr : &it->second;
}

std::string IconRules::to_json() const
{
    std::string s = "{\"component\":{";
    bool first = true;
    for (const auto &kv : component_) {
        s += (first ? "" : ",") + json::quote(kv.first) + ":" + rule_json(kv.second);
        first = false;
    }
    s += "},\"entity\":{";
    first = true;
    for (const auto &kv : entity_) {
        s += (first ? "" : ",") + json::quote(kv.first) + ":" + rule_json(kv.second);
        first = false;
    }
    return s + "}}";
}

bool IconRules::from_json(const json::Value &v)
{
    if (!v.is_object()) return false;
    component_.clear();
    entity_.clear();
    if (const json::Value *c = v.get("component"))
        for (const auto &m : c->members) {
            IconRule r;
            if (m.first.find('|') != std::string::npos && read_rule(m.second, r)) component_[m.first] = std::move(r);
        }
    if (const json::Value *e = v.get("entity"))
        for (const auto &m : e->members) {
            IconRule r;
            if (std::count(m.first.begin(), m.first.end(), '|') == 2 && read_rule(m.second, r)) entity_[m.first] = std::move(r);
        }
    return true;
}

std::string fallback_icon(Domain d, const State *s)
{
    const std::string st = s ? s->state : std::string();
    const std::string cls = s ? s->device_class : std::string();
    const bool on = st == "on";
    switch (d) {
    case Domain::Light: return st == "off" ? "mdi:lightbulb-off" : "mdi:lightbulb";
    case Domain::Switch:
    case Domain::InputBoolean: return st == "off" ? "mdi:toggle-switch-off" : "mdi:toggle-switch";
    case Domain::Scene: return "mdi:palette";
    case Domain::Script: return "mdi:script-text";
    case Domain::Sensor:
        if (cls == "battery" && s) {
            const std::string b = battery_icon(*s);
            if (!b.empty()) return b;
            return "mdi:battery-unknown";
        }
        if (cls == "temperature") return "mdi:thermometer";
        if (cls == "humidity") return "mdi:water-percent";
        if (cls == "power") return "mdi:flash";
        if (cls == "energy") return "mdi:lightning-bolt";
        if (cls == "illuminance") return "mdi:brightness-5";
        if (cls == "pressure" || cls == "atmospheric_pressure") return "mdi:gauge";
        return "mdi:eye";
    case Domain::BinarySensor:
        if (cls == "door" || cls == "garage_door" || cls == "opening") return on ? "mdi:door-open" : "mdi:door-closed";
        if (cls == "window") return on ? "mdi:window-open" : "mdi:window-closed";
        if (cls == "motion" || cls == "occupancy" || cls == "presence")
            return on ? "mdi:motion-sensor" : "mdi:motion-sensor-off";
        if (cls == "smoke") return on ? "mdi:smoke-detector-alert" : "mdi:smoke-detector";
        if (cls == "moisture") return on ? "mdi:water" : "mdi:water-off";
        return on ? "mdi:checkbox-marked-circle" : "mdi:radiobox-blank";
    case Domain::Cover: {
        const bool open = st == "open" || st == "opening" || (s && s->current_position > 0 && st != "closed");
        if (cls == "garage") return open ? "mdi:garage-open" : "mdi:garage";
        if (cls == "door") return open ? "mdi:door-open" : "mdi:door-closed";
        if (cls == "gate") return open ? "mdi:gate-open" : "mdi:gate";
        return open ? "mdi:window-shutter-open" : "mdi:window-shutter";
    }
    case Domain::Lock:
        if (st == "jammed") return "mdi:lock-alert";
        if (st == "locking" || st == "unlocking" || st == "opening") return "mdi:lock-clock";
        if (st == "unlocked" || st == "open") return "mdi:lock-open-variant";
        return "mdi:lock";
    case Domain::Climate: return "mdi:thermostat";
    default: return "mdi:help-circle-outline";
    }
}

std::vector<std::string> icon_candidates(const std::string &entity_id, const RegEntry *entry, const State *state,
                                         const IconRules &rules)
{
    std::vector<std::string> out;
    const std::string domain = domain_name(entity_id);
    const std::string st = state ? state->state : std::string();
    if (entry) push_unique(out, entry->icon);
    if (state) push_unique(out, state->icon);
    if (entry)
        if (const IconRule *r = rules.entity(entry->platform, domain, entry->translation_key)) push_unique(out, pick_icon(*r, st));
    const Domain d = domain_of(entity_id);
    if (d == Domain::Sensor && state && state->device_class == "battery") push_unique(out, battery_icon(*state));
    if (const IconRule *r = rules.component(domain, state ? state->device_class : std::string()))
        push_unique(out, pick_icon(*r, st));
    push_unique(out, fallback_icon(d, state));
    return out;
}

std::string resolve_icon(const std::string &entity_id, const RegEntry *entry, const State *state,
                         const IconRules &rules, const std::function<bool(const std::string &)> &known)
{
    for (const std::string &name : icon_candidates(entity_id, entry, state, rules))
        if (!known || known(name)) return name;
    return std::string();
}

} // namespace ha
