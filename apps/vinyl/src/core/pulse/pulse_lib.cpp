/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * The production libpulse table: dlopen + dlsym at the first use, as codec_libs.cpp loads the codecs (RTLD_NOW |
 * RTLD_LOCAL, every symbol checked, a readable reason when something is missing). The handle is never closed.
 */
#include "pulse_lib.hpp"

#include <dlfcn.h>
#include <mutex>

namespace vinyl {

bool PulseLib::complete() const {
    return threaded_mainloop_new && threaded_mainloop_free && threaded_mainloop_start && threaded_mainloop_stop &&
           threaded_mainloop_lock && threaded_mainloop_unlock && threaded_mainloop_get_api &&
           threaded_mainloop_in_thread && context_new_with_proplist && context_unref && context_connect &&
           context_disconnect && context_get_state && context_errno && context_set_state_callback &&
           context_set_subscribe_callback && context_subscribe && context_get_server_info &&
           context_get_source_info_list && context_get_source_info_by_name && context_get_source_info_by_index &&
           context_get_sink_info_list && context_get_sink_info_by_name && context_get_sink_info_by_index &&
           context_get_card_info_list && context_get_card_info_by_name && context_get_source_output_info &&
           context_get_sink_input_info && operation_unref && operation_get_state && operation_cancel &&
           stream_new_with_proplist && stream_unref && stream_connect_record && stream_connect_playback &&
           stream_disconnect && stream_get_state && stream_get_index && stream_set_state_callback &&
           stream_set_read_callback && stream_set_write_callback && stream_set_moved_callback &&
           stream_set_suspended_callback && stream_set_underflow_callback && stream_peek && stream_drop &&
           stream_readable_size && stream_writable_size && stream_write && stream_get_device_name &&
           stream_get_device_index && stream_is_suspended && stream_get_latency && stream_get_timing_info &&
           stream_get_buffer_attr && stream_get_sample_spec && stream_flush && stream_drain && proplist_new &&
           proplist_free && proplist_sets && proplist_gets && strerror && get_library_version;
}

namespace {

template <typename Fn> bool sym(void *h, Fn &fn, const char *name, const std::string &lib, std::string &why) {
    void *p = ::dlsym(h, name);
    fn = reinterpret_cast<Fn>(p);
    if (!p && why.empty()) why = lib + " lacks " + name;
    return p != nullptr;
}

struct Holder {
    std::once_flag once;
    PulseLib lib;
    bool ok = false;
    std::string why;
};

Holder &holder() {
    static Holder h;
    return h;
}

}  // namespace

bool load_pulse_lib(const char *file, PulseLib &out, std::string &why) {
    out = PulseLib();
    why.clear();
    void *h = ::dlopen(file, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = ::dlerror();
        why = std::string("libpulse not installed (libpulse0): ") + file + ": " + (e ? e : "dlopen failed");
        return false;
    }
    PulseLib &l = out;
    l.name = file;
    bool all = true;
    all &= sym(h, l.threaded_mainloop_new, "pa_threaded_mainloop_new", l.name, why);
    all &= sym(h, l.threaded_mainloop_free, "pa_threaded_mainloop_free", l.name, why);
    all &= sym(h, l.threaded_mainloop_start, "pa_threaded_mainloop_start", l.name, why);
    all &= sym(h, l.threaded_mainloop_stop, "pa_threaded_mainloop_stop", l.name, why);
    all &= sym(h, l.threaded_mainloop_lock, "pa_threaded_mainloop_lock", l.name, why);
    all &= sym(h, l.threaded_mainloop_unlock, "pa_threaded_mainloop_unlock", l.name, why);
    all &= sym(h, l.threaded_mainloop_get_api, "pa_threaded_mainloop_get_api", l.name, why);
    all &= sym(h, l.threaded_mainloop_in_thread, "pa_threaded_mainloop_in_thread", l.name, why);
    all &= sym(h, l.context_new_with_proplist, "pa_context_new_with_proplist", l.name, why);
    all &= sym(h, l.context_unref, "pa_context_unref", l.name, why);
    all &= sym(h, l.context_connect, "pa_context_connect", l.name, why);
    all &= sym(h, l.context_disconnect, "pa_context_disconnect", l.name, why);
    all &= sym(h, l.context_get_state, "pa_context_get_state", l.name, why);
    all &= sym(h, l.context_errno, "pa_context_errno", l.name, why);
    all &= sym(h, l.context_set_state_callback, "pa_context_set_state_callback", l.name, why);
    all &= sym(h, l.context_set_subscribe_callback, "pa_context_set_subscribe_callback", l.name, why);
    all &= sym(h, l.context_subscribe, "pa_context_subscribe", l.name, why);
    all &= sym(h, l.context_get_server_info, "pa_context_get_server_info", l.name, why);
    all &= sym(h, l.context_get_source_info_list, "pa_context_get_source_info_list", l.name, why);
    all &= sym(h, l.context_get_source_info_by_name, "pa_context_get_source_info_by_name", l.name, why);
    all &= sym(h, l.context_get_source_info_by_index, "pa_context_get_source_info_by_index", l.name, why);
    all &= sym(h, l.context_get_sink_info_list, "pa_context_get_sink_info_list", l.name, why);
    all &= sym(h, l.context_get_sink_info_by_name, "pa_context_get_sink_info_by_name", l.name, why);
    all &= sym(h, l.context_get_sink_info_by_index, "pa_context_get_sink_info_by_index", l.name, why);
    all &= sym(h, l.context_get_card_info_list, "pa_context_get_card_info_list", l.name, why);
    all &= sym(h, l.context_get_card_info_by_name, "pa_context_get_card_info_by_name", l.name, why);
    all &= sym(h, l.context_get_source_output_info, "pa_context_get_source_output_info", l.name, why);
    all &= sym(h, l.context_get_sink_input_info, "pa_context_get_sink_input_info", l.name, why);
    all &= sym(h, l.operation_unref, "pa_operation_unref", l.name, why);
    all &= sym(h, l.operation_get_state, "pa_operation_get_state", l.name, why);
    all &= sym(h, l.operation_cancel, "pa_operation_cancel", l.name, why);
    all &= sym(h, l.stream_new_with_proplist, "pa_stream_new_with_proplist", l.name, why);
    all &= sym(h, l.stream_unref, "pa_stream_unref", l.name, why);
    all &= sym(h, l.stream_connect_record, "pa_stream_connect_record", l.name, why);
    all &= sym(h, l.stream_connect_playback, "pa_stream_connect_playback", l.name, why);
    all &= sym(h, l.stream_disconnect, "pa_stream_disconnect", l.name, why);
    all &= sym(h, l.stream_get_state, "pa_stream_get_state", l.name, why);
    all &= sym(h, l.stream_get_index, "pa_stream_get_index", l.name, why);
    all &= sym(h, l.stream_set_state_callback, "pa_stream_set_state_callback", l.name, why);
    all &= sym(h, l.stream_set_read_callback, "pa_stream_set_read_callback", l.name, why);
    all &= sym(h, l.stream_set_write_callback, "pa_stream_set_write_callback", l.name, why);
    all &= sym(h, l.stream_set_moved_callback, "pa_stream_set_moved_callback", l.name, why);
    all &= sym(h, l.stream_set_suspended_callback, "pa_stream_set_suspended_callback", l.name, why);
    all &= sym(h, l.stream_set_underflow_callback, "pa_stream_set_underflow_callback", l.name, why);
    all &= sym(h, l.stream_peek, "pa_stream_peek", l.name, why);
    all &= sym(h, l.stream_drop, "pa_stream_drop", l.name, why);
    all &= sym(h, l.stream_readable_size, "pa_stream_readable_size", l.name, why);
    all &= sym(h, l.stream_writable_size, "pa_stream_writable_size", l.name, why);
    all &= sym(h, l.stream_write, "pa_stream_write", l.name, why);
    all &= sym(h, l.stream_get_device_name, "pa_stream_get_device_name", l.name, why);
    all &= sym(h, l.stream_get_device_index, "pa_stream_get_device_index", l.name, why);
    all &= sym(h, l.stream_is_suspended, "pa_stream_is_suspended", l.name, why);
    all &= sym(h, l.stream_get_latency, "pa_stream_get_latency", l.name, why);
    all &= sym(h, l.stream_get_timing_info, "pa_stream_get_timing_info", l.name, why);
    all &= sym(h, l.stream_get_buffer_attr, "pa_stream_get_buffer_attr", l.name, why);
    all &= sym(h, l.stream_get_sample_spec, "pa_stream_get_sample_spec", l.name, why);
    all &= sym(h, l.stream_flush, "pa_stream_flush", l.name, why);
    all &= sym(h, l.stream_drain, "pa_stream_drain", l.name, why);
    all &= sym(h, l.proplist_new, "pa_proplist_new", l.name, why);
    all &= sym(h, l.proplist_free, "pa_proplist_free", l.name, why);
    all &= sym(h, l.proplist_sets, "pa_proplist_sets", l.name, why);
    all &= sym(h, l.proplist_gets, "pa_proplist_gets", l.name, why);
    all &= sym(h, l.strerror, "pa_strerror", l.name, why);
    all &= sym(h, l.get_library_version, "pa_get_library_version", l.name, why);
    if (!all) {
        ::dlclose(h);
        out = PulseLib();
        return false;
    }
    if (const char *v = l.get_library_version()) l.version = v;
    return true;
}

const PulseLib *real_pulse_lib(std::string *why) {
    Holder &h = holder();
    std::call_once(h.once, [&h] { h.ok = load_pulse_lib("libpulse.so.0", h.lib, h.why); });
    if (!h.ok && why) *why = h.why;
    return h.ok ? &h.lib : nullptr;
}

}  // namespace vinyl
