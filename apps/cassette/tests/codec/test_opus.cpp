/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: OpusEncoder on the fake libopusenc (the settings it asks for, pages reaching the sink, the
 * decision delay and flush(), the end-of-stream page, errors).
 */
#include "check.hpp"

#include "fake_codecs.hpp"
#include "opus_encoder.hpp"
#include "siggen.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <string>

using namespace cassette;

namespace {

void test_settings() {
    OpusEncLib lib = fake::make_opus_lib();
    OpusConfig cfg;
    cfg.fmt = AudioFormat{48000, 2, 16};
    cfg.bitrate_bps = 24000;
    cfg.application = OpusApplication::Speech;
    cfg.force_mono = true;
    cfg.serialno = 77;
    MemoryByteSink sink;
    {
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {{"TITLE", "Interview"}, {"TAKE", "12"}}), 0);
        fake::OpusState &s = fake::opus_state();
        CHECK_EQ(s.rate, 48000);
        CHECK_EQ(s.channels, 2);
        CHECK_EQ(s.family, 0);
        CHECK_EQ(s.ctl[ope::kSetApplication], ope::kApplicationVoip);
        CHECK_EQ(s.ctl[ope::kSetSignal], ope::kSignalVoice);
        CHECK_EQ(s.ctl[ope::kSetBitrate], 24000);
        CHECK_EQ(s.ctl[ope::kSetVbr], 1);
        CHECK_EQ(s.ctl[ope::kSetComplexity], 10);
        CHECK_EQ(s.ctl[ope::kSetExpertFrameDuration], ope::kFramesize20Ms);
        CHECK_EQ(s.ctl[ope::kSetDecisionDelay], 96000);
        CHECK_EQ(s.ctl[ope::kSetMuxingDelay], 48000);
        CHECK_EQ(s.ctl[ope::kSetCommentPadding], 512);
        CHECK_EQ(s.ctl[ope::kSetForceChannels], 1);
        CHECK_EQ(s.ctl[ope::kSetSerialno], 77);
        CHECK_EQ(s.comments.size(), size_t(2));
        if (s.comments.size() == 2) CHECK_EQ(s.comments[1], std::string("TAKE=12"));
        /* both header pages are in the sink at once */
        CHECK_EQ(e.stats().units_out, uint64_t(2));
        CHECK_EQ(e.stats().bytes_out, sink.size());
        CHECK_EQ(e.preskip(), 312u);
        CHECK_EQ(e.stats().frames_out, uint64_t(0));
    }
    CHECK_EQ(fake::opus_state().destroys, 1);

    /* music, mono card: no forced channel count, AUDIO mode */
    lib = fake::make_opus_lib();
    cfg.fmt = AudioFormat{48000, 1, 16};
    cfg.application = OpusApplication::Music;
    cfg.bitrate_bps = 128000;
    cfg.serialno = -1;
    MemoryByteSink s2;
    OpusEncoder m(&lib, cfg);
    CHECK_EQ(m.start(s2, {}), 0);
    CHECK_EQ(fake::opus_state().ctl[ope::kSetApplication], ope::kApplicationAudio);
    CHECK_EQ(fake::opus_state().ctl[ope::kSetSignal], ope::kSignalMusic);
    CHECK(!fake::opus_state().ctl.count(ope::kSetForceChannels));
    CHECK(!fake::opus_state().ctl.count(ope::kSetSerialno));
}

void test_delay_flush_finish() {
    OpusEncLib lib = fake::make_opus_lib();
    OpusConfig cfg;
    cfg.fmt = AudioFormat{48000, 1, 16};
    MemoryByteSink sink;
    OpusEncoder e(&lib, cfg);
    CHECK_EQ(e.start(sink, {}), 0);
    const sig::Bytes pcm = sig::ramp(cfg.fmt, 48000 * 5); /* 5 s */
    for (size_t at = 0; at < 48000 * 5; at += 1024) {
        const size_t n = std::min<size_t>(1024, 48000 * 5 - at);
        CHECK_EQ(e.write(pcm.data() + at * 2, n), 0);
    }
    const fake::OpusState &s = fake::opus_state();
    CHECK_EQ(s.s16.size(), size_t(48000 * 5));
    bool same = s.s16.size() == 48000 * 5;
    for (size_t i = 0; same && i < s.s16.size(); ++i) same = s.s16[i] == sig::get(pcm, i, 16);
    CHECK(same);
    const EncoderStats before = e.stats();
    CHECK_EQ(before.frames_in, uint64_t(240000));
    /* the 2 s decision delay: at most 3 s encoded, and whole pages only (the muxing delay of 1 s) */
    CHECK(before.frames_out <= 3u * 48000u);
    CHECK_EQ(e.flush(), 0);
    const EncoderStats after = e.stats();
    CHECK(after.frames_out >= 240000u - 96000u - 960u); /* everything encoded is now in the sink */
    CHECK(after.frames_out <= 240000u - 96000u);
    CHECK(after.pending_frames() <= 96000u + 960u);
    CHECK(!e.eos_written());
    CHECK_EQ(e.finish(), 0);
    CHECK(e.eos_written());
    CHECK_EQ(e.stats().frames_out, uint64_t(240000));
    CHECK_EQ(e.last_granule(), uint64_t(240000 + 312));
    CHECK_EQ(e.stats().bytes_out, sink.size());
    CHECK_EQ(e.finish(), 0);
    CHECK_EQ(e.flush(), 0);
    CHECK_EQ(e.write(pcm.data(), 10), kCodecState);
    CHECK_EQ(s.drains, 1);
}

void test_no_decision_delay_16k_24bit() {
    OpusEncLib lib = fake::make_opus_lib();
    OpusConfig cfg;
    cfg.fmt = AudioFormat{16000, 1, 24}; /* a 16 kHz speech headset, a 24-bit path */
    cfg.decision_delay_ms = 0;
    cfg.muxing_delay_ms = 200;
    MemoryByteSink sink;
    OpusEncoder e(&lib, cfg);
    CHECK_EQ(e.start(sink, {}), 0);
    CHECK_EQ(fake::opus_state().ctl[ope::kSetDecisionDelay], 0);
    CHECK_EQ(fake::opus_state().ctl[ope::kSetMuxingDelay], 9600);
    const sig::Bytes pcm = sig::ramp(cfg.fmt, 16000 * 3);
    CHECK_EQ(e.write(pcm.data(), 16000 * 3), 0);
    const fake::OpusState &s = fake::opus_state();
    CHECK_EQ(s.f32.size(), size_t(48000)); /* 24-bit goes in as float */
    CHECK(s.s16.empty());
    bool close = s.f32.size() == 48000;
    for (size_t i = 0; close && i < s.f32.size(); ++i) close = std::fabs(s.f32[i] - sig::get(pcm, i, 24) / 8388608.0f) < 1e-6f;
    CHECK(close);
    CHECK_EQ(e.flush(), 0);
    /* frames_out at the input rate: within one 20 ms frame (320 frames at 16 kHz) of the input */
    CHECK(e.stats().frames_out >= 48000u - 320u);
    CHECK_EQ(e.finish(), 0);
    CHECK_EQ(e.stats().frames_out, uint64_t(48000));
    CHECK_EQ(e.last_granule(), uint64_t(48000 * 3 + 312)); /* 48 kHz granules */
}

void test_refusals_and_errors() {
    OpusConfig cfg;
    cfg.fmt = AudioFormat{48000, 1, 16};
    MemoryByteSink sink;
    {
        OpusEncoder e(nullptr, cfg);
        CHECK_EQ(e.start(sink, {}), kCodecMissing);
    }
    const AudioFormat bad[] = {{44100, 1, 16}, {32000, 2, 16}, {96000, 1, 16}, {48000, 3, 16}, {48000, 1, 32}};
    for (const AudioFormat &f : bad) {
        OpusEncLib lib = fake::make_opus_lib();
        OpusConfig c;
        c.fmt = f;
        OpusEncoder e(&lib, c);
        CHECK_EQ(e.start(sink, {}), kCodecRefused);
        CHECK_EQ(fake::opus_state().creates, 0);
    }
    std::string why;
    CHECK(!opus_format_ok(AudioFormat{44100, 2, 16}, &why));
    CHECK_EQ(why, std::string("Opus needs 8, 12, 16, 24 or 48 kHz"));
    CHECK(opus_rate_ok(8000) && opus_rate_ok(12000) && opus_rate_ok(16000) && opus_rate_ok(24000) && opus_rate_ok(48000));
    CHECK(!opus_rate_ok(22050) && !opus_rate_ok(44100) && !opus_rate_ok(96000));
    {
        OpusEncLib lib = fake::make_opus_lib();
        fake::opus_state().fail_ctl_request = ope::kSetBitrate;
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), kCodecRefused);
    }
    {
        OpusEncLib lib = fake::make_opus_lib();
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {{"A=B", "x"}}), kCodecRefused);
    }
    {
        /* the card fills up: ENOSPC from the sink, sticky */
        OpusEncLib lib = fake::make_opus_lib();
        MemoryByteSink s;
        OpusConfig c = cfg;
        c.decision_delay_ms = 0;
        OpusEncoder e(&lib, c);
        CHECK_EQ(e.start(s, {}), 0);
        s.fail_at = s.size() + 100;
        const sig::Bytes pcm = sig::ramp(c.fmt, 48000 * 3);
        CHECK_EQ(e.write(pcm.data(), 48000 * 3), -ENOSPC);
        CHECK_EQ(e.flush(), -ENOSPC);
        CHECK_EQ(e.finish(), -ENOSPC);
    }
    {
        OpusEncLib lib = fake::make_opus_lib();
        fake::opus_state().fail_write_at = 0;
        MemoryByteSink s;
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(s, {}), 0);
        const sig::Bytes pcm = sig::ramp(cfg.fmt, 100);
        CHECK_EQ(e.write(pcm.data(), 100), kCodecFailed);
    }
    {
        OpusEncLib lib = fake::make_opus_lib();
        MemoryByteSink s;
        s.fail_at = 20; /* the header page does not fit */
        OpusEncoder e(&lib, cfg);
        CHECK_EQ(e.start(s, {}), -ENOSPC);
    }
}

}  // namespace

void test_opus() {
    test_settings();
    test_delay_flush_finish();
    test_no_decision_delay_16k_24bit();
    test_refusals_and_errors();
}
