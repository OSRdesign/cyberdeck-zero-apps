/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the scenes list (brief 9.1, 8, 6.3) and the file actions of the options sheet (9.2). LVGL-free; the scenes worker
 * runs it.
 *   list_scenes   every .mkv / .mp4 / .mov / .jpg / .jpeg of the folder (dot files skipped, foreign names too), newest
 *                 first (modification time, then number, then name), each auto-split part its own row, with its lock and
 *                 the journal's repaired / damaged mark
 *   MetaCache     ~/.cache/vhs/index.json: duration, codec, WxH by name + size + mtime (a changed file is probed again)
 *   probe_*       a movie's header through libavformat (no decoding); a photo's SOF
 *   texts         the row's label (Cassette's label rule: the stem; a renamed scene's prefix alone, 12.4 item 4; parts
 *                 " p2"), meta ("12:34 - 1.4 GB - MJPEG 720p", photo "1920x1080 - 98 KB"; portrait "... - 720p - Nov 14")
 *                 and date ("Nov 14 18:52")
 *   rename / delete   Cassette's rules: the prefix only, its refusals; a locked file is never deleted; the journal, the
 *                 lock entry and the thumbnails follow the file
 */
#pragma once

#include "naming.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vhs {

class FileOps;
struct AvLibs;

struct SceneMeta {
    bool ok = false;               /* probed */
    std::int64_t duration_ms = -1; /* movies */
    std::string codec;             /* "MJPEG", "H.264", "HEVC" ...; photos "JPEG" */
    int w = 0, h = 0;
};

struct SceneEntry {
    std::string name, path;
    SceneKind kind = SceneKind::Movie;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool parsed = false;   /* the name follows VHS's grammar (renamable) */
    SceneName sn;
    bool locked = false;
    bool repaired = false; /* its journal says so (brief 6.3) */
    bool damaged = false;
    SceneMeta meta;
};

/* The folder's scenes and photos, newest first, with the locks and the journals' marks (no probing). */
std::vector<SceneEntry> list_scenes(FileOps &ops, const std::string &folder);
/* How many files list_scenes() would list (movies, photos), cheap: names only. */
void count_scenes(FileOps &ops, const std::string &folder, int &movies, int &photos);

/* The row texts. short_form: the portrait rows (prefix_NNN, the date in the meta). movie_prefix: Settings' prefix (a
 * scene with another prefix, and not "photo", was renamed: its label is that prefix alone). */
std::string scene_label(const SceneEntry &e, bool short_form, const std::string &movie_prefix);
std::string scene_meta_text(const SceneEntry &e, bool short_form, bool utc);
std::string scene_date_text(std::int64_t mtime, bool utc);  /* "Nov 14 18:52" */
std::string duration_text(std::int64_t ms);                  /* "12:34", "1:02:03" */
std::string video_size_label(int w, int h);                  /* "720p", "1080p", else "1280x960" */

/* The SOF of a JPEG: false when none. */
bool jpeg_size(const std::uint8_t *data, std::size_t n, int &w, int &h);
SceneMeta probe_photo(FileOps &ops, const std::string &path);
/* A movie's header (avformat_open_input only: the container's duration and the video stream's codec and size). */
SceneMeta probe_movie(const AvLibs &av, const std::string &path);
/* "MJPEG", "H.264", "HEVC", "MPEG-4", "VP8", "VP9", "AV1", else "video" (libavformat major: AV1's id moved). */
std::string codec_label(int codec_id, int lavf_major);

class MetaCache {
public:
    void load(FileOps &ops, const std::string &path);
    bool get(const SceneEntry &e, SceneMeta &out) const;
    void put(const SceneEntry &e, const SceneMeta &m);
    void erase(const std::string &name);
    /* Written (atomically) only when something changed: 0, 1 (unchanged) or -errno. */
    int save(FileOps &ops);
    std::size_t size() const { return items_.size(); }

private:
    struct Item {
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
        SceneMeta meta;
    };
    std::string path_;
    std::map<std::string, Item> items_;
    bool dirty_ = false;
};

/* Rename (brief 9.2, 8): the prefix only, movies only (photos are always "photo": "photos keep the name photo",
 * provisional). 0 with new_name, or a negative error with why (Cassette's words: "use letters, digits, space, - and _",
 * "up to 24 characters", "the name is empty", "that name is taken", "this file is not a VHS scene and cannot be
 * renamed"). The lock entry and the journal's part name follow. */
int rename_scene(FileOps &ops, const std::string &folder, const std::string &name, const std::string &new_prefix,
                 std::string &new_name, std::string &why);
/* Delete: refused (-EPERM, "the file is locked") on a locked file; else the file, its lock entry and its journal part
 * (the journal itself when no part is left). The caller removes the thumbnails and the cache entry. */
int delete_scene(FileOps &ops, const std::string &folder, const std::string &name, std::string &why);

/* The open journals of the folder (cheap: JSON only, no FFmpeg): the repair at start runs only when there is one. */
int open_journal_count(FileOps &ops, const std::string &folder);

} // namespace vhs
