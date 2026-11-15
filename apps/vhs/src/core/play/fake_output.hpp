/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: a fake sound output for the AudioPlayer (the tests of the controller, the headless scenes with VHS_FAKE=1), the way
 * audio/fake_alsa.hpp is the fake camera microphone. No sound hardware, no sound server, no launcher and no media-key device is
 * touched: the sink swallows the frames at the audio's real time (a 50 ms buffer, a delay like a real device's), so the heard
 * clock runs exactly as it does on a board.
 *
 *   ok      (default) a device that works, "Fake output"
 *   lost    it works for lost_after_ms (default 1500), then its writes fail and it never comes back: paused, output_lost
 *   none    no device at all: the open fails ("no device"), as on a Lite board without a sound card
 *   instant the frames are swallowed as fast as they come (a whole file in a few ms: tests of the end)
 *
 * LVGL-free.
 */
#pragma once

#include "play/audio_player_deps.hpp"

#include <memory>
#include <string>

namespace vhs {

struct FakeOutputOptions {
    bool pace = true;               /* real time; false: instant */
    std::string label = "Fake output";
    int lost_after_ms = 0;          /* > 0: the writes fail with -ENODEV from this long after the stream opened */
    std::string open_error;         /* not empty: the output cannot be opened, with this reason */
};

/* "ok", "lost", "none", "instant" (VHS_FAKE_AUDIO); anything else is "ok". */
FakeOutputOptions fake_output_options(const std::string &mode);

/* The dependencies of an AudioPlayer over the fake output: AudioPlayer(output, fake_audio_deps()). */
std::unique_ptr<AudioPlayerDeps> fake_audio_deps(const FakeOutputOptions &o = FakeOutputOptions());

}  // namespace vhs
