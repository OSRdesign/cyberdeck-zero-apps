/* SPDX-License-Identifier: MIT */
#include "fake_alsa.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

namespace cassette {

namespace {

class FakePcm : public AlsaPcm {
public:
    FakePcm(FakeAlsa *f, const FakeCard *spec, const HwGranted &hw)
        : f_(f), hw_(hw), short_every_(spec ? spec->short_read_every : 0), zero_(spec && spec->zero_reads) {
        if (spec) events_ = spec->events;
        std::sort(events_.begin(), events_.end(),
                  [](const FakeEvent &a, const FakeEvent &b) { return a.at_frame < b.at_frame; });
    }

    long readi(void *buf, size_t frames) override {
        if (closed_) return -ENODEV;
        if (dead_ != 0) {
            ++f_->reads_after_error;
            return dead_;
        }
        if (xrun_pending_) return -EPIPE; /* until recover() */
        if (next_ < events_.size() && events_[next_].at_frame <= delivered_) {
            const FakeEvent e = events_[next_++];
            if (e.kind == FakeEvent::Xrun) {
                f_->clock += e.gap_s;
                xrun_pending_ = true;
                return -EPIPE;
            }
            dead_ = e.err;
            return dead_;
        }
        if (zero_) return 0;
        size_t n = frames;
        ++reads_;
        if (short_every_ && reads_ % short_every_ == 0) n = std::max<size_t>(1, n / 2);
        if (next_ < events_.size()) n = std::min<size_t>(n, events_[next_].at_frame - delivered_);
        fill(buf, n);
        delivered_ += n;
        f_->clock += static_cast<double>(n) / hw_.rate;
        if (f_->pace) std::this_thread::sleep_for(std::chrono::duration<double>(static_cast<double>(n) / hw_.rate));
        else std::this_thread::sleep_for(std::chrono::microseconds(20));
        return static_cast<long>(n);
    }
    int recover(int err) override {
        if (err == -EPIPE && xrun_pending_) {
            xrun_pending_ = false;
            return 0;
        }
        return err;
    }
    int prepare() override { return closed_ ? -ENODEV : 0; }
    void close() override { closed_ = true; }

private:
    /* A counter pattern: sample value of frame i is (i & 0x7fff) on every channel (S16); S24_3 the low 24 bits; S32 the
     * counter in the top 24 bits and 0xAA in the low byte (the converter must drop that byte). */
    void fill(void *buf, size_t n) {
        uint8_t *p = static_cast<uint8_t *>(buf);
        for (size_t i = 0; i < n; ++i) {
            uint32_t v = static_cast<uint32_t>(delivered_ + i);
            for (unsigned c = 0; c < hw_.channels; ++c) {
                switch (hw_.format) {
                case SampleFormat::S16_LE:
                    *p++ = v & 0xff;
                    *p++ = (v >> 8) & 0x7f;
                    break;
                case SampleFormat::S24_3LE:
                    *p++ = v & 0xff;
                    *p++ = (v >> 8) & 0xff;
                    *p++ = (v >> 16) & 0xff;
                    break;
                case SampleFormat::S32_LE:
                    *p++ = 0xAA;
                    *p++ = v & 0xff;
                    *p++ = (v >> 8) & 0xff;
                    *p++ = (v >> 16) & 0xff;
                    break;
                }
            }
        }
    }

    FakeAlsa *f_;
    HwGranted hw_;
    std::vector<FakeEvent> events_;
    size_t next_ = 0;
    uint64_t delivered_ = 0;
    unsigned reads_ = 0, short_every_;
    bool zero_;
    bool closed_ = false, xrun_pending_ = false;
    int dead_ = 0;
};

/* "hw:CARD=Device,DEV=0" or "hw:CARD=Device" -> "Device"; "" otherwise */
std::string card_id_of(const std::string &dev) {
    const std::string key = "hw:CARD=";
    if (dev.compare(0, key.size(), key) != 0) return "";
    std::string rest = dev.substr(key.size());
    return rest.substr(0, rest.find(','));
}

} // namespace

bool FakeAlsa::available(std::string *why) {
    if (!lib_available && why) *why = "libasound.so.2 is missing (fake)";
    return lib_available;
}

std::vector<CardInfo> FakeAlsa::cards() {
    std::vector<CardInfo> out;
    for (const FakeCard &c : cards_)
        if (c.present) out.push_back(c.info);
    return out;
}

FakeCard *FakeAlsa::find_id(const std::string &id) {
    for (FakeCard &c : cards_)
        if (c.present && c.info.id == id) return &c;
    return nullptr;
}

void FakeAlsa::reboot_shuffle() {
    if (cards_.size() < 2) return;
    std::vector<int> idx;
    for (auto &c : cards_) idx.push_back(c.info.index);
    std::rotate(idx.begin(), idx.begin() + 1, idx.end());
    for (size_t i = 0; i < cards_.size(); ++i) cards_[i].info.index = idx[i];
    std::sort(cards_.begin(), cards_.end(), [](const FakeCard &a, const FakeCard &b) { return a.info.index < b.info.index; });
}

int FakeAlsa::query_capture(const std::string &device, CaptureOffer &out) {
    if (!lib_available) return kNoAlsa;
    queried.push_back(device);
    FakeCard *c = find_id(card_id_of(device));
    if (!c) return -ENOENT;
    if (c->open_error) return c->open_error;
    if (c->busy) return -EBUSY;
    out = c->offer;
    return 0;
}

int FakeAlsa::open_capture(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                           HwGranted &granted) {
    if (!lib_available) return kNoAlsa;
    opened.push_back(device);
    requests.push_back(req);
    const FakeCard *spec = nullptr;
    if (device == "default") {
        if (server_error) return server_error;
    } else {
        FakeCard *c = find_id(card_id_of(device));
        if (!c) return -ENOENT;
        if (c->open_error) return c->open_error;
        if (c->busy) return -EBUSY;
        if (!c->offer.has_rate(req.rate) || !c->offer.has_format(req.format) || req.channels < c->offer.min_channels ||
            req.channels > c->offer.max_channels)
            return -EINVAL; /* hw: does no conversion */
        spec = c;
    }
    granted.rate = req.rate;
    granted.channels = req.channels;
    granted.format = req.format;
    granted.period_frames = req.period_frames;
    granted.buffer_frames = req.buffer_frames;
    pcm.reset(new FakePcm(this, spec, granted));
    return 0;
}

int FakeAlsa::mixer_list(const std::string &card, std::vector<CtlInfo> &out) {
    if (!lib_available) return kNoAlsa;
    FakeCard *c = find_id(card_id_of(card));
    if (!c) return -ENOENT;
    out = c->ctls;
    return 0;
}

int FakeAlsa::mixer_get(const std::string &card, const std::string &name, long &value) {
    FakeCard *c = find_id(card_id_of(card));
    if (!c) return -ENOENT;
    auto it = c->values.find(name);
    if (it == c->values.end()) return -ENOENT;
    value = it->second;
    return 0;
}

int FakeAlsa::mixer_set(const std::string &card, const std::string &name, long value) {
    FakeCard *c = find_id(card_id_of(card));
    if (!c) return -ENOENT;
    for (const CtlInfo &i : c->ctls)
        if (i.name == name) {
            if (!i.writable) return -EPERM;
            if (i.type == CtlInfo::Type::Boolean ? (value != 0 && value != 1) : (value < i.min || value > i.max))
                return -EINVAL;
            c->values[name] = value;
            mixer_writes.emplace_back(name, value);
            return 0;
        }
    return -ENOENT;
}

namespace {
CtlInfo ctl_int(const std::string &name, long mn, long mx, bool db, long dbmin = 0, long dbmax = 0) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Integer;
    c.capture = name.find("Capture") != std::string::npos;
    c.min = mn;
    c.max = mx;
    c.has_db = db;
    c.db_min_cdb = dbmin;
    c.db_max_cdb = dbmax;
    return c;
}
CtlInfo ctl_bool(const std::string &name) {
    CtlInfo c;
    c.name = name;
    c.type = CtlInfo::Type::Boolean;
    c.capture = name.find("Capture") != std::string::npos;
    return c;
}
} // namespace

FakeCard fake_pcm2902(int index, const std::string &id, const std::string &port) {
    FakeCard c;
    c.info.index = index;
    c.info.id = id;
    c.info.name = "USB Audio CODEC";
    c.info.long_name = "Burr-Brown from TI USB Audio CODEC at usb-3f980000.usb-" + (port.size() > 2 ? port.substr(2) : port) + ", full speed";
    c.info.usb_by_id = "/dev/snd/by-id/usb-Burr-Brown_from_TI_USB_Audio_CODEC-00";
    c.info.usb_id = "08bb:2902";
    c.info.usb_port = port;
    c.info.capture = true;
    c.offer.rates = {44100, 48000};
    c.offer.min_channels = c.offer.max_channels = 1;
    c.offer.formats = {SampleFormat::S16_LE};
    c.ctls = {ctl_int("Mic Capture Volume", 0, 16, true, 0, 2381), ctl_bool("Mic Capture Switch"),
              ctl_bool("Auto Gain Control"), ctl_int("Mic Playback Volume", 0, 16, true, -3000, 0),
              ctl_bool("Mic Playback Switch")};
    c.values = {{"Mic Capture Volume", 0}, {"Mic Capture Switch", 1}, {"Auto Gain Control", 1},
                {"Mic Playback Volume", 0}, {"Mic Playback Switch", 0}};
    return c;
}

FakeCard fake_uc02(int index, const std::string &port) {
    FakeCard c;
    c.info.index = index;
    c.info.id = "UC02";
    c.info.name = "UC02";
    c.info.long_name = "UC02 UC02 at usb-3f980000.usb-" + (port.size() > 2 ? port.substr(2) : port) + ", full speed";
    c.info.usb_by_id = "/dev/snd/by-id/usb-UC02_UC02_UC02-00";
    c.info.usb_id = "0000:0000"; /* not unique: maker, product and serial are all "UC02" */
    c.info.usb_port = port;
    c.info.capture = true;
    c.offer.rates = {48000};
    c.offer.min_channels = c.offer.max_channels = 1;
    c.offer.formats = {SampleFormat::S16_LE};
    c.ctls = {ctl_int("Mic Capture Volume", 0, 127, false), ctl_bool("Mic Capture Switch")};
    c.values = {{"Mic Capture Volume", 0}, {"Mic Capture Switch", 1}};
    return c;
}

FakeCard fake_card_no_controls(int index) {
    FakeCard c = fake_uc02(index, "1-1.3");
    c.info.id = "Plain";
    c.info.name = "Plain Capture Card";
    c.info.usb_by_id = "/dev/snd/by-id/usb-Plain_Capture_Card-00";
    c.info.usb_id = "1234:5678";
    c.ctls.clear();
    c.values.clear();
    return c;
}

FakeAlsa fake_demo() {
    FakeAlsa f;
    f.cards_.push_back(fake_pcm2902(0));
    f.cards_.push_back(fake_uc02(1));
    f.cards_.push_back(fake_card_no_controls(2));
    f.cards_[1].busy = true; /* shows the server fallback */
    f.pace = true;
    return f;
}

} // namespace cassette
