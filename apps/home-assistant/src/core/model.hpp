/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: the entity model (no LVGL; tests/test_model.cpp).
 *
 *   Registry  areas (config/area_registry/list), devices (config/device_registry/list: only their area and name are
 *             kept) and entities (config/entity_registry/list_for_display: id, name, area or the device's area,
 *             hidden or a configuration / diagnostic entity). Only the domains of phase 1 are kept. No attributes:
 *             the lists stay small on a large install (about 70 bytes per entity on the wire).
 *   States    the live state of the subscribed entities only (the favourites and the room shown), from
 *             subscribe_entities: "a" adds or replaces, "c" merges ("+" state, times and changed attributes, "-"
 *             removed attributes), "r" removes; a change for an unknown entity is ignored (a coalesced batch can name
 *             one removed earlier in it). Only the attributes the pages show are kept.
 */
#pragma once

#include "json.hpp"

#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ha {

enum class Domain { Light, Switch, InputBoolean, Scene, Script, Sensor, BinarySensor, Cover, Lock, Climate, Other };

Domain domain_of(const std::string &entity_id);
bool supported(Domain d); /* the phase 1 domains */

/* What a row does when activated, and so its row affordance (docs/APP-GUIDELINES.md, row kinds). */
enum class RowAction {
    Toggle,  /* light, switch, input_boolean: an Action row (dot) */
    Run,     /* scene, script: an Action row */
    Confirm, /* lock: an Action row that asks first */
    Open,    /* cover: opens its Open / Close page (chevron) */
    Info     /* sensor, binary_sensor, climate (read only in phase 1): an Info row */
};
RowAction row_action(Domain d);

struct Area {
    std::string id, name;
};

struct RegEntry {
    std::string id;     /* entity_id */
    std::string name;   /* the registry's name ("en"), may be empty */
    std::string area;   /* its own area, else its device's; "" = none */
    std::string device; /* device id, may be empty */
    bool has_entity_name = false; /* "hn": the friendly name is the device's name + this name */
    bool hidden = false;          /* hidden by the user ("hb") or a configuration / diagnostic entity ("ec") */
    int precision = -1;           /* "dp": display precision of a sensor, -1 none */
    std::string icon;             /* "ic": the user's icon ("mdi:ceiling-light"), may be empty */
    std::string platform;         /* "pl": the integration ("hue") */
    std::string translation_key;  /* "tk": its key for the integration's icons and names, may be empty */
};

struct State {
    std::string state;
    std::string friendly_name, unit, device_class;
    std::string icon; /* the "icon" attribute, may be empty */
    int brightness = -1;  /* 0..255, -1 when absent */
    bool dimmable = false; /* supported_color_modes names a mode with brightness */
    double current_temperature = NAN, target_temperature = NAN;
    int current_position = -1; /* cover 0..100 */
    std::string hvac_action;
    std::string temperature_unit; /* a weather entity's (0.3.0) */
    double last_changed = 0; /* epoch seconds */
};

struct RoomInfo {
    std::string id; /* "" = the entities without a room ("No room") */
    std::string name;
    int count = 0;
};

constexpr const char *kNoRoomName = "No room";

class Model {
public:
    /* registry (each replaces what was there) */
    bool set_areas(const json::Value &result);
    bool set_devices(const json::Value &result);
    bool set_entities(const json::Value &result);
    void clear_registry();
    /* The layout cached by an earlier session (store::load_layout): areas and entries whose name and area are already
     * resolved (no devices). Replaces the registry; the next registry from Home Assistant replaces it in turn. */
    void restore(std::vector<Area> areas, std::vector<RegEntry> entries);
    /* The visible entries with their display name (registry, device) and area resolved: what restore() takes. */
    std::vector<RegEntry> resolved_entries() const;
    const std::vector<Area> &areas() const { return areas_; }
    void clear_states() { states_.clear(); }

    /* One subscribe_entities event ({"a": ..., "c": ..., "r": [...]}). True when a state changed. ids_changed gets
     * the entities that changed (may be null). */
    bool apply_event(const json::Value &event, std::set<std::string> *ids_changed = nullptr);
    /* The subscription changed: forget the states of entities no longer subscribed. */
    void keep_only(const std::set<std::string> &ids);

    const State *state(const std::string &id) const;
    const RegEntry *entry(const std::string &id) const;
    bool known(const std::string &id) const { return entry(id) || state(id); }

    /* Areas sorted by name (case-insensitive), each with the count of its visible phase-1 entities, then "No room"
     * when some visible entities have no area. */
    std::vector<RoomInfo> rooms() const;
    /* The visible phase-1 entities of an area ("" = no room), sorted by display name. */
    std::vector<std::string> entities_in(const std::string &area_id) const;
    /* The friendly name of the state when there is one (from_state), else the registry's (with the device's name
     * when "hn"), else the entity id's object part with spaces. */
    std::string display_name(const std::string &id, bool from_state = true) const;
    std::string area_name(const std::string &area_id) const;
    std::string area_of(const std::string &id) const;

    std::size_t registry_size() const { return reg_.size(); }
    std::size_t state_count() const { return states_.size(); }
    std::size_t area_count() const { return areas_.size(); }
    std::size_t device_count() const { return devices_.size(); }

private:
    struct Device {
        std::string area, name;
    };
    void read_state(State &st, const json::Value &compressed, bool replace);
    static void read_attributes(State &st, const json::Value &attrs);
    static void remove_attribute(State &st, const std::string &key);

    std::vector<Area> areas_;
    std::unordered_map<std::string, Device> devices_;
    std::unordered_map<std::string, RegEntry> reg_;
    std::unordered_map<std::string, State> states_;
};

} // namespace ha
