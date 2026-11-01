/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the entity model (see model.hpp).
 */
#include "model.hpp"

#include "text.hpp"

#include <algorithm>
#include <map>

namespace ha {

Domain domain_of(const std::string &entity_id)
{
    const std::size_t dot = entity_id.find('.');
    if (dot == std::string::npos) return Domain::Other;
    const std::string d = entity_id.substr(0, dot);
    if (d == "light") return Domain::Light;
    if (d == "switch") return Domain::Switch;
    if (d == "input_boolean") return Domain::InputBoolean;
    if (d == "scene") return Domain::Scene;
    if (d == "script") return Domain::Script;
    if (d == "sensor") return Domain::Sensor;
    if (d == "binary_sensor") return Domain::BinarySensor;
    if (d == "cover") return Domain::Cover;
    if (d == "lock") return Domain::Lock;
    if (d == "climate") return Domain::Climate;
    return Domain::Other;
}

bool supported(Domain d)
{
    return d != Domain::Other;
}

RowAction row_action(Domain d)
{
    switch (d) {
    case Domain::Light:
    case Domain::Switch:
    case Domain::InputBoolean: return RowAction::Toggle;
    case Domain::Scene:
    case Domain::Script: return RowAction::Run;
    case Domain::Lock: return RowAction::Confirm;
    case Domain::Cover: return RowAction::Open;
    default: return RowAction::Info;
    }
}

/* --------------------------------------------------------------------------------------------- registry */

bool Model::set_areas(const json::Value &result)
{
    if (!result.is_array()) return false;
    areas_.clear();
    for (const json::Value &a : result.items) {
        Area area;
        if (const json::Value *v = a.get("area_id")) area.id = v->as_string();
        if (const json::Value *v = a.get("name")) area.name = v->as_string();
        if (area.id.empty()) continue;
        if (area.name.empty()) area.name = area.id;
        areas_.push_back(std::move(area));
    }
    std::sort(areas_.begin(), areas_.end(),
              [](const Area &x, const Area &y) { return lower(x.name) < lower(y.name); });
    return true;
}

bool Model::set_devices(const json::Value &result)
{
    if (!result.is_array()) return false;
    devices_.clear();
    for (const json::Value &d : result.items) {
        const json::Value *id = d.get("id");
        if (!id || !id->is_string()) continue;
        Device dev;
        if (const json::Value *v = d.get("area_id")) dev.area = v->as_string();
        if (const json::Value *v = d.get("name_by_user")) dev.name = v->as_string();
        if (dev.name.empty())
            if (const json::Value *v = d.get("name")) dev.name = v->as_string();
        if (dev.area.empty() && dev.name.empty()) continue; /* nothing the app uses */
        devices_[id->text] = std::move(dev);
    }
    return true;
}

bool Model::set_entities(const json::Value &result)
{
    const json::Value *list = result.is_object() ? result.get("entities") : nullptr;
    if (!list || !list->is_array()) return false;
    reg_.clear();
    for (const json::Value &e : list->items) {
        const json::Value *ei = e.get("ei");
        if (!ei || !ei->is_string()) continue;
        if (!supported(domain_of(ei->text))) continue;
        RegEntry r;
        r.id = ei->text;
        if (const json::Value *v = e.get("en")) r.name = v->as_string();
        if (const json::Value *v = e.get("ai")) r.area = v->as_string();
        if (const json::Value *v = e.get("di")) r.device = v->as_string();
        if (const json::Value *v = e.get("hn")) r.has_entity_name = v->as_bool();
        if (const json::Value *v = e.get("hb")) r.hidden = v->as_bool() || r.hidden;
        if (e.get("ec")) r.hidden = true; /* configuration or diagnostic */
        if (const json::Value *v = e.get("dp")) r.precision = static_cast<int>(v->as_int(-1));
        reg_[r.id] = std::move(r);
    }
    return true;
}

void Model::clear_registry()
{
    areas_.clear();
    devices_.clear();
    reg_.clear();
}

/* --------------------------------------------------------------------------------------------- states */

void Model::read_attributes(State &st, const json::Value &attrs)
{
    if (!attrs.is_object()) return;
    for (const auto &m : attrs.members) {
        const std::string &k = m.first;
        const json::Value &v = m.second;
        if (k == "friendly_name") st.friendly_name = v.as_string();
        else if (k == "unit_of_measurement") st.unit = v.as_string();
        else if (k == "device_class") st.device_class = v.as_string();
        else if (k == "brightness") st.brightness = v.is_number() ? static_cast<int>(v.as_int(-1)) : -1;
        else if (k == "current_temperature") st.current_temperature = v.as_double(NAN);
        else if (k == "temperature") st.target_temperature = v.as_double(NAN);
        else if (k == "hvac_action") st.hvac_action = v.as_string();
        else if (k == "current_position") st.current_position = v.is_number() ? static_cast<int>(v.as_int(-1)) : -1;
        else if (k == "supported_color_modes") {
            st.dimmable = false;
            for (const json::Value &mode : v.items) {
                const std::string s = mode.as_string();
                if (!s.empty() && s != "onoff") st.dimmable = true;
            }
        }
    }
}

void Model::remove_attribute(State &st, const std::string &k)
{
    if (k == "friendly_name") st.friendly_name.clear();
    else if (k == "unit_of_measurement") st.unit.clear();
    else if (k == "device_class") st.device_class.clear();
    else if (k == "brightness") st.brightness = -1;
    else if (k == "current_temperature") st.current_temperature = NAN;
    else if (k == "temperature") st.target_temperature = NAN;
    else if (k == "hvac_action") st.hvac_action.clear();
    else if (k == "current_position") st.current_position = -1;
    else if (k == "supported_color_modes") st.dimmable = false;
}

void Model::read_state(State &st, const json::Value &c, bool replace)
{
    if (replace) st = State();
    if (const json::Value *s = c.get("s")) st.state = s->as_string(st.state);
    if (const json::Value *lc = c.get("lc")) st.last_changed = lc->as_double(st.last_changed);
    if (const json::Value *a = c.get("a")) read_attributes(st, *a);
}

bool Model::apply_event(const json::Value &event, std::set<std::string> *ids_changed)
{
    if (!event.is_object()) return false;
    bool changed = false;
    if (const json::Value *a = event.get("a")) {
        for (const auto &m : a->members) {
            if (!m.second.is_object()) continue;
            read_state(states_[m.first], m.second, true);
            if (ids_changed) ids_changed->insert(m.first);
            changed = true;
        }
    }
    if (const json::Value *c = event.get("c")) {
        for (const auto &m : c->members) {
            auto it = states_.find(m.first);
            if (it == states_.end()) continue; /* unknown: removed earlier, or never subscribed */
            if (const json::Value *plus = m.second.get("+")) read_state(it->second, *plus, false);
            if (const json::Value *minus = m.second.get("-"))
                if (const json::Value *keys = minus->get("a"))
                    for (const json::Value &k : keys->items) remove_attribute(it->second, k.as_string());
            if (ids_changed) ids_changed->insert(m.first);
            changed = true;
        }
    }
    if (const json::Value *r = event.get("r")) {
        for (const json::Value &id : r->items) {
            if (states_.erase(id.as_string())) {
                if (ids_changed) ids_changed->insert(id.as_string());
                changed = true;
            }
        }
    }
    return changed;
}

void Model::keep_only(const std::set<std::string> &ids)
{
    for (auto it = states_.begin(); it != states_.end();) {
        if (!ids.count(it->first)) it = states_.erase(it);
        else ++it;
    }
}

const State *Model::state(const std::string &id) const
{
    auto it = states_.find(id);
    return it == states_.end() ? nullptr : &it->second;
}

const RegEntry *Model::entry(const std::string &id) const
{
    auto it = reg_.find(id);
    return it == reg_.end() ? nullptr : &it->second;
}

/* --------------------------------------------------------------------------------------------- queries */

std::string Model::area_of(const std::string &id) const
{
    const RegEntry *r = entry(id);
    if (!r) return "";
    if (!r->area.empty()) return r->area;
    if (!r->device.empty()) {
        auto it = devices_.find(r->device);
        if (it != devices_.end()) return it->second.area;
    }
    return "";
}

std::string Model::area_name(const std::string &area_id) const
{
    if (area_id.empty()) return kNoRoomName;
    for (const Area &a : areas_)
        if (a.id == area_id) return a.name;
    return area_id;
}

std::string Model::display_name(const std::string &id, bool from_state) const
{
    if (from_state)
        if (const State *s = state(id))
            if (!s->friendly_name.empty()) return s->friendly_name;
    if (const RegEntry *r = entry(id)) {
        std::string device;
        if (!r->device.empty()) {
            auto it = devices_.find(r->device);
            if (it != devices_.end()) device = it->second.name;
        }
        if (r->has_entity_name && !device.empty()) return r->name.empty() ? device : device + " " + r->name;
        if (!r->name.empty()) return r->name;
        if (!device.empty()) return device;
    }
    /* "light.kitchen_ceiling" -> "kitchen ceiling" */
    std::string object = id.substr(id.find('.') == std::string::npos ? 0 : id.find('.') + 1);
    for (char &c : object)
        if (c == '_') c = ' ';
    return object.empty() ? id : object;
}

std::vector<RoomInfo> Model::rooms() const
{
    std::map<std::string, int> counts;
    for (const auto &kv : reg_) {
        if (kv.second.hidden) continue;
        ++counts[area_of(kv.first)];
    }
    std::vector<RoomInfo> out;
    for (const Area &a : areas_) {
        RoomInfo r;
        r.id = a.id;
        r.name = a.name;
        auto it = counts.find(a.id);
        r.count = it == counts.end() ? 0 : it->second;
        out.push_back(std::move(r));
    }
    /* entities whose area is not in the area list (deleted area) count as "No room" too */
    int orphans = 0;
    for (const auto &c : counts) {
        if (c.first.empty()) {
            orphans += c.second;
            continue;
        }
        bool listed = false;
        for (const Area &a : areas_) listed = listed || a.id == c.first;
        if (!listed) orphans += c.second;
    }
    if (orphans > 0) out.push_back(RoomInfo{"", kNoRoomName, orphans});
    return out;
}

std::vector<std::string> Model::entities_in(const std::string &area_id) const
{
    std::vector<std::pair<std::string, std::string>> named; /* lower name, id */
    for (const auto &kv : reg_) {
        if (kv.second.hidden) continue;
        std::string area = area_of(kv.first);
        if (area_id.empty()) {
            bool listed = false;
            for (const Area &a : areas_) listed = listed || (!area.empty() && a.id == area);
            if (listed) continue;
        } else if (area != area_id) {
            continue;
        }
        /* sorted by the registry's name: the order does not move when the states (friendly names) arrive */
        named.emplace_back(lower(display_name(kv.first, false)), kv.first);
    }
    std::sort(named.begin(), named.end());
    std::vector<std::string> out;
    out.reserve(named.size());
    for (auto &n : named) out.push_back(std::move(n.second));
    return out;
}

} // namespace ha
