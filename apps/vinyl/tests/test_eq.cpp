/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests (P2d): the equalizer's filters (the RBJ peaking response at the centres and far from them, measured on sines
 * through the filter), the curve's highest point and the headroom (no clipping at the strongest curve, with the limiter
 * after it), the presets and the setting's edits, the bypass; the EQ page's geometry at every size (no two touch areas
 * overlap, every band reachable, the buttons the toolkit's row high).
 */
#include "check.hpp"

#include "dsp/biquad.hpp"
#include "dsp/eq.hpp"
#include "dsp/limiter.hpp"
#include "engine/dsp_bench.hpp"
#include "layout/layout.hpp"
#include "settings/settings.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace vinyl;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* the RMS gain in dB of `apply` on a sine of f Hz (stereo, 1 s at fs, the first 0.25 s left out: the filters settle) */
template <typename F> double sine_gain_db(double f, double fs, F apply) {
    const size_t n = static_cast<size_t>(fs);
    std::vector<float> x(2 * n);
    for (size_t i = 0; i < n; ++i) x[2 * i] = x[2 * i + 1] = static_cast<float>(0.25 * std::sin(2 * kPi * f * static_cast<double>(i) / fs));
    std::vector<float> y = x;
    for (size_t at = 0; at < n; at += 2048) apply(y.data() + 2 * at, std::min<size_t>(2048, n - at));
    double ex = 0, ey = 0;
    for (size_t i = n / 4; i < n; ++i) {
        ex += static_cast<double>(x[2 * i]) * x[2 * i];
        ey += static_cast<double>(y[2 * i]) * y[2 * i];
    }
    return 10 * std::log10(ey / ex);
}

void test_biquad() {
    /* the peaking response at the centre is the gain, far from it 0 dB; a few bands, both signs */
    const double fs = 44100;
    for (int band : {0, 3, 5, 7, 9})
        for (int db : {-12, -6, 3, 12}) {
            const BiquadCoef c = biquad_peaking(fs, kEqFreqs[band], kEqQ, db);
            CHECK_NEAR(biquad_response_db(c, kEqFreqs[band], fs), db, 0.01);
            const double far = band < 5 ? kEqFreqs[band] * 16 : kEqFreqs[band] / 16;
            CHECK_NEAR(biquad_response_db(c, far, fs), 0.0, 0.25);
        }
    /* measured: a sine through the filter (TDF-II, double state) matches the response within 0.2 dB (report 060's test) */
    for (int band : {1, 5, 8}) {
        const BiquadCoef c = biquad_peaking(fs, kEqFreqs[band], kEqQ, 6);
        BiquadState st;
        const double g = sine_gain_db(kEqFreqs[band], fs, [&](float *p, size_t n) { biquad_process(c, st, p, n); });
        CHECK_NEAR(g, 6.0, 0.2);
        BiquadState st2;
        const double half = sine_gain_db(kEqFreqs[band] * 2, fs, [&](float *p, size_t n) { biquad_process(c, st2, p, n); });
        CHECK_NEAR(half, biquad_response_db(c, kEqFreqs[band] * 2, fs), 0.2);
    }
    /* 0 dB is a pass-through; a band at or above Nyquist too */
    const BiquadCoef z = biquad_peaking(fs, 1000, kEqQ, 0);
    CHECK_NEAR(z.b0, 1.0, 1e-12);
    CHECK_NEAR(z.b1, z.a1, 1e-12);
    CHECK_NEAR(z.b2, z.a2, 1e-12);
    const BiquadCoef ny = biquad_peaking(32000, 16000, kEqQ, 6);
    CHECK_EQ(ny.b0, 1.0);
    CHECK_EQ(ny.a1, 0.0);
    /* a long silence after a sound leaves no denormal state (it is flushed to 0) */
    {
        const BiquadCoef c = biquad_peaking(fs, 31.25, kEqQ, 12);
        BiquadState st;
        std::vector<float> buf(2 * 2048, 0.5f);
        biquad_process(c, st, buf.data(), 2048);
        for (int i = 0; i < 400; ++i) { /* 18 s of silence (the 31 Hz band's ring decays under 1e-30 in about 2 s) */
            std::fill(buf.begin(), buf.end(), 0.0f);
            biquad_process(c, st, buf.data(), 2048);
        }
        CHECK(st.s1l == 0 && st.s2l == 0 && st.s1r == 0 && st.s2r == 0);
    }
}

void test_curve() {
    /* one band: the curve's top is that band's gain; nothing above 0 dB: 0 */
    EqGains g{};
    g[5] = 6;
    CHECK_NEAR(eq_max_gain_db(g, 44100), 6.0, 0.05);
    g[5] = -9;
    CHECK_EQ(eq_max_gain_db(g, 44100), 0.0);
    CHECK_EQ(eq_max_gain_db(EqGains{}, 44100), 0.0);
    /* neighbours overlap: every band at +12 rises above 12 dB */
    EqGains all;
    all.fill(12);
    const double top = eq_max_gain_db(all, 44100);
    std::fprintf(stderr, "  EQ curve: every band +12 dB peaks at %+.2f dB\n", top);
    CHECK(top > 12.5 && top < 24);
    /* the owner's board test of the 0.7.0 draft (2026-11-20): a boost must raise its band and leave the others where they
     * are (the draft lowered the whole signal by the boost first, so a boost made the music quieter). One band at +6 and
     * at +12 dB: its centre rises by the gain, bands three octaves and more away stay at 0 dB (+-0.1), the neighbour
     * octave gets the filter's own skirt (raised, never lowered) */
    for (int db : {6, 12}) {
        Equalizer eq;
        g = EqGains{};
        g[5] = db; /* 1 kHz */
        eq.configure(g, 44100);
        CHECK(eq.active());
        CHECK_EQ(eq.bands(), 1);
        const double at_c = sine_gain_db(1000, 44100, [&](float *p, size_t n) { eq.process(p, n); });
        eq.reset();
        const double low = sine_gain_db(62.5, 44100, [&](float *p, size_t n) { eq.process(p, n); });
        eq.reset();
        const double high = sine_gain_db(16000, 44100, [&](float *p, size_t n) { eq.process(p, n); });
        eq.reset();
        const double next = sine_gain_db(500, 44100, [&](float *p, size_t n) { eq.process(p, n); });
        std::fprintf(stderr, "  EQ 1 kHz band %+d dB: 1 kHz %+.2f dB, 62.5 Hz %+.3f dB, 16 kHz %+.3f dB, 500 Hz %+.2f dB\n", db, at_c, low,
                     high, next);
        CHECK_NEAR(at_c, db, 0.2);
        CHECK_NEAR(low, 0.0, 0.1);
        CHECK_NEAR(high, 0.0, 0.1);
        CHECK_NEAR(next, biquad_response_db(biquad_peaking(44100, 1000, kEqQ, db), 500, 44100), 0.2);
        CHECK(next > 0.0);
    }
    /* the same at the bass end: 62.5 Hz +12 dB, 1 kHz and 4 kHz untouched */
    {
        Equalizer eq;
        g = EqGains{};
        g[1] = 12;
        eq.configure(g, 48000);
        CHECK_NEAR(sine_gain_db(62.5, 48000, [&](float *p, size_t n) { eq.process(p, n); }), 12.0, 0.2);
        eq.reset();
        CHECK_NEAR(sine_gain_db(1000, 48000, [&](float *p, size_t n) { eq.process(p, n); }), 0.0, 0.1);
        eq.reset();
        CHECK_NEAR(sine_gain_db(4000, 48000, [&](float *p, size_t n) { eq.process(p, n); }), 0.0, 0.1);
    }
    /* a cut only: -12 dB at the centre, the rest untouched */
    {
        Equalizer eq;
        g = EqGains{};
        g[2] = -12;
        eq.configure(g, 48000);
        CHECK_NEAR(sine_gain_db(125, 48000, [&](float *p, size_t n) { eq.process(p, n); }), -12.0, 0.2);
        eq.reset();
        CHECK_NEAR(sine_gain_db(2000, 48000, [&](float *p, size_t n) { eq.process(p, n); }), 0.0, 0.1);
    }
    /* the 16 kHz band at 32 kHz is left out (over 0.45 fs), as in the curve's top */
    {
        Equalizer eq;
        g = EqGains{};
        g[9] = 12;
        eq.configure(g, 32000);
        CHECK_EQ(eq.bands(), 0);
        CHECK(!eq.active());
        CHECK_EQ(eq_max_gain_db(g, 32000), 0.0);
        eq.configure(g, 44100);
        CHECK_EQ(eq.bands(), 1);
    }
    /* flat again: nothing to do at once, and nothing touches the samples */
    {
        Equalizer eq;
        g = EqGains{};
        g[0] = 6;
        eq.configure(g, 44100);
        std::vector<float> b(2 * 512, 0.5f);
        eq.process(b.data(), 512);
        eq.configure(EqGains{}, 44100);
        CHECK(!eq.active());
        CHECK_EQ(eq.bands(), 0);
        std::fill(b.begin(), b.end(), 0.5f);
        eq.process(b.data(), 512);
        CHECK_EQ(b[0], 0.5f);
        CHECK_EQ(b[2 * 511], 0.5f);
    }
    /* no clipping at the strongest curve: a full-scale square and white noise through every band at +12 dB, then the
     * limiter (as in the engine): never above the ceiling (the EQ alone goes far over full scale now that nothing lowers
     * the signal before it: the limiter is what keeps it clean) */
    for (int kind = 0; kind < 2; ++kind) {
        Equalizer eq;
        eq.configure(all, 44100);
        Limiter lim(44100);
        uint32_t seed = 7;
        float peak_eq = 0, peak_out = 0;
        std::vector<float> b(2 * 2048);
        for (int blk = 0; blk < 40; ++blk) {
            for (size_t f = 0; f < 2048; ++f) {
                float v;
                if (kind == 0) v = ((blk * 2048 + static_cast<int>(f)) / 220) % 2 ? 1.0f : -1.0f; /* 100 Hz square */
                else {
                    seed = seed * 1664525u + 1013904223u;
                    v = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
                }
                b[2 * f] = b[2 * f + 1] = v;
            }
            eq.process(b.data(), 2048);
            for (float v : b) peak_eq = std::max(peak_eq, std::fabs(v));
            lim.process(b.data(), 2048);
            for (float v : b) peak_out = std::max(peak_out, std::fabs(v));
        }
        std::fprintf(stderr, "  EQ max boost, %s: peak after the EQ %.3f, after the limiter %.4f (ceiling %.4f)\n",
                     kind == 0 ? "full-scale square" : "full-scale noise", peak_eq, peak_out, lim.ceiling());
        CHECK(peak_out <= lim.ceiling() + 1e-5f);
        CHECK(peak_eq > 1.0f);
    }
    /* the limiter after a single boosted band on a loud mix (a 62.5 Hz tone at -6 dBFS with a 1 kHz tone at -12 dBFS, the
     * bass band +12 dB, so the bass reaches +6 dBFS): no sample over the ceiling, a bounded reduction; a quiet mix
     * (-24 dBFS) passes the limiter untouched with its bass +12 dB */
    for (double level : {0.5, 0.0625}) {
        Equalizer eq;
        g = EqGains{};
        g[1] = 12;
        eq.configure(g, 44100);
        Limiter lim(44100);
        const size_t n = 44100 * 2;
        std::vector<float> x(2 * n);
        for (size_t i = 0; i < n; ++i)
            x[2 * i] = x[2 * i + 1] = static_cast<float>(level * std::sin(2 * kPi * 62.5 * static_cast<double>(i) / 44100) +
                                                          level * 0.5 * std::sin(2 * kPi * 1000 * static_cast<double>(i) / 44100));
        float peak = 0;
        for (size_t at = 0; at < n; at += 2048) {
            const size_t m = std::min<size_t>(2048, n - at);
            eq.process(x.data() + 2 * at, m);
            lim.process(x.data() + 2 * at, m);
        }
        for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(x[2 * i]));
        const Limiter::Stats &st = lim.stats();
        const double min_gain = st.min_gain;
        std::fprintf(stderr, "  EQ 62.5 Hz +12 dB, mix at %.1f dBFS: peak out %.3f (ceiling %.3f), %.0f %% of frames limited, max reduction %.1f dB\n",
                     20 * std::log10(level), peak, lim.ceiling(), 100.0 * static_cast<double>(st.limited) / static_cast<double>(st.frames),
                     st.min_gain < 1.0f ? -20 * std::log10(st.min_gain) : 0.0);
        CHECK(peak <= lim.ceiling() + 1e-5f);
        if (level < 0.1) {
            CHECK_EQ(st.limited, static_cast<uint64_t>(0)); /* quiet music: the boost is heard in full */
        } else {
            CHECK(st.limited > 0);
            CHECK(min_gain > 0.2); /* about -8 dB at most: the bass at +6 dBFS brought to -1 dBFS (plus the 1 kHz tone) */
        }
    }
}

void test_settings() {
    /* the presets: Flat is all 0 (a bypass); Custom's gains are the custom curve */
    EqSettings s;
    CHECK(s.on);
    CHECK(s.preset == EqPreset::Flat);
    CHECK(s.bypass());
    CHECK_EQ(eq_menu_value(s), std::string("Flat"));
    for (EqPreset p : kEqPresets) {
        CHECK(eq_preset_from(eq_preset_name(p), EqPreset::Rock) == p);
        const EqGains g = eq_preset_gains(p);
        bool in_range = true;
        for (int v : g) in_range = in_range && v >= -kEqMaxDb && v <= kEqMaxDb;
        CHECK(in_range);
    }
    CHECK(eq_preset_from("nonsense", EqPreset::Jazz) == EqPreset::Jazz);
    s.preset = EqPreset::BassBoost;
    CHECK(!s.bypass());
    CHECK_EQ(eq_menu_value(s), std::string("Bass boost"));
    CHECK_EQ(s.gains()[0], 6);
    /* Off: a bypass whatever the curve; the menu says Off */
    s.on = false;
    CHECK(s.bypass());
    CHECK_EQ(eq_menu_value(s), std::string("Off"));
    CHECK_EQ(s.effective()[0], 0);
    s.on = true;
    /* a band moved: Custom, from the preset's other bands; clamped to +-12 */
    s.set_band(7, 3);
    CHECK(s.preset == EqPreset::Custom);
    CHECK_EQ(s.gains()[0], 6);
    CHECK_EQ(s.gains()[7], 3);
    s.set_band(7, 40);
    CHECK_EQ(s.gains()[7], 12);
    s.set_band(2, -40);
    CHECK_EQ(s.gains()[2], -12);
    s.set_band(10, 5); /* no such band */
    s.set_band(-1, 5);
    CHECK_EQ(s.custom[9], 0);
    /* Reset: Flat, the custom curve kept (Custom brings it back) */
    const EqGains kept = s.custom;
    s.reset();
    CHECK(s.preset == EqPreset::Flat);
    CHECK(s.bypass());
    s.preset = EqPreset::Custom;
    CHECK(s.gains() == kept);
    /* settings.json: "eq" and "transition" round trip; bad values fall back (a wrong preset Flat, a custom value clamped, a
     * list of another length ignored); an older file has the defaults (the Transition's migration: test_settings_state) */
    {
        Settings st;
        st.eq.on = false;
        st.eq.preset = EqPreset::Custom;
        st.eq.custom = EqGains{1, 2, 3, 4, 5, -1, -2, -3, -4, -12};
        st.transition = Transition::Crossfade10;
        const std::string text = settings_to_json(st, nullptr);
        CHECK(text.find("\"eq\"") != std::string::npos && text.find("\"custom\"") != std::string::npos);
        CHECK(text.find("\"transition\": \"crossfade_10\"") != std::string::npos);
        Settings back;
        CHECK(settings_from_json(text, back, nullptr));
        CHECK(back == st);
        CHECK(settings_from_json("{\"gapless\": true}", back, nullptr));
        CHECK(back.eq == EqSettings() && back.crossfade_s() == 0);
        CHECK(settings_from_json("{\"eq\": {\"on\": 3, \"preset\": \"loud\", \"custom\": [20, -30, 0, 0, 0, 0, 0, 0, 0, 1]}}", back, nullptr));
        CHECK(back.eq.on && back.eq.preset == EqPreset::Flat && back.eq.custom[0] == 12 && back.eq.custom[1] == -12 && back.eq.custom[9] == 1);
        CHECK(settings_from_json("{\"eq\": {\"custom\": [1, 2, 3]}}", back, nullptr));
        CHECK(back.eq.custom == EqGains{});
        CHECK(settings_from_json("{\"eq\": {\"preset\": \"treble_boost\"}, \"transition\": \"crossfade_20\"}", back, nullptr));
        CHECK(back.eq.preset == EqPreset::TrebleBoost && back.crossfade_s() == 20);
    }
    /* the words */
    CHECK_EQ(eq_band_label(0), std::string("31"));
    CHECK_EQ(eq_band_label(9), std::string("16k"));
    CHECK_EQ(eq_band_name(5), std::string("1 kHz"));
    CHECK_EQ(eq_band_name(4), std::string("500 Hz"));
    CHECK_EQ(eq_db_text(3), std::string("+3 dB"));
    CHECK_EQ(eq_db_text(0), std::string("0 dB"));
    CHECK_EQ(eq_db_text(-12), std::string("-12 dB"));
}

/* the EQ page at every size: the bars and buttons inside the panel, no two touch areas overlapping, the bands' areas side by
 * side (no tap lands on a neighbour), the buttons' areas at least the toolkit's ROW high (the harness's rule) */
void test_geometry() {
    struct Size {
        int w, h, body, target, row;
    };
    /* body: the toolkit header's bottom; target, row: the toolkit's TARGET and ROW at the scenes' densities */
    const Size sizes[] = {{480, 320, 40, 44, 44}, {320, 480, 40, 44, 44}, {640, 480, 64, 63, 63}, {800, 480, 64, 64, 64},
                          {480, 480, 64, 79, 79}, {720, 720, 64, 70, 70}, {480, 800, 64, 60, 60}, {480, 640, 64, 63, 63},
                          {1280, 720, 64, 81, 81}};
    for (const Size &z : sizes) {
        const layout::EqGeom g = layout::eq_geom(z.w, z.h, z.body, z.target, z.row);
        char what[64];
        std::snprintf(what, sizeof what, "%dx%d", z.w, z.h);
        bool inside = true, areas_ok = true, side_by_side = true;
        auto in_panel = [&](const layout::R &r) {
            return r.x >= g.panel.x - 0.5 && r.y >= g.panel.y - 0.5 && r.right() <= g.panel.right() + 0.5 && r.bottom() <= g.panel.bottom() + 0.5;
        };
        auto on_screen = [&](const layout::R &r) { return r.x >= -0.5 && r.y >= z.body - 0.5 && r.right() <= z.w + 0.5 && r.bottom() <= z.h + 0.5; };
        std::vector<layout::R> areas;
        for (int i = 0; i < kEqBands; ++i) {
            inside = inside && in_panel(g.track[i]) && in_panel(g.label[i]);
            areas.push_back(g.band_area[i]);
            areas_ok = areas_ok && on_screen(g.band_area[i]) && g.band_area[i].x <= g.track[i].x && g.band_area[i].right() >= g.track[i].right() &&
                       g.band_area[i].y <= g.track[i].y && g.band_area[i].bottom() >= g.track[i].bottom();
            if (i > 0) side_by_side = side_by_side && std::fabs(g.band_area[i].x - g.band_area[i - 1].right()) < 0.01;
        }
        for (int i = 0; i < layout::kEqButtons; ++i) {
            inside = inside && in_panel(g.button[i]);
            areas.push_back(g.button_area[i]);
            areas_ok = areas_ok && on_screen(g.button_area[i]) && g.button_area[i].h >= z.row - 0.01 &&
                       g.button_area[i].x <= g.button[i].x + 0.01 && g.button_area[i].right() >= g.button[i].right() - 0.01;
        }
        bool none = true;
        for (size_t a = 0; a < areas.size(); ++a)
            for (size_t b = a + 1; b < areas.size(); ++b) none = none && !layout::overlaps(areas[a], areas[b]);
        if (!inside) std::fprintf(stderr, "  EQ page %s: something outside the panel\n", what);
        if (!areas_ok) std::fprintf(stderr, "  EQ page %s: a touch area is wrong\n", what);
        if (!none) std::fprintf(stderr, "  EQ page %s: touch areas overlap\n", what);
        CHECK(inside);
        CHECK(areas_ok);
        CHECK(side_by_side);
        CHECK(none);
        CHECK(g.portrait == (z.h > z.w));
        /* the value under a point of a bar and back: +12 at the top, 0 at the middle line, -12 at the bottom */
        const layout::R &t = g.track[3];
        CHECK_EQ(layout::eq_db_at(g, t.y), kEqMaxDb);
        CHECK_EQ(layout::eq_db_at(g, t.y + t.h / 2), 0);
        CHECK_EQ(layout::eq_db_at(g, t.bottom()), -kEqMaxDb);
        CHECK_EQ(layout::eq_db_at(g, t.y - 50), kEqMaxDb); /* clamped */
        std::fprintf(stderr, "  EQ page %s: bands %.0f px apart (%.0f wide), bars %.0f high, buttons %.0f high (areas %.0f)\n", what,
                     g.band_area[1].x - g.band_area[0].x, g.track[0].w, g.track[0].h, g.button[0].h, g.button_area[0].h);
    }
}

void test_bench() {
    const DspBench b = run_dsp_bench(3.0);
    std::fprintf(stderr, "  %s", dsp_bench_text(b).c_str());
    CHECK(b.seconds > 2.5);
    CHECK(b.total_pct > 0);
    CHECK(b.total_pct >= b.base_pct * 0.5);
}

}  // namespace

void test_eq() {
    test_biquad();
    test_curve();
    test_settings();
    test_geometry();
    test_bench();
}
