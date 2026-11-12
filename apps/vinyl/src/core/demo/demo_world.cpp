/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl demonstration world (see demo_world.hpp).
 */
#include "demo_world.hpp"

#include "../state/resume.hpp"
#include "../util/atomic_file.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vinyl {

namespace {

void le16(std::string &s, uint16_t v) {
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>(v >> 8);
}
void le32(std::string &s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void info_item(std::string &s, const char *id, const std::string &v) {
    if (v.empty()) return;
    std::string body = v;
    body += '\0';
    if (body.size() & 1) body += '\0';
    s += id;
    le32(s, static_cast<uint32_t>(body.size()));
    s += body;
}

struct Track {
    const char *folder, *file, *title, *artist, *album, *date;
    int seconds;
};

/* the queue of the drafts (Queue-480x320, NowPlaying-800x480), completed to 20 tracks; about 1 h 24 min after the third */
const Track kTracks[] = {
    {"Hot Chip", "Hot Chip - Over and Over", "Over and Over", "Hot Chip", "The Warning", "2006", 342},
    {"Bag Raiders", "Bag Raiders - Shooting Stars", "Shooting Stars", "Bag Raiders", "Bag Raiders", "2010", 236},
    {"Phoenix", "Fences (LeMarquis Remix)", "Fences (LeMarquis Remix)", "Phoenix", "Fences Remixes", "2013", 244},
    {"Amerie", "Amerie - 1 Thing (Siik Remix)", "1 Thing (Siik Remix)", "Amerie", "1 Thing", "2005", 221},
    {"C2C", "C2C - Genius (Yuksek Remix)", "Genius (Yuksek Remix)", "C2C", "Tetra", "2012", 252},
    {"Calvin Harris", "Calvin Harris - The Girls", "The Girls", "Calvin Harris", "I Created Disco", "2007", 320},
    {"Daft Punk", "Daft Punk - Instant Crush", "Instant Crush", "Daft Punk", "Random Access Memories", "2013", 337},
    {"Dub Pistols", "Dub Pistols - Back to Daylight", "Back to Daylight", "Dub Pistols", "Return of the Pistoleros", "2012", 238},
    {"FKJ", "FKJ - Lying Together", "Lying Together", "FKJ", "French Kiwi Juice", "2017", 273},
    {"Flight Facilities", "Flight Facilities - Clair De Lune", "Clair De Lune", "Flight Facilities", "Down To Earth", "2014", 361},
    {"Ghost Beach", "Ghost Beach - Been There Before", "Been There Before", "Ghost Beach", "Blonde", "2014", 287},
    {"Lisa Mitchell", "Lisa Mitchell - Neopolitan Dreams", "Neopolitan Dreams (Lanny May's Neo Neo Mix)", "Lisa Mitchell", "Neopolitan Dreams", "2010", 394},
    {"Oxford", "Oxford - You and me", "You and me", "Oxford", "You and me", "2015", 338},
    {"Sister Sledge", "Sister Sledge - Thinking Of You", "Thinking Of You (Dimitri From Paris Remix)", "Sister Sledge", "Remixes", "1993", 155},
    {"Breakbot", "Breakbot - Baby I'm Yours", "Baby I'm Yours", "Breakbot", "By Your Side", "2012", 228},
    {"Chromeo", "Chromeo - Night by Night", "Night by Night", "Chromeo", "Business Casual", "2010", 266},
    {"Classixx", "Classixx - I'll Get You", "I'll Get You", "Classixx", "Hanging Gardens", "2013", 248},
    {"Darius", "Darius - Maliblue", "Maliblue", "Darius", "Velour", "2012", 250},
    {"Empire of the Sun", "Empire of the Sun - Walking on a Dream", "Walking on a Dream", "Empire of the Sun", "Walking on a Dream", "2008", 406},
    {"Hercules", "Hercules - You Belong", "You Belong", "Hercules and Love Affair", "Hercules and Love Affair", "2008", 305},
};

/* the "cjk" variant (file names and tags in Japanese, Chinese and Korean) */
const Track kCjkTracks[] = {
    {"東アジア", "久石譲 - あの夏へ", "あの夏へ", "久石譲", "千と千尋の神隠し サウンドトラック", "2001", 395},
    {"東アジア", "邓丽君 - 月亮代表我的心", "月亮代表我的心", "邓丽君", "岛国之情歌第四集", "1977", 207},
    {"東アジア/한국", "김광석 - 서른 즈음에", "서른 즈음에", "김광석", "김광석 네번째", "1994", 287},
};

int rm_cb(const char *p, const struct stat *, int, struct FTW *) { return ::remove(p); }

}  // namespace

bool write_demo_wav(const std::string &path, double seconds, uint32_t rate, const std::string &title, const std::string &artist,
                    const std::string &album, const std::string &date, int track) {
    std::string info = "INFO";
    info_item(info, "INAM", title);
    info_item(info, "IART", artist);
    info_item(info, "IPRD", album);
    info_item(info, "ICRD", date);
    if (track > 0) info_item(info, "ITRK", std::to_string(track));
    const uint32_t frames = static_cast<uint32_t>(seconds * rate);
    const uint32_t data = frames * 4;
    std::string h = "RIFF";
    le32(h, 4 + (8 + 16) + (8 + static_cast<uint32_t>(info.size())) + (8 + data));
    h += "WAVE";
    h += "fmt ";
    le32(h, 16);
    le16(h, 1);
    le16(h, 2);
    le32(h, rate);
    le32(h, rate * 4);
    le16(h, 4);
    le16(h, 16);
    h += "LIST";
    le32(h, static_cast<uint32_t>(info.size()));
    h += info;
    h += "data";
    le32(h, data);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, h.data(), h.size()) == static_cast<ssize_t>(h.size());
    ok = ok && ::ftruncate(fd, static_cast<off_t>(h.size() + data)) == 0; /* sparse: the silence is a hole */
    ::close(fd);
    return ok;
}

bool make_demo_world(DemoWorld &w, bool lite, const std::string &state, const std::string &variant) {
    const char *tmp = std::getenv("TMPDIR");
    std::string base = (tmp && *tmp) ? tmp : "/tmp";
    w.root = base + "/vinyl-demo-" + std::to_string(::getpid());
    w.music = w.root + "/Music";
    w.config = w.root + "/config";
    w.playlist = w.music + "/Playlist SKI.m3u";
    w.lite = lite;
    w.empty = variant == "empty";
    FileOps &ops = posix_file_ops();
    if (w.empty) return make_dirs(ops, w.config + "/vinyl") >= 0; /* no music folder, no saved state */
    if (make_dirs(ops, w.music) < 0 || make_dirs(ops, w.config + "/vinyl") < 0) return false;
    std::string m3u = "#EXTM3U\n";
    std::vector<std::string> items;
    int n = 0;
    for (const Track &t : kTracks) {
        const std::string dir = w.music + "/" + t.folder;
        make_dirs(ops, dir);
        const std::string path = dir + "/" + t.file + ".wav";
        write_demo_wav(path, t.seconds, 44100, t.title, t.artist, t.album, t.date, ++n);
        m3u += std::string(t.folder) + "/" + t.file + ".wav\n";
        items.push_back(path);
    }
    /* the Phoenix folder of the browser draft: a sub-folder and two more tracks */
    make_dirs(ops, w.music + "/Phoenix/Fences Remixes");
    write_demo_wav(w.music + "/Phoenix/1901.wav", 193, 44100, "1901", "Phoenix", "Wolfgang Amadeus Phoenix", "2009", 2);
    write_demo_wav(w.music + "/Phoenix/Lisztomania.wav", 241, 44100, "Lisztomania", "Phoenix", "Wolfgang Amadeus Phoenix", "2009", 1);
    atomic_write_file(ops, w.playlist, m3u);
    ResumeState s;
    s.items = items;
    for (size_t i = 0; i < items.size(); ++i) s.order.push_back(static_cast<int>(i));
    s.current = 2;
    s.position_ms = 82500;
    s.source = w.playlist;
    s.browse = w.music + "/Phoenix";
    s.repeat = Repeat::All; /* the drafts show the repeat mark */
    s.output = "auto";
    if (variant == "cjk") {
        s.items.clear();
        s.order.clear();
        int k = 0;
        for (const Track &t : kCjkTracks) {
            const std::string dir = w.music + "/" + t.folder;
            make_dirs(ops, dir);
            const std::string path = dir + "/" + t.file + ".wav";
            write_demo_wav(path, t.seconds, 44100, t.title, t.artist, t.album, t.date, ++k);
            s.order.push_back(static_cast<int>(s.items.size()));
            s.items.push_back(path);
        }
        s.current = 0;
        s.position_ms = 61000;
        s.source = w.music + "/" + kCjkTracks[0].folder;
        s.browse = s.source;
    }
    (void)state;
    return save_resume(w.config + "/vinyl/state.json", s) == 0;
}

void remove_demo_world(const DemoWorld &w) {
    if (w.root.empty() || w.root.find("/vinyl-demo-") == std::string::npos) return;
    ::nftw(w.root.c_str(), rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

OutputWorld demo_output_world(bool lite) {
    OutputWorld o;
    if (lite) {
        o.server = false;
        CardInfo hdmi;
        hdmi.index = 0;
        hdmi.id = "vc4hdmi";
        hdmi.name = "vc4-hdmi";
        hdmi.playback = true;
        CardInfo usb;
        usb.index = 1;
        usb.id = "UC02";
        usb.name = "USB sound card UC02";
        usb.playback = true;
        usb.capture = true;
        o.alsa_cards = {usb, hdmi};
        o.facts.have_board = true;
        o.facts.bt_audio = "none";
        return o;
    }
    o.server = true;
    PulseDevice evolve;
    evolve.name = "bluez_output.70_BF_92_53_6B_8B.1";
    evolve.description = "Evolve 65";
    evolve.bt_address = "70:BF:92:53:6B:8B";
    evolve.bus = "bluetooth";
    evolve.bt_profile = "a2dp-sink";
    evolve.rate = 48000;
    evolve.channels = 2;
    evolve.volume_pct = 60; /* the levels the chip shows (D11 as changed: the playing sink's own volume) */
    PulseDevice usb;
    usb.name = "alsa_output.usb-UC02.analog-stereo";
    usb.description = "USB sound card UC02";
    usb.bus = "usb";
    usb.rate = 48000;
    usb.channels = 2;
    usb.volume_pct = 40;
    PulseDevice hdmi;
    hdmi.name = "alsa_output.platform-107c701400.hdmi.hdmi-stereo";
    hdmi.description = "Built-in Audio Digital Stereo (HDMI)";
    hdmi.rate = 48000;
    hdmi.channels = 2;
    hdmi.volume_pct = 100;
    o.sinks = {evolve, usb, hdmi};
    o.default_sink = evolve.name;
    LauncherBtDev xwf;
    xwf.mac = "9E:0C:81:E6:5D:1A";
    xwf.kind = "speaker";
    xwf.connected = false;
    LauncherBtDev ev;
    ev.mac = "70:BF:92:53:6B:8B";
    ev.kind = "headset";
    ev.connected = true;
    ev.profile = "a2dp";
    o.facts.have_status = true;
    o.facts.devices = {ev, xwf};
    o.facts.bt_audio = "pipewire";
    o.bt_names["9E:0C:81:E6:5D:1A"] = "XWF speaker";
    return o;
}

LauncherStatus demo_launcher_status() {
    LauncherStatus s;
    s.have_status = s.have_board = true;
    s.volume = 60;
    s.mute = false;
    s.battery = 0;
    s.backlight = "levels";
    s.sound = 1;
    return s;
}

namespace {

/* takes frames at the speed they would be heard */
class NullSink final : public PlaybackSink {
public:
    int open(const AudioFormat &src, SinkFormat &granted) override {
        granted.rate = src.rate;
        granted.channels = src.channels;
        granted.format = src.bits > 16 ? SampleFormat::S32_LE : SampleFormat::S16_LE;
        rate_ = src.rate;
        start_ = std::chrono::steady_clock::now();
        written_ = 0;
        return 0;
    }
    long write(const void *, size_t n) override {
        const uint64_t ahead = static_cast<uint64_t>(rate_) / 5; /* 200 ms queued at most */
        for (;;) {
            const uint64_t heard = heard_frames();
            if (written_ <= heard + ahead) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        written_ += n;
        return static_cast<long>(n);
    }
    int recover(int) override { return 0; }
    long delay() override {
        const uint64_t heard = heard_frames();
        return written_ > heard ? static_cast<long>(written_ - heard) : 0;
    }
    void drop() override { restart(); }
    void drain() override {
        while (delay() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        restart();
    }
    void close() override {}

private:
    uint64_t heard_frames() const {
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
        return static_cast<uint64_t>(s * rate_);
    }
    void restart() {
        start_ = std::chrono::steady_clock::now();
        written_ = 0;
    }
    uint32_t rate_ = 44100;
    std::chrono::steady_clock::time_point start_;
    uint64_t written_ = 0;
};

class DemoSinks final : public SinkProvider {
public:
    int open(const OutputTarget &t, OpenedSink &out, std::string &why) override {
        (void)why;
        out.sink = std::make_unique<NullSink>();
        out.pulse = true;
        if (t.kind == OutputTarget::Kind::Auto || t.id == "70:BF:92:53:6B:8B") {
            out.name = "bluez_output.70_BF_92_53_6B_8B.1";
            out.label = "Evolve 65";
            out.device = "70:BF:92:53:6B:8B";
        } else {
            out.name = t.id;
            out.label = t.id;
            out.device = t.id;
        }
        return 0;
    }
    bool still_there(const OpenedSink &) override { return true; }
};

}  // namespace

std::unique_ptr<SinkProvider> make_demo_sink_provider() { return std::make_unique<DemoSinks>(); }

}  // namespace vinyl
