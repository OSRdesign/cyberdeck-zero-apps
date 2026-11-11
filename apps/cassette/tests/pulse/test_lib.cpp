/*
 * SPDX-License-Identifier: MIT
 *
 * The library and the connection: libpulse missing, no server, a server that hangs, the error texts and the mapping of
 * libpulse's error codes; the buffer maths from milliseconds; the open parameters every implementation shares.
 */
#include "check.hpp"
#include "fake_pulse.hpp"
#include "pulse_abi.hpp"
#include "pulse_lib.hpp"

#include <cstdlib>

using namespace cassette;

namespace {

void test_missing_library() {
    PulseLib lib;
    std::string why;
    CHECK(!load_pulse_lib("libpulse-cassette-test-absent.so.0", lib, why));
    CHECK(why.find("libpulse not installed (libpulse0)") == 0);
    CHECK(why.find("libpulse-cassette-test-absent.so.0") != std::string::npos);
    CHECK(lib.name.empty());
    CHECK(!lib.complete());
    CHECK_EQ(pulse_error_text(kPulseMissing), std::string("libpulse not installed (libpulse0)"));

    FakePulse f;
    f.lib_missing = true;
    CHECK_EQ(f.connect("Cassette", 1000), kPulseMissing);
    CHECK(!f.connected());
    CHECK(f.last_error().find("libpulse not installed") == 0);
    std::vector<PulseDevice> v;
    CHECK_EQ(f.list_sources(v), kPulseState);
    std::unique_ptr<PulseCapture> cap;
    PulseCaptureRequest rq;
    rq.source = "bluez_input.70_BF_92_53_6B_8B.0";
    CHECK_EQ(f.open_capture(rq, cap, nullptr), kPulseState);
    CHECK(!cap);
}

void test_server_down() {
    FakePulse f;
    f.server_down = true;
    CHECK_EQ(f.connect("Cassette", 1000), kPulseNoServer);
    CHECK(!f.connected());
    CHECK_EQ(pulse_error_text(kPulseNoServer), std::string("No sound server is running (PipeWire or PulseAudio)"));
    f.server_down = false;
    f.server_hangs = true;
    CHECK_EQ(f.connect("Cassette", 100), kPulseTimeout);
    f.server_hangs = false;
    CHECK_EQ(f.connect("Cassette", 100), 0);
    CHECK(f.connected());
    CHECK_EQ(f.connect("Cassette", 100), 0); /* again: still fine */
    f.disconnect();
    CHECK(!f.connected());
    f.disconnect(); /* idempotent */
}

void test_error_mapping() {
    CHECK_EQ(pulse_error_from_pa(pa::kOk), 0);
    CHECK_EQ(pulse_error_from_pa(pa::kErrNoEntity), kPulseNoDevice);
    CHECK_EQ(pulse_error_from_pa(pa::kErrConnectionRefused), kPulseNoServer);
    CHECK_EQ(pulse_error_from_pa(pa::kErrInvalidServer), kPulseNoServer);
    CHECK_EQ(pulse_error_from_pa(pa::kErrKilled), kPulseKilled);
    CHECK_EQ(pulse_error_from_pa(pa::kErrConnectionTerminated), kPulseDisconnected);
    CHECK_EQ(pulse_error_from_pa(pa::kErrTimeout), kPulseTimeout);
    CHECK_EQ(pulse_error_from_pa(pa::kErrInvalid), kPulseRefused);
    CHECK_EQ(pulse_error_from_pa(pa::kErrNotSupported), kPulseRefused);
    CHECK_EQ(pulse_error_from_pa(pa::kErrBadState), kPulseState);
    CHECK_EQ(pulse_error_from_pa(pa::kErrAccess), kPulseFailed);
    CHECK_EQ(pulse_error_from_pa(pa::kErrBusy), kPulseFailed);
    const int all[] = {kPulseMissing, kPulseNoServer, kPulseNoDevice, kPulseKilled, kPulseMoved, kPulseDisconnected,
                       kPulseTimeout, kPulseFailed,  kPulseState,    kPulseRefused};
    for (int e : all) CHECK(!pulse_error_text(e).empty() && pulse_error_text(e).find("error -") == std::string::npos);
    CHECK_EQ(pulse_error_text(0), std::string("no error"));
    CHECK(pulse_error_text(-5).find("-5") != std::string::npos);
    CHECK_EQ(std::string(pulse_event_name(PulseEvent::SilenceDetected)), std::string("SilenceDetected"));
    CHECK_EQ(std::string(pulse_state_name(PulseStreamState::Connecting)), std::string("connecting"));
    CHECK_EQ(std::string(pulse_format_name(pa::kSampleS16LE)), std::string("s16le"));
    CHECK_EQ(std::string(pulse_format_name(pa::kSampleFloat32LE)), std::string("float32le"));
    CHECK_EQ(std::string(pulse_format_name(99)), std::string("invalid"));
}

void test_buffer_maths() {
    /* bytes = whole frames (rounded up) x 2 bytes x channels */
    CHECK_EQ(pulse_ms_to_bytes(1, 8000, 1), 16u);
    CHECK_EQ(pulse_ms_to_bytes(20, 8000, 1), 320u);
    CHECK_EQ(pulse_ms_to_bytes(20, 16000, 1), 640u);
    CHECK_EQ(pulse_ms_to_bytes(20, 48000, 2), 3840u);
    CHECK_EQ(pulse_ms_to_bytes(10, 44100, 2), 1764u);     /* 441 frames */
    CHECK_EQ(pulse_ms_to_bytes(1, 44100, 1), 90u);        /* 44.1 -> 45 frames (up) */
    CHECK_EQ(pulse_ms_to_bytes(2000, 16000, 1), 64000u);
    CHECK_EQ(pulse_ms_to_bytes(0, 48000, 2), 0u);
    CHECK_EQ(pulse_ms_to_bytes(20, 48000, 0), 1920u);     /* 0 channels counts as mono */
    CHECK_EQ(pulse_ms_to_bytes(4000000000u, 384000, 2), 0xFFFFFFF0u); /* clamped */
    CHECK_EQ(pulse_bytes_to_frames(640, 1), 320u);
    CHECK_EQ(pulse_bytes_to_frames(3840, 2), 960u);
    CHECK_EQ(pulse_bytes_to_frames(3, 1), 1u);

    PulseCaptureRequest c;
    c.source = "x";
    c.fragment_ms = 20;
    c.server_max_ms = 2000;
    PulseBufferAttr a = pulse_capture_attr(c, 16000, 1);
    CHECK_EQ(a.fragsize, 640u);
    CHECK_EQ(a.maxlength, 64000u);
    CHECK_EQ(a.tlength, kPulseDefault);
    CHECK_EQ(a.prebuf, kPulseDefault);
    CHECK_EQ(a.minreq, kPulseDefault);
    c.fragment_ms = 0; /* at least 1 ms */
    c.server_max_ms = 0;
    a = pulse_capture_attr(c, 8000, 1);
    CHECK_EQ(a.fragsize, 16u);  /* a fragment is at least 1 ms */
    CHECK_EQ(a.maxlength, 32u); /* and the server buffer at least two fragments */
    c.fragment_ms = 50;
    c.server_max_ms = 60; /* raised to two fragments */
    a = pulse_capture_attr(c, 8000, 1);
    CHECK_EQ(a.maxlength, pulse_ms_to_bytes(100, 8000, 1));

    PulsePlaybackRequest p;
    p.sink = "y";
    p.buffer_ms = 30;
    a = pulse_playback_attr(p, 48000, 2);
    CHECK_EQ(a.tlength, 5760u);
    CHECK_EQ(a.maxlength, kPulseDefault);
    CHECK_EQ(a.prebuf, kPulseDefault);
    CHECK_EQ(a.minreq, kPulseDefault);
    CHECK_EQ(a.fragsize, kPulseDefault);
}

void test_open_parameters() {
    PulseCaptureRequest c;
    c.source = "bluez_input.70_BF_92_53_6B_8B.0";
    const uint32_t f = pulse_capture_flags(c);
    CHECK((f & pa::kStreamDontMove) != 0); /* always: the server ends the stream instead of moving it (P5) */
    CHECK((f & pa::kStreamAdjustLatency) != 0);
    CHECK((f & pa::kStreamInterpolateTiming) != 0);
    CHECK((f & pa::kStreamAutoTimingUpdate) != 0);
    CHECK((f & pa::kStreamFailOnSuspend) == 0);
    CHECK((f & pa::kStreamFixRate) == 0); /* the rate is asked for explicitly, never fixed to the device's */
    c.fail_on_suspend = true;
    CHECK((pulse_capture_flags(c) & pa::kStreamFailOnSuspend) != 0);
    CHECK((pulse_capture_flags(c) & pa::kStreamDontMove) != 0);
    PulsePlaybackRequest p;
    p.sink = "bluez_output.70_BF_92_53_6B_8B.1";
    CHECK((pulse_playback_flags(p) & pa::kStreamDontMove) != 0);
    CHECK((pulse_playback_flags(p) & pa::kStreamFailOnSuspend) == 0);
    p.fail_on_suspend = true;
    CHECK((pulse_playback_flags(p) & pa::kStreamFailOnSuspend) != 0);

    auto props = pulse_stream_props("Cassette recording", "production", "Cassette");
    auto has = [&](const char *k, const char *v) {
        for (const auto &e : props)
            if (e.first == k && e.second == v) return true;
        return false;
    };
    CHECK(has("node.dont-reconnect", "true"));
    CHECK(has("node.dont-fallback", "true"));
    CHECK(has("node.dont-move", "true"));
    CHECK(has("media.name", "Cassette recording"));
    CHECK(has("media.role", "production"));
    CHECK(has("application.name", "Cassette"));
    props = pulse_stream_props("x", "", "");
    bool role = false;
    for (const auto &e : props) role |= e.first == "media.role";
    CHECK(!role);
    CHECK(has("application.name", "Cassette"));
}

}  // namespace

void test_lib() {
    test_missing_library();
    test_server_down();
    test_error_mapping();
    test_buffer_maths();
    test_open_parameters();
}

/* The real library, when it loads (else skipped): every symbol resolves, and a server that is not there gives
 * kPulseNoServer (PULSE_SERVER pointed at a socket that does not exist; no daemon is ever spawned). */
void test_real_lib() {
    std::string why;
    const PulseLib *lib = real_pulse_lib(&why);
    if (!lib) {
        std::printf("SKIP real libpulse: %s\n", why.c_str());
        ++g_skip;
        CHECK(why.find("libpulse not installed (libpulse0)") == 0);
        return;
    }
    CHECK(lib->complete());
    CHECK_EQ(lib->name, std::string("libpulse.so.0"));
    CHECK(!lib->version.empty());
    const char *old = std::getenv("PULSE_SERVER");
    const std::string saved = old ? old : "";
    ::setenv("PULSE_SERVER", "unix:/nonexistent/cassette-test/native", 1);
    std::unique_ptr<PulseApi> api = make_real_pulse();
    const int r = api->connect("Cassette test", 3000);
    CHECK_EQ(r, kPulseNoServer);
    CHECK(!api->connected());
    CHECK(!api->last_error().empty());
    std::vector<PulseDevice> v;
    CHECK_EQ(api->list_sources(v), kPulseState);
    api->disconnect();
    if (old) ::setenv("PULSE_SERVER", saved.c_str(), 1);
    else ::unsetenv("PULSE_SERVER");
    std::printf("real libpulse %s: no server -> \"%s\"\n", lib->version.c_str(), pulse_error_text(r).c_str());
}
