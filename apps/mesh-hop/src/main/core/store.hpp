/*
 * SPDX-License-Identifier: MIT
 *
 * History on disk: flat JSON-lines files in ~/.local/share/mesh-hop (or $MESHHOP_DATA, or $XDG_DATA_HOME/mesh-hop).
 *
 *   messages.jsonl   one line per message, then small {"u":seq,"st":state} lines for later state changes; rewritten
 *                    (compacted) when it has grown, bounded by Model::kMaxPerConversation / kMaxMessages
 *   contacts.jsonl   the contact cache, rewritten whenever the contact list changes
 *   channels.jsonl   the channel names seen last
 *   read.txt         "<conversation> <last read seq>" lines
 *   prefs.txt        local preferences: "mute <conversation>" and "retry <attempts> <reset after>" lines
 *
 * Nothing here knows LVGL or the radio. A damaged line is skipped, never fatal.
 */

#pragma once

#include "model.hpp"

#include <map>
#include <string>

namespace meshzero {

/* Minimal flat JSON object reader / writer used by the store (public for the tests). */
struct JsonValue {
    bool is_string = false;
    std::string str;
    double num = 0;
};
using JsonObject = std::map<std::string, JsonValue>;
bool parse_json_object(const std::string &line, JsonObject &out);
std::string json_quote(const std::string &s);

class Store : public Listener {
public:
    explicit Store(std::string dir);
    static std::string default_dir();
    /* One-time migration from the name this app had in the 0.1.x test builds: if `old_dir` exists and `new_dir` does
     * not, `old_dir` is renamed to `new_dir` (history, contact cache, port files are kept). Returns true if it moved.
     * default_dir() calls it for the home / XDG directories; it never touches $MESHHOP_DATA. */
    static bool migrate_legacy_dir(const std::string &old_dir, const std::string &new_dir);

    /* Creates the directory and loads everything into the model, then keeps listening to it. */
    bool attach(Model &model);
    const std::string &dir() const { return dir_; }
    bool healthy() const { return healthy_; }

    // Listener
    void message_added(const Message &m) override;
    void message_state(uint32_t seq, MsgState state, const std::string &note) override;
    void contacts_changed() override;
    void channels_changed() override;
    void read_changed() override;
    void prefs_changed() override;
    void messages_removed() override;

    /* Rewrites messages.jsonl from the model (also done automatically when the file has grown). */
    void compact();

private:
    std::string path(const char *name) const { return dir_ + "/" + name; }
    bool append_line(const char *name, const std::string &line);
    bool rewrite(const char *name, const std::string &content);
    void load_messages();
    void load_contacts();
    void load_channels();
    void load_read();
    void load_prefs();

    std::string dir_;
    Model *model_ = nullptr;
    bool healthy_ = true;
    int appended_ = 0;
};

} // namespace meshzero
