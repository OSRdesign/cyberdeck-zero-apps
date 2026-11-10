/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: fake libFLAC and libopusenc tables. They behave like the libraries where the encoders depend on
 * it (when bytes reach the callbacks / the page queue, block and page boundaries, the decision delay, the STREAMINFO
 * rewrite, end-of-stream) and record what they were given. The bytes are not real FLAC or Opus: the real-library tests
 * (test_real.cpp) check those. One fake encoder at a time (global state, reset by make_*_lib()).
 */
#pragma once

#include "codec_libs.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fake {

struct FlacState {
    /* switches */
    bool fail_new = false;
    int refuse_init = 0;             /* an init status to return instead of OK (e.g. kInitInvalidBits) */
    int fail_process_at = -1;        /* the process_interleaved call (0-based) that fails as a library error */
    uint32_t blocksize = 4096;
    /* what the encoder set */
    uint32_t channels = 0, bits = 0, rate = 0, level = 99;
    int verify = -1, subset = -1;
    std::vector<std::string> comments;
    uint32_t padding = 0;
    unsigned metadata_blocks = 0;
    /* what happened */
    int news = 0, deletes = 0, inits = 0, finishes = 0, process_calls = 0, meta_alive = 0;
    std::vector<int32_t> samples; /* every sample given, interleaved */
    uint64_t frames_emitted = 0;
    bool wrote_after_delete_refused = false;
};
FlacState &flac_state();
cassette::FlacLib make_flac_lib(); /* resets flac_state() */

struct OpusState {
    /* switches */
    int fail_ctl_request = 0;   /* this ctl request returns OPUS_BAD_ARG */
    int fail_write_at = -1;     /* the write call (0-based) that fails */
    uint32_t preskip = 312;
    /* what the encoder set */
    int32_t rate = 0;
    int channels = 0, family = -1;
    std::map<int, int> ctl;     /* request -> value */
    std::vector<std::string> comments;
    /* what happened */
    int creates = 0, destroys = 0, drains = 0, header_flushes = 0, writes = 0;
    std::vector<int16_t> s16;
    std::vector<float> f32;
    uint64_t pages_made = 0;
};
OpusState &opus_state();
cassette::OpusEncLib make_opus_lib(); /* resets opus_state() */

}  // namespace fake
