/*
 * SPDX-License-Identifier: MIT
 *
 * Home Assistant app core: which icon an entity's tile shows (no LVGL; tests/test_tiles.cpp).
 *
 * Order (study 053 section 6.6), the first name the font knows wins:
 *   1. the user's icon in the entity registry ("ic" of config/entity_registry/list_for_display);
 *   2. the state's "icon" attribute;
 *   3. the integration's icon for the entity's translation key: frontend/get_icons, category "entity", by "pl"
 *      (platform) and "tk" (translation key) of the registry;
 *   4. the domain's rule: frontend/get_icons, category "entity_component": the device class's rule, else "_";
 *   5. the app's own small table (fallback_icon): the few rules Home Assistant's frontend keeps in code (battery
 *      levels, a cover's position), and every phase-1 domain while no rule came from Home Assistant (first start
 *      offline, an older server).
 * Points 3 and 4 are data Home Assistant serves at run time to any user; nothing is copied from its frontend.
 *
 * A rule is {"default": icon, "state": {state: icon}, "range": {number: icon}}: the state's own icon, else the range
 * step at or below a numeric state, else the default. Only the rules of the phase-1 domains and of the (platform,
 * domain, translation key) triples the registry holds are kept, so the set stays small; it is cached with the layout.
 */
#pragma once

#include "json.hpp"
#include "model.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ha {

struct IconRule {
    std::string def;
    std::vector<std::pair<std::string, std::string>> state; /* state -> icon */
    std::vector<std::pair<double, std::string>> range;      /* lower bound -> icon, sorted */
    bool empty() const { return def.empty() && state.empty() && range.empty(); }
};

/* The rule's icon for a state ("" when it has none). */
std::string pick_icon(const IconRule &rule, const std::string &state);

/* The phase-1 domains, as frontend/get_icons names them ("light", "switch" ...). */
const std::vector<std::string> &icon_domains();

class IconRules {
public:
    /* frontend/get_icons results ({"resources": {...}}). entity: only the triples in keep ("platform|domain|key") are
     * kept. False when the result is not an object of that shape. */
    bool set_component(const json::Value &result);
    bool set_entity(const json::Value &result, const std::set<std::string> &keep);

    const IconRule *component(const std::string &domain, const std::string &device_class) const;
    const IconRule *entity(const std::string &platform, const std::string &domain, const std::string &key) const;
    bool has_component() const { return !component_.empty(); }
    std::size_t size() const { return component_.size() + entity_.size(); }

    /* the cache's form: {"component": {"light|_": rule}, "entity": {"hue|light|key": rule}} */
    std::string to_json() const;
    bool from_json(const json::Value &v);
    bool operator==(const IconRules &o) const { return to_json() == o.to_json(); }

private:
    std::map<std::string, IconRule> component_; /* "domain|device_class" ("_" for none) */
    std::map<std::string, IconRule> entity_;    /* "platform|domain|key" */
};

/* The app's own icon for a domain and state (point 5): never empty for a phase-1 domain. */
std::string fallback_icon(Domain d, const State *s);

/* The icon names in the order above for one entity (each source that gives one, duplicates kept out). */
std::vector<std::string> icon_candidates(const std::string &entity_id, const RegEntry *entry, const State *state,
                                         const IconRules &rules);

/* The first candidate `known` accepts (the font's table), else the domain's fallback; "" when none is known. */
std::string resolve_icon(const std::string &entity_id, const RegEntry *entry, const State *state,
                         const IconRules &rules, const std::function<bool(const std::string &)> &known);

} // namespace ha
