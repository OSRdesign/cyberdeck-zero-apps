/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette sound server: the function table of libpulse. The production table is filled with dlopen/dlsym at the first use
 * (pulse_lib.cpp; no link-time dependency: the app starts, records and plays through ALSA without it, and the binary's
 * NEEDED list never names libpulse). Only real_pulse.cpp calls through it; the tests replace the whole PulseApi with a fake
 * (tests/pulse/fake_pulse.*), so the table itself has no fake.
 *
 * Library name: libpulse.so.0 (package libpulse0, present on every desktop image: PipeWire's pipewire-pulse speaks the
 * same protocol, so the one client library reaches PipeWire and PulseAudio alike).
 */
#pragma once

#include "pulse_abi.hpp"

#include <string>

namespace cassette {

struct PulseLib {
    std::string name;    /* the file that was loaded ("libpulse.so.0") */
    std::string version; /* pa_get_library_version() ("16.1.0"), empty when unknown */
    pa::ThreadedMainloop *(*threaded_mainloop_new)() = nullptr;
    void (*threaded_mainloop_free)(pa::ThreadedMainloop *) = nullptr;
    int (*threaded_mainloop_start)(pa::ThreadedMainloop *) = nullptr;
    void (*threaded_mainloop_stop)(pa::ThreadedMainloop *) = nullptr;
    void (*threaded_mainloop_lock)(pa::ThreadedMainloop *) = nullptr;
    void (*threaded_mainloop_unlock)(pa::ThreadedMainloop *) = nullptr;
    pa::MainloopApi *(*threaded_mainloop_get_api)(pa::ThreadedMainloop *) = nullptr;
    int (*threaded_mainloop_in_thread)(pa::ThreadedMainloop *) = nullptr;
    pa::Context *(*context_new_with_proplist)(pa::MainloopApi *, const char *name, const pa::Proplist *) = nullptr;
    void (*context_unref)(pa::Context *) = nullptr;
    int (*context_connect)(pa::Context *, const char *server, int flags, const pa::SpawnApi *) = nullptr;
    void (*context_disconnect)(pa::Context *) = nullptr;
    int (*context_get_state)(const pa::Context *) = nullptr;
    int (*context_errno)(const pa::Context *) = nullptr;
    void (*context_set_state_callback)(pa::Context *, pa::ContextNotifyCb, void *) = nullptr;
    void (*context_set_subscribe_callback)(pa::Context *, pa::ContextSubscribeCb, void *) = nullptr;
    pa::Operation *(*context_subscribe)(pa::Context *, int mask, pa::ContextSuccessCb, void *) = nullptr;
    pa::Operation *(*context_get_server_info)(pa::Context *, pa::ServerInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_source_info_list)(pa::Context *, pa::SourceInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_source_info_by_name)(pa::Context *, const char *, pa::SourceInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_source_info_by_index)(pa::Context *, uint32_t, pa::SourceInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_sink_info_list)(pa::Context *, pa::SinkInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_sink_info_by_name)(pa::Context *, const char *, pa::SinkInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_sink_info_by_index)(pa::Context *, uint32_t, pa::SinkInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_card_info_list)(pa::Context *, pa::CardInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_card_info_by_name)(pa::Context *, const char *, pa::CardInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_source_output_info)(pa::Context *, uint32_t, pa::SourceOutputInfoCb, void *) = nullptr;
    pa::Operation *(*context_get_sink_input_info)(pa::Context *, uint32_t, pa::SinkInputInfoCb, void *) = nullptr;
    void (*operation_unref)(pa::Operation *) = nullptr;
    int (*operation_get_state)(const pa::Operation *) = nullptr;
    void (*operation_cancel)(pa::Operation *) = nullptr;
    pa::Stream *(*stream_new_with_proplist)(pa::Context *, const char *name, const pa::SampleSpec *,
                                            const pa::ChannelMap *, pa::Proplist *) = nullptr;
    void (*stream_unref)(pa::Stream *) = nullptr;
    int (*stream_connect_record)(pa::Stream *, const char *dev, const pa::BufferAttr *, int flags) = nullptr;
    int (*stream_connect_playback)(pa::Stream *, const char *dev, const pa::BufferAttr *, int flags,
                                   const pa::CVolume *, pa::Stream *sync_stream) = nullptr;
    int (*stream_disconnect)(pa::Stream *) = nullptr;
    int (*stream_get_state)(const pa::Stream *) = nullptr;
    uint32_t (*stream_get_index)(const pa::Stream *) = nullptr;
    void (*stream_set_state_callback)(pa::Stream *, pa::StreamNotifyCb, void *) = nullptr;
    void (*stream_set_read_callback)(pa::Stream *, pa::StreamRequestCb, void *) = nullptr;
    void (*stream_set_write_callback)(pa::Stream *, pa::StreamRequestCb, void *) = nullptr;
    void (*stream_set_moved_callback)(pa::Stream *, pa::StreamNotifyCb, void *) = nullptr;
    void (*stream_set_suspended_callback)(pa::Stream *, pa::StreamNotifyCb, void *) = nullptr;
    void (*stream_set_underflow_callback)(pa::Stream *, pa::StreamNotifyCb, void *) = nullptr;
    int (*stream_peek)(pa::Stream *, const void **data, size_t *nbytes) = nullptr;
    int (*stream_drop)(pa::Stream *) = nullptr;
    size_t (*stream_readable_size)(const pa::Stream *) = nullptr;
    size_t (*stream_writable_size)(const pa::Stream *) = nullptr;
    int (*stream_write)(pa::Stream *, const void *data, size_t nbytes, pa::FreeCb, int64_t offset, int seek) = nullptr;
    const char *(*stream_get_device_name)(const pa::Stream *) = nullptr;
    uint32_t (*stream_get_device_index)(const pa::Stream *) = nullptr;
    int (*stream_is_suspended)(const pa::Stream *) = nullptr;
    int (*stream_get_latency)(pa::Stream *, pa::usec_t *usec, int *negative) = nullptr;
    const pa::TimingInfo *(*stream_get_timing_info)(pa::Stream *) = nullptr;
    const pa::BufferAttr *(*stream_get_buffer_attr)(pa::Stream *) = nullptr;
    const pa::SampleSpec *(*stream_get_sample_spec)(pa::Stream *) = nullptr;
    pa::Operation *(*stream_flush)(pa::Stream *, pa::StreamSuccessCb, void *) = nullptr;
    pa::Operation *(*stream_drain)(pa::Stream *, pa::StreamSuccessCb, void *) = nullptr;
    pa::Proplist *(*proplist_new)() = nullptr;
    void (*proplist_free)(pa::Proplist *) = nullptr;
    int (*proplist_sets)(pa::Proplist *, const char *key, const char *value) = nullptr;
    const char *(*proplist_gets)(const pa::Proplist *, const char *key) = nullptr;
    const char *(*strerror)(int) = nullptr;
    const char *(*get_library_version)() = nullptr;
    bool complete() const; /* every function is set */
};

/* Loads `file` (dlopen RTLD_NOW | RTLD_LOCAL) and fills `out`; false with `why` ("libpulse not installed (libpulse0):
 * ...", "libpulse.so.0 lacks pa_..."), the handle closed again. Not cached: real_pulse_lib() is the cached production
 * entry; the tests call this with a name that does not exist to see the missing-library path. */
bool load_pulse_lib(const char *file, PulseLib &out, std::string &why);

/* The production table: libpulse.so.0 loaded on the first call, then cached for the life of the process (never unloaded).
 * Null with `why` filled when the library is missing or lacks a symbol. Thread-safe. */
const PulseLib *real_pulse_lib(std::string *why = nullptr);

}  // namespace cassette
