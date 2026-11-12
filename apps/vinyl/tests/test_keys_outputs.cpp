/*
 * SPDX-License-Identifier: MIT
 *
 * Vinyl tests: the media-key reader (the device list, the key map, the dedupe, a reader on a FIFO), the output chooser's
 * rows, the launcher's files and socket, and the layout geometry.
 */
#include "check.hpp"

#include "engine/outputs.hpp"
#include "input/media_keys.hpp"
#include "launcher/launcher_link.hpp"
#include "layout/layout.hpp"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace vinyl;

namespace {

/* the HackBerry's nodes as read in report 059 (13.6): the speaker's AVRCP node (PLAYCD, PAUSECD, NEXTSONG, PREVIOUSSONG,
 * STOPCD, REWIND, FASTFORWARD, volume and mute; no PLAYPAUSE) and the keyboard's Consumer Control node (PLAYPAUSE) */
std::string key_bitmap(std::initializer_list<unsigned> codes) {
    uint64_t w[8] = {0};
    for (unsigned c : codes) w[c / 64] |= uint64_t(1) << (c % 64);
    int top = 7;
    while (top > 0 && !w[top]) --top;
    std::string s;
    char b[32];
    for (int i = top; i >= 0; --i) {
        std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(w[i]));
        s += b;
        if (i) s += ' ';
    }
    return s;
}

std::string devices_text() {
    std::string t;
    t += "I: Bus=0005 Vendor=0000 Product=0000 Version=0000\nN: Name=\"XWF-M18-M28-M38 (AVRCP)\"\nH: Handlers=kbd event10\nB: EV=100013\n";
    t += "B: KEY=" + key_bitmap({200, 201, 163, 165, 166, 168, 208, 113, 114, 115}) + "\n\n";
    t += "I: Bus=0003\nN: Name=\"ZitaoTech HACKBerryPiQ20 Consumer Control\"\nH: Handlers=kbd event3\n";
    t += "B: KEY=" + key_bitmap({164, 163, 165, 166, 115, 114}) + "\n\n";
    t += "I: Bus=0018\nN: Name=\"Goodix Capacitive TouchScreen\"\nH: Handlers=mouse0 event1\nB: KEY=" + key_bitmap({330}) + "\n\n";
    return t;
}

}  // namespace

void test_media_keys() {
    CHECK(media_action_for(164) == MediaAction::Toggle); /* PLAYPAUSE */
    CHECK(media_action_for(200) == MediaAction::Toggle); /* PLAYCD */
    CHECK(media_action_for(201) == MediaAction::Toggle); /* PAUSECD */
    CHECK(media_action_for(163) == MediaAction::Next);
    CHECK(media_action_for(165) == MediaAction::Prev);
    CHECK(media_action_for(166) == MediaAction::Stop);
    CHECK(media_action_for(115) == MediaAction::None); /* volume up: the launcher's */
    const std::vector<InputDevice> devs = parse_input_devices(devices_text());
    CHECK_EQ(devs.size(), static_cast<size_t>(3));
    if (devs.size() == 3) {
        CHECK_EQ(devs[0].name, std::string("XWF-M18-M28-M38 (AVRCP)"));
        CHECK_EQ(devs[0].event, std::string("event10"));
        CHECK(devs[0].has_key(200));
        CHECK(!devs[0].has_key(164));
        CHECK(devs[0].media());
        CHECK(devs[1].has_key(164));
        CHECK(devs[1].media());
        CHECK(!devs[2].media());
    }
    /* dedupe: a press seen twice within 250 ms acts once; another action is not held back */
    Dedupe d;
    CHECK(d.accept(MediaAction::Toggle, 1000));
    CHECK(!d.accept(MediaAction::Toggle, 1100));
    CHECK(d.accept(MediaAction::Next, 1100));
    CHECK(d.accept(MediaAction::Toggle, 1300));
    CHECK(!d.accept(MediaAction::None, 5000));

    /* the reader on a FIFO standing for /dev/input/event10: presses act once, releases and repeats do not */
    const std::string dir = make_temp_dir("keys");
    write_file(dir + "/devices", devices_text());
    const std::string fifo = dir + "/event10";
    CHECK_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    MediaKeyReader r;
    r.start(dir + "/devices", dir);
    const int wfd = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK); /* RDWR: the open does not wait for the reader */
    CHECK(wfd >= 0);
    auto send = [&](unsigned code, int value) {
        input_event ev{};
        ev.type = EV_KEY;
        ev.code = static_cast<uint16_t>(code);
        ev.value = value;
        CHECK_EQ(::write(wfd, &ev, sizeof ev), static_cast<ssize_t>(sizeof ev));
        input_event syn{};
        syn.type = EV_SYN;
        CHECK_EQ(::write(wfd, &syn, sizeof syn), static_cast<ssize_t>(sizeof syn));
    };
    /* wait for the reader to open the node */
    for (int i = 0; i < 100 && r.open_nodes().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(r.open_nodes().size(), static_cast<size_t>(1));
    send(200, 1);
    send(200, 0);
    send(200, 2);
    send(163, 1);
    send(163, 0);
    std::vector<MediaAction> got;
    for (int i = 0; i < 100 && got.size() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (MediaAction a : r.take()) got.push_back(a);
    }
    CHECK_EQ(got.size(), static_cast<size_t>(2));
    if (got.size() == 2) {
        CHECK(got[0] == MediaAction::Toggle);
        CHECK(got[1] == MediaAction::Next);
    }
    /* the same press through the keyboard path right after: a duplicate */
    CHECK(!r.accept_other(MediaAction::Toggle));
    r.stop();
    ::close(wfd);
    rm_tree(dir);
}

void test_outputs() {
    CHECK_EQ(OutputTarget::from_key("bt:70:BF:92:53:6B:8B").id, std::string("70:BF:92:53:6B:8B"));
    CHECK(OutputTarget::from_key("pulse:alsa_output.x").kind == OutputTarget::Kind::Pulse);
    CHECK(OutputTarget::from_key("alsa:plughw:CARD=UC02,DEV=0").kind == OutputTarget::Kind::Alsa);
    CHECK(OutputTarget::from_key("weird").kind == OutputTarget::Kind::Auto);
    CHECK_EQ(OutputTarget::from_key("bt:AA").key(), std::string("bt:AA"));

    /* the draft's chooser: Automatic (follows the system's Evolve 65), Evolve 65 in music quality, the USB card and HDMI
     * wired, the XWF speaker paired but not connected (dimmed) */
    OutputWorld w;
    w.server = true;
    PulseDevice ev;
    ev.name = "bluez_output.70_BF_92_53_6B_8B.1";
    ev.description = "Evolve 65";
    ev.bt_address = "70:BF:92:53:6B:8B";
    ev.bus = "bluetooth";
    ev.bt_profile = "a2dp-sink";
    PulseDevice usb;
    usb.name = "alsa_output.usb-UC02.analog-stereo";
    usb.description = "USB sound card UC02";
    PulseDevice hdmi;
    hdmi.name = "alsa_output.platform.hdmi-stereo";
    hdmi.description = "Built-in Audio Digital Stereo (HDMI)";
    PulseDevice mon = usb;
    mon.name += ".monitor";
    mon.is_monitor = true;
    w.sinks = {usb, ev, hdmi, mon};
    w.default_sink = ev.name;
    LauncherBtDev off;
    off.mac = "9E:0C:81:E6:5D:1A";
    off.kind = "speaker";
    LauncherBtDev on;
    on.mac = "70:BF:92:53:6B:8B";
    on.kind = "headset";
    on.connected = true;
    w.facts.devices = {on, off};
    w.facts.bt_audio = "pipewire";
    w.bt_names["9E:0C:81:E6:5D:1A"] = "XWF speaker";
    std::vector<OutputRow> rows = build_output_rows(w, OutputTarget());
    CHECK_EQ(rows.size(), static_cast<size_t>(5));
    if (rows.size() == 5) {
        CHECK_EQ(rows[0].label, std::string("Automatic"));
        CHECK_EQ(rows[0].value, std::string("follows system: Evolve 65"));
        CHECK(rows[0].chosen);
        CHECK_EQ(rows[1].label, std::string("Evolve 65"));
        CHECK_EQ(rows[1].value, std::string("music quality"));
        CHECK(rows[1].target.kind == OutputTarget::Kind::Bt);
        CHECK_EQ(rows[1].target.id, std::string("70:BF:92:53:6B:8B"));
        CHECK_EQ(rows[2].label, std::string("USB sound card UC02"));
        CHECK_EQ(rows[2].value, std::string("wired"));
        CHECK_EQ(rows[3].label, std::string("HDMI"));
        CHECK_EQ(rows[4].label, std::string("XWF speaker"));
        CHECK_EQ(rows[4].value, std::string("paired, not connected"));
        CHECK(rows[4].dimmed);
    }
    CHECK(!bluetooth_not_set_up(w));
    /* call quality: a headset in its call profile */
    w.sinks[1].bt_profile = "headset-head-unit";
    rows = build_output_rows(w, OutputTarget::from_key("bt:70:BF:92:53:6B:8B"));
    CHECK_EQ(rows[1].value, std::string("call quality"));
    CHECK(rows[1].chosen);
    CHECK(!rows[0].chosen);
    /* a choice made earlier that is gone stays listed, dimmed */
    rows = build_output_rows(w, OutputTarget::from_key("bt:11:22:33:44:55:66"));
    CHECK(rows.back().chosen && rows.back().dimmed);
    /* a Lite board: ALSA cards, no Bluetooth */
    OutputWorld lite;
    CardInfo c;
    c.id = "UC02";
    c.name = "USB PnP Sound Device";
    c.playback = true;
    CardInfo cap;
    cap.id = "Mic";
    cap.capture = true;
    lite.alsa_cards = {c, cap};
    rows = build_output_rows(lite, OutputTarget());
    CHECK_EQ(rows.size(), static_cast<size_t>(2));
    CHECK_EQ(rows[0].value, std::string("system default"));
    CHECK_EQ(rows[1].target.id, std::string("plughw:CARD=UC02,DEV=0"));
    CHECK(bluetooth_not_set_up(lite));
    w.facts.bt_audio = "none";
    CHECK(bluetooth_not_set_up(w));
}

namespace {
class FakeIo final : public FactsIo {
public:
    std::map<std::string, std::string> files;
    bool alive = true;
    int gen = 0;
    bool stat(const std::string &p, std::string &stamp) override {
        auto it = files.find(p);
        if (it == files.end()) return false;
        stamp = std::to_string(gen) + ":" + std::to_string(it->second.size());
        return true;
    }
    bool read(const std::string &p, std::string &text, size_t) override {
        auto it = files.find(p);
        if (it == files.end()) return false;
        text = it->second;
        return true;
    }
    bool pid_alive(long) override { return alive; }
};
}  // namespace

void test_launcher() {
    LauncherStatus s;
    parse_launcher_status("v=1\npid=826\ngen=4\nvolume=60\nmute=0\nbt=connected\nbt_audio_dev=70_BF_92_53_6B_8B:headset:0;"
                          "9E_0C_81_E6_5D_1A:speaker:1:a2dp\n",
                          true, "v=1\npid=826\nbattery=1\nbacklight=levels\nsound=1\nbt_audio=pipewire\nbt_hfp=1\nrequests=beep toast dim\n",
                          true, s);
    CHECK(s.have_status && s.have_board);
    CHECK_EQ(s.volume, 60);
    CHECK(!s.mute);
    CHECK(s.battery_board());
    CHECK_EQ(s.facts.devices.size(), static_cast<size_t>(2));
    CHECK(s.facts.stack_server());
    parse_launcher_status("v=1\npid=1\nvolume=-1\nmute=1\n", true, "", false, s);
    CHECK_EQ(s.volume, -1);
    CHECK(s.mute);
    CHECK(!s.have_board);
    parse_launcher_status("v=1\npid=1\nvolume=40\n", false, "", false, s);
    CHECK(!s.have_status);
    CHECK_EQ(s.volume, -1);

    /* the reader: re-read on change, untrusted when the launcher's pid is gone */
    auto io = std::make_unique<FakeIo>();
    FakeIo *raw = io.get();
    raw->files["/s"] = "v=1\npid=5\nvolume=30\n";
    LauncherStatusReader r(std::move(io), "/s", "/b");
    CHECK(r.poll(1000));
    CHECK_EQ(r.status().volume, 30);
    CHECK(!r.poll(1500)); /* at most once a second */
    raw->files["/s"] = "v=1\npid=5\nvolume=35\n";
    raw->gen++;
    CHECK(r.poll(2100));
    CHECK_EQ(r.status().volume, 35);
    raw->alive = false;
    CHECK(r.poll(3200));
    CHECK(!r.status().have_status);

    /* launcher.sock: one line out, one line back; no socket = -1 at once */
    CHECK_EQ(launcher_call("", "dim allow", nullptr), -1);
    const std::string dir = make_temp_dir("sock");
    const std::string path = dir + "/launcher.sock";
    CHECK_EQ(launcher_call(path, "dim allow", nullptr), -1);
    const int ls = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    CHECK_EQ(::bind(ls, reinterpret_cast<sockaddr *>(&a), sizeof a), 0);
    CHECK_EQ(::listen(ls, 4), 0);
    std::string heard;
    std::thread srv([&] {
        const int c = ::accept(ls, nullptr, nullptr);
        char b[256] = {0};
        const ssize_t n = ::recv(c, b, sizeof b - 1, 0);
        if (n > 0) heard.assign(b, static_cast<size_t>(n));
        const char *reply = "ok dim\n";
        (void)::send(c, reply, std::strlen(reply), 0);
        ::close(c);
    });
    std::string reply;
    CHECK_EQ(launcher_call(path, "dim allow", &reply, 1000), 0);
    srv.join();
    CHECK_EQ(heard, std::string("dim allow\n"));
    CHECK_EQ(reply, std::string("ok dim"));
    ::close(ls);
    rm_tree(dir);
}

void test_layout() {
    using namespace vinyl::layout;
    CHECK(size_class(480, 320) == SizeClass::Small);
    CHECK(size_class(640, 480) == SizeClass::Small);
    CHECK(size_class(320, 480) == SizeClass::Portrait);
    CHECK(size_class(480, 800) == SizeClass::Portrait);
    CHECK(size_class(800, 1280) == SizeClass::Portrait);
    CHECK(size_class(720, 720) == SizeClass::Square);
    CHECK(size_class(480, 480) == SizeClass::Square);
    CHECK(size_class(800, 480) == SizeClass::Large);
    CHECK(size_class(1280, 720) == SizeClass::Large);
    CHECK(size_class(1024, 600) == SizeClass::Large);
    CHECK(size_class(1280, 800) == SizeClass::Large);
    /* at the drawn size the fit is the drafts' pixels */
    for (SizeClass c : {SizeClass::Small, SizeClass::Portrait, SizeClass::Square, SizeClass::Large}) {
        const NowGeom g = now_geom(c);
        const ui::Fit f = now_fit(g, g.dw, g.dh, 40); /* the toolkit header is 40 px at the drawn sizes */
        CHECK_NEAR(f.s, 1.0, 1e-9);
        CHECK_EQ(f.x(g.panel.x), static_cast<int>(g.panel.x));
        CHECK_EQ(f.y(g.panel.bottom()), static_cast<int>(g.panel.bottom()));
        /* everything inside the panel and the screen, the buttons apart */
        auto inside = [&](const R &r) { return r.x >= g.panel.x && r.right() <= g.panel.right() && r.y >= g.panel.y && r.bottom() <= g.panel.bottom(); };
        CHECK(inside(g.sleeve));
        CHECK(inside(g.disc));
        CHECK(inside(g.seek));
        for (const Btn &b : g.transport) CHECK(inside(b.box));
        for (const Btn &b : g.bottom) CHECK(inside(b.box));
        for (size_t i = 1; i < g.transport.size(); ++i) CHECK(g.transport[i].box.x >= g.transport[i - 1].box.right());
        /* every chip id once */
        int ids = 0;
        for (const std::vector<int> &row : g.chip_layout)
            for (int id : row) ids |= 1 << id;
        CHECK_EQ(ids, c == SizeClass::Small || c == SizeClass::Portrait ? 7 : 15);
        /* the notice box across the seek row */
        const R n = notice_box_now(g, 50);
        CHECK(n.x >= g.panel.x && n.right() <= g.panel.right());
        /* other sizes of the class: the whole design on the screen */
        for (int sz : {1, 2}) {
            const int w = static_cast<int>(g.dw * (1 + 0.5 * sz)), h = static_cast<int>(g.dh * (1 + 0.5 * sz)) + 40;
            const ui::Fit f2 = now_fit(g, w, h, 56);
            CHECK(f2.y(g.dh) <= h);
            CHECK(f2.x(g.dw) <= w);
            CHECK(f2.x(0) >= 0);
        }
    }
    /* the deck: the small design scaled 4/3, the extra height in the panel */
    const NowGeom sg = now_geom(SizeClass::Small);
    const ui::Fit deck = now_fit(sg, 640, 480, 56);
    CHECK_NEAR(deck.s, 4.0 / 3.0, 1e-6);
    CHECK(deck.y(sg.dh) <= 480 && deck.y(sg.dh) >= 470);
    CHECK_EQ(deck.y(40), 56); /* the drafts' y 40 on the toolkit header's bottom */
    /* the list pages: the queue buttons in one row at 480 wide, two rows at 320 */
    const ListGeom q480 = list_geom(ListPage::Queue, 480, 320, 40);
    CHECK_EQ(q480.queue_buttons.size(), static_cast<size_t>(4));
    CHECK_NEAR(q480.queue_buttons[0].y, q480.queue_buttons[3].y, 0.5);
    CHECK_NEAR(q480.queue_buttons[0].w, 106, 0.5);
    CHECK_NEAR(q480.list.y, 74, 0.5);
    CHECK_NEAR(q480.strip.y, 262, 0.5);
    const ListGeom q320 = list_geom(ListPage::Queue, 320, 480, 40);
    CHECK(q320.queue_buttons[2].y > q320.queue_buttons[0].y);
    CHECK(q320.queue_buttons[1].right() <= q320.panel.right());
    const ListGeom o480 = list_geom(ListPage::Output, 480, 320, 40);
    CHECK_NEAR(o480.list.y, 50, 0.5);
    CHECK_NEAR(o480.row_h, 40, 0.5);
    const ListGeom b480 = list_geom(ListPage::Browser, 480, 320, 40);
    CHECK_NEAR(b480.row_h, 36, 0.5);
    CHECK_NEAR(b480.strip.x, 16, 0.5);
    CHECK_NEAR(b480.strip.w, 448, 0.5);
}
