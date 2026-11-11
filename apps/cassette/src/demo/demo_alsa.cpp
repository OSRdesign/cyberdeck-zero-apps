/*
 * SPDX-License-Identifier: MIT
 *
 * Cassette demo mode: the scripted sound system (see demo_alsa.hpp). SYNTHETIC data only.
 */
#include "demo_alsa.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cassette {
namespace demo {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr const char *kLevelCtl = "Mic Capture Volume";
constexpr const char *kSwitchCtl = "Mic Capture Switch";
constexpr const char *kAgcCtl = "Auto Gain Control";
constexpr long kLevelSteps = 16, kLevelStart = 8;
constexpr double kLevelMaxDb = 23.81;
constexpr double kToneHz = 440.0;
constexpr double kPi = 3.14159265358979323846;

/* "hw:CARD=DemoLevel,DEV=0", "plughw:CARD=...", "hw:CARD=DemoLevel" -> "DemoLevel"; "" otherwise */
std::string card_id_of(const std::string &dev)
{
    const std::string key = "hw:CARD=";
    const size_t at = dev.compare(0, 4, "plug") == 0 ? 4 : 0;
    if (dev.compare(at, key.size(), key) != 0) return "";
    const std::string rest = dev.substr(at + key.size());
    return rest.substr(0, rest.find(','));
}

CtlInfo int_ctl(const char *name, long mn, long mx, long db_min_cdb, long db_max_cdb)
{
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Integer;
    c.capture = true;
    c.min = mn;
    c.max = mx;
    c.has_db = true;
    c.db_min_cdb = db_min_cdb;
    c.db_max_cdb = db_max_cdb;
    return c;
}

CtlInfo bool_ctl(const char *name)
{
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Boolean;
    c.capture = std::strstr(name, "Capture") != nullptr;
    return c;
}

struct DemoCard {
    CardInfo info;
    CaptureOffer offer;
    std::vector<CtlInfo> ctls;
    std::map<std::string, long> values;
};

DemoCard level_card(int index)
{
    DemoCard c;
    c.info.index = index;
    c.info.id = "DemoLevel";
    c.info.name = "Demo Microphone";
    c.info.long_name = "Demo Microphone (scripted, no hardware)";
    c.info.capture = true;
    c.offer.rates = {44100, 48000};
    c.offer.min_channels = c.offer.max_channels = 1;
    c.offer.formats = {SampleFormat::S16_LE};
    c.ctls = {int_ctl(kLevelCtl, 0, kLevelSteps, 0, static_cast<long>(kLevelMaxDb * 100)), bool_ctl(kSwitchCtl),
              bool_ctl(kAgcCtl)};
    c.values = {{kLevelCtl, kLevelStart}, {kSwitchCtl, 1}, {kAgcCtl, 0}};
    return c;
}

/* A speech headset: mono, 16000 Hz only (the call profile), a level and a mute switch, no auto gain. */
DemoCard speech_card(int index)
{
    DemoCard c;
    c.info.index = index;
    c.info.id = "DemoSpeech";
    c.info.name = "Demo Speech Headset";
    c.info.long_name = "Demo Speech Headset (scripted, no hardware, records at 16000 Hz only)";
    c.info.capture = true;
    c.offer.rates = {16000};
    c.offer.min_channels = c.offer.max_channels = 1;
    c.offer.formats = {SampleFormat::S16_LE};
    c.ctls = {int_ctl(kLevelCtl, 0, kLevelSteps, 0, static_cast<long>(kLevelMaxDb * 100)), bool_ctl(kSwitchCtl)};
    c.values = {{kLevelCtl, kLevelStart}, {kSwitchCtl, 1}};
    return c;
}

/* A stereo line input: 44100 Hz only, two channels, no mixer controls. */
DemoCard stereo_card(int index)
{
    DemoCard c;
    c.info.index = index;
    c.info.id = "DemoStereo";
    c.info.name = "Demo Stereo Input";
    c.info.long_name = "Demo Stereo Input (scripted, no hardware, records at 44100 Hz stereo only)";
    c.info.capture = true;
    c.offer.rates = {44100};
    c.offer.min_channels = c.offer.max_channels = 2;
    c.offer.formats = {SampleFormat::S16_LE};
    return c;
}

/* A studio-style input: mono, 16000, 44100, 48000 and 96000 Hz, 16-bit and 24-bit, a level and a mute switch (the card the
 * Codec / Sample rate / Quality pickers show off: every rate and both depths, Opus on 16 and 48 kHz only). */
DemoCard rich_card(int index)
{
    DemoCard c;
    c.info.index = index;
    c.info.id = "DemoRich";
    c.info.name = "Demo Studio Input";
    c.info.long_name = "Demo Studio Input (scripted, no hardware, 16 to 96 kHz, 16 and 24 bits)";
    c.info.capture = true;
    c.offer.rates = {16000, 44100, 48000, 96000};
    c.offer.min_channels = c.offer.max_channels = 1;
    c.offer.formats = {SampleFormat::S16_LE, SampleFormat::S24_3LE};
    c.ctls = {int_ctl(kLevelCtl, 0, kLevelSteps, 0, static_cast<long>(kLevelMaxDb * 100)), bool_ctl(kSwitchCtl)};
    c.values = {{kLevelCtl, kLevelStart}, {kSwitchCtl, 1}};
    return c;
}

DemoCard plain_card(int index)
{
    DemoCard c;
    c.info.index = index;
    c.info.id = "DemoPlain";
    c.info.name = "Demo Line Input";
    c.info.long_name = "Demo Line Input (scripted, no hardware, no mixer controls)";
    c.info.capture = true;
    c.offer.rates = {48000};
    c.offer.min_channels = c.offer.max_channels = 1;
    c.offer.formats = {SampleFormat::S16_LE};
    return c;
}

class DemoAlsa;

/* A capture PCM: the tone, paced in real time (a read returns when the audio would have arrived). */
class DemoPcm : public AlsaPcm {
public:
    DemoPcm(DemoAlsa *a, std::string id, const HwGranted &hw);
    long readi(void *buf, size_t frames) override;
    int recover(int err) override { return err == -EPIPE ? 0 : err; }
    int prepare() override;
    void close() override { closed_ = true; }

private:
    DemoAlsa *a_;
    std::string id_;
    HwGranted hw_;
    SteadyClock::time_point t0_;
    uint64_t delivered_ = 0;
    double phase_ = 0;
    bool closed_ = false;
};

/* A playback PCM: the samples go nowhere; writes are paced in real time so the player's position moves as it would. */
class NullPlayPcm : public AlsaPcm {
public:
    explicit NullPlayPcm(const HwGranted &hw) : hw_(hw), t0_(SteadyClock::now()) {}
    long readi(void *, size_t) override { return -EBADFD; }
    long writei(const void *, size_t frames) override
    {
        if (closed_) return -ENODEV;
        if (!started_) {
            t0_ = SteadyClock::now();
            written_ = 0;
            started_ = true;
        }
        written_ += frames;
        std::this_thread::sleep_until(t0_ + std::chrono::duration_cast<SteadyClock::duration>(
                                                std::chrono::duration<double>(static_cast<double>(written_) / hw_.rate)));
        return static_cast<long>(frames);
    }
    long delay() override { return 0; }
    int drop() override
    {
        started_ = false;
        return 0;
    }
    int drain() override
    {
        started_ = false;
        return 0;
    }
    int recover(int err) override { return err == -EPIPE ? 0 : err; }
    int prepare() override
    {
        started_ = false;
        return closed_ ? -ENODEV : 0;
    }
    void close() override { closed_ = true; }

private:
    HwGranted hw_;
    SteadyClock::time_point t0_;
    uint64_t written_ = 0;
    bool started_ = false, closed_ = false;
};

class DemoAlsa : public AlsaApi {
public:
    explicit DemoAlsa(const Script &s) : script_(s), start_(SteadyClock::now())
    {
        if (s.cards == "level" || s.cards == "both") cards_.push_back(level_card(static_cast<int>(cards_.size())));
        if (s.cards == "plain" || s.cards == "both") cards_.push_back(plain_card(static_cast<int>(cards_.size())));
        if (s.cards == "speech") cards_.push_back(speech_card(static_cast<int>(cards_.size())));
        if (s.cards == "stereo") cards_.push_back(stereo_card(static_cast<int>(cards_.size())));
        if (s.cards == "rich") cards_.push_back(rich_card(static_cast<int>(cards_.size())));
    }

    double seconds() const { return std::chrono::duration<double>(SteadyClock::now() - start_).count(); }

    bool plugged() const
    {
        const int64_t ms = static_cast<int64_t>(seconds() * 1000.0);
        if (script_.unplug_ms < 0 || ms < script_.unplug_ms) return true;
        return script_.replug_ms >= 0 && ms >= script_.replug_ms;
    }

    /* the card's gain on the tone in dB (its level relative to the start value), and whether it is muted */
    bool gain_of(const std::string &id, double &db, bool &muted)
    {
        std::lock_guard<std::mutex> g(mu_);
        db = 0;
        muted = false;
        for (const DemoCard &c : cards_) {
            if (c.info.id != id) continue;
            auto lv = c.values.find(kLevelCtl);
            if (lv != c.values.end()) db = static_cast<double>(lv->second - kLevelStart) * kLevelMaxDb / kLevelSteps;
            auto sw = c.values.find(kSwitchCtl);
            if (sw != c.values.end()) muted = sw->second == 0;
            return true;
        }
        return false;
    }

    bool available(std::string *) override { return true; }

    std::vector<CardInfo> cards() override
    {
        std::vector<CardInfo> out;
        if (!plugged()) return out;
        std::lock_guard<std::mutex> g(mu_);
        for (const DemoCard &c : cards_) out.push_back(c.info);
        return out;
    }

    int query_capture(const std::string &device, CaptureOffer &out) override
    {
        std::lock_guard<std::mutex> g(mu_);
        const DemoCard *c = find(card_id_of(device));
        if (!c) return -ENOENT;
        out = c->offer;
        return 0;
    }

    int open_capture(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                     HwGranted &granted) override
    {
        std::string id;
        {
            std::lock_guard<std::mutex> g(mu_);
            const DemoCard *c = find(card_id_of(device)); /* "default" (the sound server) is not scripted */
            if (!c) return -ENOENT;
            if (!c->offer.has_rate(req.rate) || !c->offer.has_format(req.format) || req.channels < c->offer.min_channels ||
                req.channels > c->offer.max_channels)
                return -EINVAL;
            id = c->info.id;
        }
        granted.rate = req.rate;
        granted.channels = req.channels;
        granted.format = req.format;
        granted.period_frames = req.period_frames;
        granted.buffer_frames = req.buffer_frames;
        pcm.reset(new DemoPcm(this, id, granted));
        return 0;
    }

    int mixer_list(const std::string &card, std::vector<CtlInfo> &out) override
    {
        std::lock_guard<std::mutex> g(mu_);
        const DemoCard *c = find(card_id_of(card));
        if (!c) return plugged() ? -ENOENT : -ENODEV;
        out = c->ctls;
        return 0;
    }

    int mixer_get(const std::string &card, const std::string &name, long &value) override
    {
        std::lock_guard<std::mutex> g(mu_);
        const DemoCard *c = find(card_id_of(card));
        if (!c) return -ENODEV;
        auto it = c->values.find(name);
        if (it == c->values.end()) return -ENOENT;
        value = it->second;
        return 0;
    }

    int mixer_set(const std::string &card, const std::string &name, long value) override
    {
        std::lock_guard<std::mutex> g(mu_);
        DemoCard *c = find(card_id_of(card));
        if (!c) return -ENODEV;
        for (const CtlInfo &i : c->ctls)
            if (i.name == name) {
                if (i.type == CtlInfo::Type::Boolean ? (value != 0 && value != 1) : (value < i.min || value > i.max))
                    return -EINVAL;
                c->values[name] = value;
                return 0;
            }
        return -ENOENT;
    }

    int query_playback(const std::string &, PlaybackOffer &) override { return -ENOENT; /* "default": not queried */ }

    int open_playback(const std::string &, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm, HwGranted &granted) override
    {
        granted.rate = req.rate;
        granted.channels = req.channels;
        granted.format = req.format;
        granted.period_frames = req.period_frames;
        granted.buffer_frames = req.buffer_frames;
        pcm.reset(new NullPlayPcm(granted));
        return 0;
    }

private:
    /* under mu_; null while unplugged */
    DemoCard *find(const std::string &id)
    {
        if (id.empty() || !plugged()) return nullptr;
        for (DemoCard &c : cards_)
            if (c.info.id == id) return &c;
        return nullptr;
    }

    Script script_;
    SteadyClock::time_point start_;
    std::mutex mu_;
    std::vector<DemoCard> cards_;
};

DemoPcm::DemoPcm(DemoAlsa *a, std::string id, const HwGranted &hw)
    : a_(a), id_(std::move(id)), hw_(hw), t0_(SteadyClock::now())
{
}

int DemoPcm::prepare()
{
    if (closed_ || !a_->plugged()) return -ENODEV;
    t0_ = SteadyClock::now();
    delivered_ = 0;
    return 0;
}

long DemoPcm::readi(void *buf, size_t frames)
{
    if (closed_) return -ENODEV;
    if (!a_->plugged()) return -EBADFD; /* what a pulled USB card returns ("File descriptor in bad state") */
    const uint32_t rate = hw_.rate ? hw_.rate : 48000;
    const auto due = t0_ + std::chrono::duration_cast<SteadyClock::duration>(
                               std::chrono::duration<double>(static_cast<double>(delivered_ + frames) / rate));
    std::this_thread::sleep_until(due);
    if (closed_) return -ENODEV;
    if (!a_->plugged()) return -EBADFD;
    double gain_db = 0;
    bool muted = false;
    a_->gain_of(id_, gain_db, muted);
    const double t = a_->seconds();
    const double amp = muted ? 0.0 : std::pow(10.0, (tone_level_db(t) + gain_db) / 20.0) * 32767.0;
    const double step = 2.0 * kPi * kToneHz / rate;
    const unsigned ch = hw_.channels ? hw_.channels : 1;
    if (hw_.format == SampleFormat::S24_3LE) { /* packed 24-bit: the same tone, three bytes a sample */
        uint8_t *q = static_cast<uint8_t *>(buf);
        for (size_t i = 0; i < frames; ++i) {
            const double v = std::max(-8388608.0, std::min(8388607.0, amp * 256.0 * std::sin(phase_)));
            phase_ += step;
            if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
            const int32_t s = static_cast<int32_t>(std::lround(v));
            for (unsigned c = 0; c < ch; ++c) {
                *q++ = static_cast<uint8_t>(s & 0xFF);
                *q++ = static_cast<uint8_t>((s >> 8) & 0xFF);
                *q++ = static_cast<uint8_t>((s >> 16) & 0xFF);
            }
        }
        delivered_ += frames;
        return static_cast<long>(frames);
    }
    int16_t *p = static_cast<int16_t *>(buf);
    for (size_t i = 0; i < frames; ++i) {
        const double v = std::max(-32768.0, std::min(32767.0, amp * std::sin(phase_)));
        phase_ += step;
        if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
        for (unsigned c = 0; c < ch; ++c) *p++ = static_cast<int16_t>(std::lround(v));
    }
    delivered_ += frames;
    return static_cast<long>(frames);
}

int64_t env_ms(const char *name)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return -1;
    char *end = nullptr;
    const long long n = std::strtoll(v, &end, 10);
    return end && *end == 0 && n >= 0 ? static_cast<int64_t>(n) : -1;
}

int mkdirs(const std::string &path)
{
    if (path.empty()) return -EINVAL;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        if (::mkdir(part.c_str(), 0755) < 0 && errno != EEXIST) return -errno;
    }
    return 0;
}

} // namespace

double tone_level_db(double t)
{
    return -12.0 + 8.0 * std::sin(2.0 * kPi * t / 6.0);
}

bool wanted()
{
    const char *v = std::getenv("CASSETTE_DEMO");
    return v && std::strcmp(v, "1") == 0;
}

Script script_from_env()
{
    Script s;
    if (const char *c = std::getenv("CASSETTE_DEMO_CARD"); c && *c) {
        const std::string v = c;
        if (v == "level" || v == "plain" || v == "both" || v == "speech" || v == "stereo" || v == "rich" || v == "none") s.cards = v;
    }
    s.unplug_ms = env_ms("CASSETTE_DEMO_UNPLUG_MS");
    s.replug_ms = env_ms("CASSETTE_DEMO_REPLUG_MS");
    return s;
}

std::unique_ptr<AlsaApi> make_demo_alsa(const Script &script)
{
    return std::unique_ptr<AlsaApi>(new DemoAlsa(script));
}

std::string demo_home(std::string *error)
{
    std::string home;
    if (const char *h = std::getenv("CASSETTE_DEMO_HOME"); h && *h) {
        home = h;
        if (home[0] != '/') {
            const char *base = std::getenv("APPLAUNCH_STATE_DIR");
            char cwd[PATH_MAX];
            std::string b = base && *base ? base : (::getcwd(cwd, sizeof cwd) ? cwd : ".");
            home = b + "/" + home;
        }
        const int r = mkdirs(home);
        if (r < 0) {
            if (error) *error = "cannot create " + home + ": " + std::strerror(-r);
            return "";
        }
        return home;
    }
    const char *tmp = std::getenv("TMPDIR");
    std::string templ = std::string(tmp && *tmp ? tmp : "/tmp") + "/cassette-demo-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back(0);
    if (!::mkdtemp(buf.data())) {
        if (error) *error = "cannot create " + templ + ": " + std::strerror(errno);
        return "";
    }
    return buf.data();
}

} // namespace demo
} // namespace cassette
