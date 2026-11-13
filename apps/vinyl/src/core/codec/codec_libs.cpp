/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * The production codec tables: dlopen + dlsym at the first use, as real_alsa.cpp loads libasound (RTLD_NOW | RTLD_LOCAL,
 * every symbol checked, a readable reason when something is missing). The handles are never closed.
 */
#include "codec_libs.hpp"

#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <type_traits>

namespace vinyl {

std::string codec_error_text(int err) {
    switch (err) {
    case kCodecMissing: return "codec library not installed";
    case kCodecRefused: return "codec refused the recording format";
    case kCodecFailed: return "codec failed while encoding";
    case kCodecState: return "codec used out of order";
    case kCodecBadStream: return "a file Vinyl cannot read"; /* VINYL */
    default: break;
    }
    if (err >= 0) return "no error";
    return std::strerror(-err);
}

bool FlacLib::complete() const {
    return encoder_new && encoder_delete && set_verify && set_streamable_subset && set_channels && set_bits_per_sample &&
           set_sample_rate && set_compression_level && set_total_samples_estimate && set_metadata && init_stream &&
           process_interleaved && finish && get_state && metadata_new && metadata_delete && vc_entry_from_pair &&
           vc_append_comment;
}

bool OpusEncLib::complete() const {
    return comments_create && comments_add && comments_destroy && encoder_create_pull && encoder_write &&
           encoder_write_float && encoder_get_page && encoder_drain && encoder_destroy && encoder_flush_header &&
           encoder_ctl && strerror;
}

namespace {

/* dlopen the first name that loads; `why` gets the last dlerror when none does. */
void *open_first(const char *const *names, size_t n, std::string &loaded, std::string &why) {
    for (size_t i = 0; i < n; ++i) {
        if (void *h = ::dlopen(names[i], RTLD_NOW | RTLD_LOCAL)) {
            loaded = names[i];
            return h;
        }
        const char *e = ::dlerror();
        why = std::string(names[i]) + ": " + (e ? e : "dlopen failed");
    }
    return nullptr;
}

template <typename Fn> bool sym(void *h, Fn &fn, const char *name, const std::string &lib, std::string &why) {
    void *p = ::dlsym(h, name);
    fn = reinterpret_cast<Fn>(p);
    if (!p && why.empty()) why = lib + " lacks " + name;
    return p != nullptr;
}

struct FlacHolder {
    std::once_flag once;
    FlacLib lib;
    bool ok = false;
    std::string why;
    void load() {
        static const char *const names[] = {"libFLAC.so.14", "libFLAC.so.12"};
        void *h = open_first(names, 2, lib.name, why);
        if (!h) {
            why = "FLAC library not installed (" + why + ")";
            return;
        }
        why.clear();
        bool all = true;
        all &= sym(h, lib.encoder_new, "FLAC__stream_encoder_new", lib.name, why);
        all &= sym(h, lib.encoder_delete, "FLAC__stream_encoder_delete", lib.name, why);
        all &= sym(h, lib.set_verify, "FLAC__stream_encoder_set_verify", lib.name, why);
        all &= sym(h, lib.set_streamable_subset, "FLAC__stream_encoder_set_streamable_subset", lib.name, why);
        all &= sym(h, lib.set_channels, "FLAC__stream_encoder_set_channels", lib.name, why);
        all &= sym(h, lib.set_bits_per_sample, "FLAC__stream_encoder_set_bits_per_sample", lib.name, why);
        all &= sym(h, lib.set_sample_rate, "FLAC__stream_encoder_set_sample_rate", lib.name, why);
        all &= sym(h, lib.set_compression_level, "FLAC__stream_encoder_set_compression_level", lib.name, why);
        all &= sym(h, lib.set_total_samples_estimate, "FLAC__stream_encoder_set_total_samples_estimate", lib.name, why);
        all &= sym(h, lib.set_metadata, "FLAC__stream_encoder_set_metadata", lib.name, why);
        all &= sym(h, lib.init_stream, "FLAC__stream_encoder_init_stream", lib.name, why);
        all &= sym(h, lib.process_interleaved, "FLAC__stream_encoder_process_interleaved", lib.name, why);
        all &= sym(h, lib.finish, "FLAC__stream_encoder_finish", lib.name, why);
        all &= sym(h, lib.get_state, "FLAC__stream_encoder_get_state", lib.name, why);
        all &= sym(h, lib.metadata_new, "FLAC__metadata_object_new", lib.name, why);
        all &= sym(h, lib.metadata_delete, "FLAC__metadata_object_delete", lib.name, why);
        all &= sym(h, lib.vc_entry_from_pair, "FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair",
                   lib.name, why);
        all &= sym(h, lib.vc_append_comment, "FLAC__metadata_object_vorbiscomment_append_comment", lib.name, why);
        /* FLAC__VERSION_STRING is a data symbol: a `const char *` variable */
        if (void *v = ::dlsym(h, "FLAC__VERSION_STRING")) {
            const char *s = *static_cast<const char *const *>(v);
            if (s) lib.version = s;
        }
        if (!all) {
            ::dlclose(h);
            lib = FlacLib();
            return;
        }
        ok = true;
    }
};

struct OpusHolder {
    std::once_flag once;
    OpusEncLib lib;
    bool ok = false;
    std::string why;
    void load() {
        static const char *const names[] = {"libopusenc.so.0"};
        void *h = open_first(names, 1, lib.name, why);
        if (!h) {
            why = "Opus library not installed (" + why + ")";
            return;
        }
        why.clear();
        bool all = true;
        all &= sym(h, lib.comments_create, "ope_comments_create", lib.name, why);
        all &= sym(h, lib.comments_add, "ope_comments_add", lib.name, why);
        all &= sym(h, lib.comments_destroy, "ope_comments_destroy", lib.name, why);
        all &= sym(h, lib.encoder_create_pull, "ope_encoder_create_pull", lib.name, why);
        all &= sym(h, lib.encoder_write, "ope_encoder_write", lib.name, why);
        all &= sym(h, lib.encoder_write_float, "ope_encoder_write_float", lib.name, why);
        all &= sym(h, lib.encoder_get_page, "ope_encoder_get_page", lib.name, why);
        all &= sym(h, lib.encoder_drain, "ope_encoder_drain", lib.name, why);
        all &= sym(h, lib.encoder_destroy, "ope_encoder_destroy", lib.name, why);
        all &= sym(h, lib.encoder_flush_header, "ope_encoder_flush_header", lib.name, why);
        all &= sym(h, lib.encoder_ctl, "ope_encoder_ctl", lib.name, why);
        all &= sym(h, lib.strerror, "ope_strerror", lib.name, why);
        const char *(*get_version)() = nullptr;
        std::string ignore;
        if (sym(h, get_version, "ope_get_version_string", lib.name, ignore) && get_version) {
            if (const char *s = get_version()) lib.version = s;
        }
        if (!all) {
            ::dlclose(h);
            lib = OpusEncLib();
            return;
        }
        ok = true;
    }
};

struct FlacDecHolder {
    std::once_flag once;
    FlacDecLib lib;
    bool ok = false;
    std::string why;
    void load() {
        static const char *const names[] = {"libFLAC.so.14", "libFLAC.so.12"};
        void *h = open_first(names, 2, lib.name, why);
        if (!h) {
            why = "FLAC library not installed (" + why + ")";
            return;
        }
        why.clear();
        bool all = true;
        all &= sym(h, lib.decoder_new, "FLAC__stream_decoder_new", lib.name, why);
        all &= sym(h, lib.decoder_delete, "FLAC__stream_decoder_delete", lib.name, why);
        all &= sym(h, lib.set_md5_checking, "FLAC__stream_decoder_set_md5_checking", lib.name, why);
        all &= sym(h, lib.init_stream, "FLAC__stream_decoder_init_stream", lib.name, why);
        all &= sym(h, lib.process_single, "FLAC__stream_decoder_process_single", lib.name, why);
        all &= sym(h, lib.process_until_end_of_metadata, "FLAC__stream_decoder_process_until_end_of_metadata",
                   lib.name, why);
        all &= sym(h, lib.seek_absolute, "FLAC__stream_decoder_seek_absolute", lib.name, why);
        all &= sym(h, lib.flush, "FLAC__stream_decoder_flush", lib.name, why);
        all &= sym(h, lib.get_state, "FLAC__stream_decoder_get_state", lib.name, why);
        all &= sym(h, lib.finish, "FLAC__stream_decoder_finish", lib.name, why);
        if (void *v = ::dlsym(h, "FLAC__VERSION_STRING")) {
            const char *s = *static_cast<const char *const *>(v);
            if (s) lib.version = s;
        }
        if (!all) {
            ::dlclose(h);
            lib = FlacDecLib();
            return;
        }
        ok = true;
    }
};

struct OpusDecHolder {
    std::once_flag once;
    OpusDecLib lib;
    bool ok = false;
    std::string why;
    void load() {
        static const char *const names[] = {"libopus.so.0"};
        void *h = open_first(names, 1, lib.name, why);
        if (!h) {
            why = "Opus library not installed (" + why + ")";
            return;
        }
        why.clear();
        bool all = true;
        all &= sym(h, lib.decoder_create, "opus_decoder_create", lib.name, why);
        all &= sym(h, lib.decoder_destroy, "opus_decoder_destroy", lib.name, why);
        all &= sym(h, lib.decode, "opus_decode", lib.name, why);
        all &= sym(h, lib.decoder_ctl, "opus_decoder_ctl", lib.name, why);
        all &= sym(h, lib.strerror, "opus_strerror", lib.name, why);
        const char *(*get_version)() = nullptr;
        std::string ignore;
        if (sym(h, get_version, "opus_get_version_string", lib.name, ignore) && get_version) {
            if (const char *s = get_version()) lib.version = s;
        }
        if (!all) {
            ::dlclose(h);
            lib = OpusDecLib();
            return;
        }
        ok = true;
    }
};

FlacHolder &flac_holder() {
    static FlacHolder h;
    return h;
}
OpusHolder &opus_holder() {
    static OpusHolder h;
    return h;
}
FlacDecHolder &flac_dec_holder() {
    static FlacDecHolder h;
    return h;
}
OpusDecHolder &opus_dec_holder() {
    static OpusDecHolder h;
    return h;
}

}  // namespace

bool FlacDecLib::complete() const {
    return decoder_new && decoder_delete && set_md5_checking && init_stream && process_single &&
           process_until_end_of_metadata && seek_absolute && flush && get_state && finish;
}

bool OpusDecLib::complete() const { return decoder_create && decoder_destroy && decode && decoder_ctl && strerror; }

const FlacDecLib *real_flac_decoder(std::string *why) {
    FlacDecHolder &h = flac_dec_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

const OpusDecLib *real_opus_decoder(std::string *why) {
    OpusDecHolder &h = opus_dec_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

DecoderLibs real_decoder_libs() {
    DecoderLibs d;
    d.flac = real_flac_decoder();
    d.opus = real_opus_decoder();
    return d;
}

const FlacLib *real_flac(std::string *why) {
    FlacHolder &h = flac_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

const OpusEncLib *real_opusenc(std::string *why) {
    OpusHolder &h = opus_holder();
    std::call_once(h.once, [&h] { h.load(); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

CodecAvailability codec_availability_here() {
    CodecAvailability a;
    a.flac = real_flac(&a.flac_why) != nullptr;
    a.opus = real_opusenc(&a.opus_why) != nullptr;
    return a;
}

}  // namespace vinyl
