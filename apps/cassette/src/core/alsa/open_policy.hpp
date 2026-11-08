/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette: how a capture card is opened. hw: first (never plughw: no resampling, no format conversion in ALSA); if the card is
 * busy (a sound server holds it while a client records, P0-f) open the server's ALSA "default" device and say so.
 * Rate: 48000 first, else 44100. Channels follow the device (mono, else stereo). Format: native S16, else S24_3LE kept as
 * 24-bit packed, else S32_LE converted to 24-bit packed (the low byte is dropped).
 */
#pragma once

#include <memory>
#include <string>

#include "../frame_sink.hpp"
#include "alsa_api.hpp"
#include "card.hpp"

namespace cassette {

enum class Conversion { None, S32_to_S24_3 };

struct OpenOptions {
    uint32_t period_frames = 1024;    /* P0: 256-frame periods dropped audio */
    uint32_t min_buffer_ms = 500;     /* P0: a 500 ms hardware buffer limits an SD-write stall to about 2 s lost */
    bool allow_server = true;
    std::string server_device = "default";
    const CaptureOffer *server_hint = nullptr; /* channels to ask the server for when the card could not be queried (busy) */
};

enum class OpenStatus {
    Ok,
    NoAlsa,       /* libasound.so.2 is missing */
    Busy,         /* held by another program and the server route is off or also failed */
    Unsupported,  /* the card offers no rate, channel count or format we can record without conversion */
    NoCard,       /* the device vanished (-ENODEV / -ENOENT) */
    Failed,
};

struct OpenResult {
    OpenStatus status = OpenStatus::Failed;
    int err = 0;                 /* the negative error that decided it */
    bool via_server = false;     /* recording through the sound server, not the card directly */
    std::string device;          /* the device string that was opened (or tried last) */
    std::string message;         /* a plain sentence for the UI */
    std::unique_ptr<AlsaPcm> pcm;
    HwGranted hw;                /* what the PCM delivers */
    Conversion conv = Conversion::None;
    AudioFormat format;          /* what the sink receives: rate, channels, 16 or 24 bits */
    CaptureOffer offer;          /* what the card offered (empty when it was busy) */
    bool ok() const { return status == OpenStatus::Ok; }
};

/* The pure choice of parameters from an offer: false with `why` when nothing usable. */
bool choose_params(const CaptureOffer &offer, const OpenOptions &opt, HwRequest &req, Conversion &conv, AudioFormat &fmt,
                   std::string *why);

OpenResult open_capture(AlsaApi &api, const CardInfo &card, const OpenOptions &opt = OpenOptions());

/* Converts n frames of `channels` S32_LE samples to packed S24_3LE (drops the low byte). `out` holds n*channels*3 bytes. */
void convert_s32_to_s24_3(const void *in, void *out, size_t samples);

} // namespace cassette
