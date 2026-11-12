/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound server: the parts of the libpulse C ABI the app uses, declared here by hand so no pulseaudio header is
 * needed to build (libpulse.so.0 is opened at run time with dlopen, see pulse_lib.hpp; the binary never gets a NEEDED
 * entry for it). The values and layouts come from the public headers of PulseAudio 16.1 (Ubuntu 24.04) and 17.0
 * (Debian 13): libpulse's ABI has been stable since 1.0, new fields are only appended to the info structures, so a
 * structure Cassette only reads through a pointer the library hands out may be longer in the library than here.
 * tests/pulse/check_pulse_abi.cpp compares every value below with the real headers whenever they are present (a mismatch
 * does not compile). The same API is served by pipewire-pulse (PipeWire) and by PulseAudio itself.
 * C enums are int-sized on every target (aarch64, x86-64), so enum parameters and results are declared as int.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/time.h>

namespace cassette {

namespace pa {

struct ThreadedMainloop; /* pa_threaded_mainloop (opaque) */
struct MainloopApi;      /* pa_mainloop_api (only passed through) */
struct Context;          /* pa_context */
struct Stream;           /* pa_stream */
struct Operation;        /* pa_operation */
struct Proplist;         /* pa_proplist */
struct SpawnApi;         /* pa_spawn_api (always null here) */
struct FormatInfo;       /* pa_format_info (never read) */
struct SinkPortInfo;     /* pa_sink_port_info (never read) */
struct SourcePortInfo;   /* pa_source_port_info (never read) */
struct CardPortInfo;     /* pa_card_port_info (never read) */
struct CardProfileInfo;  /* pa_card_profile_info, the deprecated profile table (never read: profiles2 is) */

using usec_t = uint64_t;   /* pa_usec_t */
using volume_t = uint32_t; /* pa_volume_t */

constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu; /* PA_INVALID_INDEX */
constexpr unsigned kChannelsMax = 32;           /* PA_CHANNELS_MAX */
constexpr uint32_t kBufferDefault = 0xFFFFFFFFu; /* (uint32_t) -1 in a pa_buffer_attr field: the server chooses */

/* pa_sample_format_t */
constexpr int kSampleU8 = 0, kSampleAlaw = 1, kSampleUlaw = 2, kSampleS16LE = 3, kSampleS16BE = 4, kSampleFloat32LE = 5,
              kSampleFloat32BE = 6, kSampleS32LE = 7, kSampleS32BE = 8, kSampleS24LE = 9, kSampleS24BE = 10,
              kSampleS24_32LE = 11, kSampleS24_32BE = 12, kSampleInvalid = -1;

/* pa_context_state_t */
constexpr int kContextUnconnected = 0, kContextConnecting = 1, kContextAuthorizing = 2, kContextSettingName = 3,
              kContextReady = 4, kContextFailed = 5, kContextTerminated = 6;
/* pa_stream_state_t */
constexpr int kStreamUnconnected = 0, kStreamCreating = 1, kStreamReady = 2, kStreamFailed = 3, kStreamTerminated = 4;
/* pa_operation_state_t */
constexpr int kOperationRunning = 0, kOperationDone = 1, kOperationCancelled = 2;
/* pa_context_flags_t */
constexpr int kContextNoFlags = 0, kContextNoAutospawn = 0x1, kContextNoFail = 0x2;
/* pa_stream_flags_t */
constexpr int kStreamNoFlags = 0, kStreamStartCorked = 0x1, kStreamInterpolateTiming = 0x2, kStreamNotMonotonic = 0x4,
              kStreamAutoTimingUpdate = 0x8, kStreamNoRemapChannels = 0x10, kStreamNoRemixChannels = 0x20,
              kStreamFixFormat = 0x40, kStreamFixRate = 0x80, kStreamFixChannels = 0x100, kStreamDontMove = 0x200,
              kStreamVariableRate = 0x400, kStreamPeakDetect = 0x800, kStreamStartMuted = 0x1000,
              kStreamAdjustLatency = 0x2000, kStreamEarlyRequests = 0x4000, kStreamDontInhibitAutoSuspend = 0x8000,
              kStreamStartUnmuted = 0x10000, kStreamFailOnSuspend = 0x20000, kStreamRelativeVolume = 0x40000,
              kStreamPassthrough = 0x80000;
/* pa_seek_mode_t */
constexpr int kSeekRelative = 0;
/* pa_subscription_mask_t */
constexpr int kMaskSink = 0x1, kMaskSource = 0x2, kMaskSinkInput = 0x4, kMaskSourceOutput = 0x8, kMaskServer = 0x80,
              kMaskCard = 0x200;
/* pa_subscription_event_type_t: facility in the low nibble, type in bits 4-5 */
constexpr int kEventSink = 0x0, kEventSource = 0x1, kEventSinkInput = 0x2, kEventSourceOutput = 0x3, kEventServer = 0x7,
              kEventCard = 0x9, kEventFacilityMask = 0xF;
constexpr int kEventNew = 0x00, kEventChange = 0x10, kEventRemove = 0x20, kEventTypeMask = 0x30;
/* pa_sink_state_t / pa_source_state_t (the same values) */
constexpr int kDeviceInvalidState = -1, kDeviceRunning = 0, kDeviceIdle = 1, kDeviceSuspended = 2;
/* pa_error_code_t */
constexpr int kOk = 0, kErrAccess = 1, kErrCommand = 2, kErrInvalid = 3, kErrExist = 4, kErrNoEntity = 5,
              kErrConnectionRefused = 6, kErrProtocol = 7, kErrTimeout = 8, kErrAuthKey = 9, kErrInternal = 10,
              kErrConnectionTerminated = 11, kErrKilled = 12, kErrInvalidServer = 13, kErrModInitFailed = 14,
              kErrBadState = 15, kErrNoData = 16, kErrVersion = 17, kErrTooLarge = 18, kErrNotSupported = 19,
              kErrUnknown = 20, kErrNoExtension = 21, kErrObsolete = 22, kErrNotImplemented = 23, kErrForked = 24,
              kErrIo = 25, kErrBusy = 26;

/* pa_sample_spec */
struct SampleSpec {
    int format; /* pa_sample_format_t */
    uint32_t rate;
    uint8_t channels;
};

/* pa_channel_map */
struct ChannelMap {
    uint8_t channels;
    int map[kChannelsMax]; /* pa_channel_position_t */
};

/* pa_cvolume */
struct CVolume {
    uint8_t channels;
    volume_t values[kChannelsMax];
};

/* pa_buffer_attr: every field in bytes; kBufferDefault lets the server choose */
struct BufferAttr {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
};

/* pa_timing_info */
struct TimingInfo {
    struct timeval timestamp;
    int synchronized_clocks;
    usec_t sink_usec;
    usec_t source_usec;
    usec_t transport_usec;
    int playing;
    int write_index_corrupt;
    int64_t write_index;
    int read_index_corrupt;
    int64_t read_index;
    usec_t configured_sink_usec;
    usec_t configured_source_usec;
    int64_t since_underrun;
};

/* pa_sink_info (read through the pointer the info callback gets) */
struct SinkInfo {
    const char *name;
    uint32_t index;
    const char *description;
    SampleSpec sample_spec;
    ChannelMap channel_map;
    uint32_t owner_module;
    CVolume volume;
    int mute;
    uint32_t monitor_source;
    const char *monitor_source_name;
    usec_t latency;
    const char *driver;
    int flags; /* pa_sink_flags_t */
    Proplist *proplist;
    usec_t configured_latency;
    volume_t base_volume;
    int state; /* pa_sink_state_t */
    uint32_t n_volume_steps;
    uint32_t card;
    uint32_t n_ports;
    SinkPortInfo **ports;
    SinkPortInfo *active_port;
    uint8_t n_formats;
    FormatInfo **formats;
};

/* pa_source_info */
struct SourceInfo {
    const char *name;
    uint32_t index;
    const char *description;
    SampleSpec sample_spec;
    ChannelMap channel_map;
    uint32_t owner_module;
    CVolume volume;
    int mute;
    uint32_t monitor_of_sink;
    const char *monitor_of_sink_name;
    usec_t latency;
    const char *driver;
    int flags; /* pa_source_flags_t */
    Proplist *proplist;
    usec_t configured_latency;
    volume_t base_volume;
    int state; /* pa_source_state_t */
    uint32_t n_volume_steps;
    uint32_t card;
    uint32_t n_ports;
    SourcePortInfo **ports;
    SourcePortInfo *active_port;
    uint8_t n_formats;
    FormatInfo **formats;
};

/* pa_server_info */
struct ServerInfo {
    const char *user_name;
    const char *host_name;
    const char *server_version;
    const char *server_name;
    SampleSpec sample_spec;
    const char *default_sink_name;
    const char *default_source_name;
    uint32_t cookie;
    ChannelMap channel_map;
};

/* pa_card_profile_info2 */
struct CardProfileInfo2 {
    const char *name;
    const char *description;
    uint32_t n_sinks;
    uint32_t n_sources;
    uint32_t priority;
    int available; /* 0: the server knows the profile cannot work now (since PulseAudio 5.0) */
};

/* pa_card_info */
struct CardInfo {
    uint32_t index;
    const char *name;
    uint32_t owner_module;
    const char *driver;
    uint32_t n_profiles;
    CardProfileInfo *profiles;       /* deprecated, superseded by profiles2 */
    CardProfileInfo *active_profile; /* deprecated, superseded by active_profile2 */
    Proplist *proplist;
    uint32_t n_ports;
    CardPortInfo **ports;
    CardProfileInfo2 **profiles2;    /* null-terminated array, or null */
    CardProfileInfo2 *active_profile2;
};

/* The first fields of pa_source_output_info / pa_sink_input_info: which device a stream is attached to, in the server's
 * own view (the identity guard reads `source` / `sink`). */
struct SourceOutputInfoHead {
    uint32_t index;
    const char *name;
    uint32_t owner_module;
    uint32_t client;
    uint32_t source;
};
struct SinkInputInfoHead {
    uint32_t index;
    const char *name;
    uint32_t owner_module;
    uint32_t client;
    uint32_t sink;
};

/* callbacks */
using ContextNotifyCb = void (*)(Context *, void *userdata);
using ContextSuccessCb = void (*)(Context *, int success, void *userdata);
using ContextSubscribeCb = void (*)(Context *, int event /* pa_subscription_event_type_t */, uint32_t idx, void *userdata);
using StreamNotifyCb = void (*)(Stream *, void *userdata);
using StreamRequestCb = void (*)(Stream *, size_t nbytes, void *userdata);
using StreamSuccessCb = void (*)(Stream *, int success, void *userdata);
using SinkInfoCb = void (*)(Context *, const SinkInfo *, int eol, void *userdata);
using SourceInfoCb = void (*)(Context *, const SourceInfo *, int eol, void *userdata);
using ServerInfoCb = void (*)(Context *, const ServerInfo *, void *userdata);
using CardInfoCb = void (*)(Context *, const CardInfo *, int eol, void *userdata);
using SourceOutputInfoCb = void (*)(Context *, const SourceOutputInfoHead *, int eol, void *userdata);
using SinkInputInfoCb = void (*)(Context *, const SinkInputInfoHead *, int eol, void *userdata);
using FreeCb = void (*)(void *);

}  // namespace pa

}  // namespace cassette
