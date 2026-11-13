/*
 * SPDX-License-Identifier: MIT
 *
 * Copied from Cassette (apps/cassette at origin/main d1cf53f) for Vinyl, report 059 decision D6; Vinyl changes are marked VINYL.
 *
 * The production AlsaApi: libasound.so.2 loaded with dlopen at the first call that needs it, symbols with dlsym, so the app
 * has no link-time dependency and starts on a board without ALSA (calls then return kNoAlsa). The opaque ALSA types are
 * declared here (the C ABI is stable); no alsa header is needed to build.
 * Playback (task T5) adds snd_pcm_writei, snd_pcm_delay, snd_pcm_drop and snd_pcm_drain on the playback stream.
 * Card listing needs no library: it reads /proc/asound, /dev/snd and sysfs.
 * The ALSA calls are not exercised by the PC tests (no hardware): the smoke test and the probe run them on a real card.
 */
#include "alsa_api.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <mutex>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace vinyl {

namespace {

struct snd_pcm_t;
struct snd_pcm_hw_params_t;
struct snd_hctl_t;
struct snd_hctl_elem_t;
struct snd_ctl_t;
struct snd_ctl_elem_id_t;
struct snd_ctl_elem_info_t;
struct snd_ctl_elem_value_t;
using uframes = unsigned long;
using sframes = long;

constexpr int kStreamPlayback = 0;
constexpr int kStreamCapture = 1;
constexpr int kAccessRwInterleaved = 3;
constexpr int kFmtS16LE = 2, kFmtS32LE = 10, kFmtS24_3LE = 32;
constexpr int kIfaceMixer = 2;
constexpr int kTypeBoolean = 1, kTypeInteger = 2;

struct Lib {
    void *h = nullptr;
    std::string why;
    int (*pcm_open)(snd_pcm_t **, const char *, int, int) = nullptr;
    int (*pcm_close)(snd_pcm_t *) = nullptr;
    int (*hw_malloc)(snd_pcm_hw_params_t **) = nullptr;
    void (*hw_free)(snd_pcm_hw_params_t *) = nullptr;
    int (*hw_any)(snd_pcm_t *, snd_pcm_hw_params_t *) = nullptr;
    int (*hw_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int) = nullptr;
    int (*hw_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int) = nullptr;
    int (*hw_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned) = nullptr;
    int (*hw_set_rate)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int) = nullptr;
    int (*hw_set_period_near)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *, int *) = nullptr;
    int (*hw_set_buffer_near)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *) = nullptr;
    int (*hw_apply)(snd_pcm_t *, snd_pcm_hw_params_t *) = nullptr;
    int (*hw_get_period)(const snd_pcm_hw_params_t *, uframes *, int *) = nullptr;
    int (*hw_get_buffer)(const snd_pcm_hw_params_t *, uframes *) = nullptr;
    int (*hw_test_rate)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int) = nullptr;
    int (*hw_test_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int) = nullptr;
    int (*hw_ch_min)(const snd_pcm_hw_params_t *, unsigned *) = nullptr;
    int (*hw_ch_max)(const snd_pcm_hw_params_t *, unsigned *) = nullptr;
    sframes (*readi)(snd_pcm_t *, void *, uframes) = nullptr;
    int (*recover)(snd_pcm_t *, int, int) = nullptr;
    int (*prepare)(snd_pcm_t *) = nullptr;
    sframes (*writei)(snd_pcm_t *, const void *, uframes) = nullptr;
    int (*pcm_delay)(snd_pcm_t *, sframes *) = nullptr;
    int (*pcm_drop)(snd_pcm_t *) = nullptr;
    int (*pcm_drain)(snd_pcm_t *) = nullptr;
    int (*hctl_open)(snd_hctl_t **, const char *, int) = nullptr;
    int (*hctl_close)(snd_hctl_t *) = nullptr;
    int (*hctl_load)(snd_hctl_t *) = nullptr;
    snd_hctl_elem_t *(*hctl_first)(snd_hctl_t *) = nullptr;
    snd_hctl_elem_t *(*hctl_next)(snd_hctl_elem_t *) = nullptr;
    snd_ctl_t *(*hctl_ctl)(snd_hctl_t *) = nullptr;
    void (*elem_get_id)(const snd_hctl_elem_t *, snd_ctl_elem_id_t *) = nullptr;
    int (*elem_info)(snd_hctl_elem_t *, snd_ctl_elem_info_t *) = nullptr;
    int (*elem_read)(snd_hctl_elem_t *, snd_ctl_elem_value_t *) = nullptr;
    int (*elem_write)(snd_hctl_elem_t *, snd_ctl_elem_value_t *) = nullptr;
    int (*id_malloc)(snd_ctl_elem_id_t **) = nullptr;
    void (*id_free)(snd_ctl_elem_id_t *) = nullptr;
    const char *(*id_name)(const snd_ctl_elem_id_t *) = nullptr;
    int (*id_iface)(const snd_ctl_elem_id_t *) = nullptr;
    int (*info_malloc)(snd_ctl_elem_info_t **) = nullptr;
    void (*info_free)(snd_ctl_elem_info_t *) = nullptr;
    int (*info_type)(const snd_ctl_elem_info_t *) = nullptr;
    unsigned (*info_count)(const snd_ctl_elem_info_t *) = nullptr;
    long (*info_min)(const snd_ctl_elem_info_t *) = nullptr;
    long (*info_max)(const snd_ctl_elem_info_t *) = nullptr;
    long (*info_step)(const snd_ctl_elem_info_t *) = nullptr;
    int (*info_readable)(const snd_ctl_elem_info_t *) = nullptr;
    int (*info_writable)(const snd_ctl_elem_info_t *) = nullptr;
    int (*val_malloc)(snd_ctl_elem_value_t **) = nullptr;
    void (*val_free)(snd_ctl_elem_value_t *) = nullptr;
    long (*val_get_int)(const snd_ctl_elem_value_t *, unsigned) = nullptr;
    void (*val_set_int)(snd_ctl_elem_value_t *, unsigned, long) = nullptr;
    int (*val_get_bool)(const snd_ctl_elem_value_t *, unsigned) = nullptr;
    void (*val_set_bool)(snd_ctl_elem_value_t *, unsigned, int) = nullptr;
    int (*get_db_range)(snd_ctl_t *, const snd_ctl_elem_id_t *, long *, long *) = nullptr;
    int (*convert_to_db)(snd_ctl_t *, const snd_ctl_elem_id_t *, long, long *) = nullptr; /* VINYL, optional */

    bool load() {
        if (h) return true;
        if (!why.empty()) return false;
        h = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            const char *e = ::dlerror();
            why = std::string("libasound.so.2 is missing (") + (e ? e : "dlopen failed") + ")";
            return false;
        }
        bool ok = true;
        auto sym = [&](auto &fn, const char *name) {
            void *p = ::dlsym(h, name);
            if (!p) {
                ok = false;
                if (why.empty()) why = std::string("libasound.so.2 lacks ") + name;
            }
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(p);
        };
        sym(pcm_open, "snd_pcm_open");
        sym(pcm_close, "snd_pcm_close");
        sym(hw_malloc, "snd_pcm_hw_params_malloc");
        sym(hw_free, "snd_pcm_hw_params_free");
        sym(hw_any, "snd_pcm_hw_params_any");
        sym(hw_set_access, "snd_pcm_hw_params_set_access");
        sym(hw_set_format, "snd_pcm_hw_params_set_format");
        sym(hw_set_channels, "snd_pcm_hw_params_set_channels");
        sym(hw_set_rate, "snd_pcm_hw_params_set_rate");
        sym(hw_set_period_near, "snd_pcm_hw_params_set_period_size_near");
        sym(hw_set_buffer_near, "snd_pcm_hw_params_set_buffer_size_near");
        sym(hw_apply, "snd_pcm_hw_params");
        sym(hw_get_period, "snd_pcm_hw_params_get_period_size");
        sym(hw_get_buffer, "snd_pcm_hw_params_get_buffer_size");
        sym(hw_test_rate, "snd_pcm_hw_params_test_rate");
        sym(hw_test_format, "snd_pcm_hw_params_test_format");
        sym(hw_ch_min, "snd_pcm_hw_params_get_channels_min");
        sym(hw_ch_max, "snd_pcm_hw_params_get_channels_max");
        sym(readi, "snd_pcm_readi");
        sym(recover, "snd_pcm_recover");
        sym(prepare, "snd_pcm_prepare");
        sym(writei, "snd_pcm_writei");
        sym(pcm_delay, "snd_pcm_delay");
        sym(pcm_drop, "snd_pcm_drop");
        sym(pcm_drain, "snd_pcm_drain");
        sym(hctl_open, "snd_hctl_open");
        sym(hctl_close, "snd_hctl_close");
        sym(hctl_load, "snd_hctl_load");
        sym(hctl_first, "snd_hctl_first_elem");
        sym(hctl_next, "snd_hctl_elem_next");
        sym(hctl_ctl, "snd_hctl_ctl");
        sym(elem_get_id, "snd_hctl_elem_get_id");
        sym(elem_info, "snd_hctl_elem_info");
        sym(elem_read, "snd_hctl_elem_read");
        sym(elem_write, "snd_hctl_elem_write");
        sym(id_malloc, "snd_ctl_elem_id_malloc");
        sym(id_free, "snd_ctl_elem_id_free");
        sym(id_name, "snd_ctl_elem_id_get_name");
        sym(id_iface, "snd_ctl_elem_id_get_interface");
        sym(info_malloc, "snd_ctl_elem_info_malloc");
        sym(info_free, "snd_ctl_elem_info_free");
        sym(info_type, "snd_ctl_elem_info_get_type");
        sym(info_count, "snd_ctl_elem_info_get_count");
        sym(info_min, "snd_ctl_elem_info_get_min");
        sym(info_max, "snd_ctl_elem_info_get_max");
        sym(info_step, "snd_ctl_elem_info_get_step");
        sym(info_readable, "snd_ctl_elem_info_is_readable");
        sym(info_writable, "snd_ctl_elem_info_is_writable");
        sym(val_malloc, "snd_ctl_elem_value_malloc");
        sym(val_free, "snd_ctl_elem_value_free");
        sym(val_get_int, "snd_ctl_elem_value_get_integer");
        sym(val_set_int, "snd_ctl_elem_value_set_integer");
        sym(val_get_bool, "snd_ctl_elem_value_get_boolean");
        sym(val_set_bool, "snd_ctl_elem_value_set_boolean");
        sym(get_db_range, "snd_ctl_get_dB_range");
        convert_to_db = reinterpret_cast<decltype(convert_to_db)>(::dlsym(h, "snd_ctl_convert_to_dB")); /* VINYL, optional */
        if (!ok) {
            ::dlclose(h);
            h = nullptr;
        }
        return ok;
    }
};

int to_alsa_fmt(SampleFormat f) {
    switch (f) {
    case SampleFormat::S16_LE: return kFmtS16LE;
    case SampleFormat::S24_3LE: return kFmtS24_3LE;
    case SampleFormat::S32_LE: return kFmtS32LE;
    }
    return kFmtS16LE;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string slurp_line(const std::string &path) {
    std::string out;
    if (FILE *f = std::fopen(path.c_str(), "r")) {
        char buf[256];
        if (std::fgets(buf, sizeof buf, f)) out = buf;
        std::fclose(f);
    }
    return trim(out);
}

class RealPcm : public AlsaPcm {
public:
    RealPcm(Lib *l, snd_pcm_t *p) : l_(l), p_(p) {}
    ~RealPcm() override { close(); }
    long readi(void *buf, size_t frames) override { return p_ ? l_->readi(p_, buf, frames) : -ENODEV; }
    int recover(int err) override { return p_ ? l_->recover(p_, err, 1) : -ENODEV; }
    int prepare() override { return p_ ? l_->prepare(p_) : -ENODEV; }
    long writei(const void *buf, size_t frames) override { return p_ ? l_->writei(p_, buf, frames) : -ENODEV; }
    long delay() override {
        if (!p_) return -ENODEV;
        sframes d = 0;
        int r = l_->pcm_delay(p_, &d);
        return r < 0 ? r : (d < 0 ? 0 : d);
    }
    int drop() override { return p_ ? l_->pcm_drop(p_) : -ENODEV; }
    int drain() override { return p_ ? l_->pcm_drain(p_) : -ENODEV; }
    void close() override {
        if (p_) {
            l_->pcm_close(p_);
            p_ = nullptr;
        }
    }

private:
    Lib *l_;
    snd_pcm_t *p_;
};

struct Hctl {
    Lib *l;
    snd_hctl_t *h = nullptr;
    explicit Hctl(Lib *lib) : l(lib) {}
    Hctl(const Hctl &) = delete;
    Hctl &operator=(const Hctl &) = delete;
    int open(const std::string &card) {
        int r = l->hctl_open(&h, card.c_str(), 0);
        if (r < 0) {
            h = nullptr;
            return r;
        }
        r = l->hctl_load(h);
        if (r < 0) {
            l->hctl_close(h);
            h = nullptr;
        }
        return r;
    }
    ~Hctl() {
        if (h) l->hctl_close(h);
    }
    /* The MIXER element called `name`; null when absent. */
    snd_hctl_elem_t *find(const std::string &name) {
        snd_ctl_elem_id_t *id = nullptr;
        if (l->id_malloc(&id) < 0) return nullptr;
        snd_hctl_elem_t *hit = nullptr;
        for (auto *e = l->hctl_first(h); e; e = l->hctl_next(e)) {
            l->elem_get_id(e, id);
            if (l->id_iface(id) == kIfaceMixer && name == l->id_name(id)) {
                hit = e;
                break;
            }
        }
        l->id_free(id);
        return hit;
    }
};

class RealAlsa : public AlsaApi {
public:
    bool available(std::string *why) override {
        std::lock_guard<std::mutex> g(mu_);
        bool ok = lib_.load();
        if (!ok && why) *why = lib_.why;
        return ok;
    }

    std::vector<CardInfo> cards() override {
        std::vector<CardInfo> out;
        FILE *f = std::fopen("/proc/asound/cards", "r");
        if (!f) return out;
        char buf[512];
        CardInfo cur;
        bool have = false;
        auto flush = [&] {
            if (have) out.push_back(cur);
            cur = CardInfo();
            have = false;
        };
        while (std::fgets(buf, sizeof buf, f)) {
            int idx = -1, n = 0;
            char id[64] = {0};
            /* " 0 [Device         ]: USB-Audio - USB PnP Sound Device" */
            if (std::sscanf(buf, " %d [%63[^]]]: %n", &idx, id, &n) >= 2 && n > 0) {
                flush();
                have = true;
                cur.index = idx;
                cur.id = trim(id);
                std::string rest = trim(buf + n);
                size_t dash = rest.find(" - ");
                cur.name = dash == std::string::npos ? rest : trim(rest.substr(dash + 3));
            } else if (have && cur.long_name.empty()) {
                cur.long_name = trim(buf);
            }
        }
        flush();
        std::fclose(f);
        for (auto &c : out) fill_identity(c);
        return out;
    }

    int query_capture(const std::string &device, CaptureOffer &out) override {
        Lib *l = lib();
        if (!l) return kNoAlsa;
        snd_pcm_t *pcm = nullptr;
        int r = l->pcm_open(&pcm, device.c_str(), kStreamCapture, 0);
        if (r < 0) return r;
        snd_pcm_hw_params_t *hw = nullptr;
        if (l->hw_malloc(&hw) < 0) {
            l->pcm_close(pcm);
            return -ENOMEM;
        }
        r = l->hw_any(pcm, hw);
        if (r >= 0) {
            out = CaptureOffer();
            static const unsigned kRates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000};
            for (unsigned rate : kRates)
                if (l->hw_test_rate(pcm, hw, rate, 0) == 0) out.rates.push_back(rate);
            const SampleFormat fm[] = {SampleFormat::S16_LE, SampleFormat::S24_3LE, SampleFormat::S32_LE};
            for (SampleFormat f : fm)
                if (l->hw_test_format(pcm, hw, to_alsa_fmt(f)) == 0) out.formats.push_back(f);
            unsigned cmin = 0, cmax = 0;
            l->hw_ch_min(hw, &cmin);
            l->hw_ch_max(hw, &cmax);
            out.min_channels = cmin;
            out.max_channels = cmax;
        }
        l->hw_free(hw);
        l->pcm_close(pcm);
        return r < 0 ? r : 0;
    }

    int open_capture(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                     HwGranted &granted) override {
        Lib *l = lib();
        if (!l) return kNoAlsa;
        snd_pcm_t *p = nullptr;
        int r = l->pcm_open(&p, device.c_str(), kStreamCapture, 0);
        if (r < 0) return r;
        snd_pcm_hw_params_t *hw = nullptr;
        if (l->hw_malloc(&hw) < 0) {
            l->pcm_close(p);
            return -ENOMEM;
        }
        uframes period = req.period_frames, buffer = req.buffer_frames;
        int dir = 0;
        r = l->hw_any(p, hw);
        if (r >= 0) r = l->hw_set_access(p, hw, kAccessRwInterleaved);
        if (r >= 0) r = l->hw_set_format(p, hw, to_alsa_fmt(req.format));
        if (r >= 0) r = l->hw_set_channels(p, hw, req.channels);
        if (r >= 0) r = l->hw_set_rate(p, hw, req.rate, 0); /* exact: no resampling decision here */
        if (r >= 0) r = l->hw_set_period_near(p, hw, &period, &dir);
        if (r >= 0) r = l->hw_set_buffer_near(p, hw, &buffer);
        if (r >= 0) r = l->hw_apply(p, hw);
        if (r >= 0) {
            uframes gp = 0, gb = 0;
            int d = 0;
            l->hw_get_period(hw, &gp, &d);
            l->hw_get_buffer(hw, &gb);
            granted.rate = req.rate;
            granted.channels = req.channels;
            granted.format = req.format;
            granted.period_frames = static_cast<uint32_t>(gp);
            granted.buffer_frames = static_cast<uint32_t>(gb);
        }
        l->hw_free(hw);
        if (r < 0) {
            l->pcm_close(p);
            return r;
        }
        pcm.reset(new RealPcm(l, p));
        return 0;
    }

    /* Playback: the same steps as the capture calls above, on the playback stream (the capture code is left as it was). */
    int query_playback(const std::string &device, PlaybackOffer &out) override {
        Lib *l = lib();
        if (!l) return kNoAlsa;
        snd_pcm_t *pcm = nullptr;
        int r = l->pcm_open(&pcm, device.c_str(), kStreamPlayback, 0);
        if (r < 0) return r;
        snd_pcm_hw_params_t *hw = nullptr;
        if (l->hw_malloc(&hw) < 0) {
            l->pcm_close(pcm);
            return -ENOMEM;
        }
        r = l->hw_any(pcm, hw);
        if (r >= 0) {
            out = PlaybackOffer();
            static const unsigned kRates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000};
            for (unsigned rate : kRates)
                if (l->hw_test_rate(pcm, hw, rate, 0) == 0) out.rates.push_back(rate);
            const SampleFormat fm[] = {SampleFormat::S16_LE, SampleFormat::S24_3LE, SampleFormat::S32_LE};
            for (SampleFormat f : fm)
                if (l->hw_test_format(pcm, hw, to_alsa_fmt(f)) == 0) out.formats.push_back(f);
            unsigned cmin = 0, cmax = 0;
            l->hw_ch_min(hw, &cmin);
            l->hw_ch_max(hw, &cmax);
            out.min_channels = cmin;
            out.max_channels = cmax;
        }
        l->hw_free(hw);
        l->pcm_close(pcm);
        return r < 0 ? r : 0;
    }

    int open_playback(const std::string &device, const HwRequest &req, std::unique_ptr<AlsaPcm> &pcm,
                      HwGranted &granted) override {
        Lib *l = lib();
        if (!l) return kNoAlsa;
        snd_pcm_t *p = nullptr;
        int r = l->pcm_open(&p, device.c_str(), kStreamPlayback, 0);
        if (r < 0) return r;
        snd_pcm_hw_params_t *hw = nullptr;
        if (l->hw_malloc(&hw) < 0) {
            l->pcm_close(p);
            return -ENOMEM;
        }
        uframes period = req.period_frames, buffer = req.buffer_frames;
        int dir = 0;
        r = l->hw_any(p, hw);
        if (r >= 0) r = l->hw_set_access(p, hw, kAccessRwInterleaved);
        if (r >= 0) r = l->hw_set_format(p, hw, to_alsa_fmt(req.format));
        if (r >= 0) r = l->hw_set_channels(p, hw, req.channels);
        if (r >= 0) r = l->hw_set_rate(p, hw, req.rate, 0);
        if (r >= 0) r = l->hw_set_period_near(p, hw, &period, &dir);
        if (r >= 0) r = l->hw_set_buffer_near(p, hw, &buffer);
        if (r >= 0) r = l->hw_apply(p, hw);
        if (r >= 0) {
            uframes gp = 0, gb = 0;
            int d = 0;
            l->hw_get_period(hw, &gp, &d);
            l->hw_get_buffer(hw, &gb);
            granted.rate = req.rate;
            granted.channels = req.channels;
            granted.format = req.format;
            granted.period_frames = static_cast<uint32_t>(gp);
            granted.buffer_frames = static_cast<uint32_t>(gb);
        }
        l->hw_free(hw);
        if (r < 0) {
            l->pcm_close(p);
            return r;
        }
        pcm.reset(new RealPcm(l, p));
        return 0;
    }

    int mixer_list(const std::string &card, std::vector<CtlInfo> &out) override {
        Lib *l = lib();
        if (!l) return kNoAlsa;
        Hctl hc(l);
        int r = hc.open(card);
        if (r < 0) return r;
        snd_ctl_elem_id_t *id = nullptr;
        snd_ctl_elem_info_t *info = nullptr;
        if (l->id_malloc(&id) < 0) return -ENOMEM;
        if (l->info_malloc(&info) < 0) {
            l->id_free(id);
            return -ENOMEM;
        }
        out.clear();
        for (auto *e = l->hctl_first(hc.h); e; e = l->hctl_next(e)) {
            l->elem_get_id(e, id);
            if (l->id_iface(id) != kIfaceMixer) continue;
            if (l->elem_info(e, info) < 0) continue;
            CtlInfo c;
            c.name = l->id_name(id);
            int t = l->info_type(info);
            c.type = t == kTypeInteger ? CtlInfo::Type::Integer
                                       : (t == kTypeBoolean ? CtlInfo::Type::Boolean : CtlInfo::Type::Other);
            c.capture = c.name.find("Capture") != std::string::npos;
            c.readable = l->info_readable(info) != 0;
            c.writable = l->info_writable(info) != 0;
            c.count = static_cast<int>(l->info_count(info));
            if (c.type == CtlInfo::Type::Integer) {
                c.min = l->info_min(info);
                c.max = l->info_max(info);
                c.step = l->info_step(info);
                long dmin = 0, dmax = 0;
                /* fails when the driver gives no dB data (the headset: the kernel disables its "bogus dB values") */
                if (l->get_db_range(l->hctl_ctl(hc.h), id, &dmin, &dmax) >= 0 && dmax > dmin) {
                    c.has_db = true;
                    c.db_min_cdb = dmin;
                    c.db_max_cdb = dmax;
                    if (dmin <= kAlsaDbGainMute) { /* VINYL: a dB scale with its mute flag: the real dB of min back */
                        long dnext = 0;
                        const bool known = l->convert_to_db && c.max > c.min + 1 &&
                                           l->convert_to_db(l->hctl_ctl(hc.h), id, c.min + 1, &dnext) >= 0;
                        alsa_fix_mute_db(c, known, dnext);
                    }
                }
            }
            out.push_back(c);
        }
        l->info_free(info);
        l->id_free(id);
        return 0;
    }

    int mixer_get(const std::string &card, const std::string &name, long &value) override {
        return rw(card, name, value, false);
    }
    int mixer_set(const std::string &card, const std::string &name, long value) override {
        return rw(card, name, value, true);
    }

private:
    Lib *lib() {
        std::lock_guard<std::mutex> g(mu_);
        return lib_.load() ? &lib_ : nullptr;
    }

    int rw(const std::string &card, const std::string &name, long &value, bool write) {
        Lib *l = lib();
        if (!l) return kNoAlsa;
        Hctl hc(l);
        int r = hc.open(card);
        if (r < 0) return r;
        snd_hctl_elem_t *e = hc.find(name);
        if (!e) return -ENOENT;
        snd_ctl_elem_info_t *info = nullptr;
        snd_ctl_elem_value_t *val = nullptr;
        if (l->info_malloc(&info) < 0) return -ENOMEM;
        if (l->val_malloc(&val) < 0) {
            l->info_free(info);
            return -ENOMEM;
        }
        r = l->elem_info(e, info);
        if (r >= 0) {
            int type = l->info_type(info);
            unsigned count = l->info_count(info);
            if (type != kTypeBoolean && type != kTypeInteger) {
                r = -EINVAL;
            } else if (!write) {
                r = l->elem_read(e, val);
                if (r >= 0) value = type == kTypeBoolean ? l->val_get_bool(val, 0) : l->val_get_int(val, 0);
            } else {
                r = l->elem_read(e, val); /* start from the current values */
                if (r >= 0) {
                    for (unsigned i = 0; i < count; ++i) {
                        if (type == kTypeBoolean) l->val_set_bool(val, i, value ? 1 : 0);
                        else l->val_set_int(val, i, value);
                    }
                    r = l->elem_write(e, val);
                }
            }
        }
        l->val_free(val);
        l->info_free(info);
        return r < 0 ? r : 0;
    }

    /* usb id, by-id link, USB port path and capture presence, without libasound. The capture PCM is assumed to be DEV=0. */
    void fill_identity(CardInfo &c) {
        const std::string base = "/proc/asound/card" + std::to_string(c.index) + "/";
        c.usb_id = slurp_line(base + "usbid");
        if (DIR *d = ::opendir("/dev/snd")) {
            const std::string want = "pcmC" + std::to_string(c.index) + "D";
            while (dirent *e = ::readdir(d)) {
                std::string n = e->d_name;
                if (n.compare(0, want.size(), want) == 0 && n.size() > want.size() && n.back() == 'c') c.capture = true;
                if (n.compare(0, want.size(), want) == 0 && n.size() > want.size() && n.back() == 'p') c.playback = true;
            }
            ::closedir(d);
        }
        if (DIR *d = ::opendir("/dev/snd/by-id")) {
            const std::string tail = "controlC" + std::to_string(c.index);
            while (dirent *e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                std::string p = std::string("/dev/snd/by-id/") + e->d_name;
                char tgt[PATH_MAX];
                ssize_t n = ::readlink(p.c_str(), tgt, sizeof tgt - 1);
                if (n <= 0) continue;
                tgt[n] = 0;
                std::string t = tgt;
                if (t.size() >= tail.size() && t.compare(t.size() - tail.size(), tail.size(), tail) == 0 &&
                    (c.usb_by_id.empty() || p < c.usb_by_id))
                    c.usb_by_id = p;
            }
            ::closedir(d);
        }
        if (!c.usb_id.empty()) {
            const std::string link = "/sys/class/sound/card" + std::to_string(c.index) + "/device";
            char real[PATH_MAX];
            if (::realpath(link.c_str(), real)) {
                std::string leaf = real;
                size_t s = leaf.rfind('/');
                if (s != std::string::npos) leaf = leaf.substr(s + 1);
                size_t colon = leaf.find(':');
                if (colon != std::string::npos) leaf = leaf.substr(0, colon); /* "1-1.2:1.0" -> "1-1.2" */
                if (!leaf.empty() && leaf.find('-') != std::string::npos) c.usb_port = leaf;
            }
        }
    }

    std::mutex mu_;
    Lib lib_;
};

} // namespace

std::unique_ptr<AlsaApi> make_real_alsa() { return std::unique_ptr<AlsaApi>(new RealAlsa()); }

} // namespace vinyl
