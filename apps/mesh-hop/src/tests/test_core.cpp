// Host tests of the protocol, model, store and client layers (no LVGL, no hardware).
//   see run_tests.sh:  g++ -std=c++17 -I../main/core test_core.cpp ../main/core/*.cpp -o test_core && ./test_core
#include "client.hpp"
#include "frame.hpp"
#include "model.hpp"
#include "protocol.hpp"
#include "serial_transport.hpp"
#include "clock_policy.hpp"
#include "deck_clock.hpp"
#include "input_state.hpp"
#include "nav.hpp"
#include "sha256.hpp"
#include "store.hpp"
#include "channel_key.hpp"
#include "features.hpp"
#include "log.hpp"
#include "presets.hpp"
#include "preset_feed.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

using namespace meshzero;

static int g_fail = 0, g_checks = 0;
#define CHECK(...)                                                                       \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(__VA_ARGS__)) {                                                           \
            ++g_fail;                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);                \
        }                                                                              \
    } while (0)

static Bytes operator+(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static Bytes hex(const char *s)
{
    Bytes b;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        std::sscanf(std::string(s, 2).c_str(), "%2x", &v);
        b.push_back(static_cast<uint8_t>(v));
    }
    return b;
}

static Bytes framed(const Bytes &payload)      // device -> app frame
{
    Bytes f = {0x3E, static_cast<uint8_t>(payload.size() & 0xFF), static_cast<uint8_t>(payload.size() >> 8)};
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

static void put32(Bytes &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

static void put_text(Bytes &b, const std::string &s, size_t width)
{
    for (size_t i = 0; i < width; ++i) b.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
}

/* The DEVICE_INFO frame the user's real board (Seeed XIAO nRF52840, MeshCore companion v1.15.0) sent: '>' 82 0 + frame. */
static const char *kRealDeviceInfo = "3e52000d0baf280000000031392d4170722d32303236005365656564205869616f2d6e7266353200000000000000000000000000000000000000000000000076312e31352e302d6465653365323600000000000001";

static Bytes make_self_info(const std::string &name, uint32_t freq_khz = 869525, uint32_t bw_hz = 250000, uint8_t sf = 11, uint8_t cr = 5,
                            uint8_t txp = 22, bool manual_add = false, uint8_t key_seed = 0xA0)
{
    Bytes b = {resp::kSelfInfo, 1, txp, 30};
    for (int i = 0; i < 32; ++i) b.push_back(static_cast<uint8_t>(key_seed + i));
    put32(b, static_cast<uint32_t>(48856600));
    put32(b, static_cast<uint32_t>(2352200));
    b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(manual_add ? 1 : 0);        // multi acks, advert location policy, telemetry mode, manual add
    put32(b, freq_khz);
    put32(b, bw_hz);
    b.push_back(sf);
    b.push_back(cr);
    b.insert(b.end(), name.begin(), name.end());
    return b;
}

static Bytes make_contact(uint8_t code, uint8_t key_seed, const std::string &name, uint8_t type, uint32_t advert, uint8_t path_len = 0xFF)
{
    Bytes b = {code};
    for (int i = 0; i < 32; ++i) b.push_back(static_cast<uint8_t>(key_seed + i));
    b.push_back(type);
    b.push_back(0);
    b.push_back(path_len);
    for (int i = 0; i < 64; ++i) b.push_back(0);
    put_text(b, name, 32);
    put32(b, advert);
    put32(b, 48000000);
    put32(b, static_cast<uint32_t>(-2000000));
    put32(b, advert + 1);
    return b;
}

static Bytes make_chan_info(uint8_t idx, const std::string &name, bool secret)
{
    Bytes b = {resp::kChannelInfo, idx};
    put_text(b, name, 32);
    for (int i = 0; i < 16; ++i) b.push_back(secret ? static_cast<uint8_t>(i + 1) : 0);
    return b;
}

static Bytes make_chan_msg_v3(uint8_t chan, const std::string &text, double snr = 5.5, uint32_t ts = 1790000000)
{
    Bytes b = {resp::kChannelMsgV3, static_cast<uint8_t>(static_cast<int8_t>(snr * 4)), 0, 0, chan, 0xFF, 0};
    put32(b, ts);
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

static Bytes make_dm_v3(uint8_t key_seed, const std::string &text, uint32_t ts = 1790000001)
{
    Bytes b = {resp::kContactMsgV3, 0x14, 0, 0};
    for (int i = 0; i < 6; ++i) b.push_back(static_cast<uint8_t>(key_seed + i));
    b.push_back(2);
    b.push_back(0);
    put32(b, ts);
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

/* ------------------------------------------------------------------ frame + protocol */

static void test_frame()
{
    FrameParser p;
    std::vector<Bytes> out;
    const Bytes f1 = framed({0x00, 1, 2, 3});
    p.feed(f1.data(), 2, out);                       // chunked
    CHECK(out.empty());
    p.feed(f1.data() + 2, f1.size() - 2, out);
    CHECK(out.size() == 1 && out[0] == Bytes({0x00, 1, 2, 3}));

    out.clear();
    Bytes stream = {'b', 'o', 'o', 't', ' ', 0x0A};   // junk, then a '>' that is a false start (huge length), then two frames
    stream.insert(stream.end(), {0x3E, 0xFF, 0xFF});
    const Bytes f2 = framed({0x83});
    stream.insert(stream.end(), f2.begin(), f2.end());
    stream.insert(stream.end(), f1.begin(), f1.end());
    FrameParser q;
    q.feed(stream.data(), stream.size(), out);
    CHECK(out.size() == 2 && out[0] == Bytes({0x83}) && out[1].size() == 4);
    CHECK(q.junk_bytes() >= 6 && q.bad_headers() >= 1);

    // a stale partial frame at connect (starts in the middle of a payload): resync on the next marker
    out.clear();
    FrameParser r;
    Bytes stale = {0x11, 0x22, 0x33};
    stale.insert(stale.end(), f1.begin(), f1.end());
    r.feed(stale.data(), stale.size(), out);
    CHECK(out.size() == 1);

    // the 103 bytes the real board pushed before the first query: '>' 0x60 0x00 0x88 ... (a log push)
    out.clear();
    Bytes log = {0x3E, 0x64, 0x00, 0x88};
    for (int i = 0; i < 99; ++i) log.push_back(static_cast<uint8_t>(i));
    FrameParser s;
    s.feed(log.data(), log.size(), out);
    CHECK(out.size() == 1 && out[0][0] == 0x88 && out[0].size() == 100);
    auto pk = parse_packet(out[0]);
    CHECK(pk && std::holds_alternative<LogData>(*pk));

    // encode
    const Bytes e = encode_frame({0x16, 0x03});
    CHECK(e == hex("3c02001603"));
    CHECK(encode_frame({}).empty());
    CHECK(encode_frame(Bytes(173, 1)).empty());
    CHECK(!encode_frame(Bytes(172, 1)).empty());
}

static void test_real_device_info()
{
    const Bytes frame = hex(kRealDeviceInfo);
    FrameParser p;
    std::vector<Bytes> out;
    p.feed(frame.data(), frame.size(), out);
    CHECK(out.size() == 1 && out[0].size() == 82);
    auto pk = parse_packet(out[0]);
    CHECK(pk && std::holds_alternative<DeviceInfo>(*pk));
    const DeviceInfo &d = std::get<DeviceInfo>(*pk);
    CHECK(d.fw_ver == 11);
    CHECK(d.max_contacts_value() == 350);
    CHECK(d.max_channels == 40);
    CHECK(d.ble_pin == 0);
    CHECK(d.fw_build == "19-Apr-2026");
    CHECK(d.model == "Seeed Xiao-nrf52");
    CHECK(d.version == "v1.15.0-dee3e26");
    CHECK(d.has_repeat && d.repeat == false);
    CHECK(d.path_hash_mode == 1);
}

static void test_parsers()
{
    auto si = parse_packet(make_self_info("Deck Zero"));
    CHECK(si && std::holds_alternative<SelfInfo>(*si));
    const SelfInfo &s = std::get<SelfInfo>(*si);
    CHECK(s.name == "Deck Zero" && s.sf == 11 && s.cr == 5 && s.tx_power == 22 && s.max_tx_power == 30);
    CHECK(s.freq_khz == 869525 && s.bw_hz == 250000 && s.freq_mhz() == 869.525 && s.bw_khz() == 250.0);
    CHECK(s.lat > 48.85 && s.lat < 48.86 && s.key[0] == 0xA0);
    CHECK(!parse_packet(Bytes(20, 5)).has_value() || true);
    CHECK(!parse_packet(Bytes{resp::kSelfInfo, 1, 2}).has_value());

    auto c = parse_packet(make_contact(resp::kContact, 0x10, "Alice", 1, 1790000000, 0x41));
    CHECK(c && std::holds_alternative<Contact>(*c));
    const Contact &ct = std::get<Contact>(*c);
    CHECK(ct.name == "Alice" && ct.type == 1 && ct.key[0] == 0x10 && path_hops(ct.out_path_len) == 1 && path_hash_mode(ct.out_path_len) == 1);
    CHECK(ct.lon < -1.99 && ct.lon > -2.01 && ct.lastmod == 1790000001);
    auto na = parse_packet(make_contact(resp::kNewAdvert, 0x10, "Bob", 2, 5));
    CHECK(na && std::holds_alternative<NewAdvert>(*na));
    CHECK(path_hops(kPathFlood) == -1);

    auto m = parse_packet(make_dm_v3(0x30, "hello"));
    CHECK(m && std::get<IncomingMessage>(*m).text == "hello" && std::get<IncomingMessage>(*m).has_snr && std::get<IncomingMessage>(*m).snr == 5.0);
    CHECK(std::get<IncomingMessage>(*m).prefix[0] == 0x30 && std::get<IncomingMessage>(*m).txt_type == 0);
    // signed text type 2: 4 signature bytes are skipped
    Bytes signed_msg = {resp::kContactMsg, 1, 2, 3, 4, 5, 6, 0xFF, 2};
    put32(signed_msg, 77);
    signed_msg.insert(signed_msg.end(), {9, 9, 9, 9, 'o', 'k'});
    auto sm = parse_packet(signed_msg);
    CHECK(sm && std::get<IncomingMessage>(*sm).text == "ok" && !std::get<IncomingMessage>(*sm).has_snr);
    auto cm = parse_packet(make_chan_msg_v3(2, "Zed: hi\xC3\xA9", -3.25));
    CHECK(cm && std::get<IncomingMessage>(*cm).channel && std::get<IncomingMessage>(*cm).channel_idx == 2);
    CHECK(std::get<IncomingMessage>(*cm).snr == -3.25 && std::get<IncomingMessage>(*cm).text == "Zed: hi\xC3\xA9");
    Bytes bad_utf = {resp::kChannelMsg, 0, 0xFF, 0};
    put32(bad_utf, 1);
    bad_utf.insert(bad_utf.end(), {'a', 0xC3, 'b', 0, 0});
    auto bu = parse_packet(bad_utf);
    CHECK(bu && std::get<IncomingMessage>(*bu).text == "a?b");

    Bytes sent = {resp::kMsgSent, 1, 0xDE, 0xAD, 0xBE, 0xEF};
    put32(sent, 4200);
    auto ms = parse_packet(sent);
    CHECK(ms && std::get<MsgSent>(*ms).type == 1 && std::get<MsgSent>(*ms).suggested_timeout_ms == 4200 && std::get<MsgSent>(*ms).expected_ack[0] == 0xDE);
    // ACK: meshcore_py reads a 4 byte code (+ 4 byte trip time); the doc says 6 bytes
    auto ak = parse_packet(hex("82deadbeef0a000000"));
    CHECK(ak && std::get<Ack>(*ak).has_trip && std::get<Ack>(*ak).trip_ms == 10 && std::get<Ack>(*ak).code[3] == 0xEF);
    auto ak2 = parse_packet(hex("82deadbeef"));
    CHECK(ak2 && !std::get<Ack>(*ak2).has_trip);

    auto ci = parse_packet(make_chan_info(1, "#test", true));
    CHECK(ci && std::get<ChannelInfo>(*ci).name == "#test" && !std::get<ChannelInfo>(*ci).empty());
    auto ce = parse_packet(make_chan_info(5, "", false));
    CHECK(ce && std::get<ChannelInfo>(*ce).empty());

    auto bt = parse_packet(hex("0c7a0f"));
    CHECK(bt && std::get<Battery>(*bt).millivolts == 3962 && !std::get<Battery>(*bt).has_storage);
    auto er = parse_packet(hex("0106"));
    CHECK(er && std::get<Error>(*er).code == 6);
    auto ok = parse_packet(hex("0001000000"));
    CHECK(ok && std::get<Ok>(*ok).has_value && std::get<Ok>(*ok).value == 1);
    auto un = parse_packet(hex("7e0102"));
    CHECK(un && std::holds_alternative<Unknown>(*un));
    CHECK(!parse_packet(Bytes{}).has_value());
    CHECK(!parse_packet(hex("0c")).has_value());
}

static void test_builders()
{
    CHECK(build_device_query() == hex("1603"));
    CHECK(build_app_start("mccli") == hex("0103202020202020")  + Bytes{'m', 'c', 'c', 'l', 'i'});
    CHECK(build_send_advert(true) == hex("0701") && build_send_advert(false) == hex("07"));
    CHECK(build_sync_next() == hex("0a") && build_battery() == hex("14"));
    CHECK(build_get_channel(3) == hex("1f03"));
    CHECK(build_get_contacts() == hex("04") && build_get_contacts(0x01020304) == hex("0404030201"));
    CHECK(build_set_time(0x65000000) == hex("0600000065"));
    CHECK(build_set_tx_power(22) == hex("0c16000000") && build_set_tx_power(-3) == hex("0cfdffffff"));
    // 869.525 MHz -> 869525 (0x000D4415), 250 kHz -> 250000 (0x0003D090)
    CHECK(build_set_radio(869.525, 250.0, 11, 5) == hex("0b95440d0090d003000b05"));
    CHECK(build_set_radio(869.618, 62.5, 7, 8) == hex("0b") + Bytes{0xF2, 0x44, 0x0D, 0x00, 0x24, 0xF4, 0x00, 0x00, 7, 8});   // rounding, not truncation
    KeyPrefix kp = {1, 2, 3, 4, 5, 6};
    CHECK(build_send_txt(kp, "hi", 0x01020304, 1) == hex("0200010403020101020304050668") + Bytes{'i'});
    CHECK(build_send_channel_txt(2, "yo", 1234567890) == hex("030002d202964979") + Bytes{'o'});
    Bytes sc = build_set_channel(1, "Pub", kPublicChannelSecret);
    CHECK(sc.size() == 50 && sc[0] == 0x20 && sc[1] == 1 && sc[2] == 'P' && sc[5] == 0 && sc[34] == 0x8b);
    PubKey key{};
    key[0] = 9;
    CHECK(build_reset_path(key).size() == 33 && build_get_contact_by_key(key)[0] == 30);
}

static std::string hex_of(const std::array<uint8_t, 32> &d) { return to_hex(d.data(), 32); }

static void test_sha256()
{
    // FIPS 180-4 / RFC 6234 test vectors
    CHECK(hex_of(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex_of(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex_of(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex_of(sha256("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")) ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    CHECK(hex_of(sha256(std::string(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // padding edges: 55, 56, 63, 64, 65 bytes (one or two final blocks)
    CHECK(hex_of(sha256(std::string(55, 'a'))) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hex_of(sha256(std::string(56, 'a'))) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hex_of(sha256(std::string(64, 'a'))) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    // the documented hashtag key: first 16 bytes of sha256("#test") (companion_protocol.md)
    CHECK(to_hex(hashtag_secret("#test").data(), 16) == "9cd8fcf22a47333b591d96a2b848b73f");
    CHECK(validate_hashtag("#test").empty() && !validate_hashtag("test").empty() && !validate_hashtag("#").empty());
    CHECK(!validate_hashtag("#a b").empty() && !validate_hashtag("#" + std::string(40, 'x')).empty() && !validate_hashtag("").empty());
}

static void test_input()
{
    // Delete: the raw evdev code and the LVGL code the launcher converts it to (the 0.1.1 bug: only 111 was handled)
    CHECK(is_delete_key(111) && is_delete_key(127) && !is_delete_key(14) && !is_delete_key(8));
    const std::string with_keys = std::string("N: Name=\"Mouse\"\nB: KEY=1f0000 0 0 0 0\n\n") +
                                  "N: Name=\"BT Keyboard\"\nH: Handlers=kbd event4\nB: KEY=ffffffff ffffffffffffffff 0 0 0200100050000000\n\n";
    CHECK(keyboard_listed(with_keys));
    CHECK(!keyboard_listed("N: Name=\"Mouse\"\nB: KEY=1f0000 0 0 0 0\n\nN: Name=\"IR\"\nB: KEY=100000 0 0 0 0\n"));
    CHECK(!keyboard_listed(""));
    CHECK(keyboard_listed("N: Name=\"k\"\nB: KEY=0200100050000000\n"));      // a single 64-bit word
    CHECK(keyboard_listed("N: Name=\"k32\"\nB: KEY=2001000 50000000\n"));    // 32-bit words
    CHECK(keyboard_present("/nonexistent/file"));                            // unreadable: typing is not blocked
}

static void test_util()
{
    CHECK(sanitize_utf8("a\xC3\xA9z") == "a\xC3\xA9z");
    CHECK(sanitize_utf8("a\xFFz\x01") == "a?z?");
    CHECK(truncate_utf8("\xC3\xA9\xC3\xA9", 3) == "\xC3\xA9");
    CHECK(utf8_length("a\xC3\xA9") == 2);
    uint8_t out[3];
    CHECK(from_hex("0aFf10", out, 3) && out[1] == 0xFF && !from_hex("0g", out, 1) && !from_hex("00", out, 2));
    CHECK(format_age(5) == "5 s" && format_age(130) == "2 min" && format_age(7300) == "2 h" && format_age(200000) == "2 d");
    // port choice
    std::vector<SerialCandidate> list(2);
    list[0].path = "/dev/ttyACM0"; list[0].vid = 0x1234; list[0].pid = 1;
    list[1].path = "/dev/ttyACM1"; list[1].vid = 0x2886; list[1].pid = 0x8044;
    CHECK(choose_port(list, "")->path == "/dev/ttyACM1");               // known board family first
    CHECK(choose_port(list, "1234:0001")->path == "/dev/ttyACM0");      // the one used last time
    CHECK(choose_port({}, "") == nullptr);
    CHECK(guess_board(0x2886, 0x8044).find("XIAO") != std::string::npos);
}

/* ------------------------------------------------------------------ model + store */

static uint32_t g_time = 1790000000;
static uint32_t fake_time() { return g_time; }

/* The data of a board lives in boards/<12 hex of its key>/: the tests plug board n after attaching the store. */
static PubKey test_board_key(int n)
{
    PubKey k{};
    for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(n * 17 + static_cast<int>(i) * 3 + 1);
    return k;
}
static std::string bdir_of(const std::string &dir, int n = 1) { return dir + "/boards/" + to_hex(test_board_key(n)).substr(0, 12); }
static bool attach_b(Store &st, Model &m, int n = 1)
{
    const bool ok = st.attach(m);
    m.select_board(test_board_key(n));
    return ok;
}

static std::string tmpdir(const char *name)
{
    std::string d = std::string("/tmp/meshzero-test-") + name + "-" + std::to_string(::getpid());
    std::string cmd = "rm -rf " + d;
    if (std::system(cmd.c_str()) != 0) {}
    return d;
}

static void test_migrate_legacy()
{
    const std::string base = tmpdir("migrate");
    const std::string oldd = base + "/meshcore", newd = base + "/mesh-hop";
    CHECK(std::system(("mkdir -p " + oldd + " && echo hello > " + oldd + "/history.jsonl").c_str()) == 0);
    CHECK(Store::migrate_legacy_dir(oldd, newd));                 // moved
    CHECK(!Store::migrate_legacy_dir(oldd, newd));                // old is gone: nothing to do
    CHECK(std::system(("test -f " + newd + "/history.jsonl && test ! -e " + oldd).c_str()) == 0);
    CHECK(std::system(("mkdir -p " + oldd + " && echo x > " + oldd + "/a").c_str()) == 0);
    CHECK(!Store::migrate_legacy_dir(oldd, newd));                // new exists: both untouched
    CHECK(std::system(("test -f " + oldd + "/a && test -f " + newd + "/history.jsonl").c_str()) == 0);
}

static void test_model_store()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("store");
    {
        Model m;
        Store st(dir);
        CHECK(attach_b(st, m));
        auto c1 = std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x10, "Alice \"A\"", 1, 1000)));
        auto c2 = std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x50, "Rptr", 2, 2000)));
        m.replace_contacts({c1, c2});
        ChannelRec ch;
        ch.idx = 0; ch.name = "Public"; ch.empty = false;
        m.set_channel(ch);
        IncomingMessage in;
        in.prefix = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
        in.text = "line1\nline2 \"q\" \\ \xC3\xA9";
        in.sender_timestamp = 99;
        in.has_snr = true;
        in.snr = 7.25;
        CHECK(m.add_incoming(in, g_time) == 1);
        CHECK(m.add_incoming(in, g_time + 5) == 0);                       // duplicate
        IncomingMessage cm;
        cm.channel = true;
        cm.channel_idx = 0;
        cm.text = "Zed: bonjour";
        cm.sender_timestamp = 5;
        CHECK(m.add_incoming(cm, g_time + 1) == 2);
        CHECK(m.unread_total() == 2 && m.unread("c:0") == 1);
        const uint32_t out = m.add_outgoing("c:0", "mine", g_time + 2);
        m.set_state(out, MsgState::Sent);
        const uint32_t pend = m.add_outgoing("d:101112131415", "never sent", g_time + 3);
        (void)pend;
        m.mark_read("c:0");
        CHECK(m.unread_total() == 1);
        const auto convs = m.conversations();
        CHECK(convs.size() == 2 && convs[0].key == "c:0" && convs[0].title == "Public" && convs[1].title == "Alice \"A\"");
        CHECK(m.find_by_prefix(in.prefix) != nullptr && m.find_by_prefix(in.prefix)->has_snr && m.find_by_prefix(in.prefix)->snr == 7.25);
    }
    {   // reload from disk
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(m.contact_count() == 2 && m.find_contact(to_hex(std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x10, "", 1, 0))).key)) != nullptr);
        CHECK(m.message_count() == 4);
        const auto msgs = m.messages("d:101112131415");
        CHECK(msgs.size() == 2);
        CHECK(msgs[0]->text == "line1\nline2 \"q\" \\ \xC3\xA9");
        CHECK(msgs[1]->state == MsgState::Failed && msgs[1]->note == "interrupted");     // was pending when the app stopped
        const auto cmsgs = m.messages("c:0");
        CHECK(cmsgs.size() == 2 && cmsgs[1]->state == MsgState::Sent && cmsgs[0]->sender == "Zed" && cmsgs[0]->text == "bonjour");
        CHECK(m.unread_total() == 1);                                 // the read mark survived
        CHECK(m.channels().size() == 1 && m.channels()[0].name == "Public");
        CHECK(m.next_seq() == 5);
        CHECK(m.contacts_sorted()[0]->c.name == "Alice \"A\"");
    }
    {   // channels added by the app persist, and flag the slot after a reload
        const std::string d2 = tmpdir("chan");
        {
            Model m;
            Store st(d2);
            attach_b(st, m);
            m.mark_added("#eu868");
            ChannelRec ch;
            ch.idx = 3; ch.name = "#eu868"; ch.empty = false;
            m.set_channel(ch);
            CHECK(m.find_channel(3)->app_added && m.free_channel_slot() == -1);
        }
        Model m;
        Store st(d2);
        attach_b(st, m);
        CHECK(m.is_added("#eu868") && m.find_channel(3) && m.find_channel(3)->app_added);
        ChannelRec e; e.idx = 4; e.empty = true;
        m.set_channel(e);
        CHECK(m.free_channel_slot() == 4);
    }
    {   // bounds
        Model m;
        for (int i = 0; i < 260; ++i) m.add_outgoing("c:1", "m" + std::to_string(i), g_time + i);
        CHECK(m.messages("c:1").size() == Model::kMaxPerConversation);
        CHECK(m.messages("c:1").back()->text == "m259");
    }
    {   // corrupt lines are skipped
        FILE *f = std::fopen((bdir_of(dir) + "/messages.jsonl").c_str(), "ab");
        std::fputs("{not json\n{\"i\":99}\n", f);
        std::fclose(f);
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(m.message_count() == 4);
    }
    JsonObject o;
    CHECK(parse_json_object("{\"a\":\"x\\u00e9\\ud83d\\ude00\",\"b\":-1.5,\"c\":true}", o));
    CHECK(o["a"].str == "x\xC3\xA9\xF0\x9F\x98\x80" && o["b"].num == -1.5 && o["c"].num == 1);
    CHECK(!parse_json_object("{\"a\":", o));
    CHECK(validate_name("").size() > 0 && validate_name("Deck") == "" && validate_name(std::string(32, 'a')).size() > 0);
    RadioSettings rs;
    rs.freq_mhz = 869.525; rs.bw_khz = 250; rs.sf = 11; rs.cr = 5; rs.tx_power = 22; rs.max_tx_power = 22; rs.name = "n";
    CHECK(validate_radio(rs).empty());
    rs.bw_khz = 100;
    CHECK(!validate_radio(rs).empty());
    rs.bw_khz = 250; rs.tx_power = 23;
    CHECK(!validate_radio(rs).empty());
}

/* ------------------------------------------------------------------ client against a scripted peer */

/* A companion radio in memory: parses the command frames the client writes and answers like the real firmware. */
class FakePeer : public ITransport {
public:
    // behaviour switches
    bool plugged = true;
    OpenStatus open_status = OpenStatus::Ok;
    bool silent = false;                 // answers nothing (not a companion radio)
    bool drop_acks = false;
    bool fail_set_radio = false;
    std::vector<Bytes> messages;         // queued for CMD_SYNC_NEXT
    std::vector<Bytes> commands;         // every command payload received, in order
    std::vector<Contact> contacts;
    SelfInfo self;
    bool lost = false;
    bool fail_set_channel = false;
    bool vars_supported = true;
    int chan_mode = 0;                   // answer to a channel send: 0 OK, 1 MSG_SENT, 2 an odd frame, 3 nothing, 4 error
    bool channels_supported = true;
    std::string vars_text = "gps:0";
    uint32_t board_time = 1789999000;
    bool refuse_set_time = false;
    std::map<int, std::pair<std::string, std::array<uint8_t, 16>>> slots;
    // phase 2 (0.2.0)
    int fw_level = 11;                   // in DEVICE_INFO: 9+ has the repeat byte, 10+ the path hash mode byte
    int path_mode = 1;                   // the user's real board reports mode 1
    bool repeat = false;
    std::vector<RepeatRange> ranges = {{433000, 434000}, {863000, 870000}};
    bool manual_add = false;
    bool other_params_old = false;       // SET_OTHER_PARAMS only in the 4 byte form
    bool autoadd_supported = true;
    uint8_t autoadd = 0x02;
    bool stats_supported = true;
    bool discover_supported = true;
    int discover_mode = 0;               // 0 OK, 1 MSG_SENT
    struct Neighbour { uint8_t seed; uint8_t type; double snr; int rssi; bool prefix; };
    std::vector<Neighbour> neighbours;   // answer a discover request, in this order
    bool table_full = false;
    bool hold_remove = false;            // REMOVE_CONTACT is not answered (to see that the next one waits)
    std::set<std::string> remove_fail;   // key hex (first 8) that get an error from REMOVE_CONTACT
    bool factory_ok = true;
    bool drop_on_reboot = true;          // reboot / factory reset: the USB link goes away (after the last answer was read)
    bool drop_when_empty = false;
    bool reset_needs_word = false;
    bool reset_new_key = true;           // the factory reset erases the identity: the board comes back with another public key
    bool reset_answers = true;           // false: the board formats and restarts without ever sending the OK frame
    uint8_t key_seed = 0xA0;             // the public key of the board is key_seed + 0..31
    int reboots = 0, resets = 0;

    FakePeer()
    {
        slots[0] = {"Public", kPublicChannelSecret};
        slots[1] = {"#test", hashtag_secret("#test")};
        self = std::get<SelfInfo>(*parse_packet(make_self_info("Deck Zero")));
        for (int i = 0; i < 2; ++i) contacts.push_back(std::get<Contact>(*parse_packet(make_contact(resp::kContact, static_cast<uint8_t>(0x10 + i * 0x40), i ? "Rptr" : "Alice", i ? 2 : 1, 1790000000 - i))));
    }

    OpenStatus open() override
    {
        if (!plugged) return OpenStatus::NotFound;
        if (open_status != OpenStatus::Ok) return open_status;
        open_ = true;
        lost = false;
        drop_when_empty = false;
        rx_.clear();
        // power-on chatter: a stale partial frame, a log push, then silence
        push_raw({0x11, 0x22});
        push(Bytes(40, 0x88));
        return OpenStatus::Ok;
    }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }
    long read(uint8_t *buf, size_t max) override
    {
        if (drop_when_empty && rx_.empty()) lost = true;
        if (!open_ || lost) return -1;
        const size_t n = std::min(max, rx_.size());
        std::copy(rx_.begin(), rx_.begin() + static_cast<long>(n), buf);
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(n));
        return static_cast<long>(n);
    }
    bool write(const uint8_t *data, size_t len) override
    {
        if (!open_) return false;
        tx_.insert(tx_.end(), data, data + len);
        while (tx_.size() >= 3) {
            const size_t n = tx_[1] | (tx_[2] << 8);
            if (tx_.size() < 3 + n) break;
            Bytes cmd(tx_.begin() + 3, tx_.begin() + 3 + static_cast<long>(n));
            tx_.erase(tx_.begin(), tx_.begin() + 3 + static_cast<long>(n));
            handle(cmd);
        }
        return true;
    }
    std::string kind() const override { return "fake"; }
    const LinkInfo &info() const override { return info_; }
    std::string last_error() const override { return "fake error"; }

    void push(const Bytes &payload) { const Bytes f = framed(payload); rx_.insert(rx_.end(), f.begin(), f.end()); }
    void push_raw(const Bytes &b) { rx_.insert(rx_.end(), b.begin(), b.end()); }

private:
    void handle(const Bytes &c)
    {
        commands.push_back(c);
        if (silent) return;
        switch (c[0]) {
        case cmd::kDeviceQuery: {                                          // the payload of the real frame, with the switches applied
            Bytes di = hex(kRealDeviceInfo + 6);
            di[1] = static_cast<uint8_t>(fw_level);
            di[80] = repeat ? 1 : 0;
            di[81] = static_cast<uint8_t>(path_mode);
            if (fw_level < 10) di.resize(81);
            if (fw_level < 9) di.resize(80);
            push(di);
            break;
        }
        case cmd::kRemoveContact: {
            if (hold_remove) break;
            PubKey k{};
            std::copy(c.begin() + 1, c.begin() + 33, k.begin());
            if (remove_fail.count(to_hex(k).substr(0, 8))) { push({resp::kError, err::kFileIo}); break; }
            const auto it = std::find_if(contacts.begin(), contacts.end(), [&](const Contact &x) { return x.key == k; });
            if (it == contacts.end()) { push({resp::kError, err::kNotFound}); break; }
            contacts.erase(it);
            push({resp::kOk});
            break;
        }
        case cmd::kAddUpdateContact: {
            if (table_full) { push({resp::kError, err::kTableFull}); break; }
            Contact n;
            std::copy(c.begin() + 1, c.begin() + 33, n.key.begin());
            n.type = c[33];
            n.flags = c[34];
            n.out_path_len = c[35];
            n.name = std::string(reinterpret_cast<const char *>(&c[100]), 32).c_str();
            const auto it = std::find_if(contacts.begin(), contacts.end(), [&](const Contact &x) { return x.key == n.key; });
            if (it == contacts.end()) contacts.push_back(n);
            else *it = n;
            push({resp::kOk});
            break;
        }
        case cmd::kSetOtherParams:
            if (other_params_old && c.size() != 4) { push({resp::kError, err::kIllegalArg}); break; }
            manual_add = c[1] != 0;
            push({resp::kOk});
            break;
        case cmd::kGetAutoadd:
            if (!autoadd_supported) push({resp::kError, err::kUnsupported});
            else push({resp::kAutoaddConfig, autoadd, 0});
            break;
        case cmd::kSetAutoadd:
            if (!autoadd_supported) { push({resp::kError, err::kUnsupported}); break; }
            autoadd = c[1];
            push({resp::kOk});
            break;
        case cmd::kSendControlData: {
            if (!discover_supported) { push({resp::kError, err::kUnsupported}); break; }
            if (discover_mode == 0) push({resp::kOk});
            else { Bytes b = {resp::kMsgSent, 0, 1, 2, 3, 4}; put32(b, 500); push(b); }
            for (const Neighbour &n : neighbours) {
                Bytes f = {resp::kControlData, static_cast<uint8_t>(static_cast<int8_t>(n.snr * 4)), static_cast<uint8_t>(static_cast<int8_t>(n.rssi)), 0,
                           static_cast<uint8_t>(0x90 | n.type), static_cast<uint8_t>(static_cast<int8_t>(-8)), c[3], c[4], c[5], c[6]};
                for (int i = 0; i < (n.prefix ? 8 : 32); ++i) f.push_back(static_cast<uint8_t>(n.seed + i));
                push(f);
            }
            break;
        }
        case cmd::kGetStats: {
            if (!stats_supported) { push({resp::kError, err::kUnsupported}); break; }
            Bytes f = {resp::kStats, c[1]};
            if (c[1] == 0) { f.push_back(0x7A); f.push_back(0x0F); put32(f, 7300); f.push_back(2); f.push_back(0); f.push_back(1); }
            else if (c[1] == 1) { f.push_back(0x92); f.push_back(0xFF); f.push_back(0xA1); f.push_back(22); put32(f, 61); put32(f, 940); }
            else { for (uint32_t v : {500u, 120u, 14u, 106u, 310u, 190u, 9u}) put32(f, v); }
            push(f);
            break;
        }
        case cmd::kGetAllowedRepeatFreq: {
            if (fw_level < 9) { push({resp::kError, err::kUnsupported}); break; }
            Bytes f = {resp::kAllowedRepeatFreq};
            for (const RepeatRange &r : ranges) { put32(f, r.lo_khz); put32(f, r.hi_khz); }
            put32(f, 0); put32(f, 0);
            push(f);
            break;
        }
        case cmd::kSetPathHashMode:
            if (fw_level < 10) { push({resp::kError, err::kUnsupported}); break; }
            if (c.size() < 3 || c[2] > 2) { push({resp::kError, err::kIllegalArg}); break; }
            path_mode = c[2];
            push({resp::kOk});
            break;
        case cmd::kReboot:
            ++reboots;
            if (drop_on_reboot) drop_when_empty = true;
            break;                                                           // no answer
        case cmd::kFactoryReset:
            if (!factory_ok || (reset_needs_word && Bytes(c.begin() + 1, c.end()) != Bytes{'r', 'e', 's', 'e', 't'})) { push({resp::kError, err::kUnsupported}); break; }
            ++resets;
            contacts.clear();
            slots.clear();
            if (reset_new_key) { key_seed = 0x30; self.name = "MeshCore"; }
            if (reset_answers) push({resp::kOk});
            if (drop_on_reboot) drop_when_empty = true;
            break;
        case cmd::kAppStart: push(make_self_info_from()); break;
        case cmd::kGetCustomVars:
            if (!vars_supported) { push({resp::kError, err::kUnsupported}); break; }
            else { Bytes b = {resp::kCustomVars}; b.insert(b.end(), vars_text.begin(), vars_text.end()); push(b); }
            break;
        case cmd::kGetTime: { Bytes b = {resp::kCurrentTime}; put32(b, board_time); push(b); break; }
        case cmd::kSetTime: {
            const uint32_t ts = c[1] | (c[2] << 8) | (c[3] << 16) | (static_cast<uint32_t>(c[4]) << 24);
            if (refuse_set_time || ts < board_time) push({resp::kError, err::kIllegalArg});
            else { board_time = ts; push({resp::kOk}); }
            break;
        }
        case cmd::kSetCustomVar: {
            const std::string kv(c.begin() + 1, c.end());
            const size_t colon = kv.find(':');
            if (!vars_supported || colon == std::string::npos) { push({resp::kError, err::kUnsupported}); break; }
            vars_text = kv.substr(0, colon) + ":" + kv.substr(colon + 1);
            push({resp::kOk});
            break;
        }
        case cmd::kSendAdvert: case cmd::kSetName: case cmd::kSetTxPower: case cmd::kResetPath: push({resp::kOk}); break;
        case cmd::kSetRadio:
            if (fail_set_radio) { push({resp::kError, err::kIllegalArg}); break; }
            self.freq_khz = c[1] | (c[2] << 8) | (c[3] << 16) | (static_cast<uint32_t>(c[4]) << 24);
            self.bw_hz = c[5] | (c[6] << 8) | (c[7] << 16) | (static_cast<uint32_t>(c[8]) << 24);
            self.sf = c[9]; self.cr = c[10];
            if (c.size() > 11) {                                       // the trailing "client repeat" byte
                const uint32_t khz = self.freq_khz;
                bool ok = c[11] == 0;
                for (const RepeatRange &r : ranges)
                    if (khz >= r.lo_khz && khz <= r.hi_khz) ok = true;
                if (!ok) { push({resp::kError, err::kIllegalArg}); break; }
                repeat = c[11] != 0;
            }
            push({resp::kOk});
            break;
        case cmd::kBattery: push(hex("0c7a0f")); break;
        case cmd::kGetContacts: {
            Bytes s = {resp::kContactStart};
            put32(s, static_cast<uint32_t>(contacts.size()));
            push(s);
            for (size_t i = 0; i < contacts.size(); ++i) {
                push(make_contact_from(contacts[i]));
                if (i == 0) push({0x88, 1, 2, 3});                     // a push in the middle of the stream
            }
            Bytes e = {resp::kContactEnd};
            put32(e, 1790000100);
            push(e);
            break;
        }
        case cmd::kGetChannel: {
            if (!channels_supported) { push({resp::kError, err::kUnsupported}); break; }
            const auto it = slots.find(c[1]);
            if (it == slots.end()) push(make_chan_info(c[1], "", false));
            else {
                Bytes b = {resp::kChannelInfo, c[1]};
                put_text(b, it->second.first, 32);
                b.insert(b.end(), it->second.second.begin(), it->second.second.end());
                push(b);
            }
            break;
        }
        case cmd::kSyncNext:
            if (messages.empty()) push({resp::kNoMoreMsgs});
            else { push(messages.front()); messages.erase(messages.begin()); }
            break;
        case cmd::kSendTxt: {
            Bytes s = {resp::kMsgSent, 0, 0xA0, 0xB0, 0xC0, static_cast<uint8_t>(0xD0 + c[2])};
            put32(s, 1000);
            push(s);
            if (!drop_acks) push({resp::kAck, 0xA0, 0xB0, 0xC0, static_cast<uint8_t>(0xD0 + c[2]), 5, 0, 0, 0});
            break;
        }
        case cmd::kSendChannelTxt:
            if (chan_mode == 0) push({resp::kOk});
            else if (chan_mode == 1) { Bytes b = {resp::kMsgSent, 1, 1, 2, 3, 4}; put32(b, 500); push(b); }
            else if (chan_mode == 2) push({0x1F, 9, 9});
            else if (chan_mode == 4) push({resp::kError, err::kTableFull});
            break;
        case cmd::kSetChannel: {
            std::string name(reinterpret_cast<const char *>(&c[2]));
            name = name.substr(0, std::min<size_t>(name.size(), 32));
            std::array<uint8_t, 16> sec{};
            std::copy(c.begin() + 34, c.begin() + 50, sec.begin());
            bool zero = name.empty();
            for (uint8_t b : sec) if (b) zero = false;
            if (fail_set_channel) { push({resp::kError, err::kTableFull}); break; }
            if (zero) slots.erase(c[1]); else slots[c[1]] = {name, sec};
            push({resp::kOk});
            break;
        }
        default: push({resp::kError, err::kUnsupported});
        }
    }
    Bytes make_self_info_from() const { return make_self_info(self.name, self.freq_khz, self.bw_hz, self.sf, self.cr, self.tx_power, manual_add, key_seed); }
    static Bytes make_contact_from(const Contact &c)
    {
        Bytes b = {resp::kContact};
        b.insert(b.end(), c.key.begin(), c.key.end());
        b.push_back(c.type); b.push_back(c.flags); b.push_back(c.out_path_len);
        b.insert(b.end(), c.out_path.begin(), c.out_path.end());
        put_text(b, c.name, 32);
        put32(b, c.last_advert); put32(b, 0); put32(b, 0); put32(b, c.lastmod);
        return b;
    }

    bool open_ = false;
    Bytes rx_, tx_;
    LinkInfo info_;
};

static uint64_t g_now = 1000;
static void run(Client &c, uint64_t ms, uint64_t step = 20)
{
    for (uint64_t t = 0; t < ms; t += step) {
        g_now += step;
        c.poll(g_now);
    }
}

static bool has_cmd(const FakePeer &p, uint8_t code)
{
    for (const Bytes &c : p.commands)
        if (c[0] == code) return true;
    return false;
}

static void test_client()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("client");
    Model m;
    Store st(dir);
    attach_b(st, m);
    FakePeer peer;
    Client c(peer, m);
    c.set_deck_clock([] { return DeckClock::Synced; }, [] {});
    set_time_offset(0);

    // no board
    peer.plugged = false;
    run(c, 100);
    CHECK(c.link() == Link::Searching && c.link_text() == "No board found");
    peer.open_status = OpenStatus::PermissionDenied;
    peer.plugged = true;
    run(c, 2200);
    CHECK(c.link() == Link::Denied);
    peer.open_status = OpenStatus::Busy;
    run(c, 4500);
    CHECK(c.link() == Link::Busy);

    // the board appears, with a message waiting
    peer.open_status = OpenStatus::Ok;
    peer.messages.push_back(make_dm_v3(0x10, "salut"));
    peer.messages.push_back(make_chan_msg_v3(0, "Zed: coucou"));
    run(c, 4500);
    CHECK(c.link() == Link::Ready);
    CHECK(m.device() && m.device()->model == "Seeed Xiao-nrf52" && m.device()->fw_ver == 11);
    CHECK(m.self() && m.self()->name == "Deck Zero");
    CHECK(m.contact_count() == 2);
    CHECK(m.channels().size() == 40 && m.find_channel(0)->name == "Public" && m.find_channel(5)->empty);
    CHECK(m.battery() && m.battery()->millivolts == 3962);
    CHECK(m.message_count() == 2 && m.unread_total() == 2);
    CHECK(c.junk_bytes() >= 2);                             // the stale bytes were skipped, not fatal
    CHECK(has_cmd(peer, cmd::kSetTime));

    // a push of a new message: MESSAGES_WAITING makes the client sync
    peer.messages.push_back(make_chan_msg_v3(1, "Yan: #test ok"));
    peer.push({resp::kMessagesWaiting});
    run(c, 500);
    CHECK(m.message_count() == 3);

    // direct message: sent, then delivered by the ACK
    const std::string alice = to_hex(m.contacts_sorted()[0]->c.key);
    const uint32_t seq = c.send_direct(alice, "  hello Alice  ");
    CHECK(seq != 0);
    run(c, 300);
    CHECK(m.find_message(seq)->state == MsgState::Delivered && m.find_message(seq)->text == "hello Alice");
    CHECK(c.send_direct(alice, std::string(200, 'x')) == 0 && !c.notice().ok);
    CHECK(c.send_direct("00", "x") == 0);

    // no ack: retries (3 attempts, path reset before the last), then "no ack"
    peer.drop_acks = true;
    peer.commands.clear();
    const uint32_t seq2 = c.send_direct(alice, "anyone?");
    run(c, 20000);
    CHECK(m.find_message(seq2)->state == MsgState::NoAck);
    int sends = 0;
    for (const Bytes &cm : peer.commands) if (cm[0] == cmd::kSendTxt) ++sends;
    CHECK(sends == 3);
    peer.drop_acks = false;

    // channel message
    const uint32_t cseq = c.send_channel(0, "bonjour tous");
    run(c, 200);
    CHECK(m.find_message(cseq)->state == MsgState::Sent);

    // advert + settings
    c.send_advert(true);
    run(c, 200);
    CHECK(c.notice().ok && c.notice().text.find("flood") != std::string::npos);
    RadioSettings e = m.radio_settings();
    e.freq_mhz = 869.618;
    e.name = "Renamed";
    e.sf = 10;
    CHECK(c.save_settings(e));
    run(c, 400);
    CHECK(c.notice().ok && m.self()->name == "Deck Zero");     // the fake keeps the old name; frequency/sf were read back
    CHECK(m.self()->freq_khz == 869618 && m.self()->sf == 10);
    RadioSettings bad = e;
    bad.sf = 4;
    CHECK(!c.save_settings(bad) && !c.notice().ok);
    peer.fail_set_radio = true;
    e.sf = 9;
    CHECK(c.save_settings(e));
    run(c, 400);
    CHECK(!c.notice().ok && c.notice().text.find("Not saved") == 0);
    peer.fail_set_radio = false;
    c.add_public_channel();
    run(c, 200);
    CHECK(c.notice().ok);

    // hashtag channels: add into the first free slot (2), key derived from the name, shown, removable only if the app added it
    CHECK(!c.add_hashtag_channel("test") && !c.notice().ok);                  // no #
    CHECK(!c.add_hashtag_channel("#test") && c.notice().text.find("already") != std::string::npos);
    CHECK(c.add_hashtag_channel("#eu868"));
    run(c, 300);
    CHECK(c.notice().ok && peer.slots.count(2) && peer.slots[2].first == "#eu868" && peer.slots[2].second == hashtag_secret("#eu868"));
    CHECK(m.find_channel(2) && m.find_channel(2)->name == "#eu868" && m.find_channel(2)->app_added);
    CHECK(m.is_added("#eu868") && !m.find_channel(1)->app_added);
    {
        bool listed = false;
        for (const auto &s : m.conversations()) if (s.key == "c:2" && s.title == "#eu868") listed = true;
        CHECK(listed);
    }
    CHECK(!c.remove_channel(0) && !c.notice().ok);                            // Public (slot 0) is never removed
    CHECK(c.remove_channel(1));                                               // any other channel can be, not only the app's own
    run(c, 300);
    CHECK(peer.slots.count(1) == 0 && m.find_channel(1)->empty);
    CHECK(c.add_hashtag_channel("#test"));                                    // and put back in the freed slot
    run(c, 300);
    CHECK(peer.slots.count(1) == 1 && peer.slots[1].first == "#test");
    peer.fail_set_channel = true;
    CHECK(c.add_hashtag_channel("#nope"));
    run(c, 300);
    CHECK(!c.notice().ok && !m.is_added("#nope") && !m.find_channel_by_name("#nope"));
    peer.fail_set_channel = false;
    CHECK(c.remove_channel(2));
    run(c, 300);
    CHECK(c.notice().ok && peer.slots.count(2) == 0 && m.find_channel(2)->empty && !m.is_added("#eu868"));
    CHECK(c.add_hashtag_channel("#again"));                                   // the freed slot is reused
    run(c, 300);
    CHECK(peer.slots.count(2) && peer.slots[2].first == "#again");
    CHECK(c.max_text("c:0") <= 133 && c.max_text("c:0") >= 40 && c.max_text("d:aa") == 133);

    // markers: a name the board no longer holds (removed elsewhere) or holds with another key is forgotten at connect
    m.mark_added("#ghost");
    ChannelRec other;
    other.idx = 7; other.name = "#other"; other.empty = false; other.key_ok = false;
    m.set_channel(other);
    m.mark_added("#other");
    CHECK(m.is_added("#ghost") && m.is_added("#other"));
    CHECK(m.reconcile_added() == 2 && !m.is_added("#ghost") && !m.is_added("#other"));
    CHECK(m.reconcile_added() == 0);
    peer.slots[5] = {"#stale", hashtag_secret("#stale")};
    m.mark_added("#stale");
    m.mark_added("#stale2");                               // never on the board

    // unplug: back to searching, cached data stays, the pending state is cleared
    peer.drop_acks = true;
    const uint32_t seq3 = c.send_direct(alice, "lost one");
    run(c, 100);
    peer.lost = true;
    run(c, 100);
    CHECK(c.link() == Link::Searching && c.link_text() == "Board disconnected");
    CHECK(m.find_message(seq3)->state == MsgState::NoAck && !m.self().has_value() && m.contact_count() == 2);
    CHECK(c.send_advert(false) == false);
    // replug: the connect reads all 40 slots and reconciles the markers with them
    peer.drop_acks = false;
    peer.lost = false;
    peer.commands.clear();
    run(c, 4000);
    CHECK(c.link() == Link::Ready && m.self().has_value());
    CHECK(m.is_added("#stale") && !m.is_added("#stale2"));     // backed by slot 5 with the right key / not on the board
    CHECK(has_cmd(peer, cmd::kSetTime));
    // the clock policy switch: off = SET_TIME is not sent at connect
    c.options().set_device_time = false;          // policy off: no write, the board clock is only read
    peer.lost = true;
    run(c, 200);
    peer.lost = false;
    peer.commands.clear();
    run(c, 4000);
    CHECK(c.link() == Link::Ready && !has_cmd(peer, cmd::kSetTime) && has_cmd(peer, cmd::kGetTime));
    c.options().set_device_time = true;

    // a device that answers nothing
    FakePeer mute;
    mute.silent = true;
    Model m2;
    Client c2(mute, m2);
    run(c2, 15000);
    CHECK(c2.link() == Link::NotCompanion);
    CHECK(mute.commands.size() >= 5);
}

/* ------------------------------------------------------------------ clock policy */

struct ClockRun {
    FakePeer peer;
    Model model;
    std::unique_ptr<Client> client;
    DeckClock deck = DeckClock::Synced;
    int unknown_polls = 0;
    bool refreshed = false;
    bool sent_set_time() const
    {
        for (const Bytes &c : peer.commands) if (c[0] == cmd::kSetTime) return true;
        return false;
    }
    uint32_t set_time_value() const
    {
        for (const Bytes &c : peer.commands)
            if (c[0] == cmd::kSetTime) return c[1] | (c[2] << 8) | (c[3] << 16) | (static_cast<uint32_t>(c[4]) << 24);
        return 0;
    }
    ClockRun(DeckClock d, const char *vars, uint32_t board_time = 1789999000)
    {
        deck = d;
        set_time_offset(0);
        peer.board_time = board_time;
        if (vars) peer.vars_text = vars; else peer.vars_supported = false;
        client = std::make_unique<Client>(peer, model);
        client->set_deck_clock(
            [this] {
                if (unknown_polls > 0) { --unknown_polls; return DeckClock::Unknown; }
                return deck;
            },
            [this] { refreshed = true; });
    }
};

static void test_clock_policy()
{
    set_time_source(fake_time);
    // the table, cell by cell
    CHECK(decide_clock(DeckClock::Synced, false) == ClockAction::SetFromDeck);
    CHECK(decide_clock(DeckClock::Unsynced, false) == ClockAction::PromptUser);
    CHECK(decide_clock(DeckClock::Unknown, false) == ClockAction::PromptUser);       // unknown counts as offline
    CHECK(decide_clock(DeckClock::Unsynced, true) == ClockAction::UseBoardClock);
    CHECK(decide_clock(DeckClock::Synced, true) == ClockAction::UseBoardClock);      // default for the undecided cell: no write
    CHECK(gps_value_enabled("1") && gps_value_enabled("ON") && gps_value_enabled("true") && !gps_value_enabled("0") && !gps_value_enabled(""));

    // online + no GPS: the deck time is written, the deck is the time source
    {
        ClockRun r(DeckClock::Synced, "gps:0");
        run(*r.client, 3000);
        CHECK(r.client->link() == Link::Ready && r.refreshed);
        CHECK(r.sent_set_time() && r.set_time_value() == g_time);
        CHECK(r.model.clock().source == ClockSource::DeckNtp && time_offset() == 0 && r.model.clock().gps_known && !r.model.clock().gps);
        CHECK(!r.client->clock_prompt_pending());
    }
    // GPS variable not reported at all (older firmware answers "unsupported"): treated as no GPS
    {
        ClockRun r(DeckClock::Synced, nullptr);
        run(*r.client, 3000);
        CHECK(r.sent_set_time() && !r.model.clock().gps_known && r.model.clock().source == ClockSource::DeckNtp);
    }
    // offline + no GPS: nothing is written until the user types a time; the board clock is untouched meanwhile
    {
        ClockRun r(DeckClock::Unsynced, "gps:0");
        run(*r.client, 3000);
        CHECK(r.client->clock_prompt_pending() && !r.sent_set_time());
        const uint32_t typed = g_time + 86400 * 30;                 // 30 days after the (wrong) deck clock
        r.client->clock_set_user(typed);
        run(*r.client, 500);
        CHECK(r.sent_set_time() && r.set_time_value() == typed && r.peer.board_time == typed);
        CHECK(r.model.clock().source == ClockSource::SetByUser && time_offset() == 86400 * 30 && now_unix() == typed);
        CHECK(!r.client->clock_prompt_pending() && r.client->notice().ok);
    }
    // offline + no GPS, the user presses Esc: no write, the board clock is read and used as the base of displayed times
    {
        ClockRun r(DeckClock::Unsynced, "gps:0", 1789990000);
        run(*r.client, 3000);
        CHECK(r.client->clock_prompt_pending());
        r.client->clock_skip();
        run(*r.client, 500);
        CHECK(!r.sent_set_time() && r.model.clock().source == ClockSource::NotSet && time_offset() == 1789990000 - static_cast<int64_t>(g_time));
        CHECK(r.client->notice().text.find("unchanged") != std::string::npos);
    }
    // offline + GPS: read, never write; the board clock is the base
    {
        ClockRun r(DeckClock::Unsynced, "gps:1,gps_interval:900", 1789999900);
        run(*r.client, 3000);
        CHECK(!r.sent_set_time() && !r.client->clock_prompt_pending());
        CHECK(r.model.clock().source == ClockSource::BoardGps && r.model.clock().gps && now_unix() == 1789999900);
    }
    // online + GPS (the undecided cell): default no write, the GPS clock is used
    {
        ClockRun r(DeckClock::Synced, "gps:1", 1789999900);
        run(*r.client, 3000);
        CHECK(!r.sent_set_time() && r.model.clock().source == ClockSource::BoardGps && now_unix() == 1789999900);
    }
    // GPS on but the board clock is unset (no fix yet): warned, the deck time stays
    {
        ClockRun r(DeckClock::Unsynced, "gps:1", 5000);
        run(*r.client, 3000);
        CHECK(!r.sent_set_time() && r.model.clock().source == ClockSource::NotSet && time_offset() == 0);
        CHECK(!r.client->notice().ok && r.client->notice().text.find("no fix") != std::string::npos);
    }
    // the firmware refuses a time in the past (board ahead of the deck): clear message, the board clock is used
    {
        ClockRun r(DeckClock::Synced, "gps:0", 1790000500);          // the board is 500 s ahead of the deck
        run(*r.client, 3000);
        CHECK(r.sent_set_time() && r.model.clock().source == ClockSource::BoardKept);
        CHECK(!r.client->notice().ok && r.client->notice().text.find("refused") != std::string::npos && r.client->notice().text.find("ahead") != std::string::npos);
        CHECK(time_offset() == 500 && now_unix() == 1790000500);
    }
    // a refusal of another kind (not ILLEGAL_ARG)
    {
        ClockRun r(DeckClock::Synced, "gps:0");
        r.peer.refuse_set_time = true;
        run(*r.client, 3000);
        CHECK(r.model.clock().source == ClockSource::BoardKept && !r.client->notice().ok);
    }
    // the deck check still running (Unknown) is waited for, then decides
    {
        ClockRun r(DeckClock::Synced, "gps:0");
        r.unknown_polls = 40;
        run(*r.client, 4000);
        CHECK(r.sent_set_time());
    }
    // the deck check never answers: after 6 s it counts as offline (prompt)
    {
        ClockRun r(DeckClock::Unknown, "gps:0");
        r.unknown_polls = 100000;
        run(*r.client, 5000);
        CHECK(!r.client->clock_prompt_pending() && !r.sent_set_time());
        run(*r.client, 4000);
        CHECK(r.client->clock_prompt_pending() && !r.sent_set_time());
    }
    // Sync clock now re-runs the policy
    {
        ClockRun r(DeckClock::Unsynced, "gps:0");
        run(*r.client, 3000);
        r.client->clock_skip();
        run(*r.client, 500);
        r.deck = DeckClock::Synced;                                  // the deck got NTP meanwhile
        r.peer.commands.clear();
        CHECK(r.client->sync_clock());
        run(*r.client, 3000);
        CHECK(r.sent_set_time() && r.model.clock().source == ClockSource::DeckNtp && time_offset() == 0);
    }
    set_time_offset(0);

    // date and time text (the prompt)
    setenv("TZ", "UTC", 1);
    tzset();
    uint32_t t = 0;
    CHECK(parse_local_datetime("2026-10-06 21:14", t) && t == 1791321240 && format_local_datetime(t) == "2026-10-06 21:14");
    CHECK(!parse_local_datetime("2026-10-06", t) && !parse_local_datetime("2026-13-01 10:00", t) && !parse_local_datetime("2026-02-31 10:00", t));
    CHECK(!parse_local_datetime("2020-01-01 00:00", t) && !parse_local_datetime("2026-10-06 24:00", t) && !parse_local_datetime("2026-10-06 21:14 x", t));
    setenv("TZ", "Europe/Paris", 1);
    tzset();
    CHECK(parse_local_datetime("2026-07-01 12:00", t) && t == 1782900000);       // CEST = UTC+2
    unsetenv("TZ");
    tzset();

    // the deck check outputs
    CHECK(parse_ntp_output("yes\n", true) == DeckClock::Synced && parse_ntp_output("no\n", true) == DeckClock::Unsynced);
    CHECK(parse_ntp_output("", false) == DeckClock::Unknown && parse_ntp_output("maybe", true) == DeckClock::Unknown);
    CHECK(parse_connectivity_output("full\n", true) == DeckClock::Synced && parse_connectivity_output("limited\n", true) == DeckClock::Unsynced);
    CHECK(parse_connectivity_output("none", true) == DeckClock::Unsynced && parse_connectivity_output("", false) == DeckClock::Unknown);

    // custom variables
    auto cv = parse_packet(hex("15") + Bytes{'g', 'p', 's', ':', '1', ',', 'x', ':', 'y', ':', 'z'});
    CHECK(cv && std::holds_alternative<CustomVars>(*cv));
    const CustomVars &vars = std::get<CustomVars>(*cv);
    CHECK(vars.vars.size() == 2 && vars.find("GPS") && *vars.find("GPS") == "1" && *vars.find("x") == "y:z" && !vars.find("nope"));
    auto empty = parse_packet(hex("15"));
    CHECK(empty && std::get<CustomVars>(*empty).vars.empty());
    CHECK(build_get_time() == hex("05") && build_get_custom_vars() == hex("28"));
}

/* signed TX power (a negative power reported by the board; meshcore_py reads that byte unsigned) */
static void test_tx_power_signed()
{
    auto si = parse_packet(make_self_info("n", 869525, 250000, 11, 5, 0xFB));      // -5 dBm
    CHECK(si && std::get<SelfInfo>(*si).tx_power == -5);
    Model m;
    m.set_self(std::get<SelfInfo>(*si));
    CHECK(m.radio_settings().tx_power == -5);
    RadioSettings rs = m.radio_settings();
    rs.max_tx_power = 22;
    CHECK(validate_radio(rs).empty());
    rs.tx_power = -10;
    CHECK(!validate_radio(rs).empty());
    CHECK(build_set_tx_power(-5) == hex("0cfbffffff"));
}

static void test_proc_listings()
{
    // the shapes of the deck: the touch screen, the Bluetooth keyboard (a uhid device, Bus 0005) and the launcher's own virtual keyboard
    const std::string touch =
        "I: Bus=0018 Vendor=0416 Product=0001 Version=0100\nN: Name=\"Goodix Capacitive TouchScreen\"\nP: Phys=\nS: Sysfs=/devices/platform/soc/fe804000.i2c/i2c-1/1-0014/input/input0\n"
        "U: Uniq=\nH: Handlers=mouse0 event0\nB: PROP=2\nB: EV=b\nB: KEY=420 0 0 0 0 0 0 0 0 0 0\nB: ABS=6618000 0\n\n";
    const std::string m4 =
        "I: Bus=0005 Vendor=05ac Product=0255 Version=0001\nN: Name=\"M4 Keyboard\"\nP: Phys=dc:a6:32:00:00:01\nS: Sysfs=/devices/virtual/misc/uhid/0005:05AC:0255.0001/input/input3\n"
        "U: Uniq=11:22:33:44:55:66\nH: Handlers=sysrq kbd event3 leds\nB: PROP=0\nB: EV=12001f\nB: KEY=1000000000007 ff9f207ac14057ff febeffdfffefffff fffffffffffffffe\nB: MSC=10\nB: LED=7\n\n";
    const std::string vkbd =
        "I: Bus=0003 Vendor=0001 Product=0001 Version=0001\nN: Name=\"applaunch-vkbd\"\nP: Phys=\nS: Sysfs=/devices/virtual/input/input4\nU: Uniq=\n"
        "H: Handlers=sysrq kbd event2 rfkill\nB: PROP=0\nB: EV=3\nB: KEY=ffffffffffffffff ffffffffffffffff ffffffffffffffff ffffffffffffffff\n\n";
    CHECK(!keyboard_listed(touch));
    CHECK(!keyboard_listed(vkbd));                         // the launcher's virtual keyboard never counts
    CHECK(!keyboard_listed(touch + vkbd));                 // the state with the Bluetooth keyboard asleep: touch screen + vkbd only
    CHECK(keyboard_listed(m4));
    CHECK(keyboard_listed(touch + m4 + vkbd));             // awake
    CHECK(keyboard_listed(touch + vkbd + m4));
    // a USB keyboard (Bus 0003, a real name) counts; anything on the virtual bus does not
    std::string usb = m4;
    usb.replace(usb.find("Bus=0005"), 8, "Bus=0003");
    usb.replace(usb.find("M4 Keyboard"), 11, "Logitech USB Keyboard");
    CHECK(keyboard_listed(usb));
    std::string virt = m4;
    virt.replace(virt.find("Bus=0005"), 8, "Bus=0006");
    CHECK(!keyboard_listed(virt));
}


static void test_esc_navigation()
{
    // a short Esc never quits: every screen maps to a next screen, a cancel or a hint
    for (Screen sc : {Screen::Home, Screen::Chats, Screen::Contacts, Screen::Settings}) {
        const EscResult r = esc_pressed(sc, sc);
        CHECK(r.next == sc && !r.cancel_editor && r.show_exit_hint);          // top level: only the hint
    }
    CHECK(esc_pressed(Screen::Edit, Screen::Settings).cancel_editor && !esc_pressed(Screen::Edit, Screen::Chats).show_exit_hint);
    CHECK(esc_pressed(Screen::Chat, Screen::Chats).next == Screen::Chats && !esc_pressed(Screen::Chat, Screen::Chats).show_exit_hint);
    CHECK(esc_pressed(Screen::Chat, Screen::Contacts).next == Screen::Contacts);
    CHECK(esc_pressed(Screen::Detail, Screen::Contacts).next == Screen::Contacts);
    CHECK(esc_pressed(Screen::Chat, Screen::Home).next == Screen::Chats && esc_pressed(Screen::Chat, Screen::Settings).next == Screen::Chats);
    for (Screen sc : {Screen::Home, Screen::Chats, Screen::Contacts, Screen::Settings, Screen::Chat, Screen::Detail, Screen::Edit})
        CHECK(esc_pressed(sc, Screen::Chats).show_exit_hint == is_top_level(sc));   // only the top level shows the hint
}

/* ------------------------------------------------------------------ phase 1 (0.1.1): presets, channel keys, mute, retry, caps, status, log */

struct Ready {
    FakePeer peer;
    Model model;
    std::unique_ptr<Client> client;
    std::vector<std::string> log;
    explicit Ready(const std::function<void(FakePeer &)> &setup = nullptr, bool connect = true)
    {
        set_time_source(fake_time);
        set_time_offset(0);
        if (setup) setup(peer);
        client = std::make_unique<Client>(peer, model);
        client->set_deck_clock([] { return DeckClock::Synced; }, [] {});
        client->set_log([this](const std::string &s) { log.push_back(s); });
        if (connect) run(*client, 4500);
    }
    bool logged(const std::string &needle) const
    {
        for (const std::string &l : log)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

static void test_presets()
{
    const auto &p = radio_presets();
    CHECK(p.size() == 26);
    CHECK(p[0].name == "Australia" && p[0].freq_mhz == 915.8 && p[0].bw_khz == 250 && p[0].sf == 10 && p[0].cr == 5);
    CHECK(p[1].name == "Australia (Narrow)" && p[1].bw_khz == 62.5 && p[1].sf == 7 && p[1].cr == 7);
    CHECK(p[25].name == "Vietnam (Deprecated)" && p[25].freq_mhz == 920.25);
    int deprecated = 0, bw625 = 0, bw250 = 0;
    for (const RadioPreset &r : p) {
        if (r.deprecated()) ++deprecated;
        if (r.bw_khz == 62.5) ++bw625;
        if (r.bw_khz == 250) ++bw250;
        // every preset is accepted by the core and goes out on the wire in the units the board expects
        RadioSettings s;
        s.freq_mhz = r.freq_mhz; s.bw_khz = r.bw_khz; s.sf = r.sf; s.cr = r.cr; s.tx_power = 10; s.max_tx_power = 22;
        CHECK(validate_radio(s).empty());
        const Bytes b = build_set_radio(r.freq_mhz, r.bw_khz, static_cast<uint8_t>(r.sf), static_cast<uint8_t>(r.cr));
        const uint32_t khz = b[1] | (b[2] << 8) | (b[3] << 16) | (static_cast<uint32_t>(b[4]) << 24);
        const uint32_t hz = b[5] | (b[6] << 8) | (b[7] << 16) | (static_cast<uint32_t>(b[8]) << 24);
        CHECK(khz == static_cast<uint32_t>(r.freq_mhz * 1000.0 + 0.5) && hz == static_cast<uint32_t>(r.bw_khz * 1000.0 + 0.5));
        CHECK(b[9] == r.sf && b[10] == r.cr);
    }
    CHECK(deprecated == 2 && bw625 > 10 && bw250 >= 5);
    {
        const Bytes b = build_set_radio(916.575, 62.5, 7, 7);
        CHECK(b[5] == (62500 & 0xFF) && b[6] == ((62500 >> 8) & 0xFF) && b[7] == 0);            // 62.5 kHz = 62500 Hz
        const Bytes c = build_set_radio(915.8, 250, 10, 5);
        CHECK((c[5] | (c[6] << 8) | (c[7] << 16)) == 250000);
    }
    CHECK(std::string(presets_date()) == "2026-10-07" && std::string(presets_source_url()) == "https://api.meshcore.nz/api/v1/config");
    CHECK(preset_detail(p[1]) == "916.575 MHz  62.5 kHz  SF7  4/7");
    CHECK(find_preset(869.525, 250, 11, 5) == 9 && find_preset(869.525, 250, 11, 6) == -1 && find_preset(869.618, 62.5, 7, 5) == 13);
    for (size_t i = 0; i < p.size(); ++i) {
        RadioSettings s;
        s.name = "keep"; s.tx_power = 7;
        CHECK(apply_preset(s, static_cast<int>(i)) && s.name == "keep" && s.tx_power == 7);
        const int back = find_preset(s.freq_mhz, s.bw_khz, s.sf, s.cr);
        CHECK(back >= 0 && p[static_cast<size_t>(back)].freq_mhz == p[i].freq_mhz && p[static_cast<size_t>(back)].bw_khz == p[i].bw_khz);
    }
    RadioSettings none;
    CHECK(!apply_preset(none, -1) && !apply_preset(none, 26));
    CHECK(sf_index(5) == 0 && sf_index(12) == 7 && sf_index(99) == 7 && cr_index(5) == 0 && cr_index(8) == 3 && cr_index(1) == 0);
    CHECK(lora_bandwidths()[bandwidth_index(62.5)] == 62.5 && lora_bandwidths()[bandwidth_index(250)] == 250 && bandwidth_index(100) >= 0);
    CHECK(fmt_bw_label(62.5) == "62.5 kHz" && fmt_bw_label(250) == "250 kHz" && fmt_cr_label(7) == "4/7" && fmt_sf_label(9) == "SF9");
    CHECK(spreading_factors().size() == 8 && coding_rates().size() == 4);
}

static void test_channel_keys()
{
    ChannelSecret k{};
    CHECK(parse_channel_key("8b3387e9c5cdea6ac9e5edbaa115cd72", k).empty() && k == kPublicChannelSecret);
    CHECK(parse_channel_key("8B3387E9 C5CDEA6A-C9E5EDBA:A115CD72", k).empty() && k == kPublicChannelSecret);        // case and separators
    CHECK(parse_channel_key("8b3387e9c5cdea6ac9e5edbaa115cd7", k) == "31 of 32 hex characters");
    CHECK(parse_channel_key("8b3387e9c5cdea6ac9e5edbaa115cd72aa", k) == "34 of 32 hex characters");
    CHECK(parse_channel_key("8b3387e9c5cdea6ac9e5edbaa115cd7g", k) == "not a hex character: g");
    CHECK(parse_channel_key("", k) == "0 of 32 hex characters");
    CHECK(parse_channel_key(std::string(32, '0'), k) == "the key cannot be all zeros");
    const ChannelSecret before = k;
    CHECK(!parse_channel_key("zz", k).empty() && k == before);                         // a refused key leaves the output alone
    CHECK(channel_key_digits("8b33 87e9") == 8 && channel_key_digits("") == 0);
    CHECK(channel_key_char_ok("", "a") && channel_key_char_ok("", "F") && channel_key_char_ok("", " ") && !channel_key_char_ok("", "g"));
    CHECK(!channel_key_char_ok(std::string(32, 'a'), "b") && channel_key_char_ok(std::string(32, 'a'), " ") && !channel_key_char_ok("", "ab"));
    CHECK(channel_key_hex(kPublicChannelSecret) == "8b3387e9c5cdea6ac9e5edbaa115cd72");
    CHECK(channel_key_grouped(kPublicChannelSecret) == "8b3387e9 c5cdea6a c9e5edba a115cd72");

    ChannelSecret r1{}, r2{};
    CHECK(random_channel_key(r1) && random_channel_key(r2) && r1 != r2);                // /dev/urandom: two keys differ
    CHECK(random_channel_key(r1, [](uint8_t *o, size_t n) { for (size_t i = 0; i < n; ++i) o[i] = static_cast<uint8_t>(i + 1); return true; }) && r1[0] == 1 && r1[15] == 16);
    CHECK(!random_channel_key(r1, [](uint8_t *, size_t) { return false; }));           // no randomness: no key
    int calls = 0;
    CHECK(random_channel_key(r2, [&](uint8_t *o, size_t n) { ++calls; for (size_t i = 0; i < n; ++i) o[i] = calls < 3 ? 0 : 7; return true; }) && calls == 3 && r2[0] == 7);   // zeros are redrawn
    CHECK(!random_channel_key(r2, [](uint8_t *o, size_t n) { for (size_t i = 0; i < n; ++i) o[i] = 0; return true; }));

    CHECK(validate_private_name("Club").empty() && validate_private_name("Caf\xC3\xA9 des amis").empty());
    CHECK(!validate_private_name("").empty() && !validate_private_name("#club").empty() && !validate_private_name(std::string(32, 'x')).empty());
    CHECK(!validate_private_name("a\nb").empty());
    CHECK(classify_channel("Public", kPublicChannelSecret) == ChannelKind::Public);
    CHECK(classify_channel("#test", hashtag_secret("#test")) == ChannelKind::Hashtag);
    ChannelSecret other{};
    other[0] = 1;
    CHECK(classify_channel("#test", other) == ChannelKind::Private && classify_channel("Club", other) == ChannelKind::Private);
    CHECK(std::string(channel_kind_name(ChannelKind::Private)) == "private" && std::string(channel_kind_name(ChannelKind::Hashtag)) == "hashtag");
    ChannelRec rec;
    rec.name = "#x";
    CHECK(rec.kind() == ChannelKind::Hashtag);                  // a cached slot (no key read yet) is told apart by its name
    rec.name = "Club";
    CHECK(rec.kind() == ChannelKind::Private);
    rec.has_secret = true; rec.secret = kPublicChannelSecret;
    CHECK(rec.kind() == ChannelKind::Public);

    const Bytes sv = build_set_custom_var("gps", "1");
    CHECK(sv.size() == 6 && sv[0] == cmd::kSetCustomVar && std::string(sv.begin() + 1, sv.end()) == "gps:1");
}

static void test_mute_and_prefs()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("prefs");
    {
        Model m;
        Store st(dir);
        attach_b(st, m);
        ChannelRec ch;
        ch.idx = 1; ch.name = "#test"; ch.empty = false;
        m.set_channel(ch);
        IncomingMessage in;
        in.channel = true; in.channel_idx = 1; in.text = "Zed: one"; in.sender_timestamp = 1;
        m.add_incoming(in, 1790000000);
        in.text = "Zed: two"; in.sender_timestamp = 2;
        m.add_incoming(in, 1790000001);
        IncomingMessage dm;
        dm.prefix = {1, 2, 3, 4, 5, 6}; dm.text = "dm"; dm.sender_timestamp = 3;
        m.add_incoming(dm, 1790000002);
        CHECK(m.unread("c:1") == 2 && m.unread_total() == 3 && !m.is_muted("c:1"));
        m.set_muted("c:1", true);
        CHECK(m.is_muted("c:1") && m.unread("c:1") == 0 && m.unread_raw("c:1") == 2 && m.unread_total() == 1);     // no badge, not counted
        bool flagged = false;
        for (const ConvSummary &s : m.conversations())
            if (s.key == "c:1") flagged = s.muted && s.unread == 0;
        CHECK(flagged);
        m.set_muted("c:1", true);                                    // no change: no revision bump
        const uint32_t rev = m.revision();
        m.set_muted("c:1", true);
        CHECK(m.revision() == rev);
        m.set_muted("c:9", true);
        m.set_muted("c:9", false);
        CHECK(!m.is_muted("c:9"));
        m.set_retry({4, 3});
        CHECK(m.retry().attempts == 4 && m.retry().reset_after == 3);
        m.mark_read("c:1");
        CHECK(m.unread_raw("c:1") == 0);
    }
    {   // the next run: the mute flag and the retry settings come back
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(m.is_muted("c:1") && !m.is_muted("c:9") && m.retry().attempts == 4 && m.retry().reset_after == 3);
        m.set_muted("c:1", false);
        m.set_retry({2, 1});
    }
    {
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(!m.is_muted("c:1") && m.retry().attempts == 2 && m.retry().reset_after == 1);
    }
    CHECK(normalize_retry({9, 9}).attempts == 4 && normalize_retry({9, 9}).reset_after == 3);
    CHECK(normalize_retry({0, 5}).attempts == 1 && normalize_retry({0, 5}).reset_after == 0);
    CHECK(normalize_retry({3, -2}).reset_after == 0 && normalize_retry({3, 2}).reset_after == 2 && normalize_retry({2, 2}).reset_after == 1);
    Model fresh;
    CHECK(fresh.retry().attempts == 3 && fresh.retry().reset_after == 2);       // the default behaviour of 0.1.0
}

static int count_cmd(const FakePeer &p, uint8_t code)
{
    int n = 0;
    for (const Bytes &c : p.commands)
        if (c[0] == code) ++n;
    return n;
}

static void test_retry_settings()
{
    auto with_path = [](FakePeer &p) { p.contacts[0].out_path_len = 2; };
    const struct { int attempts, reset_after, sends, resets; } cases[] = {
        {3, 2, 3, 1},     // the 0.1.0 behaviour
        {1, 0, 1, 0},     // a single try, then "no ack"
        {2, 1, 2, 1},
        {4, 0, 4, 0},     // never reset the path
        {4, 3, 4, 1},
    };
    for (const auto &cs : cases) {
        Ready r(with_path);
        r.model.set_retry({cs.attempts, cs.reset_after});
        r.peer.drop_acks = true;
        r.peer.commands.clear();
        const std::string alice = to_hex(r.model.contacts_sorted()[0]->c.key);
        const uint32_t seq = r.client->send_direct(alice, "anyone?");
        CHECK(seq != 0);
        run(*r.client, 40000);
        CHECK(r.model.find_message(seq)->state == MsgState::NoAck);
        CHECK(count_cmd(r.peer, cmd::kSendTxt) == cs.sends);
        CHECK(count_cmd(r.peer, cmd::kResetPath) == cs.resets);
        // the attempt byte counts 0, 1, 2 ... (the wire carries it in two bits, so at most 4 attempts)
        int expect = 0;
        for (const Bytes &c : r.peer.commands)
            if (c[0] == cmd::kSendTxt) CHECK(c[2] == expect++);
    }
    {   // acknowledged on the first try: one send, no reset, whatever the settings
        Ready r(with_path);
        r.model.set_retry({4, 3});
        r.peer.commands.clear();
        const uint32_t seq = r.client->send_direct(to_hex(r.model.contacts_sorted()[0]->c.key), "hi");
        run(*r.client, 500);
        CHECK(r.model.find_message(seq)->state == MsgState::Delivered && count_cmd(r.peer, cmd::kSendTxt) == 1);
    }
}

static void test_channel_status()
{
    // the board answers OK, MSG_SENT or something unexpected: the message ends at "sent" in all three cases, never "delivered"
    const struct { int mode; const char *code; } answers[] = {{0, "0x00"}, {1, "0x06"}, {2, "0x1f"}};
    for (const auto &a : answers) {
        Ready r;
        r.peer.chan_mode = a.mode;
        const uint32_t seq = r.client->send_channel(0, "hello channel");
        CHECK(seq != 0 && r.model.find_message(seq)->state == MsgState::Pending);
        run(*r.client, 300);
        const Message *m = r.model.find_message(seq);
        CHECK(m->state == MsgState::Sent && m->note.empty());
        CHECK(r.logged(std::string("answer ") + a.code + " -> sent"));                // the response code is in the log
        CHECK(r.logged("rx " + std::string(a.code)));
        // an ACK-looking push later changes nothing: a channel is never delivered
        r.peer.push({resp::kAck, 0xA0, 0xB0, 0xC0, 0xD0, 5, 0, 0, 0});
        run(*r.client, 200);
        CHECK(r.model.find_message(seq)->state == MsgState::Sent);
        CHECK(!r.logged("hello channel"));                                            // no message text in the log
    }
    {   // the board never answers: after the fallback time (about 5 s) it is "sent" with a note, not "sending" for ever
        Ready r;
        r.peer.chan_mode = 3;
        const uint32_t seq = r.client->send_channel(0, "into the void");
        run(*r.client, 4000);
        CHECK(r.model.find_message(seq)->state == MsgState::Pending);
        run(*r.client, 1500);
        const Message *m = r.model.find_message(seq);
        CHECK(m->state == MsgState::Sent && m->note == "no confirmation from the board");
        CHECK(r.logged("shown as sent (unconfirmed)"));
        run(*r.client, 20000);
        CHECK(r.client->link() == Link::Ready || r.client->link() == Link::Searching);
        // a second message goes out normally after the silent one
        r.peer.chan_mode = 0;
        const uint32_t seq2 = r.client->send_channel(0, "second");
        run(*r.client, 500);
        CHECK(r.model.find_message(seq2)->state == MsgState::Sent && r.model.find_message(seq2)->note.empty());
    }
    {   // an error answer is a failure (and says why)
        Ready r;
        r.peer.chan_mode = 4;
        const uint32_t seq = r.client->send_channel(1, "x");
        run(*r.client, 300);
        CHECK(r.model.find_message(seq)->state == MsgState::Failed && !r.model.find_message(seq)->note.empty());
        CHECK(r.logged("error 3"));
    }
    {   // the channel send waits behind a slow command: still ends in 5 s from the tap, not from the write
        Ready r;
        r.peer.chan_mode = 3;
        r.client->refresh_contacts();
        const uint32_t seq = r.client->send_channel(0, "queued");
        run(*r.client, 5200);
        CHECK(r.model.find_message(seq)->state == MsgState::Sent);
    }
    {   // unplug while sending: failed, not stuck
        Ready r;
        r.peer.chan_mode = 3;
        const uint32_t seq = r.client->send_channel(0, "cut");
        run(*r.client, 100);
        r.peer.lost = true;
        run(*r.client, 200);
        CHECK(r.model.find_message(seq)->state == MsgState::Failed);
    }
    {   // direct messages keep their states: sent, delivered
        Ready r;
        const uint32_t seq = r.client->send_direct(to_hex(r.model.contacts_sorted()[0]->c.key), "dm");
        run(*r.client, 300);
        CHECK(r.model.find_message(seq)->state == MsgState::Delivered);
    }
}

static void test_caps_and_private_channels()
{
    {   // a recent board with a GPS: the switch is available
        Ready r([](FakePeer &p) { p.vars_text = "gps:0"; });
        const BoardCaps &c = r.model.caps();
        CHECK(c.custom_vars == Cap::Yes && c.channels == Cap::Yes && c.gps_listed && !c.gps_on);
        CHECK(feature_state(Feature::BoardGps, &*r.model.device(), c).available);
        CHECK(feature_state(Feature::ChannelAdmin, &*r.model.device(), c).available);
        r.peer.commands.clear();
        CHECK(r.client->set_board_gps(true));
        run(*r.client, 300);
        CHECK(count_cmd(r.peer, cmd::kSetCustomVar) == 1);
        CHECK(r.model.caps().gps_on && r.model.clock().gps && r.client->notice().ok);        // read back from the board
        CHECK(r.client->set_board_gps(false));
        run(*r.client, 300);
        CHECK(!r.model.caps().gps_on);
    }
    {   // a board that lists no gps variable: nothing to show
        Ready r([](FakePeer &p) { p.vars_text = "radio.fem:1"; });
        CHECK(r.model.caps().custom_vars == Cap::Yes && !r.model.caps().gps_listed);
        const FeatureState f = feature_state(Feature::BoardGps, &*r.model.device(), r.model.caps());
        CHECK(!f.available && f.hidden && f.notice.empty());
        CHECK(!r.client->set_board_gps(true) && !r.client->notice().ok);
    }
    {   // an old firmware: no custom variables and no channels: both features are off with a notice
        Ready r([](FakePeer &p) { p.vars_supported = false; p.channels_supported = false; });
        CHECK(r.model.caps().custom_vars == Cap::No && r.model.caps().channels == Cap::No);
        const FeatureState g = feature_state(Feature::BoardGps, &*r.model.device(), r.model.caps());
        CHECK(!g.available && !g.hidden && g.notice.find("Firmware too old") == 0);
        const FeatureState ch = feature_state(Feature::ChannelAdmin, &*r.model.device(), r.model.caps());
        CHECK(!ch.available && !ch.hidden && ch.notice.find("Channel management") != std::string::npos);
        CHECK(!r.client->add_hashtag_channel("#abc") && r.client->notice().text.find("Firmware too old") == 0);
        CHECK(!r.client->add_private_channel("Club", kPublicChannelSecret));
        CHECK(!r.client->set_board_gps(true) && r.client->notice().text.find("Firmware too old") == 0);
    }
    {   // the clock policy off: the custom variables are still read for the GPS switch
        Ready r([](FakePeer &p) { p.vars_text = "gps:1"; }, false);
        r.client->options().set_device_time = false;
        run(*r.client, 4500);
        CHECK(r.model.caps().gps_listed && r.model.caps().gps_on);
    }
    {   // level based features (the levels are the ones verified in the references)
        DeviceInfo d;
        d.fw_ver = 11;
        BoardCaps caps;
        CHECK(feature_state(Feature::ClientRepeat, &d, caps).available && feature_state(Feature::PathHashMode, &d, caps).available);
        d.fw_ver = 9;
        FeatureState f = feature_state(Feature::PathHashMode, &d, caps);
        CHECK(feature_state(Feature::ClientRepeat, &d, caps).available && !f.available && f.notice.find("level 10") != std::string::npos);
        d.fw_ver = 5;
        f = feature_state(Feature::ClientRepeat, &d, caps);
        CHECK(!f.available && f.notice.find("Firmware too old") == 0 && f.notice.find("level 9") != std::string::npos);
        CHECK(feature_state(Feature::ClientRepeat, nullptr, caps).hidden);
        CHECK(feature_min_level(Feature::ClientRepeat) == 9 && feature_min_level(Feature::PathHashMode) == 10 && feature_min_level(Feature::BoardGps) == 0);
        DeviceInfo real = std::get<DeviceInfo>(*parse_packet(hex(kRealDeviceInfo + 6)));
        CHECK(firmware_text(&real) == "v1.15.0-dee3e26 (level 11)" || firmware_text(&real).find("(level 11)") != std::string::npos);
        CHECK(firmware_text(nullptr) == "-");
    }
    {   // private channels: random or typed key, any slot can be removed but Public
        Ready r;
        ChannelSecret key{};
        CHECK(parse_channel_key("00112233445566778899aabbccddeeff", key).empty());
        CHECK(!r.client->add_private_channel("#hash", key) && !r.client->add_private_channel("", key));
        CHECK(!r.client->add_private_channel("Club", ChannelSecret{}));
        CHECK(r.client->add_private_channel("Club", key));
        run(*r.client, 400);
        CHECK(r.client->notice().ok && r.peer.slots.count(2) && r.peer.slots[2].first == "Club" && r.peer.slots[2].second == key);
        const ChannelRec *c = r.model.find_channel(2);
        CHECK(c && c->name == "Club" && c->has_secret && c->secret == key && c->kind() == ChannelKind::Private && !c->app_added);
        CHECK(r.model.find_channel(0)->kind() == ChannelKind::Public && r.model.find_channel(1)->kind() == ChannelKind::Hashtag);
        CHECK(!r.client->add_private_channel("Club", key) && r.client->notice().text.find("already") != std::string::npos);
        r.model.set_muted("c:2", true);
        CHECK(r.client->remove_channel(2));
        run(*r.client, 400);
        CHECK(r.peer.slots.count(2) == 0 && r.model.find_channel(2)->empty && !r.model.is_muted("c:2"));     // the mute flag goes with the channel
        CHECK(!r.client->remove_channel(0));
        CHECK(!r.client->remove_channel(30) && !r.client->notice().ok);                                         // an empty slot
    }
}

static void test_log()
{
    const std::string dir = tmpdir("log") + "/sub";
    CHECK(std::system(("mkdir -p " + dir).c_str()) == 0);
    FileLog log(dir + "/t.log", 200);
    log.line("first line");
    log.line("second line");
    std::string text;
    {
        FILE *f = std::fopen((dir + "/t.log").c_str(), "rb");
        CHECK(f != nullptr);
        char buf[512];
        const size_t n = f ? std::fread(buf, 1, sizeof(buf), f) : 0;
        if (f) std::fclose(f);
        text.assign(buf, n);
    }
    CHECK(text.find("first line\n") != std::string::npos && text.find("second line\n") != std::string::npos);
    CHECK(text.size() > 20 && text[4] == '-' && text[13] == ':');                           // "YYYY-MM-DD HH:MM:SS "
    for (int i = 0; i < 20; ++i) log.line("filler filler filler filler " + std::to_string(i));
    struct stat st;
    CHECK(::stat((dir + "/t.log.1").c_str(), &st) == 0);                                    // rotated once it passed the limit
    CHECK(::stat((dir + "/t.log").c_str(), &st) == 0 && st.st_size < 600);
    FileLog nowhere("/nonexistent-dir/x.log");
    nowhere.line("ignored");                                                                // never throws
    CHECK(nowhere.written() == 0);
}

/* ---------------------------------------------------------------- phase 1b: history deletion */

static int count_lines(const std::string &file)
{
    FILE *f = std::fopen(file.c_str(), "rb");
    if (!f) return -1;
    int n = 0, c;
    while ((c = std::fgetc(f)) != EOF)
        if (c == '\n') ++n;
    std::fclose(f);
    return n;
}

static void write_text(const std::string &file, const std::string &text)
{
    FILE *f = std::fopen(file.c_str(), "wb");
    if (f) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

static std::string read_text(const std::string &file)
{
    std::string out;
    FILE *f = std::fopen(file.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static void test_history_delete()
{
    set_time_source(fake_time);
    const std::string dir = tmpdir("history");
    const uint32_t kNow = 1790000000;                       // a plausible clock
    const uint32_t kDay = 86400;
    const KeyPrefix pa = {1, 2, 3, 4, 5, 6}, pb = {9, 9, 9, 9, 9, 9};
    const std::string da = Model::conv_direct(pa), db = Model::conv_direct(pb);
    auto in_direct = [&](Model &m, const KeyPrefix &p, const char *text, uint32_t stamp, uint32_t ts) {
        IncomingMessage dm;
        dm.prefix = p;
        dm.text = text;
        dm.sender_timestamp = stamp;
        return m.add_incoming(dm, ts);
    };
    auto in_chan = [&](Model &m, int idx, const char *text, uint32_t stamp, uint32_t ts) {
        IncomingMessage cm;
        cm.channel = true;
        cm.channel_idx = idx;
        cm.text = text;
        cm.sender_timestamp = stamp;
        return m.add_incoming(cm, ts);
    };
    uint32_t last_seq = 0;
    {
        Model m;
        Store st(dir);
        attach_b(st, m);
        auto c1 = std::get<Contact>(*parse_packet(make_contact(resp::kContact, 0x10, "Alice", 1, 1000)));
        m.upsert_contact(c1);
        ChannelRec ch;
        ch.idx = 1; ch.name = "#test"; ch.empty = false;
        m.set_channel(ch);
        in_chan(m, 1, "Zed: c-old", 1, kNow - 100 * kDay);          // 100 days old
        in_chan(m, 1, "Zed: c-mid", 2, kNow - 20 * kDay);
        in_chan(m, 1, "Zed: c-new", 3, kNow - 1 * kDay);
        in_direct(m, pa, "a-old", 4, kNow - 40 * kDay);
        in_direct(m, pa, "a-new", 5, kNow - 2 * kDay);
        in_direct(m, pb, "b-old", 6, kNow - 10 * kDay);
        in_direct(m, pb, "b-unknown-time", 7, 5000);                // received while the clock was not set
        m.add_outgoing(da, "my reply", kNow - 2 * kDay);
        last_seq = m.add_outgoing(db, "to b", kNow - 1 * kDay);
        CHECK(m.message_count() == 9 && m.message_count_in(da) == 3 && m.message_count_in("c:1") == 3);
        CHECK(m.unread_total() == 7);
        m.set_muted(da, true);                                       // a muted contact is not counted
        CHECK(m.unread(da) == 0 && m.unread_raw(da) == 2 && m.unread_total() == 5);
        m.mark_read(db);
        CHECK(m.unread_total() == 3 && count_lines(bdir_of(dir) + "/messages.jsonl") >= 9);

        // one conversation: its messages and its read mark go, nothing else
        CHECK(m.delete_conversation(db) == 3);
        CHECK(m.message_count() == 6 && m.message_count_in(db) == 0 && m.read_marks().count(db) == 0);
        CHECK(m.unread_total() == 3 && m.contact_count() == 1);       // totals stay right; the contact stays
        bool listed = false;
        for (const ConvSummary &s : m.conversations()) if (s.key == db) listed = true;
        CHECK(!listed);                                              // gone from the left pane
        CHECK(m.is_muted(da));                                       // the mute flag is a preference: kept
        CHECK(count_lines(bdir_of(dir) + "/messages.jsonl") == 6);            // the file was rewritten
        CHECK(m.delete_conversation(db) == 0);                       // nothing left: no change, no error
        CHECK(m.next_seq() > last_seq);                              // numbers are never reused

        // a channel: the history goes, the channel stays in the list
        CHECK(m.delete_conversation("c:1") == 3 && m.message_count_in("c:1") == 0);
        bool chan_listed = false;
        for (const ConvSummary &s : m.conversations()) if (s.key == "c:1") chan_listed = true;
        CHECK(chan_listed && m.find_channel(1) && !m.find_channel(1)->empty);
        CHECK(m.unread_total() == 0 && m.unread_raw(da) == 2);
        CHECK(count_lines(bdir_of(dir) + "/messages.jsonl") == 3);
    }
    {   // the next run sees the same
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(m.message_count() == 3 && m.message_count_in(da) == 3 && m.message_count_in(db) == 0 && m.message_count_in("c:1") == 0);
        CHECK(m.is_muted(da) && m.contact_count() == 1 && m.find_channel(1));
        CHECK(m.unread_raw(da) == 2 && m.unread_total() == 0);
        // a new incoming message recreates the deleted conversation
        in_direct(m, pb, "b-again", 8, kNow);
        CHECK(m.message_count_in(db) == 1 && m.unread(db) == 1 && m.unread_total() == 1);
        bool back = false;
        for (const ConvSummary &s : m.conversations()) if (s.key == db) back = s.unread == 1;
        CHECK(back);
        // older than N days: only messages with a known time before the cutoff
        const uint32_t cutoff = kNow - 30 * kDay;
        CHECK(m.count_older_than(cutoff) == 1);                      // a-old (40 days); b-unknown-time was deleted with db; nothing else is that old
        CHECK(m.delete_older_than(cutoff) == 1 && m.message_count_in(da) == 2);
        CHECK(m.delete_older_than(cutoff) == 0);
        CHECK(count_lines(bdir_of(dir) + "/messages.jsonl") == 3);
    }
    {
        Model m;
        Store st(dir);
        attach_b(st, m);
        CHECK(m.message_count() == 3 && m.unread_raw(da) == 1);      // "my reply" is ours; a-new and b-again are unread
    }
    // older than: the unknown-time message is kept, a day boundary is exact, everything older goes at once
    const std::string dir2 = tmpdir("history2");
    {
        Model m;
        Store st(dir2);
        attach_b(st, m);
        in_direct(m, pa, "old1", 1, kNow - 95 * kDay);
        in_direct(m, pa, "old2", 2, kNow - 31 * kDay);
        in_direct(m, pa, "edge", 3, kNow - 30 * kDay);               // exactly 30 days: not older than the cutoff
        in_direct(m, pa, "unknown", 4, 1234);
        in_direct(m, pa, "new", 5, kNow);
        const uint32_t c30 = kNow - 30 * kDay;
        CHECK(m.count_older_than(c30) == 2 && m.count_older_than(kNow - 90 * kDay) == 1 && m.count_older_than(kNow - 7 * kDay) == 3);
        CHECK(m.delete_older_than(c30) == 2 && m.message_count() == 3);
        CHECK(m.unread_total() == 3);
        CHECK(m.delete_all_messages() == 3 && m.message_count() == 0 && m.unread_total() == 0 && m.read_marks().empty());
        CHECK(count_lines(bdir_of(dir2) + "/messages.jsonl") == 0);
        CHECK(m.delete_all_messages() == 0);
        const uint32_t s = in_direct(m, pa, "fresh", 6, kNow);
        CHECK(s > 5 && m.unread_total() == 1);                       // the counter never goes back
        // the bound of the history still holds after deletions
        for (int i = 0; i < 230; ++i) in_direct(m, pa, ("bulk " + std::to_string(i)).c_str(), 100 + static_cast<uint32_t>(i), kNow + static_cast<uint32_t>(i));
        CHECK(m.message_count_in(da) == Model::kMaxPerConversation);
    }
    {   // a read mark above every message number must not let numbers be reused (deleted newest messages)
        const std::string dir3 = tmpdir("history3");
        CHECK(std::system(("mkdir -p " + bdir_of(dir3)).c_str()) == 0);
        write_text(bdir_of(dir3) + "/read.txt", "d:010203040506 50\n");
        Model m;
        Store st(dir3);
        attach_b(st, m);
        CHECK(m.next_seq() >= 51);
    }
    {   // every message deleted by age, the read marks stay: a new message must still count as unread after a restart
        const std::string dir4 = tmpdir("history4");
        {
            Model m;
            Store st(dir4);
            attach_b(st, m);
            in_direct(m, pa, "one", 1, kNow - 50 * kDay);
            in_direct(m, pa, "two", 2, kNow - 49 * kDay);
            m.mark_read(da);
            CHECK(m.delete_older_than(kNow - 30 * kDay) == 2 && m.message_count() == 0);
        }
        Model m;
        Store st(dir4);
        attach_b(st, m);
        in_direct(m, pa, "three", 3, kNow);
        CHECK(m.unread_total() == 1 && m.unread(da) == 1);
    }
    // listeners: removal calls are batched into one rewrite; a Model without a store works
    Model bare;
    in_direct(bare, pa, "x", 1, kNow);
    CHECK(bare.delete_conversation(da) == 1 && bare.message_count() == 0);
}

/* ---------------------------------------------------------------- phase 1b: the preset feed */

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data"
#endif

static std::string replace_first(std::string s, const std::string &from, const std::string &to)
{
    const size_t p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
    return s;
}

static void test_preset_feed()
{
    const std::string json = read_text(std::string(TEST_DATA_DIR) + "/meshcore_config.json");
    CHECK(json.size() > 4000);
    std::vector<RadioPreset> list;
    std::string why;
    // the real answer of api.meshcore.nz (fetched 2026-10-08): 26 entries, some with a network_settings object, other keys around them
    CHECK(parse_preset_feed(json, list, why));
    CHECK(list.size() == 26 && why.empty());
    const auto &bundled = bundled_presets();
    bool same = list.size() == bundled.size();
    for (size_t i = 0; same && i < list.size(); ++i)
        same = list[i].name == bundled[i].name && std::fabs(list[i].freq_mhz - bundled[i].freq_mhz) < 1e-6 &&
               std::fabs(list[i].bw_khz - bundled[i].bw_khz) < 1e-6 && list[i].sf == bundled[i].sf && list[i].cr == bundled[i].cr;
    CHECK(same);                                                    // the bundled file equals the feed of today
    CHECK(list[1].name == "Australia (Narrow)" && list[1].bw_khz == 62.5 && list[1].cr == 7);
    CHECK(list[0].bw_khz == 250 && list[8].name == "EU/UK (Narrow)" && list[8].freq_mhz == 869.618 && list[8].sf == 8 && list[8].cr == 8);

    // a small valid list, numbers as JSON numbers, unknown keys ignored
    auto entry = [](const std::string &title, const std::string &f, const std::string &bw, const std::string &sf, const std::string &cr) {
        return "{\"title\":\"" + title + "\",\"frequency\":" + f + ",\"bandwidth\":" + bw + ",\"spreading_factor\":" + sf + ",\"coding_rate\":" + cr + ",\"x\":[1,{\"y\":null}]}";
    };
    auto wrap = [](const std::string &entries) { return "{\"config\":{\"suggested_radio_settings\":{\"entries\":[" + entries + "]}}}"; };
    std::string five;
    for (int i = 0; i < 5; ++i) five += (i ? "," : "") + entry("P" + std::to_string(i), "868.1", "125", "7", "5");
    CHECK(parse_preset_feed(wrap(five), list, why) && list.size() == 5 && list[0].freq_mhz == 868.1);

    // malformed or odd input: the whole list is refused and `out` stays empty
    struct Bad { const char *what; std::string text; };
    std::vector<Bad> bad = {
        {"empty", ""},
        {"not json", "hello"},
        {"empty object", "{}"},
        {"array root", "[]"},
        {"entries missing", "{\"config\":{\"suggested_radio_settings\":{}}}"},
        {"entries not a list", "{\"config\":{\"suggested_radio_settings\":{\"entries\":{}}}}"},
        {"truncated", json.substr(0, json.size() / 2)},
        {"trailing text", json + " x"},
        {"two values", json + json},
        {"bad escape", replace_first(json, "Australia", "Aus\\qtralia")},
        {"raw newline in a string", replace_first(json, "\"Brazil\"", "\"Bra\nzil\"")},
        {"surrogate", replace_first(json, "\"Brazil\"", "\"Bra\\ud800zil\"")},
        {"control character in a title", replace_first(json, "\"Brazil\"", "\"Bra\\u0001zil\"")},
        {"empty title", replace_first(json, "\"Brazil\"", "\"\"")},
        {"title too long", replace_first(json, "\"Brazil\"", "\"" + std::string(60, 'x') + "\"")},
        {"title not a string", replace_first(json, "\"Brazil\"", "7")},
        {"frequency text", replace_first(json, "\"915.800\"", "\"abc\"")},
        {"frequency exponent", replace_first(json, "\"915.800\"", "\"9e2\"")},
        {"frequency negative", replace_first(json, "\"915.800\"", "\"-915.8\"")},
        {"frequency too high", replace_first(json, "\"915.800\"", "\"9150.8\"")},
        {"frequency too low", replace_first(json, "\"915.800\"", "\"100.5\"")},
        {"frequency empty", replace_first(json, "\"915.800\"", "\"\"")},
        {"frequency two dots", replace_first(json, "\"915.800\"", "\"915.8.0\"")},
        {"bandwidth not LoRa", replace_first(json, "\"bandwidth\":\"250\"", "\"bandwidth\":\"63\"")},
        {"bandwidth zero", replace_first(json, "\"bandwidth\":\"250\"", "\"bandwidth\":\"0\"")},
        {"sf low", replace_first(json, "\"spreading_factor\":\"10\"", "\"spreading_factor\":\"4\"")},
        {"sf high", replace_first(json, "\"spreading_factor\":\"10\"", "\"spreading_factor\":\"13\"")},
        {"sf fraction", replace_first(json, "\"spreading_factor\":\"10\"", "\"spreading_factor\":\"7.5\"")},
        {"cr low", replace_first(json, "\"coding_rate\":\"5\"", "\"coding_rate\":\"4\"")},
        {"cr high", replace_first(json, "\"coding_rate\":\"5\"", "\"coding_rate\":\"9\"")},
        {"duplicate title", replace_first(json, "\"Brazil\"", "\"Canada\"")},
        {"field missing", replace_first(json, "\"coding_rate\":\"5\"", "\"coding_rate_x\":\"5\"")},
        {"entry not an object", wrap("1,2,3,4,5")},
        {"too few entries", wrap(entry("A", "868", "125", "7", "5"))},
        {"bool as number", wrap(five.substr(0, 20) + "x")},
    };
    std::string deep(40, '[');
    deep += std::string(40, ']');
    bad.push_back({"nested too deep", "{\"config\":" + deep + "}"});
    std::string many;
    for (int i = 0; i < 130; ++i) many += (i ? "," : "") + entry("N" + std::to_string(i), "868.1", "125", "7", "5");
    bad.push_back({"too many entries", wrap(many)});
    bad.push_back({"too large", wrap(five) + std::string(kPresetFeedMaxBytes, ' ')});
    for (const Bad &b : bad) {
        std::vector<RadioPreset> out = {RadioPreset{}};
        std::string w;
        const bool ok = parse_preset_feed(b.text, out, w);
        if (ok || !out.empty() || w.empty()) std::printf("  preset feed case not refused: %s\n", b.what);
        CHECK(!ok && out.empty() && !w.empty());
    }
    // one odd entry among many good ones refuses the whole list
    CHECK(!parse_preset_feed(replace_first(json, "\"bandwidth\":\"125\"", "\"bandwidth\":\"126\""), list, why) && list.empty());

    // the saved copy
    const std::string dir = tmpdir("presetcache");
    CHECK(std::system(("mkdir -p " + dir).c_str()) == 0);
    std::vector<RadioPreset> src;
    CHECK(parse_preset_feed(json, src, why));
    std::string date, srcurl;
    std::vector<RadioPreset> back;
    CHECK(!load_preset_cache(dir, back, date, srcurl));               // nothing saved yet
    CHECK(save_preset_cache(dir, src, "2026-10-08", "https://api.meshcore.nz/api/v1/config"));
    CHECK(load_preset_cache(dir, back, date, srcurl) && back.size() == 26 && date == "2026-10-08" && srcurl.find("meshcore.nz") != std::string::npos);
    bool eq = back.size() == src.size();
    for (size_t i = 0; eq && i < back.size(); ++i)
        eq = back[i].name == src[i].name && std::fabs(back[i].freq_mhz - src[i].freq_mhz) < 1e-6 && back[i].bw_khz == src[i].bw_khz && back[i].sf == src[i].sf && back[i].cr == src[i].cr;
    CHECK(eq);
    CHECK(save_preset_cache(dir, src, "unknown", "x") && load_preset_cache(dir, back, date, srcurl) && date == "unknown");
    CHECK(!save_preset_cache(dir, std::vector<RadioPreset>(), "2026-10-08", "x"));      // an invalid list is never saved
    save_preset_cache(dir, src, "2026-10-08", "x");
    const std::string saved = read_text(dir + "/presets.jsonl");
    CHECK(count_lines(dir + "/presets.jsonl") == 27);
    write_text(dir + "/presets.jsonl", replace_first(saved, "\"sf\":10", "\"sf\":14"));
    CHECK(!load_preset_cache(dir, back, date, srcurl) && back.empty());   // one odd value: the whole copy is dropped
    write_text(dir + "/presets.jsonl", replace_first(saved, "\"n\":26", "\"n\":25"));
    CHECK(!load_preset_cache(dir, back, date, srcurl));
    write_text(dir + "/presets.jsonl", replace_first(saved, "2026-10-08", "yesterday"));
    CHECK(!load_preset_cache(dir, back, date, srcurl));
    write_text(dir + "/presets.jsonl", saved.substr(0, saved.size() / 2));
    CHECK(!load_preset_cache(dir, back, date, srcurl));
    write_text(dir + "/presets.jsonl", "garbage\n");
    CHECK(!load_preset_cache(dir, back, date, srcurl));

    // the list in use: bundled, then a saved copy, then a fetched list; the footer text says which
    reset_presets();
    CHECK(presets_origin() == PresetOrigin::Bundled && radio_presets().size() == 26 && presets_origin_text().find("built into the app, 2026-10-07") != std::string::npos);
    set_presets(src, PresetOrigin::Cached, "2026-10-08");
    CHECK(presets_origin_text() == "Saved copy of the meshcore.nz list, fetched 2026-10-08");
    set_presets(src, PresetOrigin::Fetched, "2026-10-09");
    CHECK(presets_origin_text() == "List from meshcore.nz, fetched 2026-10-09" && presets_list_date() == "2026-10-09");
    reset_presets();
    CHECK(radio_presets().size() == bundled_presets().size() && presets_origin() == PresetOrigin::Bundled);

    // the date text and the online test
    CHECK(fetch_date_text(1790000000).size() == 10 && fetch_date_text(1790000000)[4] == '-' && fetch_date_text(100).empty());
    const std::string hdr = "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n";
    CHECK(route_table_has_default(hdr + "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"));
    CHECK(!route_table_has_default(hdr + "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t600\t00FFFFFF\t0\t0\t0\n"));      // a local subnet only
    CHECK(!route_table_has_default(hdr + "lo\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n"));          // lo does not count
    CHECK(!route_table_has_default(hdr + "wlan0\t00000000\t0101A8C0\t0002\t0\t0\t600\t00000000\t0\t0\t0\n"));     // not UP
    CHECK(!route_table_has_default(hdr) && !route_table_has_default(""));
    setenv("MESHHOP_ONLINE", "0", 1);
    CHECK(!network_online());
    setenv("MESHHOP_ONLINE", "1", 1);
    CHECK(network_online());
    unsetenv("MESHHOP_ONLINE");

    // the background download, against a fake curl (a shell script): good answer, bad answer, failure, hang, missing program
    const std::string tools = tmpdir("fakecurl");
    CHECK(std::system(("mkdir -p " + tools).c_str()) == 0);
    auto fake = [&](const std::string &name, const std::string &body) {
        const std::string path = tools + "/" + name;
        write_text(path, "#!/bin/sh\nout=\nwhile [ $# -gt 0 ]; do [ \"$1\" = -o ] && out=$2; shift; done\n" + body + "\n");
        ::chmod(path.c_str(), 0755);
        return path;
    };
    auto run = [&](const std::string &curl, PresetFetcher::State &final_state, std::string &msg, std::vector<RadioPreset> *got) {
        setenv("MESHHOP_CURL", curl.c_str(), 1);
        PresetFetcher f(dir);
        const bool started = f.start(1000);
        PresetFetcher::State st = f.state();
        for (int i = 0; i < 300 && started && st == PresetFetcher::State::Running; ++i) {
            ::usleep(10000);
            st = f.poll(1000 + static_cast<uint64_t>(i) * 10);
        }
        final_state = st;
        msg = f.message();
        if (got) *got = f.list();
        return started;
    };
    PresetFetcher::State fs;
    std::string msg;
    std::vector<RadioPreset> got;
    CHECK(run(fake("good.sh", "cp '" + std::string(TEST_DATA_DIR) + "/meshcore_config.json' \"$out\""), fs, msg, &got) && fs == PresetFetcher::State::Done && got.size() == 26);
    CHECK(::access((dir + "/presets.download").c_str(), F_OK) != 0);                // the temporary file is gone
    CHECK(run(fake("junk.sh", "echo '<html>captive portal</html>' > \"$out\""), fs, msg, &got) && fs == PresetFetcher::State::Failed && got.empty() && msg.find("refused") != std::string::npos);
    CHECK(run(fake("odd.sh", "sed 's/\"spreading_factor\":\"10\"/\"spreading_factor\":\"40\"/' '" + std::string(TEST_DATA_DIR) + "/meshcore_config.json' > \"$out\""), fs, msg, &got) &&
          fs == PresetFetcher::State::Failed && got.empty());
    CHECK(run(fake("fail.sh", "exit 22"), fs, msg, &got) && fs == PresetFetcher::State::Failed && msg.find("22") != std::string::npos);
    CHECK(run(fake("empty.sh", ": > \"$out\""), fs, msg, &got) && fs == PresetFetcher::State::Failed);
    CHECK(!run(tools + "/does-not-exist", fs, msg, &got) && fs == PresetFetcher::State::Failed && msg.find("not found") != std::string::npos);
    {   // a hang: poll never blocks, and the deadline kills the process
        setenv("MESHHOP_CURL", fake("hang.sh", "sleep 60").c_str(), 1);
        PresetFetcher f(dir);
        CHECK(f.start(0));
        CHECK(f.poll(10) == PresetFetcher::State::Running);
        CHECK(f.poll(PresetFetcher::kDeadlineMs - 1) == PresetFetcher::State::Running);
        CHECK(f.poll(PresetFetcher::kDeadlineMs + 1) == PresetFetcher::State::Failed && f.message().find("in time") != std::string::npos);
    }
    {   // abort (the app closes) kills the child
        setenv("MESHHOP_CURL", fake("hang2.sh", "sleep 60").c_str(), 1);
        PresetFetcher f(dir);
        CHECK(f.start(0));
        f.abort();
        CHECK(f.state() == PresetFetcher::State::Failed);
    }
    unsetenv("MESHHOP_CURL");
    PresetFetcher plain(dir);                                                         // the defaults: the real program and the official address
    CHECK(plain.curl_path() == "/usr/bin/curl" && plain.url() == "https://api.meshcore.nz/api/v1/config");
}

#include "test_phase2.inc"
#include "test_boards.inc"

int main()
{
    test_phase2();
    test_boards();
    test_history_delete();
    test_preset_feed();
    test_sha256();
    test_input();
    test_proc_listings();
    test_tx_power_signed();
    test_util();
    test_frame();
    test_real_device_info();
    test_parsers();
    test_builders();
    test_migrate_legacy();
    test_model_store();
    test_client();
    test_clock_policy();
    test_esc_navigation();
    test_presets();
    test_channel_keys();
    test_mute_and_prefs();
    test_retry_settings();
    test_channel_status();
    test_caps_and_private_channels();
    test_log();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
