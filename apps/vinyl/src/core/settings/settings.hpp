/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl settings (P2a, the Playback page of the Menu, and Knock-knock in its TOOLS; report 060 2.2, owner decisions D25 and D32 of 2026-11-13):
 *
 *   gapless        On (default): the next track continues on the same open stream when its format is the same; Off:
 *                  every track change drains the stream and opens it again (a short gap)
 *   replaygain     Off / Track (default, D25) / Album: the gain from the files' tags (core/dsp/replaygain.hpp)
 *   boost_db       {"<output key>": 0..4}: the software gain boost in whole dB of each output (report 060 3e, owner request
 *                  2026-11-13: up to +4 dB, with the limiter); 0 (an output not listed) is off; a value outside 0..4
 *                  is clamped, a non-number ignored
 *   volume_steps   "normal" (default: about 2 dB, the percent shown) / "1" / "0.5" / "0.1": the step of the volume in dB
 *                  and its display as the output's real level (report 060 3f, D24); one global setting
 *   remember_long  On (default): a file longer than 20 minutes keeps where it stopped (core/state/positions.hpp)
 *   knock_knock    Off (default): "Knock-knock" (owner, 2026-11-16): three quick taps on the cover switch the screen off
 *                  while the music plays, three quick taps anywhere bring it back (core/screen/knock.hpp); its row is in the
 *                  Menu's TOOLS, under the Sleep timer (not on the Playback page); shown and used only where the launcher
 *                  answers `screen` (board.state requests) on a board with a touch screen
 *   library       {"folders": [...]}: folders the library indexes besides ~/Music and the mounted drives (P2b,
 *                  core/library/roots.hpp); written only once a folder was added
 *   eq             P2d {"on": true, "preset": "flat", "custom": [10 x -12..12]}: the equalizer (core/dsp/eq.hpp), one
 *                  for every output (D30); On with Flat by default (a bypass); a bad preset name is Flat, a custom value
 *                  out of range is clamped, a list of another length is ignored
 *   crossfade      P2d: 0 (Off, default), 2, 4, 6, 8 or 10 seconds (core/dsp/crossfade.hpp, D29); another number is Off
 *
 * ~/.config/vinyl/settings.json (XDG_CONFIG_HOME when set), a file of its own: written atomically, only when a setting
 * changes (never on a timer: SD wear), never by state.json's writes; keys Vinyl does not know (a newer version's) are kept.
 * A missing or damaged file means the defaults. LVGL-free.
 */
#pragma once

#include "../dsp/crossfade.hpp"
#include "../dsp/eq.hpp"
#include "../dsp/limiter.hpp"
#include "../dsp/replaygain.hpp"
#include "../util/json.hpp"

#include <map>
#include <string>
#include <vector>

namespace vinyl {

class FileOps;

/* "Volume steps": Normal keeps the 0.4.0 levels (about 2 dB, the percent shown); the others step the output in that many
 * dB (never finer than the output's own step) and show its level in dB. */
enum class VolumeSteps { Normal, Db1, Db05, Db01 };
const char *volume_steps_name(VolumeSteps v); /* "normal", "1", "0.5", "0.1" (the settings file and the pick list's ids) */
const char *volume_steps_word(VolumeSteps v); /* "Normal", "1 dB", "0.5 dB", "0.1 dB" (the pages) */
VolumeSteps volume_steps_from(const std::string &s, VolumeSteps fallback);
double volume_steps_db(VolumeSteps v);        /* 0 for Normal, else 1, 0.5, 0.1 */

/* "Off", "+1 dB" ... "+4 dB" */
std::string boost_word(int db);

struct Settings {
    bool gapless = true;
    ReplayGainMode replaygain = ReplayGainMode::Track;
    bool remember_long = true;
    bool knock = false; /* "Knock-knock" (knock_knock in the file) */
    VolumeSteps volume_steps = VolumeSteps::Normal;
    std::map<std::string, int> boost_db; /* per output key; 0 is never stored */
    int boost_of(const std::string &key) const {
        auto it = boost_db.find(key);
        return it == boost_db.end() ? 0 : it->second;
    }
    void set_boost(const std::string &key, int db) {
        db = db < 0 ? 0 : db > kMaxBoostDb ? kMaxBoostDb : db;
        if (db == 0) boost_db.erase(key);
        else boost_db[key] = db;
    }
    std::vector<std::string> library_folders; /* absolute paths */
    EqSettings eq;       /* P2d: the equalizer */
    int crossfade_s = 0; /* P2d: the crossfade in seconds (0 Off) */
    bool operator==(const Settings &o) const {
        return gapless == o.gapless && replaygain == o.replaygain && remember_long == o.remember_long && knock == o.knock &&
               volume_steps == o.volume_steps && boost_db == o.boost_db && library_folders == o.library_folders && eq == o.eq &&
               crossfade_s == o.crossfade_s;
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
