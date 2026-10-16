/*
 * SPDX-License-Identifier: MIT
 */

#include "groups.hpp"

#include "store.hpp"          // json_quote, parse_json_object
#include "util.hpp"

#include <algorithm>
#include <sstream>

namespace meshzero {

namespace {

bool is_key_hex(const std::string &k)
{
    if (k.size() != 64) return false;
    for (char c : k)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

} // namespace

std::string ContactGroups::validate_name(const std::string &name)
{
    if (name.empty()) return "The group needs a name";
    if (utf8_length(name) > kMaxNameChars) return "The group name is limited to 24 characters";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F) return "The name has a control character";
    if (name.front() == ' ' || name.back() == ' ') return "No space at the start or the end of the name";
    return "";
}

const ContactGroup *ContactGroups::find(const std::string &name) const
{
    for (const ContactGroup &g : groups_)
        if (g.name == name) return &g;
    return nullptr;
}

ContactGroup *ContactGroups::find(const std::string &name)
{
    for (ContactGroup &g : groups_)
        if (g.name == name) return &g;
    return nullptr;
}

size_t ContactGroups::member_count(const std::string &name) const
{
    const ContactGroup *g = find(name);
    return g ? g->keys.size() : 0;
}

bool ContactGroups::is_member(const std::string &name, const std::string &key_hex) const
{
    const ContactGroup *g = find(name);
    return g && std::find(g->keys.begin(), g->keys.end(), key_hex) != g->keys.end();
}

std::vector<std::string> ContactGroups::groups_of(const std::string &key_hex) const
{
    std::vector<std::string> out;
    for (const ContactGroup &g : groups_)
        if (std::find(g.keys.begin(), g.keys.end(), key_hex) != g.keys.end()) out.push_back(g.name);
    return out;
}

std::set<std::string> ContactGroups::members(const std::string &name) const
{
    std::set<std::string> s;
    if (const ContactGroup *g = find(name)) s.insert(g->keys.begin(), g->keys.end());
    return s;
}

std::string ContactGroups::add_group(const std::string &name)
{
    std::string bad = validate_name(name);
    if (!bad.empty()) return bad;
    if (find(name)) return "A group with this name exists";
    if (groups_.size() >= kMaxGroups) return "At most 16 groups";
    groups_.push_back({name, {}});
    return "";
}

std::string ContactGroups::rename_group(const std::string &old_name, const std::string &new_name)
{
    ContactGroup *g = find(old_name);
    if (!g) return "No such group";
    if (old_name == new_name) return "";
    std::string bad = validate_name(new_name);
    if (!bad.empty()) return bad;
    if (find(new_name)) return "A group with this name exists";
    g->name = new_name;
    return "";
}

std::string ContactGroups::delete_group(const std::string &name)
{
    const auto it = std::find_if(groups_.begin(), groups_.end(), [&](const ContactGroup &g) { return g.name == name; });
    if (it == groups_.end()) return "No such group";
    groups_.erase(it);
    return "";
}

size_t ContactGroups::add_members(const std::string &name, const std::vector<std::string> &keys)
{
    ContactGroup *g = find(name);
    if (!g) return 0;
    std::set<std::string> have(g->keys.begin(), g->keys.end());
    size_t added = 0;
    for (const std::string &k : keys) {
        if (!is_key_hex(k) || have.count(k) || g->keys.size() >= kMaxMembers) continue;
        g->keys.push_back(k);
        have.insert(k);
        ++added;
    }
    return added;
}

size_t ContactGroups::remove_members(const std::string &name, const std::vector<std::string> &keys)
{
    ContactGroup *g = find(name);
    if (!g) return 0;
    const std::set<std::string> drop(keys.begin(), keys.end());
    const size_t before = g->keys.size();
    g->keys.erase(std::remove_if(g->keys.begin(), g->keys.end(), [&](const std::string &k) { return drop.count(k) != 0; }), g->keys.end());
    return before - g->keys.size();
}

size_t ContactGroups::remove_everywhere(const std::vector<std::string> &keys)
{
    size_t n = 0;
    for (ContactGroup &g : groups_) n += remove_members(g.name, keys);
    return n;
}

std::string ContactGroups::serialize() const
{
    std::string out;
    for (const ContactGroup &g : groups_) {
        std::string k;
        for (const std::string &key : g.keys) {
            if (!k.empty()) k += ",";
            k += key;
        }
        out += "{\"g\":" + json_quote(g.name) + ",\"k\":" + json_quote(k) + "}\n";
    }
    return out;
}

size_t ContactGroups::parse(const std::string &text)
{
    groups_.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        JsonObject o;
        if (!parse_json_object(line, o)) continue;
        const auto g = o.find("g");
        if (g == o.end() || !g->second.is_string) continue;
        if (!validate_name(g->second.str).empty() || find(g->second.str) || groups_.size() >= kMaxGroups) continue;
        ContactGroup grp;
        grp.name = g->second.str;
        const auto k = o.find("k");
        if (k != o.end() && k->second.is_string) {
            std::istringstream ks(k->second.str);
            std::string key;
            while (std::getline(ks, key, ','))
                if (is_key_hex(key) && std::find(grp.keys.begin(), grp.keys.end(), key) == grp.keys.end() && grp.keys.size() < kMaxMembers)
                    grp.keys.push_back(key);
        }
        groups_.push_back(std::move(grp));
    }
    return groups_.size();
}

} // namespace meshzero
