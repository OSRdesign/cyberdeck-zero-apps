/* SPDX-License-Identifier: MIT */
#include "check.hpp"
#include "gain.hpp"
#include "siggen.hpp"

using namespace cassette;

namespace {

void steps_and_quantisation() {
    SoftGain g;
    CHECK_EQ(g.db(), 0.0f);
    g.set_db(10.8);
    CHECK_EQ(g.db(), 11.0f); /* nearest 0.5 dB step */
    g.set_db(10.7);
    CHECK_EQ(g.db(), 10.5f);
    g.set_db(-3.0);
    CHECK_EQ(g.db(), 0.0f);
    g.set_db(30.0);
    CHECK_EQ(g.db(), 24.0f);
    CHECK_EQ(g.steps(), 48);
    g.set_steps(-5);
    CHECK_EQ(g.steps(), 0);
    for (int s = 0; s <= SoftGain::kMaxSteps; ++s) {
        g.set_steps(s);
        CHECK_NEAR(g.factor(), std::pow(10.0, s * 0.5 / 20.0), 1e-5);
    }
}

void apply_s16_s24() {
    for (uint16_t bits : {uint16_t(16), uint16_t(24)}) {
        AudioFormat f{48000, 1, bits};
        const int32_t base = bits == 16 ? 1000 : 100000;
        sig::Bytes b(4 * pcm::bytes_per_sample(bits));
        sig::put(b, 0, bits, base);
        sig::put(b, 1, bits, -base);
        sig::put(b, 2, bits, 0);
        sig::put(b, 3, bits, 1);
        SoftGain g;
        sig::Bytes same = b;
        CHECK_EQ(g.apply(same.data(), 4, f), size_t(0)); /* 0 dB: untouched */
        CHECK(same == b);
        g.set_db(6.0);
        CHECK_EQ(g.apply(b.data(), 4, f), size_t(0));
        const double k = std::pow(10.0, 6.0 / 20.0);
        CHECK_EQ(sig::get(b, 0, bits), int32_t(std::lround(base * k)));
        CHECK_EQ(sig::get(b, 1, bits), int32_t(-std::lround(base * k)));
        CHECK_EQ(sig::get(b, 2, bits), 0);
        CHECK_EQ(sig::get(b, 3, bits), 2);
        CHECK_EQ(g.clip_count(), 0u);
    }
}

void saturation() {
    for (uint16_t bits : {uint16_t(16), uint16_t(24)}) {
        AudioFormat f{48000, 2, bits};
        const int32_t big = pcm::max_pos(bits) / 4; /* -12 dBFS: +24 dB pushes it 12 dB over */
        sig::Bytes b(4 * pcm::bytes_per_sample(bits));
        sig::put(b, 0, bits, big);
        sig::put(b, 1, bits, -big);
        sig::put(b, 2, bits, 10);
        sig::put(b, 3, bits, -10);
        SoftGain g;
        g.set_db(24.0);
        CHECK_EQ(g.apply(b.data(), 2, f), size_t(2));
        CHECK_EQ(sig::get(b, 0, bits), pcm::max_pos(bits));
        CHECK_EQ(sig::get(b, 1, bits), pcm::min_neg(bits));
        CHECK_EQ(sig::get(b, 2, bits), 158); /* 10 * 15.85 */
        CHECK_EQ(sig::get(b, 3, bits), -158);
        CHECK_EQ(g.clip_count(), 2u);
        g.reset_clips();
        CHECK_EQ(g.clip_count(), 0u);
    }
    /* the float path only multiplies; saturation happens once at the end of ProcessingSink's chain */
    SoftGain g;
    g.set_db(12.0);
    float x[2] = {0.5f, -0.1f};
    g.process(x, 2);
    CHECK_NEAR(x[0], 0.5 * 3.98107, 1e-4);
    CHECK_NEAR(x[1], -0.1 * 3.98107, 1e-4);
}

}  // namespace

void test_gain() {
    steps_and_quantisation();
    apply_s16_s24();
    saturation();
}
