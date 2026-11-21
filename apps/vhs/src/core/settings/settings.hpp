/*
 * SPDX-License-Identifier: MIT
 *
 * VHS settings (brief section 11): ~/.config/vhs/settings.json (XDG_CONFIG_HOME when set; VHS_SETTINGS names another
 * file for tests), written atomically and only when something changed, read tolerantly (a missing or damaged file means
 * the defaults; a bad value means that key's default; numbers are clamped); keys VHS does not know are kept.
 *
 *   folder        "~/VHS"
 *   prefix        "scene" (1-24 of [A-Za-z0-9 _-], no space first or last, not starting with ".")
 *   photo_mode    "instant" | "light"; absent = the SoC default (select.hpp)
 *   photo_size    "WxH" of a listed MJPEG size; absent = the largest
 *   video_size    "WxH"; absent = 1280x720
 *   self_timer_s  0, 3, 5, 10 (others move to the nearest)
 *   split         true (at 4 GiB) | false
 *   output        Vinyl's output target string, "" = Automatic
 *   split_mib     hidden, 64..4096 (4096)
 *   min_free_mb   hidden, 100..100000 (500)
 *   camera_id     the camera remembered: its stable id (identity.hpp stable_key()); the Settings > Camera choice and the
 *                 quick switch write it, a lone camera is remembered as it is plugged in
 *   camera_name   hidden, the remembered camera's name (the "not found" toast)
 *   camera_sizes  hidden (0.1.2): per camera id, {"photo": "WxH", "video": "WxH"}; photo_size / video_size above mirror the
 *                 camera in use; with no entries they are the one pair of 0.1.1 (it becomes the entry of the camera the app
 *                 leaves at the first switch)
 *   sound_input   Settings > Sound input (0.1.2): "" Automatic, "none" No sound, else Cassette's card descriptor
 *                 "name|usb by-id|vid:pid"
 *
 * M1 reads every key and writes camera_id; the Settings page (M6) writes the others. LVGL-free.
 */
#pragma once

#include "modes/select.hpp"
#include "scenes/json.hpp"

#include <string>
#include <vector>

namespace vhs {

class FileOps;

struct Settings {
    std::string folder = "~/VHS";
    std::string prefix = "scene";
    bool have_photo_mode = false;
    PhotoMode photo_mode = PhotoMode::Instant;
    Size photo_size;
    Size video_size;
    int self_timer_s = 0;
    bool split = true;
    std::string output;
    int split_mib = 4096;
    int min_free_mb = 500;
    std::string camera_id;
    std::string camera_name;
    struct CameraSizes {
        std::string key;
        Size photo, video;
        bool operator==(const CameraSizes &o) const { return key == o.key && photo == o.photo && video == o.video; }
    };
    std::vector<CameraSizes> camera_sizes;
    std::string sound_input;
    const CameraSizes *sizes_of(const std::string &key) const;
    /* The sizes of `key` set (replacing its entry; dropped when both are invalid). */
    void set_sizes(const std::string &key, const Size &photo, const Size &video);
    bool operator==(const Settings &o) const;
    bool operator!=(const Settings &o) const { return !(*this == o); }
};

/* The prefix rule (brief 8, Cassette's). */
bool valid_prefix(const std::string &p);
/* 0, 3, 5, 10: the nearest (a tie goes to the shorter). */
int clamp_self_timer(int s);

/* "photo_mode=light,photo_size=1280x960,self_timer_s=3,prefix=take" over s (the fake camera's scenes, VHS_SET; also
 * video_size, split, split_mib and min_free_mb, clamped as the file's values); unknown keys and bad values are ignored. */
void apply_settings_overrides(Settings &s, const std::string &list);

/* $VHS_SETTINGS, else $XDG_CONFIG_HOME/vhs/settings.json, else ~/.config/vhs/settings.json. */
std::string settings_path();

/* The file's text for s; `keep` (may be null): the object last read, whose unknown keys are written back. */
std::string settings_to_json(const Settings &s, const json::Value *keep);
/* false when the text is not a settings object (out is then the defaults); raw (may be null) receives the object. */
bool settings_from_json(const std::string &text, Settings &out, json::Value *raw);

/* The settings and their file: set() writes only when something changed. */
class SettingsStore {
public:
    explicit SettingsStore(FileOps &ops);
    void load(const std::string &path);
    const Settings &get() const { return s_; }
    /* New values: written at once when they differ (0), unchanged (1), or -errno. read_only: never written (the fake
     * camera's runs). */
    int set(const Settings &s);
    void set_read_only(bool on) { read_only_ = on; }
    int writes() const { return writes_; }
    const std::string &path() const { return path_; }

private:
    FileOps &ops_;
    std::string path_;
    Settings s_;
    json::Value raw_;
    int writes_ = 0;
    bool read_only_ = false;
};

} // namespace vhs
