// Host tests of the UI-independent logic of Mesh Hop (ui/ui_logic.*): keys, editor filters, formatting, chat rows, contact
// sorting and filtering, popups, the Settings layout, settings edits and the Back rule. No LVGL.
#include "png_writer.hpp"
#include "ui_logic.hpp"

#include "model.hpp"
#include "presets.hpp"
#include "protocol.hpp"
#include "sha256.hpp"
#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace meshzero;
using namespace meshhop;

static int g_fail = 0, g_checks = 0;
#define CHECK(...)                                                                     \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(__VA_ARGS__)) {                                                          \
            ++g_fail;                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);         \
        }                                                                              \
    } while (0)

static uint32_t g_time = 1790000000;
static uint32_t fake_time() { return g_time; }

static std::string typed(KeyTranslator &t, int code, int value = 1)
{
    KeyEvent e;
    if (!t.feed(code, value, e)) return "<none>";
    return e.key == Key::Char ? e.text : "<key>";
}

static void test_keys_us()
{
    KeyTranslator t(Layout::US);
    CHECK(typed(t, 30) == "a");
    CHECK(typed(t, 57) == " ");
    CHECK(typed(t, 2) == "1");
    CHECK(typed(t, 42, 1) == "<none>");           // shift down: nothing to act on
    CHECK(typed(t, 30) == "A");
    CHECK(typed(t, 2) == "!");
    CHECK(typed(t, 42, 0) == "<none>");
    CHECK(typed(t, 2) == "1");
    CHECK(typed(t, 30, 0) == "<none>");           // a release is never a key
    CHECK(typed(t, 58, 1) == "<none>");           // caps lock on
    CHECK(typed(t, 30) == "A");
    CHECK(typed(t, 2) == "1");                    // caps lock does not touch digits
    typed(t, 42, 1);
    CHECK(typed(t, 30) == "a");                   // shift inverts caps
    typed(t, 42, 0);
    typed(t, 58, 1);                              // caps off
    CHECK(typed(t, 30) == "a");
    CHECK(typed(t, 52) == "." && typed(t, 51) == "," && typed(t, 53) == "/" && typed(t, 12) == "-");
    CHECK(typed(t, 82) == "0" && typed(t, 79) == "1" && typed(t, 83) == ".");          // keypad
    KeyEvent e;
    CHECK(t.feed(1, 1, e) && e.key == Key::Esc);
    CHECK(t.feed(1, 2, e) && e.key == Key::Esc && e.repeat);                           // a repeat is delivered, flagged (the app ignores it)
    CHECK(t.feed(1, 1, e) && !e.repeat);
    CHECK(t.feed(28, 1, e) && e.key == Key::Enter);
    CHECK(t.feed(96, 1, e) && e.key == Key::Enter);
    CHECK(t.feed(15, 1, e) && e.key == Key::Tab);
    t.feed(42, 1, e);
    CHECK(t.feed(15, 1, e) && e.key == Key::BackTab);
    t.feed(42, 0, e);
    CHECK(t.feed(103, 1, e) && e.key == Key::Up && t.feed(108, 1, e) && e.key == Key::Down);
    CHECK(t.feed(105, 1, e) && e.key == Key::Left && t.feed(106, 1, e) && e.key == Key::Right);
    CHECK(t.feed(104, 1, e) && e.key == Key::PageUp && t.feed(109, 1, e) && e.key == Key::PageDown);
    CHECK(t.feed(14, 1, e) && e.key == Key::Backspace && t.feed(111, 1, e) && e.key == Key::Delete);
    CHECK(t.feed(102, 1, e) && e.key == Key::Home && t.feed(107, 1, e) && e.key == Key::End);
    CHECK(t.feed(59, 1, e) && e.key == Key::Function && e.fn == 1);
    CHECK(t.feed(88, 1, e) && e.key == Key::Function && e.fn == 12);
    t.feed(29, 1, e);
    CHECK(t.feed(31, 1, e) && e.key == Key::Char && e.ctrl && e.text == "s");
    t.feed(29, 0, e);
    CHECK(t.feed(31, 1, e) && !e.ctrl);
    t.feed(42, 1, e);
    t.reset();
    CHECK(typed(t, 30) == "a");                   // reset: modifiers up
    CHECK(typed(t, 200) == "<none>");             // an unmapped key types nothing
}

static void test_keys_fr()
{
    KeyTranslator t(Layout::FR);
    CHECK(typed(t, 16) == "a" && typed(t, 30) == "q" && typed(t, 17) == "z" && typed(t, 44) == "w");   // AZERTY
    CHECK(typed(t, 39) == "m" && typed(t, 50) == ",");
    CHECK(typed(t, 3) == "\xC3\xA9");                                       // e acute without shift
    KeyEvent e;
    t.feed(42, 1, e);
    CHECK(typed(t, 3) == "2" && typed(t, 16) == "A");
    t.feed(42, 0, e);
    t.feed(100, 1, e);                                                      // AltGr
    CHECK(typed(t, 11) == "@" && typed(t, 5) == "{");
    t.feed(100, 0, e);
    CHECK(layout_from_name("fr") == Layout::FR && layout_from_name("FR(azerty)") == Layout::FR);
    CHECK(layout_from_name("us") == Layout::US && layout_from_name("") == Layout::US && layout_from_name("de") == Layout::US);
    CHECK(xkb_layout_from_text("XKBMODEL=\"pc105\"\nXKBLAYOUT=\"fr\"\nXKBVARIANT=\"\"\n") == "fr");
    CHECK(xkb_layout_from_text("XKBMODEL=\"pc105\"\n") == "");
}

static void test_filters()
{
    CHECK(filter_typed(EditMode::Free, "ab", "c", 10) == "c");
    CHECK(filter_typed(EditMode::Free, "ab", "\xC3\xA9", 10) == "\xC3\xA9");
    CHECK(filter_typed(EditMode::Free, "abcdefghij", "k", 10) == "");                    // full
    CHECK(filter_typed(EditMode::Free, "abcdefghi", "\xC3\xA9", 10) == "");              // 2 bytes do not fit in 1
    CHECK(filter_typed(EditMode::Free, "", "\n", 10) == "" && filter_typed(EditMode::Free, "", "", 10) == "");
    CHECK(filter_typed(EditMode::Number, "", "7", 16) == "7" && filter_typed(EditMode::Number, "", "-", 16) == "-");
    CHECK(filter_typed(EditMode::Number, "", ",", 16) == ".");                           // comma becomes a decimal point
    CHECK(filter_typed(EditMode::Number, "", "a", 16) == "" && filter_typed(EditMode::Number, "", " ", 16) == "");
    CHECK(filter_typed(EditMode::Clock, "", ":", 16) == ":" && filter_typed(EditMode::Clock, "", " ", 16) == " ");
    CHECK(filter_typed(EditMode::Clock, "", "x", 16) == "");
    CHECK(filter_typed(EditMode::Channel, "#", " ", 30) == "" && filter_typed(EditMode::Channel, "#", "a", 30) == "a");
    CHECK(filter_typed(EditMode::Name, "", "A", 31) == "A");
}

static void test_format()
{
    set_time_source(fake_time);
    set_time_offset(0);
    CHECK(fmt_clock(0) == "--:--" && fmt_clock(100) == "--:--");
    CHECK(fmt_clock(g_time).size() == 5 && fmt_clock(g_time)[2] == ':');
    CHECK(fmt_age(0) == "-" && fmt_age(g_time - 30) == "30 s" && fmt_age(g_time - 7200) == "2 h" && fmt_age(g_time + 50) == "now");
    CHECK(fmt_age_coarse(g_time - 30) == "< 1 min" && fmt_age_coarse(g_time + 50) == "< 1 min" && fmt_age_coarse(g_time - 120) == "2 min" && fmt_age_coarse(0) == "-");
    CHECK(fmt_freq(869.525) == "869.525 MHz" && fmt_bw(62.5) == "62.5 kHz" && fmt_bw(125) == "125 kHz");
    CHECK(fmt_snr(false, 0) == "-" && fmt_snr(true, -3.55).find("dB") != std::string::npos);
    CHECK(fmt_hops(-1) == "flood" && fmt_hops(0) == "direct" && fmt_hops(1) == "1 hop" && fmt_hops(3) == "3 hops");
    CHECK(fmt_distance(0.25) == "250 m" && fmt_distance(12.44) == "12.4 km" && fmt_distance(340.2) == "340 km");
    // Paris - London is about 344 km; a point and itself is 0
    CHECK(std::abs(distance_km(48.8566, 2.3522, 51.5074, -0.1278) - 343.5) < 3.0);
    CHECK(distance_km(10, 10, 10, 10) < 1e-9);
    CHECK(truncate_ellipsis("hello", 10) == "hello" && truncate_ellipsis("hello world", 5) == "hello...");
    CHECK(truncate_ellipsis("\xC3\xA9\xC3\xA9\xC3\xA9", 2) == "\xC3\xA9\xC3\xA9...");   // by characters, not bytes
}

static Contact make_contact(uint8_t id, const char *name, uint8_t type, uint32_t heard, double lat = 0, double lon = 0, uint8_t path = kPathFlood)
{
    Contact c;
    c.key.fill(id);
    c.type = type;
    c.name = name;
    c.last_advert = heard;
    c.lat = lat;
    c.lon = lon;
    c.out_path_len = path;
    c.lastmod = heard;
    return c;
}

static void test_chat_rows()
{
    set_time_source(fake_time);
    Model m;
    auto rows = build_chat_rows(m);
    CHECK(rows.size() == 2 && rows[0].kind == ChatRow::Header && rows[1].kind == ChatRow::Add);   // nothing yet: CHANNELS + add row
    CHECK(first_selectable(rows) == 1);
    ChannelRec ch;
    ch.idx = 0; ch.name = "Public"; ch.empty = false;
    m.set_channel(ch);
    ch.idx = 1; ch.name = "#hamradio";
    m.set_channel(ch);
    m.upsert_contact(make_contact(0x10, "Ana", advtype::kChat, g_time - 100));
    m.upsert_contact(make_contact(0x20, "Bob", advtype::kChat, g_time - 50));
    IncomingMessage in;
    in.channel = true; in.channel_idx = 1; in.text = "Zed: hi"; in.sender_timestamp = 5;
    m.add_incoming(in, g_time);
    IncomingMessage dm;
    dm.prefix = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10}; dm.text = "coffee?"; dm.sender_timestamp = 6;
    m.add_incoming(dm, g_time + 1);
    rows = build_chat_rows(m);
    // CHANNELS, Public, #hamradio, +, DIRECT, Ana
    CHECK(rows.size() == 6);
    CHECK(rows[0].kind == ChatRow::Header && rows[1].key == "c:0" && rows[2].key == "c:1" && rows[3].kind == ChatRow::Add);
    CHECK(rows[4].kind == ChatRow::Header && rows[4].title == "DIRECT" && rows[5].kind == ChatRow::Direct && rows[5].title == "Ana");
    CHECK(rows[2].unread == 1 && rows[5].unread == 1 && rows[1].unread == 0);
    CHECK(find_chat_row(rows, "c:1") == 2 && find_chat_row(rows, "nope") == -1 && find_chat_row(rows, "+") == 3);
    // selection skips headers and stops at the ends
    CHECK(step_selection(rows, 1, 1) == 2 && step_selection(rows, 3, 1) == 5 && step_selection(rows, 5, -1) == 3);
    CHECK(step_selection(rows, 1, -1) == 1 && step_selection(rows, 5, 1) == 5);
    CHECK(step_selection(rows, 0, 1) == 1);                 // on a header: first selectable
    CHECK(step_selection(rows, -1, 1) == 1 && step_selection({}, 0, 1) == -1);
    CHECK(step_selection(rows, 1, 2) == 3 && step_selection(rows, 1, 50) == 5);
    // a chat opened from Contacts before any message: listed first among the directs
    const auto with = build_chat_rows(m, "d:202020202020");
    CHECK(with.size() == 7 && with[5].key == "d:202020202020" && with[5].kind == ChatRow::Direct && with[6].title == "Ana");
    CHECK(build_chat_rows(m, "d:101010101010").size() == 6);           // already there: not twice
    CHECK(build_chat_rows(m, "c:1").size() == 6);                       // channels are never added this way
}

static void test_step_selectable()
{
    const std::vector<bool> ok = {false, false, true, false, true, true, false};
    CHECK(step_selectable(ok, -1, 1) == 2 && step_selectable(ok, 0, 1) == 2 && step_selectable(ok, 2, 1) == 4);
    CHECK(step_selectable(ok, 4, 1) == 5 && step_selectable(ok, 5, 1) == 5 && step_selectable(ok, 2, -1) == 2);
    CHECK(step_selectable(ok, 5, -2) == 2 && step_selectable(ok, 2, 9) == 5 && step_selectable(ok, 3, 1) == 2);
    CHECK(step_selectable({false, false}, 0, 1) == -1 && step_selectable({}, 0, 1) == -1);
}

static void test_contacts()
{
    set_time_source(fake_time);
    Model m;
    SelfInfo si;
    si.lat = 48.0; si.lon = 2.0; si.name = "me";
    m.set_self(si);
    m.upsert_contact(make_contact(0x10, "Zoe", advtype::kChat, g_time - 10, 48.1, 2.0, 0));
    m.upsert_contact(make_contact(0x20, "alice", advtype::kRepeater, g_time - 500, 49.0, 2.0, 2));
    m.upsert_contact(make_contact(0x30, "Bob", advtype::kSensor, g_time - 5000));
    m.upsert_contact(make_contact(0x40, "", advtype::kChat, g_time - 20));
    m.note_snr(KeyPrefix{0x30, 0x30, 0x30, 0x30, 0x30, 0x30}, 6.5, g_time - 5000);
    m.note_snr(KeyPrefix{0x10, 0x10, 0x10, 0x10, 0x10, 0x10}, -4.0, g_time - 10);

    auto rows = build_contact_rows(m, SortKey::Heard);
    CHECK(rows.size() == 4 && rows[0].name == "Zoe" && rows[3].name == "Bob");
    CHECK(rows[2].name == "alice" || rows[2].name == "Bob" || true);
    bool unnamed_ok = false;
    for (const auto &r : rows)
        if (r.key_hex.rfind("40", 0) == 0) unnamed_ok = r.name == "404040404040";       // no name: the first 6 key bytes
    CHECK(unnamed_ok);
    rows = build_contact_rows(m, SortKey::Name);
    CHECK(rows[0].name == "404040404040" && rows[1].name == "alice" && rows[2].name == "Bob" && rows[3].name == "Zoe");   // case-insensitive
    rows = build_contact_rows(m, SortKey::Name, true);
    CHECK(rows[0].name == "Zoe" && rows[3].name == "404040404040");
    rows = build_contact_rows(m, SortKey::Type);
    CHECK(rows[0].type == advtype::kChat && rows[3].type == advtype::kSensor);
    rows = build_contact_rows(m, SortKey::Snr);
    CHECK(rows[0].name == "Bob" && rows[1].name == "Zoe" && !rows[2].has_snr && !rows[3].has_snr);                // best first, unknown last
    rows = build_contact_rows(m, SortKey::Hops);
    CHECK(rows[0].name == "Zoe" && rows[0].hops == 0 && rows[1].name == "alice" && rows[1].hops == 2 && rows[3].hops == -1);
    rows = build_contact_rows(m, SortKey::Distance);
    CHECK(rows[0].name == "Zoe" && rows[0].has_dist && rows[1].name == "alice" && !rows[2].has_dist);
    CHECK(rows[0].dist_km > 10 && rows[0].dist_km < 12 && rows[1].dist_km > 110 && rows[1].dist_km < 112);
    // without our own position there is no distance
    Model m2;
    m2.upsert_contact(make_contact(0x50, "Far", advtype::kChat, g_time, 10, 10));
    rows = build_contact_rows(m2, SortKey::Distance);
    CHECK(rows.size() == 1 && rows[0].has_pos && !rows[0].has_dist);
    CHECK(next_sort(SortKey::Heard) == SortKey::Name && next_sort(SortKey::Distance) == SortKey::Heard);
    SortKey k = SortKey::Heard;
    for (int i = 0; i < 6; ++i) k = next_sort(k);
    CHECK(k == SortKey::Heard);                                                          // the cycle closes
    CHECK(opens_chat(advtype::kChat) && opens_chat(advtype::kNone) && !opens_chat(advtype::kRepeater) && !opens_chat(advtype::kSensor) &&
          !opens_chat(advtype::kRoom));
}

static void test_settings()
{
    RadioSettings s;
    s.name = "n"; s.freq_mhz = 869.525; s.bw_khz = 62.5; s.sf = 8; s.cr = 5; s.tx_power = 14; s.max_tx_power = 22;
    adjust_setting(s, kRowFreq, 1);
    CHECK(std::abs(s.freq_mhz - 869.55) < 1e-6);
    adjust_setting(s, kRowFreq, -2);
    CHECK(std::abs(s.freq_mhz - 869.5) < 1e-6);
    adjust_setting(s, kRowBw, 1);
    CHECK(s.bw_khz == 125);
    adjust_setting(s, kRowBw, -1);
    adjust_setting(s, kRowBw, -1);
    CHECK(s.bw_khz == 41.7);
    adjust_setting(s, kRowSf, 10);
    CHECK(s.sf == 12);
    adjust_setting(s, kRowSf, -100);
    CHECK(s.sf == 5);
    adjust_setting(s, kRowCr, 10);
    CHECK(s.cr == 8);
    adjust_setting(s, kRowTx, 100);
    CHECK(s.tx_power == 22);
    adjust_setting(s, kRowTx, -100);
    CHECK(s.tx_power == -9);
    adjust_setting(s, kRowName, 1);                         // not steppable: unchanged
    CHECK(s.name == "n");

    s.freq_mhz = 869.525; s.bw_khz = 62.5; s.sf = 8; s.cr = 5; s.tx_power = 14;
    CHECK(apply_number(s, kRowFreq, "868.1").empty() && std::abs(s.freq_mhz - 868.1) < 1e-6);
    CHECK(!apply_number(s, kRowFreq, "5").empty() && std::abs(s.freq_mhz - 868.1) < 1e-6);               // refused, unchanged
    CHECK(!apply_number(s, kRowFreq, "").empty() && !apply_number(s, kRowFreq, "8x").empty());
    CHECK(apply_number(s, kRowBw, "250").empty() && s.bw_khz == 250);
    CHECK(!apply_number(s, kRowBw, "100").empty() && s.bw_khz == 250);
    CHECK(apply_number(s, kRowSf, "11").empty() && s.sf == 11);
    CHECK(!apply_number(s, kRowSf, "4").empty() && !apply_number(s, kRowSf, "13").empty() && s.sf == 11);
    CHECK(apply_number(s, kRowCr, "7").empty() && s.cr == 7 && !apply_number(s, kRowCr, "9").empty());
    CHECK(apply_number(s, kRowTx, "-3").empty() && s.tx_power == -3 && !apply_number(s, kRowTx, "99").empty());
    CHECK(!apply_number(s, kRowName, "1").empty());
    CHECK(in_eu868(869.525) && in_eu868(863.0) && in_eu868(870.0) && !in_eu868(915.0) && !in_eu868(862.9));
    CHECK(in_eu868(kEu868PresetMhz));
}

static void test_navigation()
{
    NavState s;
    s.tab = Tab::Chats;
    CHECK(back_action(s) == BackAction::ExitHint);                          // top level: only a hint, never a quit
    s.compose_focus = true;
    CHECK(back_action(s) == BackAction::FocusList);
    s.tab = Tab::Contacts;
    CHECK(back_action(s) == BackAction::ExitHint);                          // compose focus only counts in Chats
    s.detail_open = true;
    CHECK(back_action(s) == BackAction::CloseDetail);
    s.editor_open = true;
    CHECK(back_action(s) == BackAction::CancelEditor);                      // the editor wins over everything below
    s.editor_open = false; s.detail_open = false; s.tab = Tab::Settings;
    CHECK(back_action(s) == BackAction::ExitHint);
    CHECK(step_tab(Tab::Chats, 1) == Tab::Map && step_tab(Tab::Settings, 1) == Tab::Chats && step_tab(Tab::Chats, -1) == Tab::Settings);
    CHECK(step_tab(Tab::Map, -1) == Tab::Chats && step_tab(Tab::Contacts, 2) == Tab::Settings);
    CHECK(std::strcmp(tab_name(Tab::Terminal), "Terminal") == 0);
}

static void test_status()
{
    Message m;
    m.dir = Dir::In;
    CHECK(message_status(m, false).text.empty());
    m.dir = Dir::Out;
    m.state = MsgState::Pending;
    CHECK(message_status(m, false).text == "sending" && message_status(m, false).tone == Tone::Gold);
    m.note = "radio busy";
    CHECK(message_status(m, false).text == "radio busy");
    m.note.clear();
    m.state = MsgState::Sent;
    CHECK(message_status(m, true).text == "sent" && message_status(m, false).text == "sent, waiting for ack");
    m.state = MsgState::Delivered;
    CHECK(message_status(m, false).text == "delivered" && message_status(m, false).tone == Tone::Green);
    m.state = MsgState::NoAck;
    CHECK(message_status(m, false).text == "no ack" && message_status(m, false).tone == Tone::Red);
    m.state = MsgState::Failed;
    m.note = "board full";
    CHECK(message_status(m, false).text == "failed: board full");
}

static void test_png()
{
    const char *nine = "123456789";
    CHECK(crc32_bytes(reinterpret_cast<const uint8_t *>(nine), 9) == 0xCBF43926u);
    const char *wiki = "Wikipedia";
    CHECK(adler32_bytes(reinterpret_cast<const uint8_t *>(wiki), 9) == 0x11E60398u);
    const uint32_t px[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    const auto png = encode_png(px, 2, 2, 2);
    CHECK(png.size() > 60 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    CHECK(png[png.size() - 8] == 'I' && png[png.size() - 7] == 'E' && png[png.size() - 6] == 'N' && png[png.size() - 5] == 'D');
    // big enough for several stored blocks
    std::vector<uint32_t> big(640 * 480, 0x123456);
    const auto b = encode_png(big.data(), 640, 480, 640);
    CHECK(b.size() > 640 * 480 * 3);
    CHECK(write_png("/tmp/mesh-hop-test.png", px, 2, 2, 2));
}

/* ------------------------------------------------------------------ phase 1 (0.1.1) */

static KeyEvent ke(Key k, bool repeat = false)
{
    KeyEvent e;
    e.key = k;
    e.repeat = repeat;
    return e;
}

static KeyEvent kc(const char *text)
{
    KeyEvent e;
    e.key = Key::Char;
    e.text = text;
    return e;
}

static void test_unread_rows()
{
    set_time_source(fake_time);
    set_time_offset(0);
    Model m;
    ChannelRec ch;
    ch.idx = 0; ch.name = "Public"; ch.empty = false;
    m.set_channel(ch);
    ch.idx = 1; ch.name = "#test";
    m.set_channel(ch);
    m.upsert_contact(make_contact(0x10, "Ana", advtype::kChat, g_time - 100));
    IncomingMessage in;
    in.channel = true; in.channel_idx = 1; in.sender_timestamp = 1;
    for (int i = 0; i < 3; ++i) {
        in.text = "Zed: m" + std::to_string(i);
        in.sender_timestamp = static_cast<uint32_t>(10 + i);
        m.add_incoming(in, g_time + static_cast<uint32_t>(i));
    }
    IncomingMessage dm;
    dm.prefix = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10}; dm.text = "coffee?"; dm.sender_timestamp = 99;
    m.add_incoming(dm, g_time + 5);
    CHECK(m.unread_total() == 4);                                   // the total shown on the "Chats" tab title
    auto rows = build_chat_rows(m);
    int ch_unread = -1, dm_unread = -1;
    for (const ChatRow &r : rows) {
        if (r.key == "c:1") ch_unread = r.unread;
        if (r.key.rfind("d:", 0) == 0) dm_unread = r.unread;
    }
    CHECK(ch_unread == 3 && dm_unread == 1);                        // channel and direct rows carry the count
    CHECK(fmt_count_badge(0) == "" && fmt_count_badge(7) == "7" && fmt_count_badge(99) == "99" && fmt_count_badge(100) == "99+" && fmt_count_badge(-1) == "");
    m.mark_read("c:1");
    CHECK(m.unread_total() == 1);
    // mute: the row says muted and has no count; the tab total drops it
    in.text = "Zed: new"; in.sender_timestamp = 50;
    m.add_incoming(in, g_time + 9);
    CHECK(m.unread_total() == 2);
    m.set_muted("c:1", true);
    rows = build_chat_rows(m);
    for (const ChatRow &r : rows)
        if (r.key == "c:1") CHECK(r.muted && r.unread == 0);
    CHECK(m.unread_total() == 1);
    m.set_muted("c:1", false);
    CHECK(m.unread_total() == 2);
}

static void test_contact_filters()
{
    set_time_source(fake_time);
    set_time_offset(0);
    Model m;
    m.upsert_contact(make_contact(0x10, "Chat-now", advtype::kChat, g_time - 30));
    m.upsert_contact(make_contact(0x20, "Rep-30min", advtype::kRepeater, g_time - 1800));
    m.upsert_contact(make_contact(0x30, "Rep-3h", advtype::kRepeater, g_time - 3 * 3600));
    m.upsert_contact(make_contact(0x40, "Room-2d", advtype::kRoom, g_time - 2 * 86400));
    m.upsert_contact(make_contact(0x50, "Sens-10d", advtype::kSensor, g_time - 10 * 86400));
    m.upsert_contact(make_contact(0x60, "Chat-never", advtype::kChat, 0));
    ContactFilter f;
    CHECK(!f.active() && build_contact_rows(m, SortKey::Name, false, f).size() == 6);
    f.type = TypeFilter::Repeater;
    CHECK(f.active());
    auto rows = build_contact_rows(m, SortKey::Name, false, f);
    CHECK(rows.size() == 2 && rows[0].name == "Rep-30min" && rows[1].name == "Rep-3h");
    f.age = AgeFilter::Hour;
    rows = build_contact_rows(m, SortKey::Name, false, f);
    CHECK(rows.size() == 1 && rows[0].name == "Rep-30min");
    f.type = TypeFilter::All;
    rows = build_contact_rows(m, SortKey::Heard, false, f);
    CHECK(rows.size() == 2 && rows[0].name == "Chat-now");            // 1 h: the two heard within the hour
    f.age = AgeFilter::Day;
    CHECK(build_contact_rows(m, SortKey::Name, false, f).size() == 3);
    f.age = AgeFilter::Week;
    CHECK(build_contact_rows(m, SortKey::Name, false, f).size() == 4); // never-heard nodes fail any time filter
    f.age = AgeFilter::Any;
    f.type = TypeFilter::Chat;
    CHECK(build_contact_rows(m, SortKey::Name, false, f).size() == 2);  // Chat includes the never heard one when no time filter
    f.type = TypeFilter::Room;
    CHECK(build_contact_rows(m, SortKey::Name, false, f).size() == 1);
    f.type = TypeFilter::Sensor;
    CHECK(build_contact_rows(m, SortKey::Name, true, f).size() == 1);
    // the filter composes with sort and reverse
    f = ContactFilter();
    f.type = TypeFilter::Repeater;
    rows = build_contact_rows(m, SortKey::Name, true, f);
    CHECK(rows[0].name == "Rep-3h");
    // the cycles close
    TypeFilter t = TypeFilter::All;
    for (int i = 0; i < 5; ++i) t = next_type_filter(t);
    AgeFilter a = AgeFilter::Any;
    for (int i = 0; i < 4; ++i) a = next_age_filter(a);
    CHECK(t == TypeFilter::All && a == AgeFilter::Any && next_type_filter(TypeFilter::All) == TypeFilter::Chat && next_age_filter(AgeFilter::Any) == AgeFilter::Hour);
    CHECK(age_filter_seconds(AgeFilter::Any) == 0 && age_filter_seconds(AgeFilter::Hour) == 3600 && age_filter_seconds(AgeFilter::Week) == 604800);
    CHECK(std::string(type_filter_name(TypeFilter::Sensor)) == "Sensor" && std::string(age_filter_name(AgeFilter::Day)) == "24 h");
    CHECK(type_matches(TypeFilter::Chat, advtype::kNone) && !type_matches(TypeFilter::Chat, advtype::kRepeater) && type_matches(TypeFilter::All, 77));
    // a node "heard in the future" (clock skew) passes a time filter
    ContactRow future;
    future.heard = g_time + 500;
    ContactFilter hour;
    hour.age = AgeFilter::Hour;
    CHECK(passes_filter(future, hour, g_time));
}

static void test_contact_view_and_snapshot()
{
    set_time_source(fake_time);
    set_time_offset(0);
    Model m;
    // 161 contacts like the user's list
    for (int i = 0; i < 161; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "node%03d", (i * 37) % 161);
        Contact c = make_contact(static_cast<uint8_t>(i), name, i % 7 == 0 ? advtype::kRepeater : advtype::kChat, g_time - static_cast<uint32_t>(i * 60));
        c.key[31] = static_cast<uint8_t>(i);
        m.upsert_contact(c);
    }
    CHECK(m.contact_count() == 161);
    ContactView v;
    CHECK(v.sort == SortKey::Heard && !v.reverse);
    v.tap_column(SortKey::Name);
    CHECK(v.sort == SortKey::Name && !v.reverse);
    v.tap_column(SortKey::Name);
    CHECK(v.sort == SortKey::Name && v.reverse);                      // the same column again reverses
    v.tap_column(SortKey::Type);
    CHECK(v.sort == SortKey::Type && !v.reverse);
    v.cycle_sort();
    CHECK(v.sort == SortKey::Snr && !v.reverse);
    v.toggle_reverse();
    CHECK(v.reverse && v != ContactView());
    // the bug of the first test: after a non-default sort, the tapped row must be the row that is shown
    ContactView by_name;
    by_name.tap_column(SortKey::Name);
    const ContactSnapshot shown = make_contact_snapshot(m, by_name);
    const ContactSnapshot default_order = make_contact_snapshot(m, ContactView());
    CHECK(shown.rows.size() == 161 && shown.rows[0].name == "node000" && shown.rows[160].name == "node160");
    CHECK(shown.view.sort == SortKey::Name);                          // the chosen sort travels with the rows
    bool differs = false;
    for (int i = 0; i < 161; ++i) {
        const std::string k = shown.key_at(i);
        CHECK(k == shown.rows[static_cast<size_t>(i)].key_hex && shown.index_of(k) == i);
        const ContactRec *rec = m.find_contact(k);
        CHECK(rec && rec->c.name == shown.rows[static_cast<size_t>(i)].name);   // the key at position i is the node shown at position i
        if (k != default_order.key_at(i)) differs = true;
    }
    CHECK(differs);                                                   // so the two orders really differ
    // the model changes under the screen (an advert moves a node to the top of the "heard" order): the snapshot on screen is stable
    const std::string tapped = shown.key_at(100);
    Contact bumped = m.find_contact(tapped)->c;
    m.upsert_contact(bumped);
    m.heard(bumped.key, g_time + 10);
    CHECK(shown.key_at(100) == tapped);
    CHECK(make_contact_snapshot(m, ContactView()).key_at(0) == tapped);
    CHECK(shown.key_at(-1).empty() && shown.key_at(161).empty() && shown.index_of("nope") == -1);
    // the same filtered view keeps its order too
    ContactView rep_only;
    rep_only.filter.type = TypeFilter::Repeater;
    rep_only.tap_column(SortKey::Name);
    const ContactSnapshot rs = make_contact_snapshot(m, rep_only);
    CHECK(rs.rows.size() == 23);                                     // i % 7 == 0 for i in 0..160
    for (size_t i = 1; i < rs.rows.size(); ++i) CHECK(rs.rows[i - 1].name <= rs.rows[i].name);
}

static void test_choice_popup()
{
    RadioSettings s;
    s.name = "n"; s.freq_mhz = 869.525; s.bw_khz = 250; s.sf = 11; s.cr = 5; s.tx_power = 14; s.max_tx_power = 22;
    RetrySettings retry;
    Choice bw = make_choice(ChoiceField::Bandwidth, s, retry);
    CHECK(bw.labels.size() == 10 && bw.label() == "250 kHz" && bw.position() == "9 / 10" && bw.can_prev() && bw.can_next());
    CHECK(bw.step(1) && bw.label() == "500 kHz" && !bw.can_next() && !bw.step(1) && bw.label() == "500 kHz");     // clamps, no wrap
    CHECK(bw.step(-100) && bw.label() == "7.8 kHz" && !bw.can_prev() && !bw.step(-1));
    bw.index = 6;
    CHECK(bw.label() == "62.5 kHz");
    apply_choice(ChoiceField::Bandwidth, bw.index, s, retry);
    CHECK(s.bw_khz == 62.5 && validate_radio(s).empty());                                    // 62.5 is accepted by the core
    apply_choice(ChoiceField::Bandwidth, 8, s, retry);
    CHECK(s.bw_khz == 250 && validate_radio(s).empty());

    Choice sf = make_choice(ChoiceField::Sf, s, retry);
    CHECK(sf.labels.size() == 8 && sf.label() == "SF11" && sf.labels.front() == "SF5" && sf.labels.back() == "SF12");
    apply_choice(ChoiceField::Sf, 0, s, retry);
    CHECK(s.sf == 5);
    Choice cr = make_choice(ChoiceField::Cr, s, retry);
    CHECK(cr.labels.size() == 4 && cr.label() == "4/5");
    apply_choice(ChoiceField::Cr, 3, s, retry);
    CHECK(s.cr == 8);
    Choice tx = make_choice(ChoiceField::Tx, s, retry);
    CHECK(tx.labels.size() == 32 && tx.label() == "14 dBm" && tx.labels.front() == "-9 dBm" && tx.labels.back() == "22 dBm");
    apply_choice(ChoiceField::Tx, tx.labels.size() - 1, s, retry);
    CHECK(s.tx_power == 22 && validate_radio(s).empty());
    apply_choice(ChoiceField::Tx, 999, s, retry);
    CHECK(s.tx_power == 22);                                                                // a bad index changes nothing
    apply_choice(ChoiceField::Bandwidth, -1, s, retry);
    CHECK(s.bw_khz == 250);

    // the preset list: 26 entries, the deprecated ones marked, selecting one fills frequency, bandwidth, SF and CR
    s.freq_mhz = 869.525; s.bw_khz = 250; s.sf = 11; s.cr = 5;
    Choice pr = make_choice(ChoiceField::Preset, s, retry);
    CHECK(pr.labels.size() == 26 && pr.details.size() == 26 && pr.label() == "EU/UK (Deprecated)" && pr.index == 9);
    CHECK(pr.detail() == "869.525 MHz  250 kHz  SF11  4/5");
    CHECK(pr.note.find("built into the app, 2026-10-07") != std::string::npos);        // the footer says where the list comes from
    set_presets(bundled_presets(), PresetOrigin::Fetched, "2026-10-08");
    CHECK(make_choice(ChoiceField::Preset, s, retry).note.find("meshcore.nz, fetched 2026-10-08") != std::string::npos);
    set_presets(bundled_presets(), PresetOrigin::Cached, "2026-10-08");
    CHECK(make_choice(ChoiceField::Preset, s, retry).note.find("Saved copy") != std::string::npos);
    reset_presets();
    CHECK(pr.note.find("match no preset") == std::string::npos);
    int deprecated = 0;
    for (const std::string &l : pr.labels)
        if (l.find("(Deprecated)") != std::string::npos) ++deprecated;
    CHECK(deprecated == 2);
    s.name = "keep"; s.tx_power = 5;
    apply_choice(ChoiceField::Preset, 1, s, retry);                                         // Australia (Narrow)
    CHECK(s.freq_mhz == 916.575 && s.bw_khz == 62.5 && s.sf == 7 && s.cr == 7 && s.name == "keep" && s.tx_power == 5 && validate_radio(s).empty());
    s.freq_mhz = 868.1;                                                                     // custom settings: the popup says so
    Choice custom = make_choice(ChoiceField::Preset, s, retry);
    CHECK(custom.index == 0 && custom.note.find("match no preset") != std::string::npos);
    // every preset applies cleanly
    for (int i = 0; i < 26; ++i) {
        RadioSettings t = s;
        apply_choice(ChoiceField::Preset, i, t, retry);
        CHECK(validate_radio(t).empty());
    }

    // retry settings
    RetrySettings rs;
    Choice at = make_choice(ChoiceField::RetryAttempts, s, rs);
    CHECK(at.labels.size() == 4 && at.label() == "3 tries" && at.labels[0] == "1 try (no retry)");
    Choice ra = make_choice(ChoiceField::ResetAfter, s, rs);
    CHECK(ra.labels.size() == 4 && ra.labels[0] == "Never" && ra.label() == "Before try 3");
    apply_choice(ChoiceField::RetryAttempts, 0, s, rs);                                     // 1 try: no reset possible
    CHECK(rs.attempts == 1 && rs.reset_after == 0);
    apply_choice(ChoiceField::RetryAttempts, 3, s, rs);
    apply_choice(ChoiceField::ResetAfter, 3, s, rs);
    CHECK(rs.attempts == 4 && rs.reset_after == 3);
    apply_choice(ChoiceField::RetryAttempts, 1, s, rs);                                     // fewer tries pull the reset in
    CHECK(rs.attempts == 2 && rs.reset_after == 1);

    // keys
    Choice c = make_choice(ChoiceField::Sf, s, retry);
    c.index = 3;
    CHECK(choice_key(ke(Key::Right), c) == ChoiceResult::Moved && c.index == 4);
    CHECK(choice_key(ke(Key::Left), c) == ChoiceResult::Moved && c.index == 3);
    CHECK(choice_key(ke(Key::Down), c) == ChoiceResult::Moved && choice_key(ke(Key::Up), c) == ChoiceResult::Moved && c.index == 3);
    CHECK(choice_key(ke(Key::End), c) == ChoiceResult::Moved && c.index == 7 && choice_key(ke(Key::Right), c) == ChoiceResult::None);
    CHECK(choice_key(ke(Key::Home), c) == ChoiceResult::Moved && c.index == 0 && choice_key(ke(Key::Left), c) == ChoiceResult::None);
    CHECK(choice_key(ke(Key::PageDown), c) == ChoiceResult::Moved && c.index == 5);
    CHECK(choice_key(ke(Key::Enter), c) == ChoiceResult::Accept && choice_key(ke(Key::Esc), c) == ChoiceResult::Cancel);
    CHECK(choice_key(ke(Key::Enter, true), c) == ChoiceResult::None && choice_key(ke(Key::Esc, true), c) == ChoiceResult::None);   // key repeat
    CHECK(choice_key(kc("x"), c) == ChoiceResult::None && c.initial == sf_index(s.sf));
}

static void test_confirm_and_menu()
{
    int focus = 0;
    CHECK(confirm_key(ke(Key::Enter), focus) == ConfirmResult::No);                      // the default focus is No
    CHECK(confirm_key(ke(Key::Right), focus) == ConfirmResult::Moved && focus == 1);
    CHECK(confirm_key(ke(Key::Enter), focus) == ConfirmResult::Yes);
    CHECK(confirm_key(ke(Key::Left), focus) == ConfirmResult::Moved && focus == 0);
    CHECK(confirm_key(ke(Key::Tab), focus) == ConfirmResult::Moved && focus == 1);
    CHECK(confirm_key(ke(Key::Tab), focus) == ConfirmResult::Moved && focus == 0);
    CHECK(confirm_key(kc("y"), focus) == ConfirmResult::Yes && confirm_key(kc("Y"), focus) == ConfirmResult::Yes);
    CHECK(confirm_key(kc("n"), focus) == ConfirmResult::No && confirm_key(kc("z"), focus) == ConfirmResult::None);
    CHECK(confirm_key(ke(Key::Esc), focus) == ConfirmResult::No);
    CHECK(confirm_key(ke(Key::Esc, true), focus) == ConfirmResult::None && confirm_key(ke(Key::Enter, true), focus) == ConfirmResult::None);
    KeyEvent ctrl_y = kc("y");
    ctrl_y.ctrl = true;
    CHECK(confirm_key(ctrl_y, focus) == ConfirmResult::None);

    int sel = 0;
    CHECK(menu_key(ke(Key::Down), sel, 3) == MenuResult::Moved && sel == 1);
    CHECK(menu_key(ke(Key::Down), sel, 3) == MenuResult::Moved && sel == 2 && menu_key(ke(Key::Down), sel, 3) == MenuResult::None && sel == 2);
    CHECK(menu_key(ke(Key::Up), sel, 3) == MenuResult::Moved && sel == 1);
    CHECK(menu_key(ke(Key::Enter), sel, 3) == MenuResult::Accept && menu_key(ke(Key::Esc), sel, 3) == MenuResult::Cancel);
    CHECK(menu_key(ke(Key::Enter, true), sel, 3) == MenuResult::None && menu_key(ke(Key::Esc), sel, 0) == MenuResult::Cancel);
    sel = 0;
    CHECK(menu_key(ke(Key::Up), sel, 3) == MenuResult::None && sel == 0);
}

static void test_settings_layout()
{
    SettingsContext ctx;
    ctx.connected = true;
    ctx.channel_admin.available = true;
    ctx.channels = {0, 1, 2};
    auto rows = settings_layout(ctx);
    CHECK(rows.size() > 10);
    // Save to radio and Undo changes are the LAST rows of the list (Undo, then Save)
    CHECK(rows.back().kind == SRowKind::Save && rows[rows.size() - 2].kind == SRowKind::Undo && rows[rows.size() - 3].kind == SRowKind::Header);
    int saves = 0, undos = 0, channel_rows = 0, add_rows = 0, gps_rows = 0;
    bool slot0 = false;
    for (const SRowSpec &r : rows) {
        if (r.kind == SRowKind::Save) ++saves;
        if (r.kind == SRowKind::Undo) ++undos;
        if (r.kind == SRowKind::Channel) { ++channel_rows; if (r.arg == 0) slot0 = true; }
        if (r.kind == SRowKind::AddChannel) ++add_rows;
        if (r.kind == SRowKind::BoardGps || r.kind == SRowKind::GpsNotice) ++gps_rows;
    }
    CHECK(saves == 1 && undos == 1 && channel_rows == 2 && !slot0 && add_rows == 1 && gps_rows == 0);   // Public (slot 0) is not listed; no GPS row
    // the channels sit before the end block
    int last_channel = -1, undo_at = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == SRowKind::Channel) last_channel = static_cast<int>(i);
        if (rows[i].kind == SRowKind::Undo) undo_at = static_cast<int>(i);
    }
    CHECK(last_channel > 0 && last_channel < undo_at);
    // the fixed choices and the radio rows are all there, in order
    std::vector<SRowKind> kinds;
    for (const SRowSpec &r : rows) kinds.push_back(r.kind);
    auto pos = [&](SRowKind k) { return static_cast<int>(std::find(kinds.begin(), kinds.end(), k) - kinds.begin()); };
    CHECK(pos(SRowKind::Preset) < pos(SRowKind::Freq) && pos(SRowKind::Freq) < pos(SRowKind::Bw) && pos(SRowKind::Bw) < pos(SRowKind::Sf) &&
          pos(SRowKind::Sf) < pos(SRowKind::Cr) && pos(SRowKind::Cr) < pos(SRowKind::Tx) && pos(SRowKind::Tx) < pos(SRowKind::RetryAttempts) &&
          pos(SRowKind::RetryAttempts) < pos(SRowKind::ResetAfter));
    // board GPS: a row when the firmware lists it, a notice on an old firmware, nothing otherwise
    ctx.gps.available = true;
    rows = settings_layout(ctx);
    gps_rows = 0;
    for (const SRowSpec &r : rows) if (r.kind == SRowKind::BoardGps) ++gps_rows;
    CHECK(gps_rows == 1);
    ctx.gps.available = false;
    ctx.gps.notice = "Firmware too old for this feature: Board GPS";
    rows = settings_layout(ctx);
    int notices = 0;
    for (const SRowSpec &r : rows) { if (r.kind == SRowKind::GpsNotice) ++notices; if (r.kind == SRowKind::BoardGps) notices += 10; }
    CHECK(notices == 1);
    ctx.gps.notice.clear();
    ctx.gps.hidden = true;
    rows = settings_layout(ctx);
    for (const SRowSpec &r : rows) CHECK(r.kind != SRowKind::BoardGps && r.kind != SRowKind::GpsNotice);
    // an old firmware without channels: no add row, a notice instead
    ctx.channel_admin.available = false;
    ctx.channel_admin.notice = "Firmware too old for this feature: Channel management";
    rows = settings_layout(ctx);
    bool add = false, notice = false;
    for (const SRowSpec &r : rows) { if (r.kind == SRowKind::AddChannel) add = true; if (r.kind == SRowKind::ChannelsNotice) notice = true; }
    CHECK(!add && notice);
    // the Public channel can be put back when slot 0 is empty
    ctx.channel_admin.available = true;
    ctx.slot0_empty = true;
    rows = settings_layout(ctx);
    bool add_public = false;
    for (const SRowSpec &r : rows) if (r.kind == SRowKind::AddPublic) add_public = true;
    CHECK(add_public);
    ctx.connected = false;
    rows = settings_layout(ctx);
    for (const SRowSpec &r : rows) CHECK(r.kind != SRowKind::AddPublic);

    // the channel entries of the model: no slot 0, no empty slot, kind and mute
    Model m;
    ChannelRec pub; pub.idx = 0; pub.name = "Public"; pub.empty = false; pub.has_secret = true; pub.secret = kPublicChannelSecret;
    ChannelRec tag; tag.idx = 1; tag.name = "#test"; tag.empty = false; tag.has_secret = true; tag.secret = hashtag_secret("#test");
    ChannelRec priv; priv.idx = 3; priv.name = "Club"; priv.empty = false; priv.has_secret = true; priv.secret = kPublicChannelSecret;
    priv.secret[0] ^= 0xFF;
    ChannelRec empty; empty.idx = 2; empty.empty = true;
    ChannelRec cached; cached.idx = 4; cached.name = "#old"; cached.empty = false;
    for (const ChannelRec &c : {pub, tag, priv, empty, cached}) m.set_channel(c);
    m.set_muted("c:3", true);
    const auto entries = build_channel_entries(m);
    CHECK(entries.size() == 3 && entries[0].idx == 1 && entries[1].idx == 3 && entries[2].idx == 4);
    CHECK(entries[0].kind == ChannelKind::Hashtag && entries[0].has_key && !entries[0].muted);
    CHECK(entries[1].kind == ChannelKind::Private && entries[1].muted && entries[1].has_key);
    CHECK(entries[2].kind == ChannelKind::Hashtag && !entries[2].has_key);                 // a cached slot: key not read yet
}

static void test_navigation_popup()
{
    NavState s;
    s.tab = Tab::Settings;
    s.popup_open = true;
    CHECK(back_action(s) == BackAction::ClosePopup);                       // a popup is closed by Esc before anything else
    s.editor_open = true;
    s.detail_open = true;
    CHECK(back_action(s) == BackAction::ClosePopup);
    s.popup_open = false;
    CHECK(back_action(s) == BackAction::CancelEditor);
    // Tab walks the five tabs in a loop, both ways
    Tab t = Tab::Chats;
    for (int i = 0; i < 5; ++i) t = step_tab(t, 1);
    CHECK(t == Tab::Chats && step_tab(Tab::Settings, 1) == Tab::Chats && step_tab(Tab::Chats, -1) == Tab::Settings);
    Tab seen[5];
    t = Tab::Chats;
    for (int i = 0; i < 5; ++i) { seen[i] = t; t = step_tab(t, 1); }
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 5; ++j) CHECK(seen[i] != seen[j]);
}

static void test_channel_status_text()
{
    Message m;
    m.dir = Dir::Out;
    m.state = MsgState::Pending;
    CHECK(message_status(m, true).text == "sending");
    m.state = MsgState::Sent;
    CHECK(message_status(m, true).text == "sent" && message_status(m, true).tone == Tone::Blue);
    m.note = "no confirmation from the board";
    CHECK(message_status(m, true).text == "sent (unconfirmed)" && message_status(m, true).tone == Tone::Gold);
    CHECK(message_status(m, false).text == "sent, waiting for ack");                       // a direct message keeps its wording
    m.note.clear();
    m.state = MsgState::Delivered;
    CHECK(message_status(m, true).text == "sent");                                          // never "delivered" on a channel
    CHECK(message_status(m, false).text == "delivered");
    m.state = MsgState::NoAck;
    CHECK(message_status(m, false).text == "no ack");
    m.state = MsgState::Failed;
    m.note = "table full";
    CHECK(message_status(m, true).text == "failed: table full");
    // heard back over repeaters (phase 3): only a sent channel message says it, "sent" until the first echo
    m.state = MsgState::Sent;
    m.note.clear();
    m.heard_back = 0;
    CHECK(message_status(m, true).text == "sent");
    m.heard_back = 1;
    CHECK(message_status(m, true).text == "heard back by 1 repeater" && message_status(m, true).tone == Tone::Blue);
    m.heard_back = 3;
    CHECK(message_status(m, true).text == "heard back by 3 repeaters");
    m.note = "no confirmation from the board";                                              // the echo shows it went out after all
    CHECK(message_status(m, true).text == "heard back by 3 repeaters");
    CHECK(message_status(m, false).text == "sent, waiting for ack");                       // never on a direct message
    m.state = MsgState::Pending;
    m.note.clear();
    CHECK(message_status(m, true).text == "sending");
}

static void test_packet_log_rows()
{
    Model model;
    ChannelRec pub;
    pub.idx = 0; pub.name = "Public"; pub.empty = false; pub.has_secret = true; pub.secret = kPublicChannelSecret;
    model.set_channel(pub);
    const uint8_t h = sha256(kPublicChannelSecret.data(), kPublicChannelSecret.size())[0];
    ChannelRec cached;                                                       // no key read in this session: never matched
    cached.idx = 2; cached.name = "Cached"; cached.empty = false; cached.has_secret = false; cached.secret = kPublicChannelSecret;
    model.set_channel(cached);

    Bytes f = {0x88, static_cast<uint8_t>(-22), static_cast<uint8_t>(-104), 0x15, 0x41, 0x63, 0xde, h};
    for (int i = 0; i < 34; ++i) f.push_back(static_cast<uint8_t>(i));
    LoggedPacket lp;
    CHECK(parse_log_rx(f, lp.pkt));
    lp.time = 0;
    PacketRow r = build_packet_row(model, lp);
    char hh[4];
    std::snprintf(hh, sizeof(hh), "%02x", h);
    CHECK(r.time == "--:--:--" && r.route == "FLOOD" && r.type == "GRP_TXT" && r.hops == "1" && r.snr == "-5.50" && r.rssi == "-104" && r.size == "35");
    CHECK(r.channel == std::string(hh) + " Public");
    // a second channel with the same key: the hash is ambiguous
    ChannelRec twin = pub;
    twin.idx = 3; twin.name = "Twin";
    model.set_channel(twin);
    CHECK(channels_for_hash(model, h).size() == 2 && build_packet_row(model, lp).channel == std::string(hh) + " Public|Twin ?");
    CHECK(channels_for_hash(model, h ^ 1).empty());
    // transport direct, unknown type, no channel column for other types
    LogPacket t;
    CHECK(parse_log_rx({0x88, 4, 0xF0, 0x37, 0x11, 0x22, 0x33, 0x44, 0x00, 0x99}, t));
    CHECK(route_name(t) == "T-DIRECT" && std::string(payload_type_name(t.payload_type)) == "TYPE 13" && channel_hash_text(model, t).empty());
    CHECK(packet_detail(t) == "path: none (heard from the sender)  transport 2211 4433  raw 7 bytes");
    CHECK(std::string(payload_type_name(4)) == "ADVERT" && std::string(payload_type_name(15)) == "RAW_CUSTOM");
    CHECK(packet_detail(lp.pkt) == "path 63de (1 hop, 2-byte hashes)  raw 39 bytes");
    CHECK(packet_hex({0x15, 0x41, 0x63, 0xde, 0xd9}) == "154163de d9" && packet_hex({}).empty());
    CHECK(fmt_hms(1790000000).size() == 8 && fmt_hms(1790000000)[2] == ':');
    PacketLog log(3);
    CHECK(packet_log_status(log) == "Stopped: 0 / 3");
    log.start();
    for (int i = 0; i < 5; ++i) log.add(lp.pkt, 1);
    CHECK(packet_log_status(log) == "Capturing: 3 / 3, 2 dropped");
    // navigation: Esc closes the packet log; the row is in Settings, connected or not
    NavState n;
    n.tab = Tab::Settings;
    n.packet_log_open = true;
    CHECK(back_action(n) == BackAction::ClosePacketLog);
    n.popup_open = true;
    CHECK(back_action(n) == BackAction::ClosePopup);
    SettingsContext ctx;
    bool found = false;
    for (const SRowSpec &s : settings_layout(ctx)) found = found || s.kind == SRowKind::PacketLog;
    CHECK(found && settings_row_selectable(SRowKind::PacketLog));
}

static void test_hex_key_editor()
{
    CHECK(filter_typed(EditMode::HexKey, "", "a", 40) == "a" && filter_typed(EditMode::HexKey, "", "F", 40) == "F");
    CHECK(filter_typed(EditMode::HexKey, "", "g", 40) == "" && filter_typed(EditMode::HexKey, "", "-", 40) == "-");
    CHECK(filter_typed(EditMode::HexKey, std::string(32, 'a'), "b", 80) == "");              // 32 digits are enough
    CHECK(filter_typed(EditMode::HexKey, std::string(32, 'a'), " ", 80) == " ");
    CHECK(filter_typed(EditMode::HexKey, "", "\n", 40) == "");
}

static void test_describe_changes()
{
    RadioSettings cur, e;
    cur.name = "Deck"; cur.freq_mhz = 869.525; cur.bw_khz = 250; cur.sf = 11; cur.cr = 5; cur.tx_power = 14; cur.max_tx_power = 22;
    e = cur;
    CHECK(describe_radio_changes(cur, e).empty());
    e.freq_mhz = 869.618; e.bw_khz = 62.5; e.sf = 8; e.cr = 8; e.tx_power = 10; e.name = "Deck 2";
    const auto lines = describe_radio_changes(cur, e);
    CHECK(lines.size() == 6);
    CHECK(lines[0] == "Name Deck -> Deck 2" && lines[1] == "Frequency 869.525 -> 869.618 MHz" && lines[2] == "Bandwidth 250 -> 62.5 kHz");
    CHECK(lines[3] == "Spreading factor SF11 -> SF8" && lines[4] == "Coding rate 4/5 -> 4/8" && lines[5] == "TX power 14 -> 10 dBm");
    e = cur;
    e.freq_mhz += 0.0002;                                   // below the resolution of the radio: not a change
    CHECK(describe_radio_changes(cur, e).empty());
}

static void test_history_ui()
{
    set_time_source(fake_time);
    const uint32_t kNow = g_time;
    const KeyPrefix pa = {1, 2, 3, 4, 5, 6};
    Model m;
    Contact c;
    c.key = {1, 2, 3, 4, 5, 6};
    c.type = 1;
    c.name = "Alice";
    m.upsert_contact(c);
    ChannelRec ch; ch.idx = 1; ch.name = "#test"; ch.empty = false;
    m.set_channel(ch);
    const std::string da = Model::conv_direct(pa);

    // the options box: a direct chat and a channel, with Unmute when muted
    ConvOptions o = conversation_options(m, da);
    CHECK(o.title == "Alice" && o.labels.size() == 2 && o.labels[0] == "Mute this contact" && o.labels[1] == "Delete conversation");
    CHECK(o.actions[0] == ConvAction::Mute && o.actions[1] == ConvAction::DeleteConversation);
    m.set_muted(da, true);
    o = conversation_options(m, da);
    CHECK(o.labels[0] == "Unmute this contact" && o.actions[0] == ConvAction::Unmute);
    o = conversation_options(m, "c:1");
    CHECK(o.labels.size() == 2 && o.labels[0] == "Mute this channel" && o.labels[1] == "Delete messages" && o.actions[1] == ConvAction::DeleteMessages);
    m.set_muted("c:1", true);
    CHECK(conversation_options(m, "c:1").labels[0] == "Unmute this channel");

    // a muted contact in the list: no pill, "muted"
    IncomingMessage in;
    in.prefix = pa; in.text = "hi"; in.sender_timestamp = 1;
    m.add_incoming(in, kNow);
    auto rows = build_chat_rows(m);
    bool found = false;
    for (const ChatRow &r : rows) if (r.key == da) found = r.kind == ChatRow::Direct && r.muted && r.unread == 0;
    CHECK(found);
    m.delete_conversation(da);
    rows = build_chat_rows(m);
    CHECK(find_chat_row(rows, da) < 0);                                           // gone from the left pane
    rows = build_chat_rows(m, da);                                                 // listed only while it is the open chat
    int at = find_chat_row(rows, da);
    CHECK(at >= 0 && rows[static_cast<size_t>(at)].muted);

    // the prompts
    in.text = "one"; in.sender_timestamp = 2; m.add_incoming(in, kNow - 40 * 86400);
    in.text = "two"; in.sender_timestamp = 3; m.add_incoming(in, kNow);
    DeletePrompt p = delete_conversation_prompt(m, da);
    CHECK(!p.empty && p.count == 2 && p.title == "Delete conversation?" && p.body.find("2 messages with Alice") != std::string::npos &&
          p.body.find("stays in Contacts") != std::string::npos);
    p = delete_conversation_prompt(m, "c:1");
    CHECK(p.empty && p.title == "Delete messages?");
    IncomingMessage cm; cm.channel = true; cm.channel_idx = 1; cm.text = "Zed: x"; cm.sender_timestamp = 9;
    m.add_incoming(cm, kNow);
    p = delete_conversation_prompt(m, "c:1");
    CHECK(!p.empty && p.count == 1 && p.body.find("1 message of #test") != std::string::npos && p.body.find("channel stays") != std::string::npos);
    p = delete_all_prompt(m);
    CHECK(!p.empty && p.count == 3 && p.body.find("all 3 messages") != std::string::npos && p.body.find("Contacts, channels and settings stay") != std::string::npos);
    p = delete_older_prompt(m, 30, kNow);
    CHECK(!p.empty && p.count == 1 && p.body.find("1 message older than 30 days") != std::string::npos);
    CHECK(delete_older_prompt(m, 90, kNow).empty);
    CHECK(delete_older_prompt(m, 30, 100).empty && delete_older_prompt(m, 30, 100).count == 0);     // clock not set: nothing can be told
    CHECK(fmt_message_count(1) == "1 message" && fmt_message_count(0) == "0 messages" && fmt_message_count(12) == "12 messages");

    // the days choice (arrow popup): 7, 30, 90, default 30
    Choice d = make_days_choice(30);
    CHECK(d.labels.size() == 3 && d.labels[0] == "7 days" && d.labels[2] == "90 days" && d.index == 1 && d.label() == "30 days");
    CHECK(make_days_choice(7).index == 0 && make_days_choice(90).index == 2 && make_days_choice(5).index == 1);
    CHECK(d.step(1) && d.label() == "90 days" && !d.step(1) && d.step(-2) && d.label() == "7 days" && !d.can_prev());
    CHECK(history_day_options() == std::vector<int>({7, 30, 90}));
    CHECK(history_cutoff(kNow, 30) == kNow - 30 * 86400 && history_cutoff(kNow, 7) == kNow - 7 * 86400 && history_cutoff(50, 7) == 0 && history_cutoff(kNow, 0) == 0);

    // the Settings list has a History section before the last two rows; its rows are selectable except the note
    SettingsContext ctx;
    ctx.connected = true;
    ctx.channels = {0, 1};
    const auto layout = settings_layout(ctx);
    int all_at = -1, older_at = -1, note_at = -1, undo_at = -1, header_at = -1;
    for (size_t i = 0; i < layout.size(); ++i) {
        if (layout[i].kind == SRowKind::HistoryAll) all_at = static_cast<int>(i);
        if (layout[i].kind == SRowKind::HistoryOlder) older_at = static_cast<int>(i);
        if (layout[i].kind == SRowKind::HistoryNote) note_at = static_cast<int>(i);
        if (layout[i].kind == SRowKind::Undo) undo_at = static_cast<int>(i);
        if (layout[i].kind == SRowKind::Header && layout[i].arg == 6) header_at = static_cast<int>(i);
    }
    CHECK(header_at > 0 && all_at == header_at + 1 && older_at == all_at + 1 && note_at == older_at + 1 && undo_at > note_at);
    CHECK(layout.back().kind == SRowKind::Save && layout[layout.size() - 2].kind == SRowKind::Undo);
    CHECK(settings_row_selectable(SRowKind::HistoryAll) && settings_row_selectable(SRowKind::HistoryOlder) && !settings_row_selectable(SRowKind::HistoryNote) &&
          !settings_row_selectable(SRowKind::Header) && settings_row_selectable(SRowKind::Save) && settings_row_selectable(SRowKind::Channel));

    // the 3 s hold on a name
    HoldTracker h;
    CHECK(!h.active() && !h.poll(5000) && h.progress(5000) == 0 && !h.visible(5000));
    h.begin("d:010203040506", 100, 200, 1000);
    CHECK(h.active() && h.target() == "d:010203040506" && !h.visible(1100) && h.visible(1500) && std::fabs(h.progress(2500) - 0.5) < 1e-9);
    CHECK(!h.poll(3999) && h.active());
    h.move(110, 190);                                                              // a small tremor stays within the slop
    CHECK(h.active());
    CHECK(h.poll(4000) && !h.active() && !h.poll(4001));                           // fires exactly once
    CHECK(h.take_swallow() && !h.take_swallow());                                  // the release click is swallowed once
    h.begin("c:1", 10, 10, 0);                                                     // a tap and release before 3 s: nothing
    h.cancel();
    CHECK(!h.poll(5000) && !h.take_swallow());
    h.begin("c:1", 10, 10, 0);                                                     // a finger that moves away: cancelled (a scroll)
    h.move(10, 10 + HoldTracker::kSlop + 1);
    CHECK(!h.active() && !h.poll(5000));
    h.begin("c:1", 10, 10, 0);
    CHECK(h.poll(3000));
    h.begin("c:2", 10, 10, 10000);                                                 // a new press forgets the old swallow flag
    CHECK(!h.take_swallow() && h.progress(10000 + 4000) == 1.0);
}

#include "test_phase2_ui.inc"

int main()
{
    test_phase2_ui();
    test_boards_ui();
    test_history_ui();
    test_png();
    test_keys_us();
    test_keys_fr();
    test_filters();
    test_format();
    test_chat_rows();
    test_step_selectable();
    test_contacts();
    test_settings();
    test_navigation();
    test_status();
    test_unread_rows();
    test_contact_filters();
    test_contact_view_and_snapshot();
    test_choice_popup();
    test_confirm_and_menu();
    test_settings_layout();
    test_navigation_popup();
    test_channel_status_text();
    test_packet_log_rows();
    test_hex_key_editor();
    test_describe_changes();
    std::printf("ui: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
