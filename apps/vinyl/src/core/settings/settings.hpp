/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl settings (P2a, the Playback page of the Menu; report 060 2.2, owner decisions D25 and D32 of 2026-11-13):
 *
 *   gapless        On (default): the next track continues on the same open stream when its format is the same; Off:
 *                  every track change drains the stream and opens it again (a short gap)
 *   replaygain     Off / Track (default, D25) / Album: the gain from the files' tags (core/dsp/replaygain.hpp)
 *   remember_long  On (default): a file longer than 20 minutes keeps where it stopped (core/state/positions.hpp)
 *
 * ~/.config/vinyl/settings.json (XDG_CONFIG_HOME when set), a file of its own: written atomically, only when a setting
 * changes (never on a timer: SD wear), never by state.json's writes; keys Vinyl does not know (a newer version's) are kept.
 * A missing or damaged file means the defaults. LVGL-free.
 */
#pragma once

#include "../dsp/replaygain.hpp"
#include "../util/json.hpp"

#include <string>

namespace vinyl {

class FileOps;

struct Settings {
    bool gapless = true;
    ReplayGainMode replaygain = ReplayGainMode::Track;
    bool remember_long = true;
    bool operator==(const Settings &o) const {
        return gapless == o.gapless && replaygain == o.replaygain && remember_long == o.remember_long;
    }
    bool operator!=(const Settings &o) const { return !(*this == o); }
};

/* ~/.config/vinyl/settings.json */
std::string settings_path();

/* The file's text for s; `keep` (may be null): the object last read, whose unknown keys are written back. */
std::string settings_to_json(const Settings &s, const json::Value *keep);
/* false when the text is not a settings object (out is then the defaults); raw (may be null) receives the object. */
bool settings_from_json(const std::string &text, Settings &out, json::Value *raw);

/* The settings and their file: set() writes only when something changed. */
class SettingsStore {
public:
    explicit SettingsStore(FileOps &ops);
    /* Reads the file (the defaults when it is missing or damaged). */
    void load(const std::string &path);
    const Settings &get() const { return s_; }
    /* New values: written at once when they differ (0), unchanged (1), or -errno. */
    int set(const Settings &s);
    int writes() const { return writes_; }
    const std::string &path() const { return path_; }

private:
    FileOps &ops_;
    std::string path_;
    Settings s_;
    json::Value raw_;
    int writes_ = 0;
};

}  // namespace vinyl
