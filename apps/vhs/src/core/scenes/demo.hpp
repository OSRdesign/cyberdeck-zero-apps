/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: the demo scenes folder of the headless scenes (brief 15.3; VHS_DEMO_SCENES with the fake camera only, never the
 * user's folder). Synthetic pictures (colour bars, a different hue per file) as photos and as short MJPEG movies made by
 * the recorder (when FFmpeg loads), named as VHS names them, with spread modification times, a locked renamed scene
 * ("Stopwatch test", canvas row 3) and an auto-split scene (_p2):
 *   "1"     the set above (14 files: "Sunset" renamed; the newest, "Trip", 10 s with a tone: the playback fixture)
 *   "cjk"   the set plus foreign files with CJK names (qa-cjk)
 * LVGL-free; runs on the scenes worker before the startup repair.
 */
#pragma once

#include <string>

namespace vhs {

class FileOps;
struct AvLibs;

/* Fills an empty folder; the number of files made (0 when the folder held files already). */
int make_demo_scenes(FileOps &ops, const std::string &folder, const AvLibs *av, const std::string &set);

} // namespace vhs
