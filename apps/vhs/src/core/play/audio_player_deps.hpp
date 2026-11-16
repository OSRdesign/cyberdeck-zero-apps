/*
 * SPDX-License-Identifier: MIT
 *
 * VHS: what an AudioPlayer is built on (audio_player.hpp). The default (real) set is real_audio_deps(); the tests fill the
 * fields with fakes: a SinkProvider whose sinks record what they are given (no sound device in WSL), an AlsaApi with a
 * mixer, a start state of the output world (the sound server's sinks) and the FFmpeg getter. LVGL-free.
 */
#pragma once

#include "audio/alsa/alsa_api.hpp"
#include "launcher/launcher_facts.hpp"
#include "play/decode/track_decoder.hpp"
#include "play/engine/outputs.hpp"
#include "play/pulse/pulse_api.hpp"

#include <memory>
#include <string>

namespace vhs {

struct AudioPlayerDeps {
    /* the order is the destruction order's reverse: the provider refers to the sound server and ALSA */
    std::unique_ptr<PulseApi> pulse;      /* null: no sound server (ALSA boards, tests) */
    std::unique_ptr<AlsaApi> alsa;        /* the card list and the playback mixer; null: no mixer, no software-volume key */
    std::unique_ptr<SinkProvider> sinks;  /* required: where the sound goes */
    DecodeLibs libs;                      /* empty ffmpeg getter: the real libraries (real_decode_libs) */
    OutputWorld world;                    /* the output world at start: a fake sound server's sinks, the launcher's facts. With
                                             no worker (use_pulse_worker false) a volume step on a sink changes it in memory */
    bool use_pulse_worker = false;        /* scan `pulse` in the background (every 5 s and on its change events) */
    bool read_launcher_files = false;     /* status.state and board.state, through `facts_io` (null: the real files) */
    std::unique_ptr<FactsIo> facts_io;
    bool media_keys = false;              /* read the transport keys of the devices below */
    std::string keys_devices_path = "/proc/bus/input/devices", keys_dev_dir = "/dev/input";
    std::string volume_state_file;        /* the software levels and own mutes ("" = not kept); the real one: volume_state_path() */
    int open_timeout_ms = 5000;           /* how long open() waits for the engine thread */
};

/* pulse = make_real_pulse(), alsa = make_real_alsa(), the real sink provider over them, the worker, the launcher files, the
 * media keys, the output-levels file. */
std::unique_ptr<AudioPlayerDeps> real_audio_deps();

}  // namespace vhs
