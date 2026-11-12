/*
 * SPDX-License-Identifier: MIT
 *
 * Compile-time check: the libpulse ABI Cassette declares by hand (src/core/pulse/pulse_abi.hpp and the function table of
 * pulse_lib.hpp; the app opens libpulse.so.0 at run time and includes no pulseaudio header) matches the real headers.
 * Built by tests/pulse/run.sh with -I<include root> for every header set it finds (PULSE_INCLUDES; PulseAudio 16.1 was
 * checked); a mismatch does not compile.
 * Checked: every constant, the size and every field offset of the structures Cassette declares (for the info structures
 * libpulse hands out by pointer: the offset of every field up to the last one Cassette declares), and the shape of every
 * function and callback (number of parameters, and per parameter: a pointer for a pointer, an integer or enum of the same
 * size for an integer or enum, a structure of the same size and alignment for a structure).
 */
#include <pulse/pulseaudio.h>
#include <pulse/thread-mainloop.h>

#include "pulse_lib.hpp"

#include <cstddef>
#include <cstdio>
#include <tuple>
#include <type_traits>

namespace p = cassette::pa;
using cassette::PulseLib;

/* ------------------------------------------------------------------ shapes (as tests/codec/check_codec_abi.cpp) */

template <typename T> struct Fn;
template <typename R, typename... A> struct Fn<R (*)(A...)> {
    using Ret = R;
    using Args = std::tuple<A...>;
    static constexpr size_t arity = sizeof...(A);
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
    if constexpr (R::arity != O::arity) {
        return false;
    } else {
        return same_kind<typename R::Ret, typename O::Ret>() &&
               same_args<typename R::Args, typename O::Args>(std::make_index_sequence<R::arity>());
    }
}

#define SHAPE(real, ours) static_assert(same_shape<decltype(&real), decltype(PulseLib::ours)>(), #real)
#define CBSHAPE(real, ours) static_assert(same_shape<real, p::ours>(), #real)
#define FIELD(ours, real, f)                                                                                           \
    static_assert(offsetof(p::ours, f) == offsetof(real, f), #real "." #f);                                            \
    static_assert(sizeof(p::ours::f) == sizeof(real::f), #real "." #f " size")

/* ------------------------------------------------------------------ constants */

static_assert(sizeof(pa_sample_format_t) == sizeof(int) && sizeof(pa_stream_flags_t) == sizeof(int) &&
                  sizeof(pa_context_state_t) == sizeof(int) && sizeof(pa_subscription_event_type_t) == sizeof(int),
              "enum sizes");
static_assert(sizeof(pa_usec_t) == sizeof(p::usec_t) && sizeof(pa_volume_t) == sizeof(p::volume_t), "integer types");
static_assert(p::kInvalidIndex == PA_INVALID_INDEX && p::kChannelsMax == PA_CHANNELS_MAX, "limits");
static_assert(p::kSampleU8 == PA_SAMPLE_U8 && p::kSampleAlaw == PA_SAMPLE_ALAW && p::kSampleUlaw == PA_SAMPLE_ULAW &&
                  p::kSampleS16LE == PA_SAMPLE_S16LE && p::kSampleS16BE == PA_SAMPLE_S16BE &&
                  p::kSampleFloat32LE == PA_SAMPLE_FLOAT32LE && p::kSampleFloat32BE == PA_SAMPLE_FLOAT32BE &&
                  p::kSampleS32LE == PA_SAMPLE_S32LE && p::kSampleS32BE == PA_SAMPLE_S32BE &&
                  p::kSampleS24LE == PA_SAMPLE_S24LE && p::kSampleS24BE == PA_SAMPLE_S24BE &&
                  p::kSampleS24_32LE == PA_SAMPLE_S24_32LE && p::kSampleS24_32BE == PA_SAMPLE_S24_32BE &&
                  p::kSampleInvalid == PA_SAMPLE_INVALID,
              "pa_sample_format_t");
static_assert(p::kContextUnconnected == PA_CONTEXT_UNCONNECTED && p::kContextConnecting == PA_CONTEXT_CONNECTING &&
                  p::kContextAuthorizing == PA_CONTEXT_AUTHORIZING && p::kContextSettingName == PA_CONTEXT_SETTING_NAME &&
                  p::kContextReady == PA_CONTEXT_READY && p::kContextFailed == PA_CONTEXT_FAILED &&
                  p::kContextTerminated == PA_CONTEXT_TERMINATED,
              "pa_context_state_t");
static_assert(p::kStreamUnconnected == PA_STREAM_UNCONNECTED && p::kStreamCreating == PA_STREAM_CREATING &&
                  p::kStreamReady == PA_STREAM_READY && p::kStreamFailed == PA_STREAM_FAILED &&
                  p::kStreamTerminated == PA_STREAM_TERMINATED,
              "pa_stream_state_t");
static_assert(p::kOperationRunning == PA_OPERATION_RUNNING && p::kOperationDone == PA_OPERATION_DONE &&
                  p::kOperationCancelled == PA_OPERATION_CANCELLED,
              "pa_operation_state_t");
static_assert(p::kContextNoFlags == PA_CONTEXT_NOFLAGS && p::kContextNoAutospawn == PA_CONTEXT_NOAUTOSPAWN &&
                  p::kContextNoFail == PA_CONTEXT_NOFAIL,
              "pa_context_flags_t");
static_assert(p::kStreamNoFlags == PA_STREAM_NOFLAGS && p::kStreamStartCorked == PA_STREAM_START_CORKED &&
                  p::kStreamInterpolateTiming == PA_STREAM_INTERPOLATE_TIMING &&
                  p::kStreamNotMonotonic == PA_STREAM_NOT_MONOTONIC &&
                  p::kStreamAutoTimingUpdate == PA_STREAM_AUTO_TIMING_UPDATE &&
                  p::kStreamNoRemapChannels == PA_STREAM_NO_REMAP_CHANNELS &&
                  p::kStreamNoRemixChannels == PA_STREAM_NO_REMIX_CHANNELS &&
                  p::kStreamFixFormat == PA_STREAM_FIX_FORMAT && p::kStreamFixRate == PA_STREAM_FIX_RATE &&
                  p::kStreamFixChannels == PA_STREAM_FIX_CHANNELS && p::kStreamDontMove == PA_STREAM_DONT_MOVE &&
                  p::kStreamVariableRate == PA_STREAM_VARIABLE_RATE && p::kStreamPeakDetect == PA_STREAM_PEAK_DETECT &&
                  p::kStreamStartMuted == PA_STREAM_START_MUTED && p::kStreamAdjustLatency == PA_STREAM_ADJUST_LATENCY &&
                  p::kStreamEarlyRequests == PA_STREAM_EARLY_REQUESTS &&
                  p::kStreamDontInhibitAutoSuspend == PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND &&
                  p::kStreamStartUnmuted == PA_STREAM_START_UNMUTED &&
                  p::kStreamFailOnSuspend == PA_STREAM_FAIL_ON_SUSPEND &&
                  p::kStreamRelativeVolume == PA_STREAM_RELATIVE_VOLUME &&
                  p::kStreamPassthrough == PA_STREAM_PASSTHROUGH,
              "pa_stream_flags_t");
static_assert(p::kSeekRelative == PA_SEEK_RELATIVE, "pa_seek_mode_t");
static_assert(p::kMaskSink == PA_SUBSCRIPTION_MASK_SINK && p::kMaskSource == PA_SUBSCRIPTION_MASK_SOURCE &&
                  p::kMaskSinkInput == PA_SUBSCRIPTION_MASK_SINK_INPUT &&
                  p::kMaskSourceOutput == PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT &&
                  p::kMaskServer == PA_SUBSCRIPTION_MASK_SERVER && p::kMaskCard == PA_SUBSCRIPTION_MASK_CARD,
              "pa_subscription_mask_t");
static_assert(p::kEventSink == PA_SUBSCRIPTION_EVENT_SINK && p::kEventSource == PA_SUBSCRIPTION_EVENT_SOURCE &&
                  p::kEventSinkInput == PA_SUBSCRIPTION_EVENT_SINK_INPUT &&
                  p::kEventSourceOutput == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT &&
                  p::kEventServer == PA_SUBSCRIPTION_EVENT_SERVER && p::kEventCard == PA_SUBSCRIPTION_EVENT_CARD &&
                  p::kEventFacilityMask == PA_SUBSCRIPTION_EVENT_FACILITY_MASK &&
                  p::kEventNew == PA_SUBSCRIPTION_EVENT_NEW && p::kEventChange == PA_SUBSCRIPTION_EVENT_CHANGE &&
                  p::kEventRemove == PA_SUBSCRIPTION_EVENT_REMOVE &&
                  p::kEventTypeMask == PA_SUBSCRIPTION_EVENT_TYPE_MASK,
              "pa_subscription_event_type_t");
static_assert(p::kDeviceInvalidState == PA_SOURCE_INVALID_STATE && p::kDeviceRunning == PA_SOURCE_RUNNING &&
                  p::kDeviceIdle == PA_SOURCE_IDLE && p::kDeviceSuspended == PA_SOURCE_SUSPENDED &&
                  p::kDeviceInvalidState == PA_SINK_INVALID_STATE && p::kDeviceRunning == PA_SINK_RUNNING &&
                  p::kDeviceIdle == PA_SINK_IDLE && p::kDeviceSuspended == PA_SINK_SUSPENDED,
              "pa_source_state_t / pa_sink_state_t");
static_assert(p::kOk == PA_OK && p::kErrAccess == PA_ERR_ACCESS && p::kErrCommand == PA_ERR_COMMAND &&
                  p::kErrInvalid == PA_ERR_INVALID && p::kErrExist == PA_ERR_EXIST &&
                  p::kErrNoEntity == PA_ERR_NOENTITY && p::kErrConnectionRefused == PA_ERR_CONNECTIONREFUSED &&
                  p::kErrProtocol == PA_ERR_PROTOCOL && p::kErrTimeout == PA_ERR_TIMEOUT &&
                  p::kErrAuthKey == PA_ERR_AUTHKEY && p::kErrInternal == PA_ERR_INTERNAL &&
                  p::kErrConnectionTerminated == PA_ERR_CONNECTIONTERMINATED && p::kErrKilled == PA_ERR_KILLED &&
                  p::kErrInvalidServer == PA_ERR_INVALIDSERVER && p::kErrModInitFailed == PA_ERR_MODINITFAILED &&
                  p::kErrBadState == PA_ERR_BADSTATE && p::kErrNoData == PA_ERR_NODATA &&
                  p::kErrVersion == PA_ERR_VERSION && p::kErrTooLarge == PA_ERR_TOOLARGE &&
                  p::kErrNotSupported == PA_ERR_NOTSUPPORTED && p::kErrUnknown == PA_ERR_UNKNOWN &&
                  p::kErrNoExtension == PA_ERR_NOEXTENSION && p::kErrObsolete == PA_ERR_OBSOLETE &&
                  p::kErrNotImplemented == PA_ERR_NOTIMPLEMENTED && p::kErrForked == PA_ERR_FORKED &&
                  p::kErrIo == PA_ERR_IO && p::kErrBusy == PA_ERR_BUSY,
              "pa_error_code_t");

/* ------------------------------------------------------------------ structures */

static_assert(sizeof(p::SampleSpec) == sizeof(pa_sample_spec) && alignof(p::SampleSpec) == alignof(pa_sample_spec), "");
FIELD(SampleSpec, pa_sample_spec, format);
FIELD(SampleSpec, pa_sample_spec, rate);
FIELD(SampleSpec, pa_sample_spec, channels);
static_assert(sizeof(p::ChannelMap) == sizeof(pa_channel_map), "pa_channel_map");
FIELD(ChannelMap, pa_channel_map, channels);
FIELD(ChannelMap, pa_channel_map, map);
static_assert(sizeof(p::CVolume) == sizeof(pa_cvolume), "pa_cvolume");
FIELD(CVolume, pa_cvolume, channels);
FIELD(CVolume, pa_cvolume, values);
static_assert(sizeof(p::BufferAttr) == sizeof(pa_buffer_attr), "pa_buffer_attr");
FIELD(BufferAttr, pa_buffer_attr, maxlength);
FIELD(BufferAttr, pa_buffer_attr, tlength);
FIELD(BufferAttr, pa_buffer_attr, prebuf);
FIELD(BufferAttr, pa_buffer_attr, minreq);
FIELD(BufferAttr, pa_buffer_attr, fragsize);
static_assert(sizeof(p::TimingInfo) == sizeof(pa_timing_info), "pa_timing_info");
FIELD(TimingInfo, pa_timing_info, timestamp);
FIELD(TimingInfo, pa_timing_info, synchronized_clocks);
FIELD(TimingInfo, pa_timing_info, sink_usec);
FIELD(TimingInfo, pa_timing_info, source_usec);
FIELD(TimingInfo, pa_timing_info, transport_usec);
FIELD(TimingInfo, pa_timing_info, playing);
FIELD(TimingInfo, pa_timing_info, write_index_corrupt);
FIELD(TimingInfo, pa_timing_info, write_index);
FIELD(TimingInfo, pa_timing_info, read_index_corrupt);
FIELD(TimingInfo, pa_timing_info, read_index);
FIELD(TimingInfo, pa_timing_info, configured_sink_usec);
FIELD(TimingInfo, pa_timing_info, configured_source_usec);
FIELD(TimingInfo, pa_timing_info, since_underrun);

#define SINK(f) FIELD(SinkInfo, pa_sink_info, f)
SINK(name);
SINK(index);
SINK(description);
SINK(sample_spec);
SINK(channel_map);
SINK(owner_module);
SINK(volume);
SINK(mute);
SINK(monitor_source);
SINK(monitor_source_name);
SINK(latency);
SINK(driver);
SINK(flags);
SINK(proplist);
SINK(configured_latency);
SINK(base_volume);
SINK(state);
SINK(n_volume_steps);
SINK(card);
SINK(n_ports);
SINK(ports);
SINK(active_port);
SINK(n_formats);
SINK(formats);
#define SOURCE(f) FIELD(SourceInfo, pa_source_info, f)
SOURCE(name);
SOURCE(index);
SOURCE(description);
SOURCE(sample_spec);
SOURCE(channel_map);
SOURCE(owner_module);
SOURCE(volume);
SOURCE(mute);
SOURCE(monitor_of_sink);
SOURCE(monitor_of_sink_name);
SOURCE(latency);
SOURCE(driver);
SOURCE(flags);
SOURCE(proplist);
SOURCE(configured_latency);
SOURCE(base_volume);
SOURCE(state);
SOURCE(n_volume_steps);
SOURCE(card);
SOURCE(n_ports);
SOURCE(ports);
SOURCE(active_port);
SOURCE(n_formats);
SOURCE(formats);
#define SERVER(f) FIELD(ServerInfo, pa_server_info, f)
static_assert(sizeof(p::ServerInfo) == sizeof(pa_server_info), "pa_server_info");
SERVER(user_name);
SERVER(host_name);
SERVER(server_version);
SERVER(server_name);
SERVER(sample_spec);
SERVER(default_sink_name);
SERVER(default_source_name);
SERVER(cookie);
SERVER(channel_map);
static_assert(sizeof(p::CardProfileInfo2) == sizeof(pa_card_profile_info2), "pa_card_profile_info2");
FIELD(CardProfileInfo2, pa_card_profile_info2, name);
FIELD(CardProfileInfo2, pa_card_profile_info2, description);
FIELD(CardProfileInfo2, pa_card_profile_info2, n_sinks);
FIELD(CardProfileInfo2, pa_card_profile_info2, n_sources);
FIELD(CardProfileInfo2, pa_card_profile_info2, priority);
FIELD(CardProfileInfo2, pa_card_profile_info2, available);
#define CARD(f) FIELD(CardInfo, pa_card_info, f)
CARD(index);
CARD(name);
CARD(owner_module);
CARD(driver);
CARD(n_profiles);
CARD(profiles);
CARD(active_profile);
CARD(proplist);
CARD(n_ports);
CARD(ports);
CARD(profiles2);
CARD(active_profile2);
FIELD(SourceOutputInfoHead, pa_source_output_info, index);
FIELD(SourceOutputInfoHead, pa_source_output_info, name);
FIELD(SourceOutputInfoHead, pa_source_output_info, owner_module);
FIELD(SourceOutputInfoHead, pa_source_output_info, client);
FIELD(SourceOutputInfoHead, pa_source_output_info, source);
FIELD(SinkInputInfoHead, pa_sink_input_info, index);
FIELD(SinkInputInfoHead, pa_sink_input_info, name);
FIELD(SinkInputInfoHead, pa_sink_input_info, owner_module);
FIELD(SinkInputInfoHead, pa_sink_input_info, client);
FIELD(SinkInputInfoHead, pa_sink_input_info, sink);

/* ------------------------------------------------------------------ callbacks */

CBSHAPE(pa_context_notify_cb_t, ContextNotifyCb);
CBSHAPE(pa_context_success_cb_t, ContextSuccessCb);
CBSHAPE(pa_context_subscribe_cb_t, ContextSubscribeCb);
CBSHAPE(pa_stream_notify_cb_t, StreamNotifyCb);
CBSHAPE(pa_stream_request_cb_t, StreamRequestCb);
CBSHAPE(pa_stream_success_cb_t, StreamSuccessCb);
CBSHAPE(pa_sink_info_cb_t, SinkInfoCb);
CBSHAPE(pa_source_info_cb_t, SourceInfoCb);
CBSHAPE(pa_server_info_cb_t, ServerInfoCb);
CBSHAPE(pa_card_info_cb_t, CardInfoCb);
CBSHAPE(pa_source_output_info_cb_t, SourceOutputInfoCb);
CBSHAPE(pa_sink_input_info_cb_t, SinkInputInfoCb);
CBSHAPE(pa_free_cb_t, FreeCb);

/* ------------------------------------------------------------------ functions */

SHAPE(pa_threaded_mainloop_new, threaded_mainloop_new);
SHAPE(pa_threaded_mainloop_free, threaded_mainloop_free);
SHAPE(pa_threaded_mainloop_start, threaded_mainloop_start);
SHAPE(pa_threaded_mainloop_stop, threaded_mainloop_stop);
SHAPE(pa_threaded_mainloop_lock, threaded_mainloop_lock);
SHAPE(pa_threaded_mainloop_unlock, threaded_mainloop_unlock);
SHAPE(pa_threaded_mainloop_get_api, threaded_mainloop_get_api);
SHAPE(pa_threaded_mainloop_in_thread, threaded_mainloop_in_thread);
SHAPE(pa_context_new_with_proplist, context_new_with_proplist);
SHAPE(pa_context_unref, context_unref);
SHAPE(pa_context_connect, context_connect);
SHAPE(pa_context_disconnect, context_disconnect);
SHAPE(pa_context_get_state, context_get_state);
SHAPE(pa_context_errno, context_errno);
SHAPE(pa_context_set_state_callback, context_set_state_callback);
SHAPE(pa_context_set_subscribe_callback, context_set_subscribe_callback);
SHAPE(pa_context_subscribe, context_subscribe);
SHAPE(pa_context_get_server_info, context_get_server_info);
SHAPE(pa_context_get_source_info_list, context_get_source_info_list);
SHAPE(pa_context_get_source_info_by_name, context_get_source_info_by_name);
SHAPE(pa_context_get_source_info_by_index, context_get_source_info_by_index);
SHAPE(pa_context_get_sink_info_list, context_get_sink_info_list);
SHAPE(pa_context_get_sink_info_by_name, context_get_sink_info_by_name);
SHAPE(pa_context_get_sink_info_by_index, context_get_sink_info_by_index);
SHAPE(pa_context_get_card_info_list, context_get_card_info_list);
SHAPE(pa_context_get_card_info_by_name, context_get_card_info_by_name);
SHAPE(pa_context_get_source_output_info, context_get_source_output_info);
SHAPE(pa_context_get_sink_input_info, context_get_sink_input_info);
SHAPE(pa_operation_unref, operation_unref);
SHAPE(pa_operation_get_state, operation_get_state);
SHAPE(pa_operation_cancel, operation_cancel);
SHAPE(pa_stream_new_with_proplist, stream_new_with_proplist);
SHAPE(pa_stream_unref, stream_unref);
SHAPE(pa_stream_connect_record, stream_connect_record);
SHAPE(pa_stream_connect_playback, stream_connect_playback);
SHAPE(pa_stream_disconnect, stream_disconnect);
SHAPE(pa_stream_get_state, stream_get_state);
SHAPE(pa_stream_get_index, stream_get_index);
SHAPE(pa_stream_set_state_callback, stream_set_state_callback);
SHAPE(pa_stream_set_read_callback, stream_set_read_callback);
SHAPE(pa_stream_set_write_callback, stream_set_write_callback);
SHAPE(pa_stream_set_moved_callback, stream_set_moved_callback);
SHAPE(pa_stream_set_suspended_callback, stream_set_suspended_callback);
SHAPE(pa_stream_set_underflow_callback, stream_set_underflow_callback);
SHAPE(pa_stream_peek, stream_peek);
SHAPE(pa_stream_drop, stream_drop);
SHAPE(pa_stream_readable_size, stream_readable_size);
SHAPE(pa_stream_writable_size, stream_writable_size);
SHAPE(pa_stream_write, stream_write);
SHAPE(pa_stream_get_device_name, stream_get_device_name);
SHAPE(pa_stream_get_device_index, stream_get_device_index);
SHAPE(pa_stream_is_suspended, stream_is_suspended);
SHAPE(pa_stream_get_latency, stream_get_latency);
SHAPE(pa_stream_get_timing_info, stream_get_timing_info);
SHAPE(pa_stream_get_buffer_attr, stream_get_buffer_attr);
SHAPE(pa_stream_get_sample_spec, stream_get_sample_spec);
SHAPE(pa_stream_flush, stream_flush);
SHAPE(pa_stream_drain, stream_drain);
SHAPE(pa_proplist_new, proplist_new);
SHAPE(pa_proplist_free, proplist_free);
SHAPE(pa_proplist_sets, proplist_sets);
SHAPE(pa_proplist_gets, proplist_gets);
SHAPE(pa_strerror, strerror);
SHAPE(pa_get_library_version, get_library_version);

int main() {
    std::printf("pulse ABI as declared by Cassette: identical to the headers (libpulse %s, protocol %d)\n",
                pa_get_headers_version(), PA_PROTOCOL_VERSION);
    return 0;
}
