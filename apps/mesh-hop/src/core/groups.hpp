/*
 * SPDX-License-Identifier: MIT
 *
 * Contact groups (C1): named sets of contacts that live only on the deck. The board has no group concept (its only per-contact marker
 * is the favourite bit of the flags), so the groups are kept in the data folder (groups.jsonl, with the previous copy in groups.bak) and are
 * not lost when the board is factory reset. A member is a contact public key in hex. No LVGL, no I/O.
 */

#pragma once

#include <set>
#include <string>
#include <vector>

namespace meshzero {

struct ContactGroup {
    std::string name;
    std::vector<std::string> keys;        // 64 hex digits each, no duplicates
};

class ContactGroups {
public:
    static constexpr size_t kMaxGroups = 16;
    static constexpr size_t kMaxNameChars = 24;
    static constexpr size_t kMaxMembers = 1000;

    /* "" when the name is usable: 1 to 24 characters, no control character, no leading or trailing space. */
    static std::string validate_name(const std::string &name);

    const std::vector<ContactGroup> &groups() const { return groups_; }
    bool empty() const { return groups_.empty(); }
    bool has(const std::string &name) const { return find(name) != nullptr; }
    size_t member_count(const std::string &name) const;
    bool is_member(const std::string &name, const std::string &key_hex) const;
    /* The names of the groups a contact belongs to. */
    std::vector<std::string> groups_of(const std::string &key_hex) const;
    /* The member keys of a group as a set (for filtering a list). */
    std::set<std::string> members(const std::string &name) const;

    /* The mutators return "" on success, else the reason. */
    std::string add_group(const std::string &name);
    std::string rename_group(const std::string &old_name, const std::string &new_name);
    std::string delete_group(const std::string &name);
    /* Returns how many keys were added (those already there are skipped). */
    size_t add_members(const std::string &name, const std::vector<std::string> &keys);
    size_t remove_members(const std::string &name, const std::vector<std::string> &keys);
    /* A contact was deleted: it leaves every group. */
    size_t remove_everywhere(const std::vector<std::string> &keys);

    /* Text form for the file: one JSON object per line, {"g":"name","k":["hex",...]}. */
    std::string serialize() const;
    /* Replaces the content from the text of a file; a damaged line is skipped. Returns the number of groups read. */
    size_t parse(const std::string &text);
    void clear() { groups_.clear(); }

private:
    const ContactGroup *find(const std::string &name) const;
    ContactGroup *find(const std::string &name);
    std::vector<ContactGroup> groups_;
};

} // namespace meshzero
