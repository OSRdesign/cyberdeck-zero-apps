/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette codec tests: FlacEncoder on the fake libFLAC (what reaches the library, when bytes reach the sink, the
 * STREAMINFO rewrite, errors).
 */
#include "check.hpp"

#include "fake_codecs.hpp"
#include "flac_encoder.hpp"
#include "siggen.hpp"

#include <algorithm>
#include <cerrno>
#include <string>

using namespace cassette;

namespace {

uint64_t le64_at(const std::vector<unsigned char> &d, size_t off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | d[off + static_cast<size_t>(i)];
    return v;
}

bool contains(const std::vector<unsigned char> &d, const std::string &s) {
    return std::search(d.begin(), d.end(), s.begin(), s.end()) != d.end();
}

void test_start_and_settings() {
    FlacLib lib = fake::make_flac_lib();
    FlacConfig cfg;
    cfg.fmt = AudioFormat{48000, 1, 16};
    MemoryByteSink sink;
    {
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {{"TITLE", "Take 7"}, {"DATE", "2026-11-10"}, {"COMMENT", "caf\xC3\xA9"}}), 0);
        const fake::FlacState &s = fake::flac_state();
        CHECK_EQ(s.channels, 1u);
        CHECK_EQ(s.bits, 16u);
        CHECK_EQ(s.rate, 48000u);
        CHECK_EQ(s.level, 5u);
        CHECK_EQ(s.verify, 0);
        CHECK_EQ(s.subset, 1);
        CHECK_EQ(s.metadata_blocks, 2u); /* comments + padding */
        CHECK_EQ(s.padding, 4096u);
        CHECK_EQ(s.comments.size(), size_t(3));
        if (s.comments.size() == 3) {
            CHECK_EQ(s.comments[0], std::string("TITLE=Take 7"));
            CHECK_EQ(s.comments[2], std::string("COMMENT=caf\xC3\xA9"));
        }
        /* the header is in the sink at once: an empty take is a valid file */
        CHECK(sink.size() > 4096);
        CHECK(sink.data.size() >= 4 && std::string(sink.data.begin(), sink.data.begin() + 4) == "fLaC");
        CHECK(contains(sink.data, "TITLE=Take 7"));
        CHECK_EQ(e.stats().frames_out, uint64_t(0));
        CHECK(e.stats().bytes_out == sink.size());
        CHECK_EQ(e.start(sink, {}), kCodecState); /* twice */
    }
    CHECK_EQ(fake::flac_state().deletes, 1);
    CHECK_EQ(fake::flac_state().meta_alive, 0); /* both metadata objects freed */

    /* no padding, level clamped */
    lib = fake::make_flac_lib();
    cfg.padding_bytes = 0;
    cfg.level = 12;
    MemoryByteSink s2;
    FlacEncoder e2(&lib, cfg);
    CHECK_EQ(e2.start(s2, {}), 0);
    CHECK_EQ(fake::flac_state().metadata_blocks, 1u);
    CHECK_EQ(fake::flac_state().level, 8u);
}

void test_blocks_and_finish() {
    FlacLib lib = fake::make_flac_lib();
    FlacConfig cfg;
    cfg.fmt = AudioFormat{48000, 1, 16};
    MemoryByteSink sink;
    FlacEncoder e(&lib, cfg);
    CHECK_EQ(e.write("xx", 1), kCodecState); /* before start */
    lib = fake::make_flac_lib();
    FlacEncoder f(&lib, cfg);
    CHECK_EQ(f.start(sink, {}), 0);
    const sig::Bytes pcm = sig::ramp(cfg.fmt, 10000);
    const size_t parts[] = {1, 777, 3000, 10000 - 1 - 777 - 3000};
    size_t at = 0;
    for (size_t n : parts) {
        CHECK_EQ(f.write(pcm.data() + at * 2, n), 0);
        at += n;
    }
    const fake::FlacState &s = fake::flac_state();
    CHECK_EQ(s.samples.size(), size_t(10000));
    bool same = s.samples.size() == 10000;
    for (size_t i = 0; same && i < 10000; ++i) same = s.samples[i] == sig::get(pcm, i, 16);
    CHECK(same);
    CHECK_EQ(f.stats().frames_in, uint64_t(10000));
    CHECK_EQ(f.stats().frames_out, uint64_t(8192)); /* two whole blocks of 4096 */
    CHECK_EQ(f.stats().pending_frames(), uint64_t(1808));
    const uint64_t before = sink.size();
    CHECK_EQ(f.flush(), 0); /* nothing more can leave before finish */
    CHECK_EQ(sink.size(), before);
    CHECK_EQ(le64_at(sink.data, 8), uint64_t(0)); /* STREAMINFO total: unknown until finish */
    CHECK_EQ(f.finish(), 0);
    CHECK_EQ(f.stats().frames_out, uint64_t(10000));
    CHECK(f.stats().finished);
    CHECK_EQ(le64_at(sink.data, 8), uint64_t(10000)); /* rewritten in place */
    CHECK_EQ(sink.seeks(), uint64_t(1));
    CHECK(sink.size() > before);
    CHECK_EQ(f.finish(), 0); /* idempotent */
    CHECK_EQ(f.write(pcm.data(), 1), kCodecState);
    CHECK_EQ(s.finishes, 1);
}

void test_24bit_stereo() {
    FlacLib lib = fake::make_flac_lib();
    FlacConfig cfg;
    cfg.fmt = AudioFormat{44100, 2, 24};
    MemoryByteSink sink;
    FlacEncoder e(&lib, cfg);
    CHECK_EQ(e.start(sink, {}), 0);
    const sig::Bytes pcm = sig::ramp(cfg.fmt, 5000); /* crosses the 4096-frame chunk inside one write */
    CHECK_EQ(e.write(pcm.data(), 5000), 0);
    const fake::FlacState &s = fake::flac_state();
    CHECK_EQ(s.bits, 24u);
    CHECK_EQ(s.channels, 2u);
    CHECK_EQ(s.samples.size(), size_t(10000));
    bool same = s.samples.size() == 10000, negative = false;
    for (size_t i = 0; same && i < 10000; ++i) {
        same = s.samples[i] == sig::get(pcm, i, 24);
        negative |= s.samples[i] < -4000000;
    }
    CHECK(same);
    CHECK(negative); /* sign extension from bit 23 */
    CHECK_EQ(s.process_calls, 2);
    CHECK_EQ(e.finish(), 0);
    CHECK_EQ(e.stats().frames_out, uint64_t(5000));
}

void test_refusals() {
    FlacConfig cfg;
    cfg.fmt = AudioFormat{48000, 1, 16};
    MemoryByteSink sink;
    {
        FlacEncoder e(nullptr, cfg); /* the library is missing */
        CHECK_EQ(e.start(sink, {}), kCodecMissing);
        CHECK_EQ(e.write("ab", 1), kCodecMissing); /* sticky */
    }
    {
        FlacLib lib = fake::make_flac_lib();
        lib.vc_append_comment = nullptr; /* a library that lacks a symbol */
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), kCodecMissing);
    }
    const AudioFormat bad[] = {{48000, 1, 32}, {48000, 3, 16}, {48000, 0, 16}, {0, 1, 16}, {700000, 1, 16},
                               {96001, 1, 16}};
    for (const AudioFormat &f : bad) {
        FlacLib lib = fake::make_flac_lib();
        FlacConfig c;
        c.fmt = f;
        FlacEncoder e(&lib, c);
        CHECK_EQ(e.start(sink, {}), kCodecRefused);
        CHECK_EQ(fake::flac_state().news, 0); /* refused before the library is asked */
    }
    std::string why;
    CHECK(flac_format_ok(AudioFormat{96000, 2, 24}, &why));
    CHECK(!flac_format_ok(AudioFormat{48000, 8, 16}, &why));
    CHECK_EQ(why, std::string("more than 2 channels"));
    {
        FlacLib lib = fake::make_flac_lib();
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {{"BAD=NAME", "x"}}), kCodecRefused);
    }
    {
        FlacLib lib = fake::make_flac_lib();
        fake::flac_state().refuse_init = flac::kInitInvalidBits;
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), kCodecRefused);
    }
    {
        FlacLib lib = fake::make_flac_lib();
        fake::flac_state().fail_new = true;
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), kCodecFailed);
    }
}

void test_sink_errors() {
    FlacConfig cfg;
    cfg.fmt = AudioFormat{48000, 1, 16};
    const sig::Bytes pcm = sig::ramp(cfg.fmt, 20000);
    {
        /* the card fills up mid-take: the sink's ENOSPC comes back and sticks */
        FlacLib lib = fake::make_flac_lib();
        MemoryByteSink sink;
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), 0);
        sink.fail_at = sink.size() + 5000; /* the first frame fits, the second not */
        CHECK_EQ(e.write(pcm.data(), 4096), 0);
        CHECK_EQ(e.write(pcm.data(), 8000), -ENOSPC);
        CHECK_EQ(e.stats().error, -ENOSPC);
        CHECK_EQ(e.write(pcm.data(), 10), -ENOSPC);
        CHECK_EQ(e.finish(), -ENOSPC);
        CHECK_EQ(e.stats().frames_out, uint64_t(4096));
    }
    {
        /* no room even for the header */
        FlacLib lib = fake::make_flac_lib();
        MemoryByteSink sink;
        sink.fail_at = 10;
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), -ENOSPC);
    }
    {
        /* the library fails by itself */
        FlacLib lib = fake::make_flac_lib();
        fake::flac_state().fail_process_at = 1;
        MemoryByteSink sink;
        FlacEncoder e(&lib, cfg);
        CHECK_EQ(e.start(sink, {}), 0);
        CHECK_EQ(e.write(pcm.data(), 100), 0);
        CHECK_EQ(e.write(pcm.data(), 100), kCodecFailed);
    }
    {
        /* destroyed without finish (the writer gave up): libFLAC's delete finishes, nothing reaches the sink */
        FlacLib lib = fake::make_flac_lib();
        MemoryByteSink sink;
        uint64_t size = 0;
        {
            FlacEncoder e(&lib, cfg);
            CHECK_EQ(e.start(sink, {}), 0);
            CHECK_EQ(e.write(pcm.data(), 1000), 0);
            size = sink.size();
        }
        CHECK_EQ(sink.size(), size);
        CHECK(fake::flac_state().wrote_after_delete_refused);
        CHECK_EQ(fake::flac_state().deletes, 1);
    }
}

}  // namespace

void test_flac() {
    test_start_and_settings();
    test_blocks_and_finish();
    test_24bit_stereo();
    test_refusals();
    test_sink_errors();
}
