/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl demonstration world (VINYL_DEMO=1): the headless scenes and the screenshots run the real app on it, with no sound
 * hardware and nothing played:
 *
 *   music     a folder of WAV files named and tagged as the drafts' tracks (LIST/INFO title, artist, album, date), each
 *             as long as the draft says, written SPARSE (the silence takes no disk space), plus the "Playlist SKI.m3u"
 *             queue of 20 tracks, the third playing ("03/20", "Fences (LeMarquis Remix)" by Phoenix);
 *   state     a resume file that opens that queue at 01:23;
 *   outputs   a sound server with the drafts' devices (Evolve 65 in music quality, a USB card, HDMI; the XWF speaker paired
 *             but not connected), or, with VINYL_DEMO_LITE=1, a board without a server (ALSA cards, Bluetooth not set up);
 *   sinks     a null output that takes the frames at the speed they would be heard;
 *   launcher  the volume 60.
 * Two variants for the design-QA scenes: "empty" (VINYL_DEMO_EMPTY=1: no music folder at all and no saved state, the first
 * start on a board without ~/Music) and "cjk" (VINYL_DEMO_CJK=1: a folder 東アジア with a sub-folder 한국 and three tracks
 * whose file names and tags are Japanese, Chinese and Korean, the Japanese one playing).
 * LVGL-free.
 */
#pragma once

#include "../engine/outputs.hpp"
#include "../launcher/launcher_link.hpp"

#include <memory>
#include <string>

namespace vinyl {

struct DemoWorld {
    std::string root;       /* the demo folder */
    std::string music;      /* <root>/Music */
    std::string config;     /* <root>/config (XDG_CONFIG_HOME) */
    std::string playlist;   /* <music>/Playlist SKI.m3u */
    bool lite = false;
    bool empty = false;     /* the "empty" variant: no music folder (the app gets no root) */
};

/* Creates the folder (under $TMPDIR or /tmp, unique per process) with its files and the resume file. variant: "",
 * "empty" or "cjk" (above). */
bool make_demo_world(DemoWorld &w, bool lite, const std::string &state, const std::string &variant = "");
void remove_demo_world(const DemoWorld &w);

OutputWorld demo_output_world(bool lite);
LauncherStatus demo_launcher_status();
std::unique_ptr<SinkProvider> make_demo_sink_provider();

/* A sparse WAV of `seconds` (16-bit stereo at `rate`) with a LIST/INFO chunk; for the demo and the tests. */
bool write_demo_wav(const std::string &path, double seconds, uint32_t rate, const std::string &title, const std::string &artist,
                    const std::string &album, const std::string &date, int track);

}  // namespace vinyl
