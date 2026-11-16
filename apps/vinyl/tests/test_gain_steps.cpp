/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the software gain boost with its look-ahead limiter and the "Volume steps" setting (report 060 3e and
 * 3f, owner requests of 2026-11-13; decisions D23 and D24):
 *   - the limiter: no sample above the -1 dBFS ceiling after a +4 dB boost, whatever the signal; below the ceiling the
 *     signal is only delayed (one frame out per frame in); the delay, the flush, the release, the reset;
 *   - the bypass at 0 dB: convert_pcm (today's integer path) is bit-exact at unity gain, and the limited path is used only
 *     when the net gain is above 0 dB;
 *   - boost with ReplayGain and the volume: the gains multiply; the peak guard and the limiter together;
 *   - Volume steps: the model on its grid, clamped to the output's own step, the nearest dB kept when the step changes,
 *     the dB text; the sink's continuous volume;
 *   - hold to repeat with a fake clock.
 * (The engine's end to end boost tests are in test_replaygain.cpp, the settings file's in test_settings_state.cpp, the
 * pages' in test_menu.cpp.)
 */
#include "check.hpp"

#include "dsp/limiter.hpp"
#include "dsp/replaygain.hpp"
#include "engine/engine.hpp"
#include "volume/hold_repeat.hpp"
#include "volume/output_volume.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vinyl;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* the input of a test: stereo float frames */
std::vector<float> square(size_t frames, float amp, size_t half = 50) {
    std::vector<float> v(frames * 2);
    for (size_t i = 0; i < frames; ++i) v[2 * i] = v[2 * i + 1] = ((i / half) % 2) ? -amp : amp;
    return v;
}
std::vector<float> sine(size_t frames, float amp, double hz, double rate = 44100) {
    std::vector<float> v(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        v[2 * i] = amp * static_cast<float>(std::sin(2 * kPi * hz * static_cast<double>(i) / rate));
        v[2 * i + 1] = amp * static_cast<float>(std::sin(2 * kPi * hz * 1.37 * static_cast<double>(i) / rate + 0.5));
    }
    return v;
}
float peak_of(const std::vector<float> &v, size_t from = 0) {
    float p = 0;
    for (size_t i = from * 2; i < v.size(); ++i) p = std::max(p, std::fabs(v[i]));
    return p;
}
/* feeds `in` in blocks of 1024 (the engine's period) through the limiter and appends the tail */
std::vector<float> through(Limiter &lim, const std::vector<float> &in) {
    std::vector<float> out;
    for (size_t at = 0; at < in.size() / 2; at += 1024) {
        const size_t n = std::min<size_t>(1024, in.size() / 2 - at);
        std::vector<float> blk(in.begin() + static_cast<long>(at * 2), in.begin() + static_cast<long>((at + n) * 2));
        lim.process(blk.data(), n);
        out.insert(out.end(), blk.begin(), blk.end());
    }
    std::vector<float> tail(2 * lim.delay());
    lim.flush(tail.data());
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

void test_limiter() {
    const float ceil = static_cast<float>(std::pow(10.0, kLimiterCeilingDb / 20.0));
    CHECK_NEAR(ceil, 0.8913, 1e-4);
    Limiter lim(44100);
    CHECK_EQ(lim.delay(), static_cast<size_t>(221)); /* 5 ms at 44.1 kHz: 220.5 rounded */
    CHECK_EQ(Limiter(48000).delay(), static_cast<size_t>(240));
    CHECK_EQ(Limiter(96000).delay(), static_cast<size_t>(480));

    /* a full-scale square wave +4 dB (peak 1.585): never above the ceiling, whatever the block boundaries */
    {
        const std::vector<float> in = square(44100, 0.999f * std::pow(10.0f, 4.0f / 20.0f));
        const std::vector<float> out = through(lim, in);
        CHECK_EQ(out.size(), in.size() + 2 * lim.delay()); /* one frame out per frame in, plus the delay's worth of frames */
        CHECK(peak_of(out) <= ceil + 1e-6f);
        CHECK(peak_of(out) <= 1.0f); /* "no sample exceeds full scale" */
        CHECK(peak_of(out, 22050) > 0.5f); /* still loud: the limiter does not mute it */
    }
    /* loud music-like signal: a sum of sines at +4 dB over full scale: the same */
    {
        lim.reset();
        std::vector<float> a = sine(88200, 0.8f, 440);
        const std::vector<float> b = sine(88200, 0.7f, 3100);
        for (size_t i = 0; i < a.size(); ++i) a[i] = (a[i] + b[i]) * std::pow(10.0f, 4.0f / 20.0f);
        CHECK(peak_of(a) > 1.2f);
        const std::vector<float> out = through(lim, a);
        CHECK(peak_of(out) <= ceil + 1e-6f);
    }
    /* a lone loud click in silence: the gain ramps down before it (look-ahead): the click is at the ceiling */
    {
        lim.reset();
        std::vector<float> in(2 * 4000, 0.0f);
        for (size_t i = 2000; i < 2010; ++i) in[2 * i] = in[2 * i + 1] = 1.585f;
        const std::vector<float> out = through(lim, in);
        CHECK(peak_of(out) <= ceil + 1e-6f);
        CHECK(peak_of(out) > 0.85f);
    }
    /* below the ceiling the signal passes unchanged, only delayed: out[n + delay] == in[n] */
    {
        lim.reset();
        const std::vector<float> in = sine(30000, 0.5f, 997);
        const std::vector<float> out = through(lim, in);
        const size_t d = lim.delay();
        bool same = out.size() == in.size() + 2 * d;
        for (size_t i = 0; same && i < in.size(); ++i) same = std::fabs(out[i + 2 * d] - in[i]) < 1e-6f;
        CHECK(same);
        for (size_t i = 0; i < 2 * d; ++i) CHECK(out[i] == 0.0f); /* the delay's first frames are silence */
    }
    /* the release: after a loud passage the gain comes back to 1 in about 150 ms (it is not yet back after 30 ms) */
    {
        lim.reset();
        std::vector<float> in = square(44100 / 2, 1.585f);
        const std::vector<float> quiet(2 * 44100, 0.1f);
        in.insert(in.end(), quiet.begin(), quiet.end());
        const std::vector<float> out = through(lim, in);
        const size_t loud_end = 44100 / 2 + lim.delay();
        const float g30 = out[2 * (loud_end + 44100 * 30 / 1000)] / 0.1f;
        const float g400 = out[2 * (loud_end + 44100 * 400 / 1000)] / 0.1f;
        CHECK(g30 < 0.95f);
        CHECK_NEAR(g400, 1.0, 1e-3);
    }
    /* reset forgets the delay line and the gain */
    {
        std::vector<float> loud = square(2000, 1.585f);
        lim.process(loud.data(), 2000);
        lim.reset();
        CHECK_NEAR(lim.last_gain(), 1.0, 1e-9);
        std::vector<float> z(2 * 400, 0.0f);
        lim.process(z.data(), 400);
        CHECK_EQ(peak_of(z), 0.0f); /* nothing of the old signal comes out */
    }
    /* linked stereo: one gain for both channels (a loud left, a quiet right keep their ratio) */
    {
        lim.reset();
        std::vector<float> in(2 * 2000, 0.0f);
        for (size_t i = 0; i < 2000; ++i) {
            in[2 * i] = 1.5f;     /* left over full scale */
            in[2 * i + 1] = 0.3f; /* right quiet */
        }
        const std::vector<float> out = through(lim, in);
        const size_t f = 1500 + lim.delay();
        CHECK_NEAR(out[2 * f] / 1.5f, out[2 * f + 1] / 0.3f, 1e-4);
    }
}

void test_bypass_and_conversion() {
    /* unity gain: convert_pcm is the integer path, bit exact for S16 and S32 outputs */
    std::vector<int16_t> in16(2 * 1000);
    for (size_t i = 0; i < in16.size(); ++i) in16[i] = static_cast<int16_t>(static_cast<int>((i * 7919) % 65536) - 32768);
    std::vector<int16_t> out16(in16.size());
    convert_pcm(in16.data(), 16, 2, out16.data(), SampleFormat::S16_LE, 2, 1000, 1.0f);
    CHECK(std::memcmp(in16.data(), out16.data(), in16.size() * 2) == 0);
    std::vector<int32_t> in32(2 * 1000);
    for (size_t i = 0; i < in32.size(); ++i) in32[i] = static_cast<int32_t>(static_cast<uint32_t>((i + 1) * 2654435761u));
    std::vector<int32_t> out32(in32.size());
    convert_pcm(in32.data(), 32, 2, out32.data(), SampleFormat::S32_LE, 2, 1000, 1.0f);
    CHECK(std::memcmp(in32.data(), out32.data(), in32.size() * 4) == 0);
    /* the engine's choice: the limiter is for a net gain above 0 dB only */
    CHECK(!gain_needs_limiter(1.0f));
    CHECK(!gain_needs_limiter(0.5f));
    CHECK(!gain_needs_limiter(boost_factor(0)));
    CHECK(gain_needs_limiter(boost_factor(1)));
    CHECK(gain_needs_limiter(boost_factor(4)));
    CHECK(!gain_needs_limiter(boost_factor(4) * 0.5f)); /* a boost under a lower volume or ReplayGain stays on the integer path */
    /* the boost factors: whole dB, 0 to 4, anything else clamped */
    CHECK_NEAR(boost_factor(0), 1.0, 1e-12);
    CHECK_NEAR(boost_factor(1), std::pow(10.0, 1 / 20.0), 1e-6);
    CHECK_NEAR(boost_factor(4), std::pow(10.0, 4 / 20.0), 1e-6);
    CHECK_NEAR(boost_factor(5), boost_factor(4), 1e-12);
    CHECK_NEAR(boost_factor(-2), 1.0, 1e-12);
    /* limited conversion: 16-bit in, S16 out: delayed by the limiter, exact below the ceiling at unity */
    {
        Limiter lim(44100);
        std::vector<float> scratch;
        std::vector<int16_t> src(2 * 1000, 10000), dst(2 * 1000);
        convert_pcm_limited(src.data(), 16, 2, dst.data(), SampleFormat::S16_LE, 2, 1000, 1.0f, lim, scratch);
        CHECK_EQ(dst[0], 0); /* the delay */
        CHECK_EQ(dst[2 * 220], 0);
        CHECK_EQ(dst[2 * 221], 10000); /* then the signal at unity: exact */
        CHECK_EQ(dst[2 * 999 + 1], 10000);
        /* mono output: the average of the two channels; three channels: zeros after the first two */
        std::vector<int16_t> mono(1000), tri(3 * 1000, 7);
        Limiter l2(44100), l3(44100);
        convert_pcm_limited(src.data(), 16, 2, mono.data(), SampleFormat::S16_LE, 1, 1000, 1.0f, l2, scratch);
        CHECK_EQ(mono[500], 10000);
        convert_pcm_limited(src.data(), 16, 2, tri.data(), SampleFormat::S16_LE, 3, 1000, 1.0f, l3, scratch);
        CHECK(tri[3 * 500] == 10000 && tri[3 * 500 + 1] == 10000 && tri[3 * 500 + 2] == 0);
        /* mono input is duplicated; S24 out */
        Limiter l4(44100);
        std::vector<int16_t> m1(1000, -8000);
        std::vector<unsigned char> s24(1000 * 2 * 3);
        convert_pcm_limited(m1.data(), 16, 1, s24.data(), SampleFormat::S24_3LE, 2, 1000, 1.0f, l4, scratch);
        const size_t k = 3 * 2 * 700;
        const uint32_t u = static_cast<uint32_t>(s24[k]) | (static_cast<uint32_t>(s24[k + 1]) << 8) | (static_cast<uint32_t>(s24[k + 2]) << 16);
        const int32_t v = static_cast<int32_t>(u << 8) >> 8;
        CHECK_EQ(v, -8000 * 256);
        /* 32-bit in and out above the ceiling: limited, never past it */
        Limiter l5(44100);
        std::vector<int32_t> big(2 * 4000, 2000000000), outb(2 * 4000);
        convert_pcm_limited(big.data(), 32, 2, outb.data(), SampleFormat::S32_LE, 2, 4000, boost_factor(4), l5, scratch);
        int64_t pk = 0;
        for (int32_t x : outb) pk = std::max<int64_t>(pk, std::llabs(static_cast<long long>(x)));
        CHECK(pk <= static_cast<int64_t>(0.8913 * 2147483648.0) + 4000);
    }
    /* the tail of a stream: delay() frames, the last of the signal; the limiter is reset after it */
    {
        Limiter lim(44100);
        std::vector<float> scratch;
        std::vector<int16_t> src(2 * 500, 12000), dst(2 * 500), tail(2 * 221);
        convert_pcm_limited(src.data(), 16, 2, dst.data(), SampleFormat::S16_LE, 2, 500, 1.0f, lim, scratch);
        CHECK_EQ(limiter_tail_pcm(lim, tail.data(), SampleFormat::S16_LE, 2), static_cast<size_t>(221));
        CHECK(tail[0] == 12000 && tail[2 * 220] == 12000); /* what was still inside */
        std::vector<int16_t> again(2 * 10);
        convert_pcm_limited(src.data(), 16, 2, again.data(), SampleFormat::S16_LE, 2, 10, 1.0f, lim, scratch);
        CHECK_EQ(again[0], 0); /* a new stream starts with silence again */
    }
}

void test_gain_product() {
    /* boost + ReplayGain + volume: the amplitude factors multiply; ReplayGain's peak guard keeps it under 0 dB on its own,
     * the boost then needs the limiter */
    ReplayGainTags rg;
    rg.has_track = true;
    rg.track_db = -7.0f;
    const float rgf = replaygain_factor(ReplayGainMode::Track, rg);
    CHECK_NEAR(rgf, std::pow(10.0, -7.0 / 20), 1e-6);
    const float vol = volume_gain_db(-6.0); /* a software volume level */
    const float net = vol * rgf * boost_factor(4);
    CHECK_NEAR(net, std::pow(10.0, (-6.0 - 7.0 + 4.0) / 20), 1e-5);
    CHECK(!gain_needs_limiter(net));
    /* a quiet file tagged +3 dB with a +4 dB boost at full volume: +7 dB, limited */
    rg.track_db = 3.0f;
    const float net2 = replaygain_factor(ReplayGainMode::Track, rg) * boost_factor(4);
    CHECK_NEAR(net2, std::pow(10.0, 7.0 / 20), 1e-5);
    CHECK(gain_needs_limiter(net2));
    /* the peak guard first: a peak of 0.9 caps the ReplayGain at +0.92 dB, the boost comes on top and the limiter takes the
     * peaks to the ceiling */
    rg.has_track_peak = true;
    rg.track_peak = 0.9f;
    rg.track_db = 6.0f;
    CHECK_NEAR(replaygain_db(ReplayGainMode::Track, rg), -20 * std::log10(0.9), 1e-4);
    Limiter lim(44100);
    const float total = replaygain_factor(ReplayGainMode::Track, rg) * boost_factor(4);
    const std::vector<float> in = square(30000, 0.9f * total);
    CHECK(peak_of(in) > 1.0f);
    CHECK(peak_of(through(lim, in)) <= 0.8914f);
    /* ReplayGain off and no boost: never the limiter (net 0 dB) */
    CHECK(!gain_needs_limiter(replaygain_factor(ReplayGainMode::Off, rg) * boost_factor(0)));
    for (int db = 0; db <= kMaxBoostDb; ++db) CHECK_NEAR(20 * std::log10(boost_factor(db)), db, 1e-5);
}

void test_volume_steps() {
    /* the model on its grid: levels hi, hi - s, ... down to the window's floor; the dB shown is exactly on that grid */
    for (double s : {1.0, 0.5, 0.1}) {
        const OutputVolumeModel m = volume_model_steps(-45, 0, 0, s);
        CHECK_NEAR(m.hi_db, 0.0, 1e-12);
        CHECK_EQ(m.n, static_cast<int>(std::floor(45 / s + 1e-9)) + 1);
        CHECK_NEAR(m.step_db(), s, 1e-9);
        CHECK_NEAR(m.db_of(m.n), 0.0, 1e-9);
        CHECK_NEAR(m.db_of(1), -45.0, 1e-9);
        for (int i = 2; i <= m.n; ++i) CHECK_NEAR(m.db_of(i) - m.db_of(i - 1), s, 1e-9);
    }
    /* a 60 dB window: 61, 121, 601 levels (report 060 3f) */
    CHECK_EQ(volume_model_steps(-60, 0, 0, 1.0).n, 61);
    CHECK_EQ(volume_model_steps(-60, 0, 0, 0.5).n, 121);
    CHECK_EQ(volume_model_steps(-60, 0, 0, 0.1).n, 601);
    /* Normal (0) is today's model, unchanged */
    for (double w : {60.0, 45.0, 34.0, 10.0}) {
        const OutputVolumeModel a = volume_model_steps(-w, 0, 0.5, 0), b = volume_model(-w, 0, 0.5);
        CHECK(a.n == b.n && a.lo_db == b.lo_db && a.hi_db == b.hi_db);
    }
    CHECK_EQ(default_volume_model().n, 30);
    CHECK_EQ(default_volume_model(0.5).n, 121);
    CHECK_NEAR(default_volume_model(0.1).step_db(), 0.1, 1e-9);
    /* never finer than the output's own step: a 1 dB control (the Sharkoon) at 0.1 and 0.5 dB stays at 1 dB, the Apple
     * adapter's 0.5 dB at 0.1 dB stays at 0.5 dB, a 3 dB control at 1 dB stays at 3 dB; a coarser choice wins */
    CHECK_NEAR(volume_model_steps(-45, 0, 1.0, 0.1).step_db(), 1.0, 1e-9);
    CHECK_EQ(volume_model_steps(-45, 0, 1.0, 0.1).n, 46);
    CHECK_NEAR(volume_model_steps(-45, 0, 1.0, 0.5).step_db(), 1.0, 1e-9);
    CHECK_NEAR(volume_model_steps(-60, 0, 0.5, 0.1).step_db(), 0.5, 1e-9);
    CHECK_NEAR(volume_model_steps(-30, 0, 3.0, 1.0).step_db(), 3.0, 1e-9);
    CHECK_NEAR(volume_model_steps(-60, 0, 0.5, 1.0).step_db(), 1.0, 1e-9);
    /* the Pi jack: -102.38..+4.00 dB in 0.01 dB steps: the top 60 dB, the top level is +4.0 dB exactly */
    {
        const OutputVolumeModel j = volume_model_steps(-102.38, 4.0, 0.01, 0.1);
        CHECK_EQ(j.n, 601);
        CHECK_NEAR(j.db_of(601), 4.0, 1e-9);
        CHECK_NEAR(j.db_of(1), -56.0, 1e-9);
        CHECK_EQ(volume_level_text(j, 601, VolumeUnit::Decibel), std::string("+4.0 dB"));
        CHECK_EQ(volume_level_text(j, 600, VolumeUnit::Decibel), std::string("+3.9 dB"));
        CHECK_EQ(volume_level_text(j, 1, VolumeUnit::Decibel), std::string("-56.0 dB"));
        const OutputVolumeModel j1 = volume_model_steps(-102.38, 4.0, 0.01, 1.0);
        CHECK_EQ(volume_level_text(j1, 61, VolumeUnit::Decibel), std::string("+4.0 dB"));
        CHECK_EQ(volume_level_text(j1, 60, VolumeUnit::Decibel), std::string("+3.0 dB"));
    }
    /* the text: one step moves the shown dB by exactly the chosen amount; zero is "0.0 dB", never "-0.0" */
    for (double s : {1.0, 0.5, 0.1}) {
        const OutputVolumeModel m = volume_model_steps(-45, 0, 0, s);
        bool all = true;
        for (int i = 2; i <= m.n; ++i) {
            const double a = std::atof(volume_level_text(m, i - 1, VolumeUnit::Decibel).c_str());
            const double b = std::atof(volume_level_text(m, i, VolumeUnit::Decibel).c_str());
            all = all && std::fabs((b - a) - s) < 0.051;
        }
        CHECK(all);
        CHECK_EQ(volume_level_text(m, m.n, VolumeUnit::Decibel), std::string("0.0 dB"));
    }
    CHECK_EQ(volume_level_text(volume_model_steps(-45, 0, 0, 0.5), 90, VolumeUnit::Decibel), std::string("-0.5 dB"));
    /* Normal keeps the percent when asked for it */
    CHECK_EQ(volume_level_text(volume_model_steps(-45, 0, 1.0, 0), 12), std::string("52"));
    /* steps clamp at both ends; a press moves one level */
    {
        const OutputVolumeModel m = volume_model_steps(-60, 0, 0, 0.1);
        CHECK_EQ(m.step(m.n, 1), m.n);
        CHECK_EQ(m.step(1, -1), 1);
        CHECK_EQ(m.step(300, 1), 301);
        CHECK_EQ(m.step(300, -1), 299);
        CHECK_NEAR(m.db_of(301) - m.db_of(300), 0.1, 1e-9);
    }
    /* changing the step keeps the nearest dB */
    {
        const OutputVolumeModel m01 = volume_model_steps(-60, 0, 0, 0.1), m1 = volume_model_steps(-60, 0, 0, 1.0);
        CHECK_NEAR(m01.db_of(m01.nearest(-22.34)), -22.3, 1e-9);
        CHECK_NEAR(m1.db_of(m1.nearest(m01.db_of(m01.nearest(-22.34)))), -22.0, 1e-9);
        for (double s : {0.0, 1.0, 0.5, 0.1}) {
            const OutputVolumeModel m = volume_model_steps(-60, 0, 0, s);
            CHECK(std::fabs(m.db_of(m.nearest(-22.3)) + 22.3) <= std::max(m.step_db() / 2, 0.05) + 1e-9);
        }
    }
    /* a model of a control without dB data is untouched by the setting: its percent */
    OutputVolumeModel raw;
    raw.db = false;
    raw.n = 12;
    CHECK_EQ(volume_level_text(raw, 6, VolumeUnit::Decibel), std::string("50"));
    CHECK_EQ(volume_model_steps(0, 0, 0, 0.1).n, 1); /* a one-value range */
    /* a mixer with 0.5 dB raw steps in the 0.1 dB mode (kept at 0.5): raw value and level agree both ways */
    {
        MixerChoice mc;
        mc.volume = "PCM Playback Volume";
        mc.min = 0;
        mc.max = 120;
        mc.has_db = true;
        mc.db_min = -6000;
        mc.db_max = 0;
        mc.model = volume_model_steps(-60, 0, 0.5, 0.1);
        CHECK_EQ(mc.model.n, 121);
        for (int lv : {1, 40, 77, 121}) CHECK_EQ(mixer_level_for_raw(mc, mixer_raw_for_level(mc, lv)), lv);
        CHECK_EQ(mixer_raw_for_level(mc, 121), 120L);
        CHECK_EQ(mixer_raw_for_level(mc, 120), 119L);
    }
    /* a sink (the server's volume is continuous): the card's own 1 dB step does not limit the fine modes; Normal unchanged */
    {
        MixerChoice card;
        card.volume = "PCM Playback Volume";
        card.min = 0;
        card.max = 45;
        card.has_db = true;
        card.db_min = -4500;
        card.db_max = 0;
        card.model = volume_model(-45, 0, 1.0);
        const OutputVolumeModel normal = sink_volume_model(&card);
        CHECK(normal.n == card.model.n && normal.lo_db == card.model.lo_db - card.model.hi_db);
        const OutputVolumeModel fine = sink_volume_model(&card, 0.1);
        CHECK_EQ(fine.n, 451);
        CHECK_NEAR(fine.step_db(), 0.1, 1e-9);
        CHECK_NEAR(fine.hi_db, 0.0, 1e-12);
        CHECK_NEAR(sink_volume_model(&card, 0.5).step_db(), 0.5, 1e-9);
        CHECK_EQ(sink_volume_model(nullptr, 1.0).n, 61); /* no card range: -60..0 */
        CHECK_EQ(sink_volume_model(nullptr).n, 30);
        for (int i = 1; i <= fine.n; i += 37) CHECK_EQ(sink_level_for_volume(fine, sink_volume_for_db(fine.db_of(i))), i);
    }
}

void test_hold_repeat() {
    HoldRepeat h;
    CHECK(!h.active());
    CHECK_EQ(h.poll(1000), 0);
    h.start(1, 10000); /* the press itself stepped once (the caller's); the repeat comes later */
    CHECK(h.active());
    CHECK_EQ(h.dir(), 1);
    CHECK_EQ(h.poll(10000), 0);
    CHECK_EQ(h.poll(10499), 0); /* held under 0.5 s: no repeat */
    CHECK_EQ(h.poll(10500), 1); /* at 0.5 s the first repeat */
    CHECK_EQ(h.poll(10550), 0);
    CHECK_EQ(h.poll(10600), 1); /* then 10 a second */
    CHECK_EQ(h.poll(10700), 1);
    /* a 100 ms tick: one step a tick until 2 s of holding */
    int n = 0;
    uint64_t t = 10700;
    while (t < 12000 - 100) {
        t += 100;
        n += h.poll(t);
    }
    CHECK_EQ(n, 12); /* 10800 .. 11900 */
    /* over 2 s of holding: about 30 a second (the 100 ms tick gets 3 a time) */
    n = 0;
    for (int i = 0; i < 10; ++i) { /* 1 s */
        t += 100;
        n += h.poll(t);
    }
    CHECK(n >= 27 && n <= 31);
    /* a fine tick: 10 a second in the slow phase, 30 a second in the fast one */
    HoldRepeat g;
    g.start(-1, 0);
    int slow = 0, fast = 0;
    for (uint64_t ms = 1; ms <= 1999; ++ms) slow += g.poll(ms);
    for (uint64_t ms = 2000; ms < 3000; ++ms) fast += g.poll(ms);
    CHECK(slow >= 14 && slow <= 16); /* 0.5 s .. 2 s at 10 a second */
    CHECK(fast >= 29 && fast <= 31);
    CHECK_EQ(g.dir(), -1);
    /* a release stops it; a new press starts again with the half second */
    g.stop();
    CHECK(!g.active());
    CHECK_EQ(g.poll(50000), 0);
    g.start(1, 60000);
    CHECK_EQ(g.poll(60400), 0);
    CHECK_EQ(g.poll(60500), 1);
    /* a stall (the app was busy for 5 s) never floods: at most kHoldMaxPerPoll steps, the rest is dropped */
    HoldRepeat s;
    s.start(1, 0);
    CHECK(s.poll(5000) <= kHoldMaxPerPoll);
    CHECK_EQ(s.poll(5000), 0);
}

/* A deterministic "master": a kick every 0.45 s, a bass line, a noisy top, soft clipped by `drive` and scaled to a peak. */
std::vector<float> master(size_t frames, double peak_db, double drive) {
    std::vector<float> v(frames * 2);
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<int32_t>(seed >> 8)) / 8388608.0f - 1.0f;
    };
    float lp = 0;
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / 44100.0, ph = std::fmod(t, 0.45);
        const double kick = std::sin(2 * kPi * (50 + 80 * std::exp(-ph * 30)) * ph) * std::exp(-ph * 9);
        const double bass = 0.35 * std::sin(2 * kPi * 55 * t);
        lp += 0.3f * (rnd() - lp);
        const double top = 0.5 * rnd() * (0.4 + 0.6 * std::exp(-std::fmod(t, 0.225) * 12)) + 0.3 * lp;
        const double x = std::tanh(drive * (0.9 * kick + bass + 0.45 * top));
        v[2 * i] = static_cast<float>(x);
        v[2 * i + 1] = static_cast<float>(0.97 * x + 0.03 * rnd());
    }
    const float k = static_cast<float>(std::pow(10.0, peak_db / 20.0)) / peak_of(v);
    for (float &x : v) x *= k;
    return v;
}
double rms_db(const std::vector<float> &v, size_t from, size_t to) {
    double a = 0;
    for (size_t i = from * 2; i < to * 2; ++i) a += static_cast<double>(v[i]) * v[i];
    return 10 * std::log10(a / (static_cast<double>(to - from) * 2) + 1e-30);
}
/* the loudness gain (RMS, dB) of a +4 dB boost through the limiter on `in` (the delay aligned) */
double boosted_rms_gain(const std::vector<float> &in, Limiter::Stats *st = nullptr) {
    Limiter lim(44100);
    std::vector<float> o = in;
    for (float &x : o) x *= boost_factor(4);
    for (size_t at = 0; at < o.size() / 2; at += 1024) lim.process(&o[at * 2], std::min<size_t>(1024, o.size() / 2 - at));
    const size_t d = lim.delay(), n = in.size() / 2;
    if (st) *st = lim.stats();
    return rms_db(o, d, n) - rms_db(in, 0, n - d);
}

/* How much of the 4 dB a boost really gives (owner's board test, 2026-11-15: "not clearly louder" on loud dance masters). The
 * sample peak limiter cannot raise a signal whose peaks are within the ceiling's reach: the gain is what the headroom and the
 * crest allow. The numbers go to the journal of the test run. */
void test_dense_loudness() {
    Limiter::Stats st;
    const std::vector<float> dense = master(44100 * 12, -0.5, 1.0);      /* a limited master: peaks -0.5 dBFS, crest 9 dB */
    const std::vector<float> normal = master(44100 * 12, -12.0, 1.0);    /* an ordinary level: peaks -12 dBFS */
    const std::vector<float> crushed = master(44100 * 12, -0.5, 3.0);    /* a hard clipped master: crest 4 dB */
    const std::vector<float> mid = master(44100 * 12, -3.0, 1.0);        /* peaks only 3 dB under full scale */
    const double g_dense = boosted_rms_gain(dense, &st);
    const double g_normal = boosted_rms_gain(normal);
    const double g_crushed = boosted_rms_gain(crushed);
    const double g_mid = boosted_rms_gain(mid);
    std::fprintf(stderr,
                 "  +4 dB boost, RMS loudness gain: dense master (peak -0.5 dBFS, crest %.1f dB) %+.2f dB, peaks -3 dBFS %+.2f dB, normal "
                 "(peak -12 dBFS) %+.2f dB, hard clipped (crest %.1f dB) %+.2f dB; dense: avg reduction %.2f dB, max %.2f dB, %.0f %% of "
                 "frames limited\n",
                 -0.5 - rms_db(dense, 0, dense.size() / 2), g_dense, g_mid, g_normal, -0.5 - rms_db(crushed, 0, crushed.size() / 2),
                 g_crushed, st.red_sum_db / static_cast<double>(st.frames), -20 * std::log10(st.min_gain),
                 100.0 * static_cast<double>(st.limited) / static_cast<double>(st.frames));
    CHECK_NEAR(g_normal, 4.0, 0.05); /* nothing to limit: the full 4 dB */
    CHECK(g_mid > 3.5);              /* peaks at -3 dBFS: most of it */
    CHECK(g_dense > 2.4 && g_dense < 3.0); /* a dense master (crest 9 dB) keeps about 2.7 of the 4 dB: the limiter takes its peaks */
    CHECK(g_crushed > 0.0 && g_crushed < 1.0); /* a hard clipped one has no headroom: the ceiling is the limit, not the limiter */
    CHECK(g_normal > g_mid && g_mid > g_dense && g_dense > g_crushed);
    /* the stats the journal line reports */
    CHECK(st.frames > 500000);
    CHECK(st.limited > 0 && st.limited < st.frames);
    CHECK(st.peak_out <= static_cast<float>(std::pow(10.0, kLimiterCeilingDb / 20.0)) + 1e-6f);
    CHECK(st.peak_in > 1.2f);
    /* the journal line */
    Limiter::Stats s2;
    s2.frames = 1000;
    s2.limited = 480;
    s2.red_sum_db = 780.0;
    s2.min_gain = static_cast<float>(std::pow(10.0, -4.5 / 20));
    s2.peak_in = static_cast<float>(std::pow(10.0, -0.1 / 20));
    s2.peak_out = 0.8913f;
    const std::string line = gain_report_text(4.0, 4, 0.0, 0.0, true, s2, "a.m4a", "first 2 s");
    CHECK_EQ(line, std::string("gain: net +4.0 dB (boost +4 dB, replaygain +0.0 dB, app volume +0.0 dB) limiter on: reduction avg 0.8 dB, "
                               "max 4.5 dB, 48 % of frames limited, peak in -4.1 dBFS, after gain -0.1 dBFS, out -1.0 dBFS (first 2 s of "
                               "a.m4a)"));
    CHECK(gain_report_text(-3.0, 4, -7.0, 0.0, false, Limiter::Stats(), "b.flac", "track end").find("limiter off") != std::string::npos);
}

}  // namespace

void test_gain_steps() {
    test_dense_loudness();
    test_limiter();
    test_bypass_and_conversion();
    test_gain_product();
    test_volume_steps();
    test_hold_repeat();
}
