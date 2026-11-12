/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * Cassette codecs: the parts of the libFLAC and libopusenc C ABI the app uses, declared here by hand so no codec header
 * is needed to build (the libraries are opened at run time with dlopen, see codec_libs.hpp). The values and layouts come
 * from the public headers of FLAC 1.4.3 / 1.5.0 (libFLAC.so.12 / .14: the encoder API used here is the same in both),
 * libopus 1.4 / 1.5.2 and libopusenc 0.2.1 / 0.3. tests/codec/check_codec_abi.cpp compares every value below with the
 * real headers whenever they are present (a mismatch does not compile).
 * C enums are int-sized on every target (aarch64, x86-64), so enum parameters and results are declared as int.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace vinyl {

namespace flac {

struct Encoder;  /* FLAC__StreamEncoder (opaque) */
struct Metadata; /* FLAC__StreamMetadata (opaque apart from MetadataHead) */

using Bool = int; /* FLAC__bool */

/* The first fields of FLAC__StreamMetadata: Cassette sets `length` of a PADDING block (the size of the padding). */
struct MetadataHead {
    int type;        /* FLAC__MetadataType */
    Bool is_last;
    uint32_t length; /* bytes of the block body */
};

/* FLAC__StreamMetadata_VorbisComment_Entry, passed by value to vorbiscomment_append_comment. */
struct VorbisCommentEntry {
    uint32_t length;
    uint8_t *entry; /* "NAME=value", not NUL-terminated (libFLAC adds a NUL it does not count) */
};

/* FLAC__StreamEncoderState */
constexpr int kStateOk = 0, kStateUninitialized = 1, kStateOggError = 2, kStateVerifyDecoderError = 3,
              kStateVerifyMismatch = 4, kStateClientError = 5, kStateIoError = 6, kStateFramingError = 7,
              kStateMemoryError = 8;
/* FLAC__StreamEncoderInitStatus */
constexpr int kInitOk = 0, kInitEncoderError = 1, kInitUnsupportedContainer = 2, kInitInvalidCallbacks = 3,
              kInitInvalidChannels = 4, kInitInvalidBits = 5, kInitInvalidRate = 6, kInitInvalidBlockSize = 7,
              kInitInvalidLpcOrder = 8, kInitInvalidQlpPrecision = 9, kInitBlockTooSmall = 10, kInitNotStreamable = 11,
              kInitInvalidMetadata = 12, kInitAlreadyInitialized = 13;
/* FLAC__StreamEncoderWriteStatus, SeekStatus, TellStatus */
constexpr int kWriteOk = 0, kWriteFatal = 1;
constexpr int kSeekOk = 0, kSeekError = 1, kSeekUnsupported = 2;
constexpr int kTellOk = 0, kTellError = 1, kTellUnsupported = 2;
/* FLAC__MetadataType */
constexpr int kMetaStreamInfo = 0, kMetaPadding = 1, kMetaSeekTable = 3, kMetaVorbisComment = 4;

/* FLAC__StreamEncoderWriteCallback: samples is 0 for metadata, else the frame's samples per channel. */
using WriteCb = int (*)(const Encoder *, const uint8_t *buffer, size_t bytes, uint32_t samples, uint32_t current_frame,
                        void *client);
using SeekCb = int (*)(const Encoder *, uint64_t absolute_byte_offset, void *client);
using TellCb = int (*)(const Encoder *, uint64_t *absolute_byte_offset, void *client);
using MetadataCb = void (*)(const Encoder *, const Metadata *, void *client);

/* ---- the stream DECODER (phase C: playback, peaks and trim of FLAC takes). The values below are the same in FLAC 1.4
 * and 1.5; 1.5 only appended values (END_OF_LINK for chained Ogg FLAC, two error statuses) that native FLAC never gives. */
struct Decoder; /* FLAC__StreamDecoder (opaque) */

/* FLAC__FrameHeader, the first member of the FLAC__Frame the write callback gets (only the header is read). */
struct FrameHeader {
    uint32_t blocksize;
    uint32_t sample_rate;
    uint32_t channels;
    int channel_assignment; /* FLAC__ChannelAssignment */
    uint32_t bits_per_sample;
    int number_type;        /* FLAC__FrameNumberType: 0 frame number, 1 sample number */
    union {
        uint32_t frame_number;
        uint64_t sample_number;
    } number;
    uint8_t crc;
};
constexpr int kFrameNumberTypeFrame = 0, kFrameNumberTypeSample = 1;

/* FLAC__StreamDecoderState */
constexpr int kDecSearchForMetadata = 0, kDecReadMetadata = 1, kDecSearchForFrameSync = 2, kDecReadFrame = 3,
              kDecEndOfStream = 4, kDecOggError = 5, kDecSeekError = 6, kDecAborted = 7, kDecMemoryError = 8,
              kDecUninitialized = 9;
/* FLAC__StreamDecoderInitStatus */
constexpr int kDecInitOk = 0;
/* FLAC__StreamDecoderReadStatus, SeekStatus, TellStatus, LengthStatus, WriteStatus */
constexpr int kReadContinue = 0, kReadEndOfStream = 1, kReadAbort = 2;
constexpr int kDecSeekStatusOk = 0, kDecSeekStatusError = 1, kDecSeekStatusUnsupported = 2;
constexpr int kDecTellOk = 0, kDecTellError = 1, kDecTellUnsupported = 2;
constexpr int kLengthOk = 0, kLengthError = 1, kLengthUnsupported = 2;
constexpr int kWriteContinue = 0, kWriteAbort = 1;

using ReadCb = int (*)(const Decoder *, uint8_t buffer[], size_t *bytes, void *client);
using DecSeekCb = int (*)(const Decoder *, uint64_t absolute_byte_offset, void *client);
using DecTellCb = int (*)(const Decoder *, uint64_t *absolute_byte_offset, void *client);
using LengthCb = int (*)(const Decoder *, uint64_t *stream_length, void *client);
using EofCb = Bool (*)(const Decoder *, void *client);
/* `frame` points at a FLAC__Frame whose first member is the FrameHeader; buffer[c] holds blocksize samples of channel c */
using DecWriteCb = int (*)(const Decoder *, const FrameHeader *frame, const int32_t *const buffer[], void *client);
using DecMetadataCb = void (*)(const Decoder *, const Metadata *, void *client);
using ErrorCb = void (*)(const Decoder *, int status, void *client);

}  // namespace flac

/* libopus' decoder (phase C: Opus playback; the Ogg layer is Cassette's own, codec/ogg_opus.*). */
namespace opus {

struct Decoder; /* OpusDecoder (opaque) */

/* opus_defines.h */
constexpr int kOk = 0, kBadArg = -1, kBufferTooSmall = -2, kInternalError = -3, kInvalidPacket = -4;
constexpr int kResetState = 4028;    /* OPUS_RESET_STATE: no argument */
constexpr int kSetGainRequest = 4034; /* OPUS_SET_GAIN(x): one int32, Q8 dB (OpusHead's output gain) */

}  // namespace opus

namespace ope {

struct Enc;      /* OggOpusEnc (opaque) */
struct Comments; /* OggOpusComments (opaque) */

/* opusenc.h error codes */
constexpr int kOk = 0, kBadArg = -11, kInternalError = -13, kUnimplemented = -15, kAllocFail = -17, kCannotOpen = -30,
              kTooLate = -31, kWriteFail = -34, kCloseFail = -35;
/* opusenc.h ctl requests */
constexpr int kSetDecisionDelay = 14000, kGetDecisionDelay = 14001, kSetMuxingDelay = 14002, kGetMuxingDelay = 14003,
              kSetCommentPadding = 14004, kSetSerialno = 14006;
/* opus_defines.h ctl requests (forwarded by ope_encoder_ctl to the Opus encoder) and their values */
constexpr int kOpusOk = 0;
constexpr int kSetApplication = 4000, kSetBitrate = 4002, kSetVbr = 4006, kSetComplexity = 4010,
              kSetVbrConstraint = 4020, kSetForceChannels = 4022, kSetSignal = 4024, kGetLookahead = 4027,
              kSetExpertFrameDuration = 4040;
constexpr int kAuto = -1000;
constexpr int kApplicationVoip = 2048, kApplicationAudio = 2049;
constexpr int kSignalVoice = 3001, kSignalMusic = 3002;
constexpr int kFramesize20Ms = 5004;

}  // namespace ope

}  // namespace vinyl
