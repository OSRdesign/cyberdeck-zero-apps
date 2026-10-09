/*
 * SPDX-License-Identifier: MIT
 *
 * History on disk: flat JSON-lines files under ~/.local/share/mesh-hop (or $MESHHOP_DATA, or $XDG_DATA_HOME/mesh-hop).
 *
 * Global (about the deck, not about a board), directly in that folder:
 *   prefs.txt        "retry <attempts> <reset after>" (the retry rules of direct messages)
 *   mesh-hop.log, port, presets.jsonl ...   (kept by other parts of the app)
 *
 * Per board, in boards/<first 12 hex digits of the board's public key>/ (the folder is chosen when SELF_INFO tells which board is connected;
 * plugging another board shows that board's own data, plugging the first one back restores it):
 *   messages.jsonl   one line per message, then small {"u":seq,"st":state} lines for later state changes ("hb": the final heard-back
 *                    count of a channel message we sent, an optional key that older versions ignore); rewritten
 *                    (compacted) when it has grown, bounded by Model::kMaxPerConversation / kMaxMessages
 *   contacts.jsonl   the contact cache, rewritten whenever the contact list changes
 *   channels.jsonl   the channel names seen last
 *   read.txt         "<conversation> <last read seq>" lines
 *   prefs.txt        "mute <conversation>", "advert <hours> <flood 0|1>" (the scheduled self advert) and "ignore <node key>" (nodes hidden in Nearby)
 *   groups.jsonl     contact groups (header line {"v":1,"n":N}, then one group per line); groups.bak is the version before the last change
 *
 * Migration: the files of the layout before 0.2.2 (messages.jsonl, read.txt, contacts.jsonl, channels.jsonl, groups.jsonl, groups.bak and the
 * mute / advert / ignore lines of prefs.txt, all directly in the data folder) belong to the board that is connected at the first start of the
 * new layout: at its first SELF_INFO they are copied into its folder and the originals are renamed "<name>.pre-boards.bak" (nothing is deleted).
 *
 * Disk use: every board folder is bounded by the history limits (1500 messages, 200 per conversation, 16 groups, 300 ignored nodes); the folders
 * of boards that are not connected are kept (so that plugging them back restores them) until the user forgets them in Settings > History.
 *
 * Nothing here knows LVGL or the radio. A damaged line is skipped, never fatal.
 */

#pragma once

#include "model.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

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

    /* Creates the directory, loads the global preferences, then keeps listening to the model. The board's own data is loaded when the model
     * learns which board is connected (Listener::board_changed). */
    bool attach(Model &model);
    const std::string &dir() const { return dir_; }
    /* The folder of the board whose data is loaded ("" while no board is known) and its id (12 hex digits). */
    const std::string &board_dir() const { return bdir_; }
    std::string board_id() const;
    void set_log(std::function<void(const std::string &)> log) { log_ = std::move(log); }
    bool healthy() const { return healthy_; }

    // Listener
    void message_added(const Message &m) override;
    void message_state(uint32_t seq, MsgState state, const std::string &note) override;
    void message_heard_back(uint32_t seq, int count) override;
    void contacts_changed() override;
    void channels_changed() override;
    void read_changed() override;
    void prefs_changed() override;
    void messages_removed() override;
    void groups_changed() override;
    void board_changed(const std::string &key_hex) override;
    void board_data_forgotten() override;

    /* The saved data of boards other than the current one (their folders under boards/). */
    struct SavedBoard {
        std::string id;                  // 12 hex digits
        uint64_t bytes = 0;
    };
    std::vector<SavedBoard> other_boards() const;
    /* Deletes the folders of the other boards. Returns how many were removed. The current board is never touched. */
    size_t forget_other_boards();
    /* Files the migration moved at the first board of the new layout (tests, the log). */
    int migrated_files() const { return migrated_; }

    /* Rewrites messages.jsonl from the model (also done automatically when the file has grown). */
    void compact();
    /* True when the groups were read from groups.bak at start because groups.jsonl was missing or damaged. */
    bool groups_restored() const { return groups_restored_; }

private:
    std::string path(const char *name) const { return bdir_ + "/" + name; }     // a file of the current board
    std::string gpath(const char *name) const { return dir_ + "/" + name; }     // a global file
    bool append_line(const char *name, const std::string &line);
    bool rewrite(const char *name, const std::string &content);
    bool rewrite_at(const std::string &file, const std::string &content);
    void load_messages();
    void load_contacts();
    void load_channels();
    void load_read();
    void load_global_prefs();
    void load_prefs();
    void load_groups();
    void write_global_prefs();
    void migrate_legacy_files();
    void say(const std::string &s) const { if (log_) log_(s); }

    std::string dir_;
    std::string bdir_;                   // boards/<id> of the current board, "" before the first SELF_INFO
    std::function<void(const std::string &)> log_;
    int migrated_ = 0;
    Model *model_ = nullptr;
    bool healthy_ = true;
    bool groups_restored_ = false;
    int appended_ = 0;
};

} // namespace meshzero
