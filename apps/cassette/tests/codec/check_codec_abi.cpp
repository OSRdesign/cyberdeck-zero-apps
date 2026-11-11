/*
 * SPDX-License-Identifier: MIT
 *
 * Compile-time check: the libFLAC and libopusenc ABI Cassette declares by hand (src/core/codec/codec_abi.hpp and the
 * function tables of codec_libs.hpp; the app opens the libraries at run time and includes no codec header) matches the
 * real headers. Built by tests/codec/run.sh with -I<include root> -I<include root>/opus for every header set it finds
 * (FLAC 1.4.3 and 1.5.0, libopus 1.4 / 1.5.2, libopusenc 0.2.1 / 0.3 were checked); a mismatch does not compile.
 * Checked: every constant, the structures Cassette touches (VorbisComment_Entry, the head of StreamMetadata, the
 * FrameHeader the decoder's write callback reads), and the shape of every function and callback (number of parameters,
 * and per parameter: a pointer for a pointer, an integer or enum of the same size for an integer or enum, a structure of
 * the same size and alignment for a structure). Phase C adds libFLAC's stream decoder and libopus' decoder.
 */
#include <FLAC/format.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <opus/opus.h>
#include <opus/opusenc.h>

#include "codec_libs.hpp"

#include <cstddef>
#include <cstdio>
#include <tuple>
#include <type_traits>

namespace f = cassette::flac;
namespace o = cassette::ope;
namespace od = cassette::opus;
using cassette::FlacDecLib;
using cassette::FlacLib;
using cassette::OpusDecLib;
using cassette::OpusEncLib;

/* ------------------------------------------------------------------ shapes */

template <typename T> struct Fn;
template <typename R, typename... A> struct Fn<R (*)(A...)> {
    using Ret = R;
    using Args = std::tuple<A...>;
    static constexpr size_t arity = sizeof...(A);
    static constexpr bool variadic = false;
};
template <typename R, typename... A> struct Fn<R (*)(A..., ...)> {
    using Ret = R;
    using Args = std::tuple<A...>;
    static constexpr size_t arity = sizeof...(A);
    static constexpr bool variadic = true;
};

template <typename X, typename Y> constexpr bool same_kind() {
    if constexpr (std::is_void<X>::value || std::is_void<Y>::value) {
        return std::is_void<X>::value && std::is_void<Y>::value;
    } else if constexpr (std::is_pointer<X>::value || std::is_pointer<Y>::value) {
        return std::is_pointer<X>::value && std::is_pointer<Y>::value;
    } else if constexpr (std::is_class<X>::value || std::is_class<Y>::value) {
        return std::is_class<X>::value && std::is_class<Y>::value && sizeof(X) == sizeof(Y) && alignof(X) == alignof(Y);
    } else {
        return (std::is_integral<X>::value || std::is_enum<X>::value) &&
               (std::is_integral<Y>::value || std::is_enum<Y>::value) && sizeof(X) == sizeof(Y);
    }
}

template <typename A, typename B, size_t... I> constexpr bool same_args(std::index_sequence<I...>) {
    return (true && ... && same_kind<std::tuple_element_t<I, A>, std::tuple_element_t<I, B>>());
}

template <typename Real, typename Ours> constexpr bool same_shape() {
    using R = Fn<Real>;
    using O = Fn<Ours>;
    if constexpr (R::arity != O::arity || R::variadic != O::variadic) {
        return false;
    } else {
        return same_kind<typename R::Ret, typename O::Ret>() &&
               same_args<typename R::Args, typename O::Args>(std::make_index_sequence<R::arity>());
    }
}

#define SHAPE(real, ours) static_assert(same_shape<decltype(&real), decltype(FlacLib::ours)>(), #real)
#define OSHAPE(real, ours) static_assert(same_shape<decltype(&real), decltype(OpusEncLib::ours)>(), #real)
#define DSHAPE(real, ours) static_assert(same_shape<decltype(&real), decltype(FlacDecLib::ours)>(), #real)
#define PSHAPE(real, ours) static_assert(same_shape<decltype(&real), decltype(OpusDecLib::ours)>(), #real)

/* ------------------------------------------------------------------ libFLAC */

static_assert(sizeof(f::Bool) == sizeof(FLAC__bool), "FLAC__bool");
static_assert(sizeof(FLAC__StreamEncoderState) == sizeof(int), "enum size");
static_assert(f::kStateOk == FLAC__STREAM_ENCODER_OK && f::kStateUninitialized == FLAC__STREAM_ENCODER_UNINITIALIZED &&
                  f::kStateOggError == FLAC__STREAM_ENCODER_OGG_ERROR &&
                  f::kStateVerifyDecoderError == FLAC__STREAM_ENCODER_VERIFY_DECODER_ERROR &&
                  f::kStateVerifyMismatch == FLAC__STREAM_ENCODER_VERIFY_MISMATCH_IN_AUDIO_DATA &&
                  f::kStateClientError == FLAC__STREAM_ENCODER_CLIENT_ERROR &&
                  f::kStateIoError == FLAC__STREAM_ENCODER_IO_ERROR &&
                  f::kStateFramingError == FLAC__STREAM_ENCODER_FRAMING_ERROR &&
                  f::kStateMemoryError == FLAC__STREAM_ENCODER_MEMORY_ALLOCATION_ERROR,
              "FLAC__StreamEncoderState");
static_assert(f::kInitOk == FLAC__STREAM_ENCODER_INIT_STATUS_OK &&
                  f::kInitEncoderError == FLAC__STREAM_ENCODER_INIT_STATUS_ENCODER_ERROR &&
                  f::kInitUnsupportedContainer == FLAC__STREAM_ENCODER_INIT_STATUS_UNSUPPORTED_CONTAINER &&
                  f::kInitInvalidCallbacks == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_CALLBACKS &&
                  f::kInitInvalidChannels == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_NUMBER_OF_CHANNELS &&
                  f::kInitInvalidBits == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_BITS_PER_SAMPLE &&
                  f::kInitInvalidRate == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_SAMPLE_RATE &&
                  f::kInitInvalidBlockSize == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_BLOCK_SIZE &&
                  f::kInitInvalidLpcOrder == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_MAX_LPC_ORDER &&
                  f::kInitInvalidQlpPrecision == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_QLP_COEFF_PRECISION &&
                  f::kInitBlockTooSmall == FLAC__STREAM_ENCODER_INIT_STATUS_BLOCK_SIZE_TOO_SMALL_FOR_LPC_ORDER &&
                  f::kInitNotStreamable == FLAC__STREAM_ENCODER_INIT_STATUS_NOT_STREAMABLE &&
                  f::kInitInvalidMetadata == FLAC__STREAM_ENCODER_INIT_STATUS_INVALID_METADATA &&
                  f::kInitAlreadyInitialized == FLAC__STREAM_ENCODER_INIT_STATUS_ALREADY_INITIALIZED,
              "FLAC__StreamEncoderInitStatus");
static_assert(f::kWriteOk == FLAC__STREAM_ENCODER_WRITE_STATUS_OK &&
                  f::kWriteFatal == FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR &&
                  f::kSeekOk == FLAC__STREAM_ENCODER_SEEK_STATUS_OK && f::kSeekError == FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR &&
                  f::kSeekUnsupported == FLAC__STREAM_ENCODER_SEEK_STATUS_UNSUPPORTED &&
                  f::kTellOk == FLAC__STREAM_ENCODER_TELL_STATUS_OK && f::kTellError == FLAC__STREAM_ENCODER_TELL_STATUS_ERROR &&
                  f::kTellUnsupported == FLAC__STREAM_ENCODER_TELL_STATUS_UNSUPPORTED,
              "callback statuses");
static_assert(f::kMetaStreamInfo == FLAC__METADATA_TYPE_STREAMINFO && f::kMetaPadding == FLAC__METADATA_TYPE_PADDING &&
                  f::kMetaSeekTable == FLAC__METADATA_TYPE_SEEKTABLE &&
                  f::kMetaVorbisComment == FLAC__METADATA_TYPE_VORBIS_COMMENT,
              "FLAC__MetadataType");

static_assert(sizeof(f::VorbisCommentEntry) == sizeof(FLAC__StreamMetadata_VorbisComment_Entry), "entry size");
static_assert(offsetof(f::VorbisCommentEntry, length) == offsetof(FLAC__StreamMetadata_VorbisComment_Entry, length),
              "entry.length");
static_assert(offsetof(f::VorbisCommentEntry, entry) == offsetof(FLAC__StreamMetadata_VorbisComment_Entry, entry),
              "entry.entry");
static_assert(offsetof(f::MetadataHead, type) == offsetof(FLAC__StreamMetadata, type), "metadata.type");
static_assert(offsetof(f::MetadataHead, is_last) == offsetof(FLAC__StreamMetadata, is_last), "metadata.is_last");
static_assert(offsetof(f::MetadataHead, length) == offsetof(FLAC__StreamMetadata, length), "metadata.length");
static_assert(sizeof(FLAC__StreamMetadata::length) == sizeof(f::MetadataHead::length), "metadata.length size");
static_assert(std::is_same<decltype(FLAC__VERSION_STRING), const char *>::value, "FLAC__VERSION_STRING");

static_assert(same_shape<FLAC__StreamEncoderWriteCallback, f::WriteCb>(), "write callback");
static_assert(same_shape<FLAC__StreamEncoderSeekCallback, f::SeekCb>(), "seek callback");
static_assert(same_shape<FLAC__StreamEncoderTellCallback, f::TellCb>(), "tell callback");
static_assert(same_shape<FLAC__StreamEncoderMetadataCallback, f::MetadataCb>(), "metadata callback");

SHAPE(FLAC__stream_encoder_new, encoder_new);
SHAPE(FLAC__stream_encoder_delete, encoder_delete);
SHAPE(FLAC__stream_encoder_set_verify, set_verify);
SHAPE(FLAC__stream_encoder_set_streamable_subset, set_streamable_subset);
SHAPE(FLAC__stream_encoder_set_channels, set_channels);
SHAPE(FLAC__stream_encoder_set_bits_per_sample, set_bits_per_sample);
SHAPE(FLAC__stream_encoder_set_sample_rate, set_sample_rate);
SHAPE(FLAC__stream_encoder_set_compression_level, set_compression_level);
SHAPE(FLAC__stream_encoder_set_total_samples_estimate, set_total_samples_estimate);
SHAPE(FLAC__stream_encoder_set_metadata, set_metadata);
SHAPE(FLAC__stream_encoder_init_stream, init_stream);
SHAPE(FLAC__stream_encoder_process_interleaved, process_interleaved);
SHAPE(FLAC__stream_encoder_finish, finish);
SHAPE(FLAC__stream_encoder_get_state, get_state);
SHAPE(FLAC__metadata_object_new, metadata_new);
SHAPE(FLAC__metadata_object_delete, metadata_delete);
SHAPE(FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair, vc_entry_from_pair);
SHAPE(FLAC__metadata_object_vorbiscomment_append_comment, vc_append_comment);

/* ------------------------------------------------------------------ libFLAC's stream decoder (phase C) */

static_assert(sizeof(FLAC__StreamDecoderState) == sizeof(int) && sizeof(FLAC__FrameNumberType) == sizeof(int) &&
                  sizeof(FLAC__ChannelAssignment) == sizeof(int),
              "decoder enum sizes");
static_assert(f::kDecSearchForMetadata == FLAC__STREAM_DECODER_SEARCH_FOR_METADATA &&
                  f::kDecReadMetadata == FLAC__STREAM_DECODER_READ_METADATA &&
                  f::kDecSearchForFrameSync == FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC &&
                  f::kDecReadFrame == FLAC__STREAM_DECODER_READ_FRAME &&
                  f::kDecEndOfStream == FLAC__STREAM_DECODER_END_OF_STREAM &&
                  f::kDecOggError == FLAC__STREAM_DECODER_OGG_ERROR && f::kDecSeekError == FLAC__STREAM_DECODER_SEEK_ERROR &&
                  f::kDecAborted == FLAC__STREAM_DECODER_ABORTED &&
                  f::kDecMemoryError == FLAC__STREAM_DECODER_MEMORY_ALLOCATION_ERROR &&
                  f::kDecUninitialized == FLAC__STREAM_DECODER_UNINITIALIZED,
              "FLAC__StreamDecoderState");
static_assert(f::kDecInitOk == FLAC__STREAM_DECODER_INIT_STATUS_OK, "FLAC__StreamDecoderInitStatus");
static_assert(f::kReadContinue == FLAC__STREAM_DECODER_READ_STATUS_CONTINUE &&
                  f::kReadEndOfStream == FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM &&
                  f::kReadAbort == FLAC__STREAM_DECODER_READ_STATUS_ABORT &&
                  f::kDecSeekStatusOk == FLAC__STREAM_DECODER_SEEK_STATUS_OK &&
                  f::kDecSeekStatusError == FLAC__STREAM_DECODER_SEEK_STATUS_ERROR &&
                  f::kDecSeekStatusUnsupported == FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED &&
                  f::kDecTellOk == FLAC__STREAM_DECODER_TELL_STATUS_OK &&
                  f::kDecTellError == FLAC__STREAM_DECODER_TELL_STATUS_ERROR &&
                  f::kDecTellUnsupported == FLAC__STREAM_DECODER_TELL_STATUS_UNSUPPORTED &&
                  f::kLengthOk == FLAC__STREAM_DECODER_LENGTH_STATUS_OK &&
                  f::kLengthError == FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR &&
                  f::kLengthUnsupported == FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED &&
                  f::kWriteContinue == FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE &&
                  f::kWriteAbort == FLAC__STREAM_DECODER_WRITE_STATUS_ABORT,
              "decoder callback statuses");
static_assert(f::kFrameNumberTypeFrame == FLAC__FRAME_NUMBER_TYPE_FRAME_NUMBER &&
                  f::kFrameNumberTypeSample == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER,
              "FLAC__FrameNumberType");
static_assert(sizeof(f::FrameHeader) == sizeof(FLAC__FrameHeader) && alignof(f::FrameHeader) == alignof(FLAC__FrameHeader),
              "FrameHeader size");
static_assert(offsetof(f::FrameHeader, blocksize) == offsetof(FLAC__FrameHeader, blocksize) &&
                  offsetof(f::FrameHeader, sample_rate) == offsetof(FLAC__FrameHeader, sample_rate) &&
                  offsetof(f::FrameHeader, channels) == offsetof(FLAC__FrameHeader, channels) &&
                  offsetof(f::FrameHeader, channel_assignment) == offsetof(FLAC__FrameHeader, channel_assignment) &&
                  offsetof(f::FrameHeader, bits_per_sample) == offsetof(FLAC__FrameHeader, bits_per_sample) &&
                  offsetof(f::FrameHeader, number_type) == offsetof(FLAC__FrameHeader, number_type) &&
                  offsetof(f::FrameHeader, number) == offsetof(FLAC__FrameHeader, number) &&
                  offsetof(f::FrameHeader, crc) == offsetof(FLAC__FrameHeader, crc),
              "FrameHeader fields");
static_assert(offsetof(FLAC__Frame, header) == 0, "the header comes first in FLAC__Frame");

static_assert(same_shape<FLAC__StreamDecoderReadCallback, f::ReadCb>(), "decoder read callback");
static_assert(same_shape<FLAC__StreamDecoderSeekCallback, f::DecSeekCb>(), "decoder seek callback");
static_assert(same_shape<FLAC__StreamDecoderTellCallback, f::DecTellCb>(), "decoder tell callback");
static_assert(same_shape<FLAC__StreamDecoderLengthCallback, f::LengthCb>(), "decoder length callback");
static_assert(same_shape<FLAC__StreamDecoderEofCallback, f::EofCb>(), "decoder eof callback");
static_assert(same_shape<FLAC__StreamDecoderWriteCallback, f::DecWriteCb>(), "decoder write callback");
static_assert(same_shape<FLAC__StreamDecoderMetadataCallback, f::DecMetadataCb>(), "decoder metadata callback");
static_assert(same_shape<FLAC__StreamDecoderErrorCallback, f::ErrorCb>(), "decoder error callback");

DSHAPE(FLAC__stream_decoder_new, decoder_new);
DSHAPE(FLAC__stream_decoder_delete, decoder_delete);
DSHAPE(FLAC__stream_decoder_set_md5_checking, set_md5_checking);
DSHAPE(FLAC__stream_decoder_init_stream, init_stream);
DSHAPE(FLAC__stream_decoder_process_single, process_single);
DSHAPE(FLAC__stream_decoder_process_until_end_of_metadata, process_until_end_of_metadata);
DSHAPE(FLAC__stream_decoder_seek_absolute, seek_absolute);
DSHAPE(FLAC__stream_decoder_flush, flush);
DSHAPE(FLAC__stream_decoder_get_state, get_state);
DSHAPE(FLAC__stream_decoder_finish, finish);

/* ------------------------------------------------------------------ libopus' decoder (phase C) */

static_assert(od::kOk == OPUS_OK && od::kBadArg == OPUS_BAD_ARG && od::kBufferTooSmall == OPUS_BUFFER_TOO_SMALL &&
                  od::kInternalError == OPUS_INTERNAL_ERROR && od::kInvalidPacket == OPUS_INVALID_PACKET &&
                  od::kResetState == OPUS_RESET_STATE && od::kSetGainRequest == OPUS_SET_GAIN_REQUEST,
              "libopus decoder constants");
PSHAPE(opus_decoder_create, decoder_create);
PSHAPE(opus_decoder_destroy, decoder_destroy);
PSHAPE(opus_decode, decode);
PSHAPE(opus_decoder_ctl, decoder_ctl);
PSHAPE(opus_strerror, strerror);

/* ------------------------------------------------------------------ libopusenc / libopus */

static_assert(o::kOk == OPE_OK && o::kBadArg == OPE_BAD_ARG && o::kInternalError == OPE_INTERNAL_ERROR &&
                  o::kUnimplemented == OPE_UNIMPLEMENTED && o::kAllocFail == OPE_ALLOC_FAIL &&
                  o::kCannotOpen == OPE_CANNOT_OPEN && o::kTooLate == OPE_TOO_LATE && o::kWriteFail == OPE_WRITE_FAIL &&
                  o::kCloseFail == OPE_CLOSE_FAIL,
              "OPE_* errors");
static_assert(o::kSetDecisionDelay == OPE_SET_DECISION_DELAY_REQUEST &&
                  o::kGetDecisionDelay == OPE_GET_DECISION_DELAY_REQUEST &&
                  o::kSetMuxingDelay == OPE_SET_MUXING_DELAY_REQUEST && o::kGetMuxingDelay == OPE_GET_MUXING_DELAY_REQUEST &&
                  o::kSetCommentPadding == OPE_SET_COMMENT_PADDING_REQUEST && o::kSetSerialno == OPE_SET_SERIALNO_REQUEST,
              "OPE_* requests");
static_assert(o::kOpusOk == OPUS_OK && o::kSetApplication == OPUS_SET_APPLICATION_REQUEST &&
                  o::kSetBitrate == OPUS_SET_BITRATE_REQUEST && o::kSetVbr == OPUS_SET_VBR_REQUEST &&
                  o::kSetComplexity == OPUS_SET_COMPLEXITY_REQUEST &&
                  o::kSetVbrConstraint == OPUS_SET_VBR_CONSTRAINT_REQUEST &&
                  o::kSetForceChannels == OPUS_SET_FORCE_CHANNELS_REQUEST && o::kSetSignal == OPUS_SET_SIGNAL_REQUEST &&
                  o::kGetLookahead == OPUS_GET_LOOKAHEAD_REQUEST &&
                  o::kSetExpertFrameDuration == OPUS_SET_EXPERT_FRAME_DURATION_REQUEST,
              "OPUS_* requests");
static_assert(o::kAuto == OPUS_AUTO && o::kApplicationVoip == OPUS_APPLICATION_VOIP &&
                  o::kApplicationAudio == OPUS_APPLICATION_AUDIO && o::kSignalVoice == OPUS_SIGNAL_VOICE &&
                  o::kSignalMusic == OPUS_SIGNAL_MUSIC && o::kFramesize20Ms == OPUS_FRAMESIZE_20_MS,
              "OPUS_* values");
static_assert(sizeof(opus_int32) == sizeof(int32_t) && sizeof(opus_int16) == sizeof(int16_t), "opus integer types");

OSHAPE(ope_comments_create, comments_create);
OSHAPE(ope_comments_add, comments_add);
OSHAPE(ope_comments_destroy, comments_destroy);
OSHAPE(ope_encoder_create_pull, encoder_create_pull);
OSHAPE(ope_encoder_write, encoder_write);
OSHAPE(ope_encoder_write_float, encoder_write_float);
OSHAPE(ope_encoder_get_page, encoder_get_page);
OSHAPE(ope_encoder_drain, encoder_drain);
OSHAPE(ope_encoder_destroy, encoder_destroy);
OSHAPE(ope_encoder_flush_header, encoder_flush_header);
OSHAPE(ope_encoder_ctl, encoder_ctl);
OSHAPE(ope_strerror, strerror);

int main() {
    /* libFLAC.so.N: N = CURRENT - AGE (13 - 1 = 12 for FLAC 1.4, 14 - 0 = 14 for FLAC 1.5) */
    std::printf("codec ABI as declared by Cassette: identical to the headers (libFLAC.so.%d, libopusenc API %d)\n",
                FLAC_API_VERSION_CURRENT - FLAC_API_VERSION_AGE, OPE_API_VERSION);
    return 0;
}
