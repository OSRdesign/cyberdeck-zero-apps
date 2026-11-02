/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the stored dashboard's model (see lovelace.hpp).
 */
#include "lovelace.hpp"

#include <algorithm>
#include <set>

namespace ha {

bool DashRow::operator==(const DashRow &o) const
{
    return kind == o.kind && entity == o.entity && name == o.name && icon == o.icon && secondary == o.secondary &&
           tap == o.tap && type == o.type;
}

bool DashCard::operator==(const DashCard &o) const
{
    return kind == o.kind && type == o.type && title == o.title && icon == o.icon && header_toggle == o.header_toggle &&
           rows == o.rows && entity == o.entity && unit == o.unit && graph == o.graph && hours == o.hours &&
           detail == o.detail && current == o.current && forecast == o.forecast;
}

bool DashView::operator==(const DashView &o) const
{
    return title == o.title && path == o.path && icon == o.icon && type == o.type && shown == o.shown &&
           users == o.users && subview == o.subview && cards == o.cards;
}

namespace {

std::string str(const json::Value &obj, const char *key)
{
    const json::Value *v = obj.get(key);
    return v && v->is_string() ? v->text : std::string();
}

bool flag(const json::Value &obj, const char *key, bool def)
{
    const json::Value *v = obj.get(key);
    return v && v->kind == json::Value::Kind::Bool ? v->boolean : def;
}

int number(const json::Value &obj, const char *key, int def, int lo, int hi)
{
    const json::Value *v = obj.get(key);
    if (!v || !v->is_number()) return def;
    const double d = v->as_double(def);
    if (!(d == d)) return def;
    return std::clamp(static_cast<int>(d), lo, hi);
}

bool entity_id(const std::string &s)
{
    const std::size_t dot = s.find('.');
    return dot != std::string::npos && dot > 0 && dot + 1 < s.size() && s.size() <= 255;
}

TapAction tap_of(const json::Value &row)
{
    const json::Value *t = row.get("tap_action");
    if (!t || !t->is_object()) return TapAction::Default;
    const std::string a = str(*t, "action");
    if (a == "more-info") return TapAction::MoreInfo;
    if (a == "toggle") return TapAction::Toggle;
    if (a == "none") return TapAction::None;
    return TapAction::Default; /* navigate, url, perform-action ...: the default (the details) */
}

bool import_row(const json::Value &r, DashRow &row)
{
    if (r.is_string()) {
        if (!entity_id(r.text)) return false;
        row.kind = DashRow::Kind::Entity;
        row.entity = r.text;
        return true;
    }
    if (!r.is_object()) return false;
    const std::string type = str(r, "type");
    if (type == "section") {
        row.kind = DashRow::Kind::Section;
        row.name = str(r, "label");
        return true;
    }
    if (type == "divider") {
        row.kind = DashRow::Kind::Divider;
        return true;
    }
    const std::string entity = str(r, "entity");
    if (type.empty() || type == "attribute") {
        if (!entity_id(entity)) return false;
        row.kind = DashRow::Kind::Entity;
        row.entity = entity;
        row.name = str(r, "name");
        row.icon = str(r, "icon");
        row.secondary = str(r, "secondary_info");
        row.tap = tap_of(r);
        return true;
    }
    row.kind = DashRow::Kind::Other; /* button, weblink, call-service, buttons, cast, conditional, custom:... */
    row.type = type;
    row.name = str(r, "name");
    if (row.name.empty()) row.name = str(r, "label");
    return true;
}

DashCard import_card(const json::Value &c)
{
    DashCard card;
    card.type = str(c, "type");
    card.kind = card_kind(card.type);
    switch (card.kind) {
    case DashCard::Kind::Entities: {
        card.title = str(c, "title");
        card.header_toggle = flag(c, "show_header_toggle", true);
        if (const json::Value *list = c.get("entities"))
            for (const json::Value &r : list->items) {
                if (card.rows.size() >= kMaxDashRows) break;
                DashRow row;
                if (import_row(r, row)) card.rows.push_back(std::move(row));
            }
        break;
    }
    case DashCard::Kind::Sensor:
        card.entity = str(c, "entity");
        card.title = str(c, "name");
        card.icon = str(c, "icon");
        card.unit = str(c, "unit");
        card.graph = str(c, "graph") == "line";
        card.hours = number(c, "hours_to_show", 24, 1, 24 * 31);
        card.detail = number(c, "detail", 1, 1, 2);
        if (!entity_id(card.entity)) card.entity.clear();
        break;
    case DashCard::Kind::Weather:
        card.entity = str(c, "entity");
        card.title = str(c, "name");
        card.current = flag(c, "show_current", true);
        card.forecast = flag(c, "show_forecast", true);
        if (!entity_id(card.entity)) card.entity.clear();
        break;
    case DashCard::Kind::Unsupported:
        card.title = str(c, "title");
        if (card.title.empty()) card.title = str(c, "name");
        if (card.type.empty()) card.type = "card";
        break;
    }
    return card;
}

void import_cards(const json::Value *list, DashView &v)
{
    if (!list) return;
    for (const json::Value &c : list->items) {
        if (v.cards.size() >= kMaxDashCards) return;
        if (c.is_object()) v.cards.push_back(import_card(c));
    }
}

/* ------------------------------------------------------------------------------------------------- export */

void add(std::string &s, const char *key, const std::string &value)
{
    if (value.empty()) return;
    s += std::string(s.back() == '{' ? "" : ",") + "\"" + key + "\":" + json::quote(value);
}

void add_raw(std::string &s, const char *key, const std::string &raw)
{
    s += std::string(s.back() == '{' ? "" : ",") + "\"" + key + "\":" + raw;
}

const char *tap_word(TapAction t)
{
    switch (t) {
    case TapAction::MoreInfo: return "more-info";
    case TapAction::Toggle: return "toggle";
    case TapAction::None: return "none";
    case TapAction::Default: break;
    }
    return "";
}

std::string row_text(const DashRow &r)
{
    if (r.kind == DashRow::Kind::Entity && r.name.empty() && r.icon.empty() && r.secondary.empty() &&
        r.tap == TapAction::Default)
        return json::quote(r.entity);
    std::string s = "{";
    switch (r.kind) {
    case DashRow::Kind::Entity:
        add(s, "entity", r.entity);
        add(s, "name", r.name);
        add(s, "icon", r.icon);
        add(s, "secondary_info", r.secondary);
        if (r.tap != TapAction::Default) add_raw(s, "tap_action", "{\"action\":" + json::quote(tap_word(r.tap)) + "}");
        break;
    case DashRow::Kind::Section:
        add_raw(s, "type", "\"section\"");
        add(s, "label", r.name);
        break;
    case DashRow::Kind::Divider: add_raw(s, "type", "\"divider\""); break;
    case DashRow::Kind::Other:
        add(s, "type", r.type);
        add(s, "name", r.name);
        break;
    }
    return s + "}";
}

std::string card_text(const DashCard &c)
{
    std::string s = "{";
    add(s, "type", c.type);
    switch (c.kind) {
    case DashCard::Kind::Entities: {
        add(s, "title", c.title);
        if (!c.header_toggle) add_raw(s, "show_header_toggle", "false");
        std::string rows = "[";
        for (std::size_t i = 0; i < c.rows.size(); ++i) rows += (i ? "," : "") + row_text(c.rows[i]);
        add_raw(s, "entities", rows + "]");
        break;
    }
    case DashCard::Kind::Sensor:
        add(s, "entity", c.entity);
        add(s, "name", c.title);
        add(s, "icon", c.icon);
        add(s, "unit", c.unit);
        add_raw(s, "graph", c.graph ? "\"line\"" : "\"none\"");
        add_raw(s, "hours_to_show", std::to_string(c.hours));
        add_raw(s, "detail", std::to_string(c.detail));
        break;
    case DashCard::Kind::Weather:
        add(s, "entity", c.entity);
        add(s, "name", c.title);
        if (!c.current) add_raw(s, "show_current", "false");
        if (!c.forecast) add_raw(s, "show_forecast", "false");
        break;
    case DashCard::Kind::Unsupported: add(s, "title", c.title); break;
    }
    return s + "}";
}

} // namespace

DashCard::Kind card_kind(const std::string &type)
{
    if (type == "entities") return DashCard::Kind::Entities;
    if (type == "sensor") return DashCard::Kind::Sensor;
    if (type == "weather-forecast") return DashCard::Kind::Weather;
    return DashCard::Kind::Unsupported;
}

Import import_lovelace(const json::Value &config, Dashboard &out)
{
    out = Dashboard();
    if (!config.is_object()) return Import::Invalid;
    if (config.get("strategy")) return Import::Generated;
    out.title = str(config, "title");
    const json::Value *views = config.get("views");
    if (!views || !views->is_array()) return Import::Invalid;
    for (const json::Value &x : views->items) {
        if (out.views.size() >= kMaxDashViews) break;
        if (!x.is_object()) continue;
        DashView v;
        v.title = str(x, "title");
        v.path = str(x, "path");
        v.icon = str(x, "icon");
        v.type = str(x, "type");
        if (v.type.empty()) v.type = flag(x, "panel", false) ? "panel" : "masonry";
        if (const json::Value *vis = x.get("visible")) {
            if (vis->kind == json::Value::Kind::Bool) v.shown = vis->boolean;
            else if (vis->is_array())
                for (const json::Value &u : vis->items) {
                    const std::string user = u.is_object() ? str(u, "user") : std::string();
                    if (!user.empty() && v.users.size() < 100) v.users.push_back(user);
                }
        }
        v.subview = flag(x, "subview", false);
        if (x.get("strategy")) {
            /* a view generated by a strategy: drawn as one unsupported card, never dropped */
            DashCard c;
            c.type = "strategy";
            c.title = v.title;
            v.cards.push_back(c);
        } else {
            import_cards(x.get("cards"), v);
            if (const json::Value *sections = x.get("sections"))
                for (const json::Value &sec : sections->items)
                    if (sec.is_object()) import_cards(sec.get("cards"), v);
        }
        out.views.push_back(std::move(v));
    }
    return Import::Ok;
}

std::string lovelace_text(const Dashboard &d)
{
    std::string s = "{";
    add(s, "title", d.title);
    std::string views = "[";
    for (std::size_t i = 0; i < d.views.size(); ++i) {
        const DashView &v = d.views[i];
        std::string o = "{";
        add(o, "title", v.title);
        add(o, "path", v.path);
        add(o, "icon", v.icon);
        add(o, "type", v.type);
        if (!v.shown) add_raw(o, "visible", "false");
        else if (!v.users.empty()) {
            std::string u = "[";
            for (std::size_t k = 0; k < v.users.size(); ++k) u += (k ? "," : "") + std::string("{\"user\":") + json::quote(v.users[k]) + "}";
            add_raw(o, "visible", u + "]");
        }
        if (v.subview) add_raw(o, "subview", "true");
        std::string cards = "[";
        for (std::size_t k = 0; k < v.cards.size(); ++k) cards += (k ? "," : "") + card_text(v.cards[k]);
        add_raw(o, "cards", cards + "]");
        views += (i ? "," : "") + o + "}";
    }
    add_raw(s, "views", views + "]");
    return s + "}";
}

int supported_cards(const Dashboard &d)
{
    int n = 0;
    for (const DashView &v : d.views) {
        if (v.subview) continue;
        for (const DashCard *c : view_cards(v)) n += c->kind != DashCard::Kind::Unsupported;
    }
    return n;
}

bool view_visible(const DashView &v, const std::string &user_id)
{
    if (!v.shown || v.subview) return false;
    if (v.users.empty() || user_id.empty()) return true;
    return std::find(v.users.begin(), v.users.end(), user_id) != v.users.end();
}

std::vector<const DashCard *> view_cards(const DashView &v)
{
    std::vector<const DashCard *> out;
    for (const DashCard &c : v.cards) {
        out.push_back(&c);
        if (v.type == "panel") break;
    }
    return out;
}

std::vector<std::string> view_entities(const DashView &v)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto take = [&](const std::string &id) {
        if (!id.empty() && seen.insert(id).second) out.push_back(id);
    };
    for (const DashCard *c : view_cards(v)) {
        take(c->entity);
        for (const DashRow &r : c->rows)
            if (r.kind == DashRow::Kind::Entity) take(r.entity);
    }
    return out;
}

std::vector<std::string> view_forecasts(const DashView &v)
{
    std::vector<std::string> out;
    for (const DashCard *c : view_cards(v))
        if (c->kind == DashCard::Kind::Weather && c->forecast && !c->entity.empty() &&
            std::find(out.begin(), out.end(), c->entity) == out.end())
            out.push_back(c->entity);
    return out;
}

/* ------------------------------------------------------------------------------------------------- choices */

namespace {

const DashChoice *find_choice(const std::vector<DashChoice> &choices, const std::string &key)
{
    for (const DashChoice &c : choices)
        if (c.key == key) return &c;
    return nullptr;
}

bool usable(const DashChoice *c)
{
    return c && c->loaded && c->supported > 0 && !c->too_large && !c->generated && !c->missing;
}

} // namespace

std::string wanted_dashboard(const std::string &setting, const std::vector<DashChoice> &choices,
                             const std::string &default_panel)
{
    if (setting == kDashOff) return std::string();
    if (!setting.empty() && setting != kDashAuto && find_choice(choices, setting)) return setting;
    /* Automatic: the system's default panel when it is one of the listed dashboards, else the default dashboard */
    if (!default_panel.empty() && default_panel != kDashDefault && find_choice(choices, default_panel)) return default_panel;
    return kDashDefault;
}

std::string resolve_dashboard(const std::string &setting, const std::vector<DashChoice> &choices,
                              const std::string &default_panel)
{
    const std::string key = wanted_dashboard(setting, choices, default_panel);
    return usable(find_choice(choices, key)) ? key : std::string();
}

std::vector<DashChoice> offered_dashboards(const std::vector<DashChoice> &choices)
{
    std::vector<DashChoice> out;
    for (const DashChoice &c : choices)
        if (usable(&c)) out.push_back(c);
    return out;
}

std::string next_dashboard_setting(const std::string &setting, const std::vector<DashChoice> &choices)
{
    std::vector<std::string> cycle{kDashAuto};
    for (const DashChoice &c : offered_dashboards(choices)) cycle.push_back(c.key);
    cycle.push_back(kDashOff);
    const std::string cur = setting.empty() ? std::string(kDashAuto) : setting;
    for (std::size_t i = 0; i < cycle.size(); ++i)
        if (cycle[i] == cur) return cycle[(i + 1) % cycle.size()];
    return kDashAuto; /* a dashboard no longer offered: back to Automatic */
}

} // namespace ha
