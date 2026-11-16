/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the recording journal and the repair (brief 6.3; Cassette's pattern). <folder>/.journal/<stem>.json holds
 * {"state":"open","file":"<stem>.mkv","parts":[...],"started":<unix s>} from the first byte of a scene and becomes
 * "closed" after the trailer and the fdatasync; a split adds its part. At the app's start the scenes worker repairs
 * every "open" journal: each part is remuxed with libavformat into .<part stem>.repair.mkv until the first read
 * error, checked (at least one video packet, a plausible duration) and renamed over the original; the journal becomes
 * "closed" with "repaired": true. A part that yields nothing keeps its original and the journal says "damaged" (the
 * scenes list shows it, M4). LVGL-free.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vhs {

class FileOps;
struct AvLibs;

struct Journal {
    std::string state = "open";    /* "open", "closed", "damaged" */
    std::string file;              /* part 1 */
    std::vector<std::string> parts;
    std::int64_t started = 0;      /* unix seconds */
    bool repaired = false;
};

std::string journal_path(const std::string &folder, const std::string &stem);
std::string journal_to_json(const Journal &j);
bool journal_from_json(const std::string &text, Journal &out);
/* Atomic (temp, fsync, rename): 0 or -errno. */
int journal_write(FileOps &ops, const std::string &folder, const std::string &stem, const Journal &j);
bool journal_read(FileOps &ops, const std::string &path, Journal &out);

struct RepairItem {
    std::string stem;
    std::string part;
    bool repaired = false;   /* a repaired file replaced the original */
    bool damaged = false;    /* nothing could be saved: the original stays */
    std::int64_t duration_ms = 0;
    std::string error;
};

/* Every open journal of the folder repaired (the scenes worker, at start). */
std::vector<RepairItem> repair_open_journals(FileOps &ops, const AvLibs &av, const std::string &folder);
/* One part: remux, check, rename over the original. */
RepairItem repair_part(FileOps &ops, const AvLibs &av, const std::string &folder, const std::string &part);

} // namespace vhs
