/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codecs: the function tables of libFLAC and libopusenc. The production tables are filled with dlopen/dlsym at
 * the first use (codec_libs.cpp; no link-time dependency, the app starts and records WAV without either library); the
 * tests fill the same tables with fakes (tests/codec/fake_codecs.*). Only the encoder side is loaded in phase A; the
 * decoders (libFLAC's stream decoder, libopusfile) come with playback of FLAC/Opus takes (see decoder.hpp).
 *
 * Library names tried, in order:
 *   FLAC: libFLAC.so.14 (FLAC 1.5, Debian trixie and later: package libflac14), then libFLAC.so.12 (FLAC 1.4, Debian
 *         bookworm, Ubuntu 24.04: libflac12 / libflac12t64). The encoder calls used here are identical in both.
 *   Opus: libopusenc.so.0 (package libopusenc0), which itself needs libopus.so.0 (libopus0). libopusenc has its own Ogg
 *         page writer: libogg is NOT needed to record Opus (libopusfile needs it for playback later).
 */
#pragma once

#include "codec_abi.hpp"

#include <string>

namespace cassette {

/* Error values of the codec layer (the byte sink's errors are -errno and pass through unchanged). */
constexpr int kCodecMissing = -100100; /* the library is not installed or lacks a symbol */
constexpr int kCodecRefused = -100101; /* the library refused the parameters (rate, channels, bits, settings) */
constexpr int kCodecFailed = -100102;  /* the library failed while encoding */
constexpr int kCodecState = -100103;   /* a call out of order: write before start, write after finish... */
std::string codec_error_text(int err);

struct FlacLib {
    std::string name;    /* the file that was loaded ("libFLAC.so.14"), or "fake" */
    std::string version; /* FLAC__VERSION_STRING ("1.5.0"), empty when unknown */
    flac::Encoder *(*encoder_new)() = nullptr;
    void (*encoder_delete)(flac::Encoder *) = nullptr;
    flac::Bool (*set_verify)(flac::Encoder *, flac::Bool) = nullptr;
    flac::Bool (*set_streamable_subset)(flac::Encoder *, flac::Bool) = nullptr;
    flac::Bool (*set_channels)(flac::Encoder *, uint32_t) = nullptr;
    flac::Bool (*set_bits_per_sample)(flac::Encoder *, uint32_t) = nullptr;
    flac::Bool (*set_sample_rate)(flac::Encoder *, uint32_t) = nullptr;
    flac::Bool (*set_compression_level)(flac::Encoder *, uint32_t) = nullptr;
    flac::Bool (*set_total_samples_estimate)(flac::Encoder *, uint64_t) = nullptr;
    flac::Bool (*set_metadata)(flac::Encoder *, flac::Metadata **, uint32_t) = nullptr;
    int (*init_stream)(flac::Encoder *, flac::WriteCb, flac::SeekCb, flac::TellCb, flac::MetadataCb,
                       void *client) = nullptr;
    flac::Bool (*process_interleaved)(flac::Encoder *, const int32_t *buffer, uint32_t samples) = nullptr;
    flac::Bool (*finish)(flac::Encoder *) = nullptr;
    int (*get_state)(const flac::Encoder *) = nullptr;
    flac::Metadata *(*metadata_new)(int type) = nullptr;
    void (*metadata_delete)(flac::Metadata *) = nullptr;
    flac::Bool (*vc_entry_from_pair)(flac::VorbisCommentEntry *, const char *name, const char *value) = nullptr;
    flac::Bool (*vc_append_comment)(flac::Metadata *, flac::VorbisCommentEntry, flac::Bool copy) = nullptr;
    bool complete() const; /* every function is set */
};

struct OpusEncLib {
    std::string name;    /* "libopusenc.so.0", or "fake" */
    std::string version; /* ope_get_version_string() ("libopusenc 0.2.1") */
    ope::Comments *(*comments_create)() = nullptr;
    int (*comments_add)(ope::Comments *, const char *tag, const char *value) = nullptr;
    void (*comments_destroy)(ope::Comments *) = nullptr;
    /* The pull API: the encoder queues Ogg pages and Cassette takes them with get_page (flush != 0 closes the page being
     * filled), so a page boundary can be forced at every sync: the crash-safe prefix (the callback API cannot). */
    ope::Enc *(*encoder_create_pull)(ope::Comments *, int32_t rate, int channels, int family, int *error) = nullptr;
    int (*encoder_write)(ope::Enc *, const int16_t *pcm, int samples_per_channel) = nullptr;
    int (*encoder_write_float)(ope::Enc *, const float *pcm, int samples_per_channel) = nullptr;
    int (*encoder_get_page)(ope::Enc *, unsigned char **page, int32_t *len, int flush) = nullptr;
    int (*encoder_drain)(ope::Enc *) = nullptr;
    void (*encoder_destroy)(ope::Enc *) = nullptr;
    int (*encoder_flush_header)(ope::Enc *) = nullptr;
    int (*encoder_ctl)(ope::Enc *, int request, ...) = nullptr; /* every request used here takes one int32 value */
    const char *(*strerror)(int) = nullptr;
    bool complete() const;
};

/* The production tables: loaded on the first call, then cached for the life of the process (never unloaded).
 * Null with `why` filled when the library is missing or lacks a symbol. Thread-safe. */
const FlacLib *real_flac(std::string *why = nullptr);
const OpusEncLib *real_opusenc(std::string *why = nullptr);

/* What the format list needs to know: which encoders can be used on this system. */
struct CodecAvailability {
    bool flac = false;
    bool opus = false;
    std::string flac_why; /* why not (the dlopen message), empty when available */
    std::string opus_why;
};
/* Loads both libraries (once) and reports. Cheap after the first call. */
CodecAvailability codec_availability_here();

}  // namespace cassette
