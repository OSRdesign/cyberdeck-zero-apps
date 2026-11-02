/*
 * SPDX-License-Identifier: MIT
 *
 * Unit tests: json, text, format, qr.
 */
#include "test_util.hpp"

#include "format.hpp"
#include "json.hpp"
#include "qr.hpp"
#include "text.hpp"

#include <cmath>
#include <ctime>

using namespace ha;

void test_json_group()
{
    json::Value v;
    CHECK(json::parse("{\"id\":3,\"type\":\"result\",\"success\":true,\"result\":null}", v));
    CHECK(v.get("id")->as_int() == 3);
    CHECK(v.get("success")->as_bool());
    CHECK(v.get("result")->is_null());
    CHECK(json::parse("[{\"a\":1},{\"b\":[1,2.5,-3e2]}]", v));
    CHECK(v.is_array() && v.items.size() == 2);
    CHECK(v.items[1].get("b")->items[1].as_double() == 2.5);
    CHECK(v.items[1].get("b")->items[2].as_double() == -300.0);
    CHECK(v.items[1].get("b")->items[1].as_int() == 3); /* rounded */
    CHECK(json::parse("{\"lc\":1730484000.123456}", v));
    CHECK(std::fabs(v.get("lc")->as_double() - 1730484000.123456) < 1e-5);
    CHECK(json::parse("\"\\u00b0C \\ud83d\\ude00 \\\"q\\\"\"", v));
    CHECK_EQ(v.text, std::string("\xC2\xB0" "C \xF0\x9F\x98\x80 \"q\""));
    CHECK(!json::parse("{\"a\":1,}", v));
    CHECK(!json::parse("{\"a\":1} x", v));
    CHECK(!json::parse("[1,2", v));
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK(!json::parse(deep, v));
    std::string big = "\"" + std::string(json::kMaxText, 'x') + "\"";
    std::string why;
    CHECK(!json::parse(big, v, &why));
    CHECK_EQ(why, std::string("message too large"));
    CHECK_EQ(json::quote("a\"b\\c\n\x01"), std::string("\"a\\\"b\\\\c\\n\\u0001\""));
    CHECK_EQ(json::number(21), std::string("21"));
    CHECK_EQ(json::number(21.5), std::string("21.5"));
}

void test_text_group()
{
    CHECK_EQ(url_encode("http://192.168.1.42:41234/"), std::string("http%3A%2F%2F192.168.1.42%3A41234%2F"));
    CHECK_EQ(url_encode("a b~_-."), std::string("a%20b~_-."));
    CHECK_EQ(form_encode({{"grant_type", "refresh_token"}, {"token", "a+b/c"}}),
             std::string("grant_type=refresh_token&token=a%2Bb%2Fc"));
    auto q = parse_query("code=ab%2Fc&state=xyz&state=second&empty=&flag");
    CHECK_EQ(q["code"], std::string("ab/c"));
    CHECK_EQ(q["state"], std::string("xyz"));
    CHECK(q.count("empty") && q["empty"].empty());
    CHECK(q.count("flag"));
    CHECK_EQ(url_decode("a+b%20c%zz%4"), std::string("a b c%zz%4"));
    CHECK(constant_time_equal("abc", "abc"));
    CHECK(!constant_time_equal("abc", "abd"));
    CHECK(!constant_time_equal("abc", "abcd"));

    Server s;
    std::string why;
    CHECK(parse_server("homeassistant.local:8123", s));
    CHECK_EQ(s.base_url(), std::string("http://homeassistant.local:8123"));
    CHECK_EQ(s.websocket_url(), std::string("ws://homeassistant.local:8123/api/websocket"));
    CHECK_EQ(s.display(), std::string("homeassistant.local:8123"));
    CHECK(parse_server(" HTTP://HA.Lan:8123/lovelace/0 ", s));
    CHECK_EQ(s.base_url(), std::string("http://ha.lan:8123"));
    CHECK(parse_server("192.168.68.178", s));
    CHECK_EQ(s.effective_port(), 80);
    CHECK_EQ(s.base_url(), std::string("http://192.168.68.178"));
    CHECK(parse_server("https://ha.example:443", s));
    CHECK_EQ(s.base_url(), std::string("https://ha.example"));
    CHECK_EQ(s.websocket_url(), std::string("wss://ha.example/api/websocket"));
    CHECK_EQ(s.display(), std::string("https://ha.example"));
    CHECK(parse_server("[fd00::5]:8123", s));
    CHECK_EQ(s.host, std::string("fd00::5"));
    CHECK_EQ(s.base_url(), std::string("http://[fd00::5]:8123"));
    CHECK(parse_server("homeassistant.local:8123", s));
    CHECK_EQ(s.base_url_with_host("192.168.68.178"), std::string("http://192.168.68.178:8123"));
    CHECK(!parse_server("", s, &why));
    CHECK(!parse_server("ftp://x", s, &why));
    CHECK(!parse_server("host:99999", s, &why));
    CHECK(!parse_server("host:80a", s, &why));
    CHECK(!parse_server("user@host", s, &why));
    CHECK(!parse_server("my host", s, &why));

    CHECK(is_private_ip("192.168.68.113"));
    CHECK(is_private_ip("10.1.2.3"));
    CHECK(is_private_ip("172.20.0.1"));
    CHECK(!is_private_ip("172.32.0.1"));
    CHECK(is_private_ip("127.0.0.1"));
    CHECK(is_private_ip("169.254.1.1"));
    CHECK(is_private_ip("fe80::1"));
    CHECK(is_private_ip("fd12::1"));
    CHECK(!is_private_ip("8.8.8.8"));
    CHECK(!is_private_ip("2001:db8::1"));
    CHECK(!is_private_ip("homeassistant.local"));
    const unsigned char b[3] = {0x00, 0xab, 0xff};
    CHECK_EQ(hex(b, 3), std::string("00abff"));
}

void test_format_group()
{
    State s;
    CHECK_EQ(clean_unit("\xC2\xB0" "C"), std::string("C"));
    CHECK_EQ(clean_unit("W"), std::string("W"));
    CHECK_EQ(format_number("21.53", 1), std::string("21.5"));
    CHECK_EQ(format_number("21.56", 1), std::string("21.6"));
    CHECK_EQ(format_number("21.53", -1), std::string("21.53"));
    CHECK_EQ(format_number("12.0", 0), std::string("12"));
    CHECK_EQ(format_number("-0.04", 1), std::string("0.0"));
    CHECK_EQ(format_number("abc", 1), std::string("abc"));
    CHECK_EQ(format_temperature(21.4, "\xC2\xB0" "C"), std::string("21.4 C"));
    CHECK_EQ(format_temperature(21.0, "C"), std::string("21 C"));
    CHECK(format_temperature(NAN, "C").empty());

    s.state = "on";
    CHECK_EQ(state_label(Domain::Light, s, -1, "C"), std::string("On"));
    s.state = "off";
    CHECK_EQ(state_label(Domain::Switch, s, -1, "C"), std::string("Off"));
    s.state = "unavailable";
    CHECK_EQ(state_label(Domain::Light, s, -1, "C"), std::string("Unavailable"));
    CHECK(unavailable(s));
    s.state = "unknown";
    CHECK_EQ(state_label(Domain::Sensor, s, -1, "C"), std::string("Unknown"));
    s = State();
    s.state = "21.53";
    s.unit = "\xC2\xB0" "C";
    CHECK_EQ(state_label(Domain::Sensor, s, 1, "C"), std::string("21.5 C"));
    s.state = "2026-10-31T19:00:00+00:00";
    CHECK_EQ(state_label(Domain::Scene, s, -1, "C"), std::string(""));
    s.state = "on";
    CHECK_EQ(state_label(Domain::Script, s, -1, "C"), std::string("Running"));
    s.state = "off";
    CHECK_EQ(state_label(Domain::Script, s, -1, "C"), std::string(""));
    s.state = "on";
    s.device_class = "door";
    CHECK_EQ(state_label(Domain::BinarySensor, s, -1, "C"), std::string("Open"));
    s.device_class = "motion";
    CHECK_EQ(state_label(Domain::BinarySensor, s, -1, "C"), std::string("Detected"));
    s.state = "off";
    CHECK_EQ(state_label(Domain::BinarySensor, s, -1, "C"), std::string("Clear"));
    s.device_class = "";
    CHECK_EQ(state_label(Domain::BinarySensor, s, -1, "C"), std::string("Off"));
    s = State();
    s.state = "locked";
    CHECK_EQ(state_label(Domain::Lock, s, -1, "C"), std::string("Locked"));
    s.state = "closed";
    CHECK_EQ(state_label(Domain::Cover, s, -1, "C"), std::string("Closed"));
    s.state = "heat_cool";
    s.current_temperature = 21.5;
    CHECK_EQ(state_label(Domain::Climate, s, -1, "C"), std::string("Heat/cool, 21.5 C"));
    s.state = "heat";
    s.current_temperature = NAN;
    CHECK_EQ(state_label(Domain::Climate, s, -1, "C"), std::string("Heat"));

    s = State();
    s.state = "on";
    s.brightness = 184;
    CHECK(brightness_pct(s) == 72);
    s.brightness = 1;
    CHECK(brightness_pct(s) == 1);
    s.state = "off";
    CHECK(brightness_pct(s) == 0);
    s.device_class = "garage";
    CHECK(confirm_cover(s));
    s.device_class = "shade";
    CHECK(!confirm_cover(s));
    CHECK_EQ(confirm_question("unlock", "Front door"), std::string("Unlock Front door?"));
    CHECK_EQ(confirm_question("close_cover", "Garage door"), std::string("Close Garage door?"));

    const std::int64_t now = 1762000000;
    s = State();
    s.state = "heat";
    s.current_temperature = 21.4;
    s.target_temperature = 21;
    s.hvac_action = "heating";
    s.last_changed = now - 120;
    auto rows = detail_rows(Domain::Climate, s, "C", "Living room", now);
    CHECK(rows.size() == 5);
    CHECK_EQ(rows[0].first, std::string("Temperature"));
    CHECK_EQ(rows[0].second, std::string("21.4 C"));
    CHECK_EQ(rows[1].second, std::string("21 C"));
    CHECK_EQ(rows[2].second, std::string("Heating"));
    CHECK_EQ(rows[3].second, std::string("Living room"));
    CHECK_EQ(rows[4].second, clock_hhmm(now - 120));
    CHECK(changed_text(now - 3 * 86400, now).size() == 10); /* a date */
}

void test_qr_group()
{
    const std::string url = "http://homeassistant.local:8123/auth/authorize?client_id=http%3A%2F%2F192.168.68.113%3A41234%2F"
                            "&redirect_uri=http%3A%2F%2F192.168.68.113%3A41234%2Fcallback&state=0123456789abcdef0123456789abcdef";
    const QrMatrix m = qr_encode(url);
    CHECK(m.size >= 49 && m.size <= 61); /* version 8 to 11 */
    /* the three finder patterns: dark outer ring, light ring, dark 3x3 core */
    for (int ox : {0, m.size - 7})
        for (int oy : {0, m.size - 7}) {
            if (ox != 0 && oy != 0) continue;
            CHECK(m.at(ox, oy) && m.at(ox + 6, oy) && m.at(ox, oy + 6) && m.at(ox + 6, oy + 6));
            CHECK(!m.at(ox + 1, oy + 1) && !m.at(ox + 5, oy + 5));
            CHECK(m.at(ox + 3, oy + 3));
        }
    CHECK(!m.at(-1, 0) && !m.at(m.size, 0));
    CHECK(qr_module_px(53, 2, 216) == 3);
    CHECK(qr_module_px(53, 2, 50) == 1);
    CHECK(qr_encode(std::string(4000, 'x')).size == 0); /* too long: no code, no crash */
}
