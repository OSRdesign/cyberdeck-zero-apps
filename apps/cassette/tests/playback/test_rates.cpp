/* SPDX-License-Identifier: MIT
 *
 * Cassette playback tests at the rates cards really have: takes at 16000 Hz (a speech headset), outputs that play only 48000
 * (or only 16000). The output converts through ALSA's plug device (AlsaPlaybackSink opens plughw: when the card does not take
 * the take's rate); this test models that conversion with a linear interpolator behind the plug device and checks that a
 * 1 kHz tone arrives as a 1 kHz tone, and that the 5x scan and the 5 s jump count source frames at any rate.
 */
#include "check.hpp"
#include "fake_alsa.hpp"
#include "player.hpp"
#include "sink.hpp"
#include "support.hpp"

#include <cerrno>
#include <cmath>
#include <vector>

using namespace cassette;
using namespace tp;

namespace {

constexpr double kPi = 3.14159265358979323846;

/* The device behind plughw:, as ALSA's plug plugin: the application writes at its own rate, the card gets `hw_rate`
 * (linear interpolation, mono S16). Everything the card receives is kept in `out`. */
class PlugPcm final : public AlsaPcm {
public:
    PlugPcm(std::vector<int16_t> *out, uint32_t app_rate, uint32_t hw_rate) : out_(out), app_(app_rate), hw_(hw_rate) {}
    long readi(void *, size_t) override { return -EBADFD; }
    long writei(const void *buf, size_t frames) override {
        const int16_t *in = static_cast<const int16_t *>(buf);
        for (size_t i = 0; i < frames; ++i) in_.push_back(in[i]);
        /* emit every output sample whose source position (n * app / hw) lies within the input received so far */
        for (;;) {
            const double pos = static_cast<double>(n_) * app_ / hw_;
            const size_t i0 = static_cast<size_t>(pos);
            if (i0 + 1 >= in_.size()) break;
            const double fr = pos - static_cast<double>(i0);
            out_->push_back(static_cast<int16_t>(std::lround(in_[i0] * (1.0 - fr) + in_[i0 + 1] * fr)));
            ++n_;
        }
        return static_cast<long>(frames);
    }
    int recover(int err) override { return err; }
    int prepare() override { return 0; }
    void close() override {}

private:
    std::vector<int16_t> *out_;
    std::vector<int16_t> in_;
    uint32_t app_, hw_;
    uint64_t n_ = 0;
};

/* FakeAlsa whose plug devices convert to the card's own (single) playback rate. */
class PlugAlsa final : public FakeAlsa {
public:
    std::vector<int16_t> hw_out;     /* what the card received via the plug device */
    int open_playback(const std::string &d, const HwRequest &r, std::unique_ptr<AlsaPcm> &pcm, HwGranted &g) override {
        const int e = FakeAlsa::open_playback(d, r, pcm, g);
        if (e != 0 || d.compare(0, 4, "plug") != 0) return e;
        FakeCard *c = find_id(card_of(d));
        const uint32_t hw_rate = c && !c->play_offer.rates.empty() ? c->play_offer.rates[0] : r.rate;
        pcm.reset(new PlugPcm(&hw_out, r.rate, hw_rate));
        return 0;
    }
    static std::string card_of(const std::string &dev) {
        const size_t a = dev.find("CARD=");
        if (a == std::string::npos) return "";
        const size_t b = dev.find(',', a);
        return dev.substr(a + 5, b == std::string::npos ? std::string::npos : b - a - 5);
    }
};

std::vector<int32_t> tone16(uint64_t frames, uint32_t rate, double hz, double amp = 12000.0) {
    std::vector<int32_t> v(frames);
    for (uint64_t i = 0; i < frames; ++i) v[i] = static_cast<int32_t>(std::lround(amp * std::sin(2.0 * kPi * hz * double(i) / rate)));
    return v;
}

/* Frequency by rising zero crossings over the whole buffer, at `rate`. */
double crossing_hz(const std::vector<int16_t> &v, uint32_t rate) {
    int crossings = 0;
    long first = -1, last = -1;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] < 0 && v[i] >= 0) {
            if (first < 0) first = static_cast<long>(i);
            last = static_cast<long>(i);
            ++crossings;
        }
    if (crossings < 2) return 0;
    return (crossings - 1) * static_cast<double>(rate) / static_cast<double>(last - first);
}

void run_to_end(Player &p) {
    int n = 100000;
    while (n-- > 0 && p.step()) {
    }
}

FakeCard card_48_only(int index) {
    FakeCard c = fake_pcm2902(index);
    c.play_offer.rates = {48000};
    c.play_offer.min_channels = c.play_offer.max_channels = 1;
    return c;
}

void tone_through_plug_keeps_its_pitch() {
    TempDir d;
    /* a 16000 Hz take played on an output that takes 48000 only */
    {
        const auto s = tone16(16000, 16000, 1000.0);
        const std::string path = d.file("t16.wav");
        write_wav(path, afmt(16000, 1, 16), s);
        PlugAlsa fa;
        fa.cards_.push_back(card_48_only(0));
        AlsaPlaybackSink sink(fa, "hw:CARD=Device,DEV=0");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(sink.opened_device(), std::string("plughw:CARD=Device,DEV=0"));
        CHECK_EQ(fa.playback_requests[0].rate, 16000u); /* the take's own rate goes in; ALSA converts */
        CHECK_EQ(p.sink_format().rate, 16000u);
        run_to_end(p);
        /* one second of tape is 48000 samples at the card of a 1 kHz tone */
        CHECK(fa.hw_out.size() > 47990 && fa.hw_out.size() <= 48000);
        const double hz = crossing_hz(fa.hw_out, 48000);
        CHECK(std::fabs(hz - 1000.0) < 5.0);
        CHECK(std::fabs(p.length_s() - 1.0) < 1e-9);
    }
    /* a 48000 Hz take on the headset's playback side that plays 16000 only */
    {
        const auto s = tone16(48000, 48000, 1000.0);
        const std::string path = d.file("t48.wav");
        write_wav(path, afmt(48000, 1, 16), s);
        PlugAlsa fa;
        fa.cards_.push_back(fake_headset16(0));
        AlsaPlaybackSink sink(fa, "hw:CARD=J65,DEV=0");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(sink.opened_device(), std::string("plughw:CARD=J65,DEV=0"));
        CHECK_EQ(fa.playback_requests[0].rate, 48000u);
        run_to_end(p);
        CHECK(fa.hw_out.size() > 15990 && fa.hw_out.size() <= 16000);
        CHECK(std::fabs(crossing_hz(fa.hw_out, 16000) - 1000.0) < 5.0);
    }
    /* a 16000 Hz take on the headset's own 16000 Hz output: no conversion, opened as hw: */
    {
        const auto s = tone16(16000, 16000, 440.0);
        const std::string path = d.file("same.wav");
        write_wav(path, afmt(16000, 1, 16), s);
        PlugAlsa fa;
        fa.cards_.push_back(fake_headset16(0));
        AlsaPlaybackSink sink(fa, "hw:CARD=J65,DEV=0");
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK_EQ(p.play(), 0);
        CHECK_EQ(sink.opened_device(), std::string("hw:CARD=J65,DEV=0"));
        run_to_end(p);
        std::vector<int32_t> got;
        for (size_t i = 0; i + 1 < fa.played.size(); i += 2) got.push_back(decode(&fa.played[i], 2));
        CHECK(got == s);
    }
}

void position_and_scan_count_source_frames() {
    TempDir d;
    for (uint32_t rate : {16000u, 22050u, 96000u}) {
        const uint64_t len = static_cast<uint64_t>(rate) * 40; /* 40 s */
        const auto s = ramp_samples(len, 1);
        const std::string path = d.file("r" + std::to_string(rate) + ".wav");
        write_wav(path, afmt(rate, 1, 16), s);
        FakeSink sink;
        Player p(posix_file_ops(), sink, Player::Mode::Manual);
        CHECK_EQ(p.open(path), 0);
        CHECK(std::fabs(p.length_s() - 40.0) < 1e-9);
        CHECK_EQ(p.play(), 0);
        for (int i = 0; i < 4; ++i) p.step();
        CHECK_EQ(p.position(), 4u * 1024u);
        CHECK(std::fabs(p.position_s() - 4096.0 / rate) < 1e-9);
        CHECK_EQ(p.pause(), 0);
        /* the 5 s jump is 5 * rate source frames, either way, clamped at both ends */
        CHECK_EQ(p.seek(0), 0);
        CHECK_EQ(p.jump(Direction::Forward), 0);
        CHECK_EQ(p.position(), 5ull * rate);
        CHECK_EQ(p.jump(Direction::Forward, 2), 0);
        CHECK_EQ(p.position(), 7ull * rate);
        CHECK_EQ(p.jump(Direction::Reverse), 0);
        CHECK_EQ(p.position(), 2ull * rate);
        CHECK_EQ(p.jump(Direction::Reverse), 0);
        CHECK_EQ(p.position(), 0u);
        CHECK_EQ(p.seek(len - 3ull * rate), 0);
        CHECK_EQ(p.jump(Direction::Forward), 0);
        CHECK_EQ(p.position(), len);
        /* the 5x scan: each output frame stands for exactly 5 source frames; one second of cue is 5 s of tape */
        CHECK_EQ(p.seek(10ull * rate), 0);
        const uint64_t start = p.position();
        const size_t before = sink.heard_frames();
        CHECK_EQ(p.scan(Direction::Forward), 0);
        const uint64_t want_out = rate; /* one second of output */
        while (sink.heard_frames() - before < want_out) p.step();
        const uint64_t out = sink.heard_frames() - before;
        CHECK_EQ(p.position() - start, 5 * out);
        CHECK(std::fabs(static_cast<double>(p.position() - start) / rate - 5.0 * out / rate) < 1e-9);
        CHECK(out >= want_out && out < want_out + 1024);
        CHECK_EQ(p.end_scan(), 0);
    }
}

/* a sink that grants another rate than the take's would play at the wrong speed: the player refuses it */
class WrongRateSink final : public PlaybackSink {
public:
    int open(const AudioFormat &src, SinkFormat &granted) override {
        granted.rate = src.rate == 16000 ? 48000 : 16000;
        granted.channels = src.channels;
        granted.format = SampleFormat::S16_LE;
        return 0;
    }
    long write(const void *, size_t n) override { return static_cast<long>(n); }
    int recover(int e) override { return e; }
    long delay() override { return 0; }
    void drop() override {}
    void drain() override {}
    void close() override { ++closes; }
    int closes = 0;
};

void a_sink_that_changes_the_rate_is_refused() {
    TempDir d;
    const std::string path = d.file("w.wav");
    write_wav(path, afmt(16000, 1, 16), ramp_samples(16000, 1));
    WrongRateSink sink;
    Player p(posix_file_ops(), sink, Player::Mode::Manual);
    CHECK_EQ(p.open(path), 0);
    CHECK_EQ(p.play(), -EINVAL);
    CHECK(sink.closes >= 1);
}

} // namespace

void test_rates() {
    tone_through_plug_keeps_its_pitch();
    position_and_scan_count_source_frames();
    a_sink_that_changes_the_rate_is_refused();
}
