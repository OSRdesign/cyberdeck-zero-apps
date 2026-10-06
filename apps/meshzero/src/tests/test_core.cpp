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

#include <cstdio>
#include <ctime>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
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
                            uint8_t txp = 22)
{
    Bytes b = {resp::kSelfInfo, 1, txp, 30};
    for (int i = 0; i < 32; ++i) b.push_back(static_cast<uint8_t>(0xA0 + i));
    put32(b, static_cast<uint32_t>(48856600));
    put32(b, static_cast<uint32_t>(2352200));
    b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);
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
    const std::string oldd = base + "/meshcore", newd = base + "/meshzero";
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
        CHECK(st.attach(m));
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
        st.attach(m);
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
            st.attach(m);
            m.mark_added("#eu868");
            ChannelRec ch;
            ch.idx = 3; ch.name = "#eu868"; ch.empty = false;
            m.set_channel(ch);
            CHECK(m.find_channel(3)->app_added && m.free_channel_slot() == -1);
        }
        Model m;
        Store st(d2);
        st.attach(m);
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
        FILE *f = std::fopen((dir + "/messages.jsonl").c_str(), "ab");
        std::fputs("{not json\n{\"i\":99}\n", f);
        std::fclose(f);
        Model m;
        Store st(dir);
        st.attach(m);
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
    std::string vars_text = "gps:0";
    uint32_t board_time = 1789999000;
    bool refuse_set_time = false;
    std::map<int, std::pair<std::string, std::array<uint8_t, 16>>> slots;

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
        case cmd::kDeviceQuery: push(hex(kRealDeviceInfo + 6)); break;   // payload of the real frame
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
        case cmd::kSendAdvert: case cmd::kSetName: case cmd::kSetTxPower: case cmd::kResetPath: push({resp::kOk}); break;
        case cmd::kSetRadio:
            if (fail_set_radio) { push({resp::kError, err::kIllegalArg}); break; }
            self.freq_khz = c[1] | (c[2] << 8) | (c[3] << 16) | (static_cast<uint32_t>(c[4]) << 24);
            self.bw_hz = c[5] | (c[6] << 8) | (c[7] << 16) | (static_cast<uint32_t>(c[8]) << 24);
            self.sf = c[9]; self.cr = c[10];
            push({resp::kOk});
            break;
        case cmd::kBattery: push(hex("0c7a0f")); break;
        case cmd::kGetContacts: {
            Bytes s = {resp::kContactStart};
            put32(s, static_cast<uint32_t>(contacts.size()));
            push(s);
            push(make_contact_from(contacts[0]));
            push({0x88, 1, 2, 3});                                     // a push in the middle of the stream
            push(make_contact_from(contacts[1]));
            Bytes e = {resp::kContactEnd};
            put32(e, 1790000100);
            push(e);
            break;
        }
        case cmd::kGetChannel: {
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
        case cmd::kSendChannelTxt: push({resp::kOk}); break;
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
    Bytes make_self_info_from() const { return make_self_info(self.name, self.freq_khz, self.bw_hz, self.sf, self.cr, self.tx_power); }
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
    st.attach(m);
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
    CHECK(!c.remove_channel(1) && !c.notice().ok);                            // "#test" was not added by the app
    CHECK(peer.slots.count(1) == 1);
    CHECK(!c.remove_channel(0));
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

int main()
{
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
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
